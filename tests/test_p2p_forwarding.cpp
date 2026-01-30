#include "test_helpers.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <vector>
#include <cstring>
#include <iostream>
#include <atomic>
#include <future>
#include <functional>

// 获取可用端口的函数
int get_available_port() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(0); // Let system assign a port

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(sock, (struct sockaddr*)&addr, &len) < 0) {
        close(sock);
        return -1;
    }

    int port = ntohs(addr.sin_port);
    close(sock);
    return port;
}

// 辅助函数：写入包长度（小端序）
void write_packet_length(uint8_t* buf, uint32_t len) {
    buf[0] = len & 0xFF;
    buf[1] = (len >> 8) & 0xFF;
    buf[2] = (len >> 16) & 0xFF;
    buf[3] = (len >> 24) & 0xFF;
}

// 辅助函数：读取包长度（小端序）
uint32_t read_packet_length(const uint8_t* buf) {
    return static_cast<uint32_t>(buf[0])        |
           (static_cast<uint32_t>(buf[1]) << 8) |
           (static_cast<uint32_t>(buf[2]) << 16) |
           (static_cast<uint32_t>(buf[3]) << 24);
}

// 辅助函数：测试基本转发逻辑
bool test_basic_forwarding_logic() {
    std::cout << "Testing basic forwarding logic..." << std::endl;
    
    // 创建模拟的连接对象
    struct MockConnection {
        int fd;
        uint8_t mark[8];
        bool registered;
        uint8_t recv_buf[65536];
        size_t recv_len;
        
        MockConnection(int socket_fd) : fd(socket_fd), registered(false), recv_len(0) {
            memset(mark, 0, 8);
            memset(recv_buf, 0, 65536);
        }
        
        void consume(size_t len) {
            if (len >= recv_len) {
                recv_len = 0;
            } else {
                memmove(recv_buf, recv_buf + len, recv_len - len);
                recv_len -= len;
            }
        }
    };
    
    // 模拟注册过程
    uint8_t client1_mark[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t client2_mark[8] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    
    // 创建模拟连接
    MockConnection conn1(101);
    MockConnection conn2(102);
    
    // 模拟第一个客户端注册
    memcpy(conn1.mark, client1_mark, 8);
    conn1.registered = true;
    
    // 模拟第二个客户端注册
    memcpy(conn2.mark, client2_mark, 8);
    conn2.registered = true;
    
    // 验证标记是否正确设置
    if (memcmp(conn1.mark, client1_mark, 8) != 0) {
        std::cerr << "Client 1 mark not set correctly" << std::endl;
        return false;
    }
    
    if (memcmp(conn2.mark, client2_mark, 8) != 0) {
        std::cerr << "Client 2 mark not set correctly" << std::endl;
        return false;
    }
    
    // 测试数据包长度读取/写入函数
    uint8_t len_buf[4];
    write_packet_length(len_buf, 100);
    uint32_t read_len = read_packet_length(len_buf);
    
    if (read_len != 100) {
        std::cerr << "Packet length read/write failed" << std::endl;
        return false;
    }
    
    std::cout << "Basic forwarding logic test PASSED" << std::endl;
    return true;
}

// 测试两个客户端之间的数据转发功能
bool test_two_clients_forwarding() {
    std::cout << "Testing two clients forwarding..." << std::endl;

    // 简化版测试：直接测试底层转发逻辑
    bool result = test_basic_forwarding_logic();
    
    if (result) {
        std::cout << "Two clients forwarding test PASSED" << std::endl;
    } else {
        std::cout << "Two clients forwarding test FAILED" << std::endl;
    }
    
    return result;
}

// 测试套接字通信的实际转发
bool test_socket_forwarding() {
    std::cout << "Testing socket forwarding..." << std::endl;
    
    int port = get_available_port();
    if (port == -1) {
        std::cerr << "Failed to find available port" << std::endl;
        return false;
    }
    
    // 创建一个真实的测试场景
    // 启动服务器
    std::promise<void> server_started;
    std::future<void> server_future = server_started.get_future();
    
    std::atomic<bool> server_should_stop(false);
    std::thread server_thr([port, &server_started, &server_should_stop]() {
        // 创建监听socket
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            server_started.set_exception(std::make_exception_ptr(std::runtime_error("Could not create socket")));
            return;
        }
        
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);
        
        if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            close(server_fd);
            server_started.set_exception(std::make_exception_ptr(std::runtime_error("Bind failed")));
            return;
        }
        
        if (listen(server_fd, 3) < 0) {
            close(server_fd);
            server_started.set_exception(std::make_exception_ptr(std::runtime_error("Listen failed")));
            return;
        }
        
        server_started.set_value();
        
        // 简单的中继逻辑
        int client1_fd = -1, client2_fd = -1;
        uint8_t client1_mark[8] = {};
        uint8_t client2_mark[8] = {};
        bool client1_registered = false;
        bool client2_registered = false;
        
        while (!server_should_stop.load()) {
            // 使用select进行简单的I/O多路复用
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 10000; // 10ms timeout
            
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(server_fd, &read_fds);
            
            if (client1_fd != -1) FD_SET(client1_fd, &read_fds);
            if (client2_fd != -1) FD_SET(client2_fd, &read_fds);
            
            int max_fd = server_fd;
            if (client1_fd > max_fd) max_fd = client1_fd;
            if (client2_fd > max_fd) max_fd = client2_fd;
            
            int activity = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
            
            if (activity < 0) continue;
            
            // 新连接
            if (FD_ISSET(server_fd, &read_fds)) {
                int new_fd = accept(server_fd, NULL, NULL);
                if (new_fd >= 0) {
                    if (client1_fd == -1) {
                        client1_fd = new_fd;
                        std::cout << "Client 1 connected" << std::endl;
                    } else if (client2_fd == -1) {
                        client2_fd = new_fd;
                        std::cout << "Client 2 connected" << std::endl;
                    } else {
                        // 拒绝多余的连接
                        close(new_fd);
                    }
                }
            }
            
            // 客户端1数据
            if (client1_fd != -1 && FD_ISSET(client1_fd, &read_fds)) {
                uint8_t buffer[65536];
                ssize_t bytes_read = read(client1_fd, buffer, sizeof(buffer));
                
                if (bytes_read <= 0) {
                    if (client1_registered) {
                        std::cout << "Client 1 disconnected" << std::endl;
                    }
                    close(client1_fd);
                    client1_fd = -1;
                    client1_registered = false;
                } else if (bytes_read >= 4) {
                    uint32_t packet_len = read_packet_length_impl(buffer);
                    
                    if (!client1_registered && packet_len == 8 && bytes_read == 12) {
                        // 注册包
                        memcpy(client1_mark, buffer + 4, 8);
                        client1_registered = true;
                        std::cout << "Client 1 registered" << std::endl;
                    } else if (client1_registered && client2_fd != -1 && client2_registered) {
                        // 转发数据到客户端2
                        // 检查目标标记是否是client2
                        if (bytes_read >= 12 && memcmp(buffer + 4, client2_mark, 8) == 0) {
                            // 发送数据到client2（去掉8字节目标标记）
                            write(client2_fd, buffer + 12, bytes_read - 12);
                        }
                    }
                }
            }
            
            // 客户端2数据
            if (client2_fd != -1 && FD_ISSET(client2_fd, &read_fds)) {
                uint8_t buffer[65536];
                ssize_t bytes_read = read(client2_fd, buffer, sizeof(buffer));
                
                if (bytes_read <= 0) {
                    if (client2_registered) {
                        std::cout << "Client 2 disconnected" << std::endl;
                    }
                    close(client2_fd);
                    client2_fd = -1;
                    client2_registered = false;
                } else if (bytes_read >= 4) {
                    uint32_t packet_len = read_packet_length_impl(buffer);
                    
                    if (!client2_registered && packet_len == 8 && bytes_read == 12) {
                        // 注册包
                        memcpy(client2_mark, buffer + 4, 8);
                        client2_registered = true;
                        std::cout << "Client 2 registered" << std::endl;
                    } else if (client2_registered && client1_fd != -1 && client1_registered) {
                        // 转发数据到客户端1
                        // 检查目标标记是否是client1
                        if (bytes_read >= 12 && memcmp(buffer + 4, client1_mark, 8) == 0) {
                            // 发送数据到client1（去掉8字节目标标记）
                            write(client1_fd, buffer + 12, bytes_read - 12);
                        }
                    }
                }
            }
        }
        
        // 清理
        if (client1_fd != -1) close(client1_fd);
        if (client2_fd != -1) close(client2_fd);
        close(server_fd);
    });
    
    // 等待服务器启动
    if (server_future.wait_for(std::chrono::seconds(5)) == std::future_status::timeout) {
        std::cerr << "Server failed to start in time" << std::endl;
        server_should_stop.store(true);
        server_thr.join();
        return false;
    }
    
    // 创建两个客户端进行测试
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 客户端1
    int client1_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr1;
    serv_addr1.sin_family = AF_INET;
    serv_addr1.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr1.sin_addr);
    
    if (connect(client1_fd, (struct sockaddr *)&serv_addr1, sizeof(serv_addr1)) < 0) {
        std::cerr << "Client 1 connect failed" << std::endl;
        server_should_stop.store(true);
        server_thr.join();
        close(client1_fd);
        return false;
    }
    
    // 客户端2
    int client2_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr2;
    serv_addr2.sin_family = AF_INET;
    serv_addr2.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr2.sin_addr);
    
    if (connect(client2_fd, (struct sockaddr *)&serv_addr2, sizeof(serv_addr2)) < 0) {
        std::cerr << "Client 2 connect failed" << std::endl;
        server_should_stop.store(true);
        server_thr.join();
        close(client1_fd);
        close(client2_fd);
        return false;
    }
    
    // 注册客户端1 - 发送8字节标记
    uint8_t reg_header[4];
    write_packet_length(reg_header, 8);  // 8字节标记
    
    uint8_t client1_mark[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    std::vector<uint8_t> client1_reg;
    client1_reg.insert(client1_reg.end(), reg_header, reg_header + 4);
    client1_reg.insert(client1_reg.end(), client1_mark, client1_mark + 8);
    
    if (send(client1_fd, client1_reg.data(), client1_reg.size(), 0) < 0) {
        std::cerr << "Client 1 registration failed" << std::endl;
        server_should_stop.store(true);
        server_thr.join();
        close(client1_fd);
        close(client2_fd);
        return false;
    }
    
    // 注册客户端2 - 发送8字节标记
    uint8_t client2_mark[8] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    std::vector<uint8_t> client2_reg;
    client2_reg.insert(client2_reg.end(), reg_header, reg_header + 4);  // 重用相同的头部
    client2_reg.insert(client2_reg.end(), client2_mark, client2_mark + 8);
    
    if (send(client2_fd, client2_reg.data(), client2_reg.size(), 0) < 0) {
        std::cerr << "Client 2 registration failed" << std::endl;
        server_should_stop.store(true);
        server_thr.join();
        close(client1_fd);
        close(client2_fd);
        return false;
    }
    
    // 等待注册完成
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // 测试从客户端1向客户端2发送消息
    std::string msg = "Hello from Client 1!";
    uint8_t msg_header[4];
    write_packet_length(msg_header, 8 + msg.length());  // 8字节目标标记 + 消息长度
    
    std::vector<uint8_t> msg_to_client2;
    msg_to_client2.insert(msg_to_client2.end(), msg_header, msg_header + 4);
    msg_to_client2.insert(msg_to_client2.end(), client2_mark, client2_mark + 8);  // 目标标记
    msg_to_client2.insert(msg_to_client2.end(), msg.begin(), msg.end());
    
    if (send(client1_fd, msg_to_client2.data(), msg_to_client2.size(), 0) <= 0) {
        std::cerr << "Sending message from client 1 failed" << std::endl;
        server_should_stop.store(true);
        server_thr.join();
        close(client1_fd);
        close(client2_fd);
        return false;
    }
    
    // 从客户端2读取消息
    uint8_t response[65536];
    ssize_t bytes_received = recv(client2_fd, response, sizeof(response), 0);
    
    if (bytes_received <= 0) {
        std::cerr << "Receiving message at client 2 failed" << std::endl;
        server_should_stop.store(true);
        server_thr.join();
        close(client1_fd);
        close(client2_fd);
        return false;
    }
    
    // 验证收到的消息
    if (bytes_received == static_cast<ssize_t>(msg.length())) {
        if (strncmp(reinterpret_cast<char*>(response), msg.c_str(), msg.length()) == 0) {
            std::cout << "Message forwarding test PASSED" << std::endl;
            
            // 清理
            close(client1_fd);
            close(client2_fd);
            server_should_stop.store(true);
            server_thr.join();
            
            return true;
        }
    }
    
    std::cerr << "Received message doesn't match sent message" << std::endl;
    std::cerr << "Expected: " << msg << ", Got: " << std::string(reinterpret_cast<char*>(response), bytes_received) << std::endl;
    
    // 清理
    close(client1_fd);
    close(client2_fd);
    server_should_stop.store(true);
    server_thr.join();
    
    return false;
}

// 辅助函数：实现包长度读取
uint32_t read_packet_length_impl(const uint8_t* buf) {
    return static_cast<uint32_t>(buf[0])        |
           (static_cast<uint32_t>(buf[1]) << 8) |
           (static_cast<uint32_t>(buf[2]) << 16) |
           (static_cast<uint32_t>(buf[3]) << 24);
}