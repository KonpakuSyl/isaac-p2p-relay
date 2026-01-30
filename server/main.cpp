/**
 * TCP P2P Relay Server
 *
 * 低并发TCP转发服务器，最多支持4个连接
 * - 首次连接：客户端发送8字节标记注册自己
 * - 后续数据：前8字节为目标标记，转发时去掉标记只发数据
 * - 无匹配时：丢弃不处理
 *
 * 使用: ./relay_server <port>
 *
 * 编译选项:
 *   -DDEBUG_MODE  启用debug级别日志
 */

#include <iostream>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <ctime>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <array>
#include <vector>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

// 常量定义
constexpr int MAX_CONNECTIONS = 4;
constexpr int MAX_EVENTS = 8;
constexpr int BUFFER_SIZE = 65536;          // 接收缓冲区大小
constexpr int MAX_PACKET_SIZE = 65535;      // 最大包大小
constexpr int LENGTH_SIZE = 4;              // 长度字段大小（4字节）
constexpr int MARK_SIZE = 8;                // 标记大小（8字节）
constexpr int LOG_BUFFER_SIZE = 1024;

// 日志级别枚举（避免与syslog宏冲突）
enum class LogLevel {
    LVL_DEBUG = 0,
    LVL_INFO,
    LVL_WARN,
    LVL_ERR
};

// 日志级别名称
static const char* log_level_names[] = {
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR"
};

// 日志级别对应的syslog优先级
static int log_level_to_syslog[] = {
    7,  // LOGD
    6,  // LOGI
    4,  // LOGWING
    3   // LOGE
};

/**
 * 日志类 - 同时输出到标准输出和syslog
 */
class Logger {
public:
    static void init(const char* ident) {
        openlog(ident, LOG_PID | LOG_NDELAY, LOG_DAEMON);
        initialized_ = true;
    }

    static void close() {
        if (initialized_) {
            closelog();
            initialized_ = false;
        }
    }

    static void log(LogLevel level, const char* fmt, ...) {
        char buffer[LOG_BUFFER_SIZE];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        // 获取当前时间
        time_t now = time(nullptr);
        struct tm* tm_info = localtime(&now);
        char time_buf[32];
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

        // 输出到标准输出/错误
        FILE* out = (level >= LogLevel::LVL_WARN) ? stderr : stdout;
        fprintf(out, "[%s] [%s] %s\n", time_buf, log_level_names[static_cast<int>(level)], buffer);
        fflush(out);

        // 输出到syslog
        if (initialized_) {
            syslog(log_level_to_syslog[static_cast<int>(level)], "%s", buffer);
        }
    }

    // 格式化8字节标记为十六进制字符串
    static std::string format_mark(const uint8_t* mark) {
        char buf[MARK_SIZE * 2 + 1];
        for (int i = 0; i < MARK_SIZE; i++) {
            snprintf(buf + i * 2, 3, "%02x", mark[i]);
        }
        return std::string(buf);
    }

private:
    static bool initialized_;
};

bool Logger::initialized_ = false;

// 日志宏定义（使用LOGX避免与syslog宏冲突）
#define LOGI(fmt, ...)  Logger::log(LogLevel::LVL_INFO, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...)  Logger::log(LogLevel::LVL_WARN, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...)  Logger::log(LogLevel::LVL_ERR, fmt, ##__VA_ARGS__)

// DEBUG级别日志 - 仅在编译时定义DEBUG_MODE时启用
#ifdef DEBUG_MODE
    #define LOGD(fmt, ...) Logger::log(LogLevel::LVL_DEBUG, fmt, ##__VA_ARGS__)
#else
    #define LOGD(fmt, ...) ((void)0)
#endif

// 全局运行标志
volatile sig_atomic_t g_running = 1;

// 连接信息结构
struct Connection {
    int fd;
    uint8_t mark[MARK_SIZE];  // 8字节标记
    bool registered;           // 是否已注册

    std::vector<uint8_t> send_buf;

    // 接收缓冲区（处理粘包/拆包）
    uint8_t recv_buf[BUFFER_SIZE];
    size_t recv_len;           // 当前缓冲区中的数据长度

    Connection() : fd(-1), registered(false), recv_len(0) {
        std::memset(mark, 0, MARK_SIZE);
        std::memset(recv_buf, 0, BUFFER_SIZE);
    }

    explicit Connection(int socket_fd) : fd(socket_fd), registered(false), recv_len(0) {
        std::memset(mark, 0, MARK_SIZE);
        std::memset(recv_buf, 0, BUFFER_SIZE);
    }

    // 从缓冲区移除已处理的数据
    void consume(size_t len) {
        if (len >= recv_len) {
            recv_len = 0;
        } else {
            std::memmove(recv_buf, recv_buf + len, recv_len - len);
            recv_len -= len;
        }
    }
};

// 将8字节标记转换为uint64_t用于map key
inline uint64_t mark_to_key(const uint8_t* mark) {
    uint64_t key;
    std::memcpy(&key, mark, sizeof(key));
    return key;
}

// 全局连接管理
std::unordered_map<int, Connection> g_connections;      // fd -> Connection
std::unordered_map<uint64_t, int> g_mark_to_fd;         // mark -> fd

static std::atomic<uint64_t> g_stat_bytes_in{0};
static std::atomic<uint64_t> g_stat_bytes_out{0};
static std::atomic<uint64_t> g_stat_packets_in{0};
static std::atomic<uint64_t> g_stat_packets_out{0};
static std::atomic<uint64_t> g_stat_drop_no_target{0};
static std::atomic<uint64_t> g_stat_drop_small_packet{0};
static std::atomic<uint64_t> g_stat_drop_send_eagain{0};
static std::atomic<uint64_t> g_stat_partial_writes{0};
static std::atomic<uint64_t> g_stat_write_errors{0};
static std::atomic<uint64_t> g_stat_event_loops{0};
static std::atomic<uint64_t> g_stat_events{0};

void close_connection(int fd, int epfd);

static bool update_epoll_events(int epfd, int fd, bool want_write) {
    struct epoll_event ev;
    ev.data.fd = fd;
    uint32_t events = EPOLLIN;
    if (want_write) {
        events |= EPOLLOUT;
    }
    ev.events = events;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == -1) {
        LOGE("epoll_ctl MOD 失败 fd=%d: %s", fd, strerror(errno));
        return false;
    }
    return true;
}

static void flush_send_buffer(Connection& conn, int epfd) {
    if (conn.fd < 0) {
        return;
    }
    if (conn.send_buf.empty()) {
        (void)update_epoll_events(epfd, conn.fd, false);
        return;
    }

    while (!conn.send_buf.empty()) {
        ssize_t sent = write(conn.fd, conn.send_buf.data(), conn.send_buf.size());
        if (sent > 0) {
            g_stat_bytes_out.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
            conn.send_buf.erase(conn.send_buf.begin(), conn.send_buf.begin() + sent);
        } else if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            (void)update_epoll_events(epfd, conn.fd, true);
            return;
        } else {
            g_stat_write_errors.fetch_add(1, std::memory_order_relaxed);
            close_connection(conn.fd, epfd);
            return;
        }
    }

    (void)update_epoll_events(epfd, conn.fd, false);
}

void handle_client_write(int fd, int epfd) {
    auto it = g_connections.find(fd);
    if (it == g_connections.end()) {
        close_connection(fd, epfd);
        return;
    }
    flush_send_buffer(it->second, epfd);
}

// 信号处理函数 - 只能使用异步信号安全的操作
void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        // 只设置标志，不调用非异步信号安全的函数（如printf、syslog等）
        g_running = 0;
    }
}

// 设置socket为非阻塞模式
bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        LOGE("fcntl F_GETFL 失败: %s", strerror(errno));
        return false;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        LOGE("fcntl F_SETFL 失败: %s", strerror(errno));
        return false;
    }
    LOGD("设置fd=%d为非阻塞模式", fd);
    return true;
}

// 创建监听socket
int create_listen_socket(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        LOGE("创建socket失败: %s", strerror(errno));
        return -1;
    }
    LOGD("创建socket fd=%d", listen_fd);

    // 设置SO_REUSEADDR
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        LOGE("setsockopt SO_REUSEADDR 失败: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }

    // 绑定地址
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        LOGE("bind失败: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }
    LOGD("bind成功 port=%d", port);

    // 开始监听
    if (listen(listen_fd, SOMAXCONN) == -1) {
        LOGE("listen失败: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }

    // 设置非阻塞
    if (!set_nonblocking(listen_fd)) {
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

// 关闭连接并清理资源
void close_connection(int fd, int epfd) {
    auto it = g_connections.find(fd);
    if (it != g_connections.end()) {
        // 如果已注册，从标记映射中移除
        if (it->second.registered) {
            uint64_t key = mark_to_key(it->second.mark);
            g_mark_to_fd.erase(key);
            LOGI("连接断开 fd=%d mark=%s", fd, Logger::format_mark(it->second.mark).c_str());
        } else {
            LOGI("未注册连接断开 fd=%d", fd);
        }
        g_connections.erase(it);
    }

    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    LOGD("已清理fd=%d资源", fd);
}

// 处理新连接
void handle_new_connection(int listen_fd, int epfd) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOGE("accept失败: %s", strerror(errno));
        }
        return;
    }

    // 检查连接数限制
    if (g_connections.size() >= MAX_CONNECTIONS) {
        LOGW("连接数已满，拒绝新连接 fd=%d", client_fd);
        close(client_fd);
        return;
    }

    // 设置非阻塞
    if (!set_nonblocking(client_fd)) {
        close(client_fd);
        return;
    }

    // 设置TCP_NODELAY，禁用Nagle算法，减少小包延迟
    int flag = 1;
    if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == -1) {
        LOGW("setsockopt TCP_NODELAY 失败 fd=%d: %s", client_fd, strerror(errno));
        // 非致命错误，继续
    }

    // 设置SO_KEEPALIVE，检测死连接
    if (setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) == -1) {
        LOGW("setsockopt SO_KEEPALIVE 失败 fd=%d: %s", client_fd, strerror(errno));
        // 非致命错误，继续
    }

    // 添加到epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = client_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
        LOGE("epoll_ctl ADD 失败: %s", strerror(errno));
        close(client_fd);
        return;
    }

    // 记录连接
    g_connections[client_fd] = Connection(client_fd);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
    LOGI("新连接 fd=%d from %s:%d (当前连接数: %zu/%d)",
             client_fd, ip_str, ntohs(client_addr.sin_port),
             g_connections.size(), MAX_CONNECTIONS);
}

// 从缓冲区读取4字节长度（小端序）
inline uint32_t read_packet_length(const uint8_t* buf) {
    return static_cast<uint32_t>(buf[0])        |
           (static_cast<uint32_t>(buf[1]) << 8) |
           (static_cast<uint32_t>(buf[2]) << 16) |
           (static_cast<uint32_t>(buf[3]) << 24);
}

// 写入4字节长度（小端序）
inline void write_packet_length(uint8_t* buf, uint32_t len) {
    buf[0] = len & 0xFF;
    buf[1] = (len >> 8) & 0xFF;
    buf[2] = (len >> 16) & 0xFF;
    buf[3] = (len >> 24) & 0xFF;
}

// 处理单个完整的数据包
// 返回值: true=继续处理, false=需要关闭连接
bool process_packet(Connection& conn, const uint8_t* data, uint32_t data_len, int epfd) {
    int fd = conn.fd;

    g_stat_packets_in.fetch_add(1, std::memory_order_relaxed);

    // 首次数据：注册标记
    // 包格式: 4字节长度 + 8字节标记
    if (!conn.registered) {
        if (data_len != MARK_SIZE) {
            LOGE("注册包长度错误 fd=%d expected=%d received=%u", fd, MARK_SIZE, data_len);
            return false;
        }

        // 复制标记
        std::memcpy(conn.mark, data, MARK_SIZE);
        uint64_t key = mark_to_key(conn.mark);

        // 检查标记是否已存在
        if (g_mark_to_fd.find(key) != g_mark_to_fd.end()) {
            LOGW("标记已存在，拒绝注册 fd=%d mark=%s", fd, Logger::format_mark(conn.mark).c_str());
            return false;
        }

        // 注册
        conn.registered = true;
        g_mark_to_fd[key] = fd;

        LOGI("注册成功 fd=%d mark=%s", fd, Logger::format_mark(conn.mark).c_str());
        return true;
    }

    // 后续数据：转发
    // 包格式: 4字节长度 + 8字节目标标记 + 实际数据
    if (data_len < MARK_SIZE) {
        LOGW("数据包过小，无法解析目标标记 fd=%d size=%u", fd, data_len);
        g_stat_drop_small_packet.fetch_add(1, std::memory_order_relaxed);
        return true;  // 丢弃但不断开连接
    }

    // 解析目标标记
    uint64_t target_key = mark_to_key(data);

    auto target_it = g_mark_to_fd.find(target_key);
    if (target_it == g_mark_to_fd.end()) {
        // 目标不存在，丢弃
        LOGW("目标不存在，丢弃数据 from_fd=%d target_mark=%s data_size=%u",
                 fd, Logger::format_mark(data).c_str(), data_len - MARK_SIZE);
        g_stat_drop_no_target.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    int target_fd = target_it->second;

    // 转发数据（去掉前8字节目标标记，但保留长度前缀格式）
    uint32_t payload_len = data_len - MARK_SIZE;
    if (payload_len > 0) {
        auto t_it = g_connections.find(target_fd);
        if (t_it == g_connections.end()) {
            return true;
        }

        Connection& target_conn = t_it->second;
        size_t prev_size = target_conn.send_buf.size();
        target_conn.send_buf.resize(prev_size + LENGTH_SIZE + payload_len);

        write_packet_length(target_conn.send_buf.data() + prev_size, payload_len);
        std::memcpy(target_conn.send_buf.data() + prev_size + LENGTH_SIZE, data + MARK_SIZE, payload_len);

        g_stat_packets_out.fetch_add(1, std::memory_order_relaxed);

        flush_send_buffer(target_conn, epfd);
    }

    return true;
}

// 处理客户端数据
void handle_client_data(int fd, int epfd) {
    auto it = g_connections.find(fd);
    if (it == g_connections.end()) {
        LOGE("未找到连接信息 fd=%d", fd);
        close_connection(fd, epfd);
        return;
    }

    Connection& conn = it->second;

    // 读取数据到连接缓冲区
    size_t available = BUFFER_SIZE - conn.recv_len;
    if (available == 0) {
        LOGE("接收缓冲区已满 fd=%d", fd);
        close_connection(fd, epfd);
        return;
    }

    ssize_t n = read(fd, conn.recv_buf + conn.recv_len, available);

    if (n <= 0) {
        if (n == 0) {
            LOGD("连接关闭 fd=%d (对端关闭)", fd);
            close_connection(fd, epfd);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOGE("read失败 fd=%d: %s", fd, strerror(errno));
            close_connection(fd, epfd);
        }
        return;
    }

    conn.recv_len += n;
    g_stat_bytes_in.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    LOGD("收到数据 fd=%d size=%zd total_buffered=%zu", fd, n, conn.recv_len);

    // 循环处理所有完整的数据包
    // 包格式: 4字节长度(网络字节序) + 数据
    while (conn.recv_len >= LENGTH_SIZE) {
        // 读取包长度
        uint32_t packet_len = read_packet_length(conn.recv_buf);

        // 检查包长度合法性
        // 注意：total_len = LENGTH_SIZE + packet_len 必须 <= BUFFER_SIZE，否则永远无法接收完整包
        if (packet_len == 0 || packet_len > MAX_PACKET_SIZE ||
            (LENGTH_SIZE + packet_len) > BUFFER_SIZE) {
            LOGE("非法包长度 fd=%d packet_len=%u (max_allowed=%zu)",
                 fd, packet_len, BUFFER_SIZE - LENGTH_SIZE);
            close_connection(fd, epfd);
            return;
        }

        // 检查是否收到完整的包
        uint32_t total_len = LENGTH_SIZE + packet_len;
        if (conn.recv_len < total_len) {
            LOGD("等待更多数据 fd=%d need=%u have=%zu", fd, total_len, conn.recv_len);
            break;  // 等待更多数据
        }

        // 处理完整的数据包
        const uint8_t* packet_data = conn.recv_buf + LENGTH_SIZE;
        if (!process_packet(conn, packet_data, packet_len, epfd)) {
            close_connection(fd, epfd);
            return;
        }

        // 从缓冲区移除已处理的数据
        conn.consume(total_len);
        LOGD("处理完成 fd=%d consumed=%u remaining=%zu", fd, total_len, conn.recv_len);
    }
}

// 打印使用帮助
void print_usage(const char* program) {
    std::cout << "使用: " << program << " <port>" << std::endl;
    std::cout << std::endl;
    std::cout << "协议说明 (4字节长度使用小端序):" << std::endl;
    std::cout << "  包格式: [4字节长度] + [数据]" << std::endl;
    std::cout << std::endl;
    std::cout << "  1. 注册包 (首次发送):" << std::endl;
    std::cout << "     [4字节长度=8] + [8字节标记]" << std::endl;
    std::cout << std::endl;
    std::cout << "  2. 转发包 (后续发送):" << std::endl;
    std::cout << "     [4字节长度] + [8字节目标标记] + [实际数据]" << std::endl;
    std::cout << std::endl;
    std::cout << "  3. 接收包 (服务器转发给目标):" << std::endl;
    std::cout << "     [4字节长度] + [实际数据] (目标标记已去除)" << std::endl;
    std::cout << std::endl;
    std::cout << "  4. 目标不存在时丢弃数据" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        print_usage(argv[0]);
        return 1;
    }

    int port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "无效端口号: %s\n", argv[1]);
        return 1;
    }

    // 初始化日志系统
    Logger::init("relay_server");

    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);  // 忽略SIGPIPE，避免写入关闭的socket导致进程退出

    // 创建监听socket
    int listen_fd = create_listen_socket(port);
    if (listen_fd == -1) {
        Logger::close();
        return 1;
    }

    // 创建epoll实例
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        LOGE("epoll_create1失败: %s", strerror(errno));
        close(listen_fd);
        Logger::close();
        return 1;
    }

    // 添加监听socket到epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        LOGE("epoll_ctl ADD listen_fd 失败: %s", strerror(errno));
        close(epfd);
        close(listen_fd);
        Logger::close();
        return 1;
    }

    LOGI("========================================");
    LOGI("TCP P2P 中转服务器已启动");
    LOGI("监听端口: %d", port);
    LOGI("最大连接数: %d", MAX_CONNECTIONS);
#ifdef DEBUG_MODE
    LOGI("DEBUG模式: 已启用");
#endif
    LOGI("按 Ctrl+C 退出");
    LOGI("========================================");

    // 事件数组
    struct epoll_event events[MAX_EVENTS];

    uint64_t last_bytes_in = 0;
    uint64_t last_bytes_out = 0;
    uint64_t last_packets_in = 0;
    uint64_t last_packets_out = 0;
    uint64_t last_drop_no_target = 0;
    uint64_t last_drop_small_packet = 0;
    uint64_t last_drop_send_eagain = 0;
    uint64_t last_partial_writes = 0;
    uint64_t last_write_errors = 0;
    uint64_t last_event_loops = 0;
    uint64_t last_events = 0;
    auto last_perf_ts = std::chrono::steady_clock::now();

    // 主循环
    while (g_running) {
        g_stat_event_loops.fetch_add(1, std::memory_order_relaxed);
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);  // 1秒超时，便于检查g_running

        if (nfds == -1) {
            if (errno == EINTR) {
                continue;  // 被信号中断，继续
            }
            LOGE("epoll_wait失败: %s", strerror(errno));
            break;
        }

        LOGD("epoll_wait返回 nfds=%d", nfds);

        g_stat_events.fetch_add(static_cast<uint64_t>(nfds), std::memory_order_relaxed);

        auto now_ts = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_ts - last_perf_ts).count();
        if (elapsed_ms >= 1000) {
            uint64_t bytes_in = g_stat_bytes_in.load(std::memory_order_relaxed);
            uint64_t bytes_out = g_stat_bytes_out.load(std::memory_order_relaxed);
            uint64_t packets_in = g_stat_packets_in.load(std::memory_order_relaxed);
            uint64_t packets_out = g_stat_packets_out.load(std::memory_order_relaxed);
            uint64_t drop_no_target = g_stat_drop_no_target.load(std::memory_order_relaxed);
            uint64_t drop_small_packet = g_stat_drop_small_packet.load(std::memory_order_relaxed);
            uint64_t drop_send_eagain = g_stat_drop_send_eagain.load(std::memory_order_relaxed);
            uint64_t partial_writes = g_stat_partial_writes.load(std::memory_order_relaxed);
            uint64_t write_errors = g_stat_write_errors.load(std::memory_order_relaxed);
            uint64_t event_loops = g_stat_event_loops.load(std::memory_order_relaxed);
            uint64_t events_cnt = g_stat_events.load(std::memory_order_relaxed);

            double secs = static_cast<double>(elapsed_ms) / 1000.0;
            uint64_t din = bytes_in - last_bytes_in;
            uint64_t dout = bytes_out - last_bytes_out;
            uint64_t pin = packets_in - last_packets_in;
            uint64_t pout = packets_out - last_packets_out;
            uint64_t d_drop_no_target = drop_no_target - last_drop_no_target;
            uint64_t d_drop_small_packet = drop_small_packet - last_drop_small_packet;
            uint64_t d_drop_send_eagain = drop_send_eagain - last_drop_send_eagain;
            uint64_t d_partial_writes = partial_writes - last_partial_writes;
            uint64_t d_write_errors = write_errors - last_write_errors;
            uint64_t d_event_loops = event_loops - last_event_loops;
            uint64_t d_events = events_cnt - last_events;

            LOGI("PERF dt=%.2fs in=%lluB(%.2fMB/s) out=%lluB(%.2fMB/s) pin=%llu(%.0f/s) pout=%llu(%.0f/s) eagain_drop=%llu partial=%llu werr=%llu loops=%llu events=%llu",
                 secs,
                 static_cast<unsigned long long>(din), (din / secs) / (1024.0 * 1024.0),
                 static_cast<unsigned long long>(dout), (dout / secs) / (1024.0 * 1024.0),
                 static_cast<unsigned long long>(pin), pin / secs,
                 static_cast<unsigned long long>(pout), pout / secs,
                 static_cast<unsigned long long>(d_drop_send_eagain),
                 static_cast<unsigned long long>(d_partial_writes),
                 static_cast<unsigned long long>(d_write_errors),
                 static_cast<unsigned long long>(d_event_loops),
                 static_cast<unsigned long long>(d_events));

            if (d_drop_no_target > 0 || d_drop_small_packet > 0) {
                LOGI("PERF_DROP no_target=%llu small_packet=%llu",
                     static_cast<unsigned long long>(d_drop_no_target),
                     static_cast<unsigned long long>(d_drop_small_packet));
            }

            last_bytes_in = bytes_in;
            last_bytes_out = bytes_out;
            last_packets_in = packets_in;
            last_packets_out = packets_out;
            last_drop_no_target = drop_no_target;
            last_drop_small_packet = drop_small_packet;
            last_drop_send_eagain = drop_send_eagain;
            last_partial_writes = partial_writes;
            last_write_errors = write_errors;
            last_event_loops = event_loops;
            last_events = events_cnt;
            last_perf_ts = now_ts;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                // 新连接
                handle_new_connection(listen_fd, epfd);
            } else {
                // 客户端数据
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    LOGD("fd=%d 收到 EPOLLERR/EPOLLHUP", fd);
                    close_connection(fd, epfd);
                } else if (events[i].events & EPOLLIN) {
                    handle_client_data(fd, epfd);
                }
                if (events[i].events & EPOLLOUT) {
                    handle_client_write(fd, epfd);
                }
            }
        }
    }

    // 清理
    LOGI("收到退出信号，正在清理资源...");

    // 关闭所有客户端连接
    for (auto& pair : g_connections) {
        close(pair.first);
    }
    g_connections.clear();
    g_mark_to_fd.clear();

    close(epfd);
    close(listen_fd);

    LOGI("服务器已关闭");
    Logger::close();
    return 0;
}
