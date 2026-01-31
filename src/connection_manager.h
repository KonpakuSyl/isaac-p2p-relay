#ifndef CONNECTION_MANAGER_H
#define CONNECTION_MANAGER_H

#include "platform_win.h"
#include "packet_queue.h"
#include <p2p_network.h>

#include <unordered_map>
#include <vector>
#include <string>
#include <functional>
#include <cstdint>

namespace p2p {

/**
 * 连接信息
 */
struct Connection {
    SocketHandle socket = INVALID_SOCKET_HANDLE;
    P2PPeerID peerID = P2P_INVALID_PEER_ID;
    std::string remoteIP;
    uint16_t remotePort = 0;
    bool connected = false;
    
    bool autoReconnect = false;
    uint32_t reconnectBaseDelayMs = 500;
    uint32_t reconnectMaxDelayMs = 10 * 1000;
    uint32_t reconnectAttempt = 0;
    uint64_t nextReconnectAtMs = 0;
    
    PacketQueue receiveQueue;    // 接收到的完整数据包队列
    ReceiveBuffer recvBuffer;    // 接收缓冲区 (用于 TCP 流解析)
    std::vector<uint8_t> sendBuffer;  // 待发送数据缓冲区
    
    Connection() = default;
    ~Connection() = default;
    
    // 禁止拷贝
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    
    // 允许移动
    Connection(Connection&&) = default;
    Connection& operator=(Connection&&) = default;
};

/**
 * 连接事件类型
 */
enum class ConnectionEvent {
    Connected,
    Disconnected
};

/**
 * 连接管理器 - 管理所有 P2P 连接
 */
class ConnectionManager {
public:
    using ConnectionCallback = std::function<void(P2PPeerID, ConnectionEvent)>;
    
    ConnectionManager();
    ~ConnectionManager();
    
    // 禁止拷贝
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;
    
    /**
     * 初始化
     * @return true 成功
     */
    bool initialize();
    
    /**
     * 关闭并清理所有资源
     */
    void shutdown();
    
    /**
     * 开始监听端口
     * @param port 端口号
     * @return true 成功
     */
    bool listen(uint16_t port);
    
    /**
     * 停止监听
     */
    void stopListen();
    
    /**
     * 连接到远程对端
     * @param ip IP 地址
     * @param port 端口号
     * @param outPeerID 输出 PeerID
     * @return true 成功
     */
    bool connect(const char* ip, uint16_t port, P2PPeerID& outPeerID);
    
    /**
     * 启用/禁用指定连接的自动重连
     * @param peerID 对端 ID
     * @param enable 是否启用
     * @param baseDelayMs 初始重连延迟
     * @param maxDelayMs 最大重连延迟
     */
    void setAutoReconnect(P2PPeerID peerID, bool enable, uint32_t baseDelayMs = 500, uint32_t maxDelayMs = 10 * 1000);
    
    /**
     * 断开指定连接
     * @param peerID 对端 ID
     */
    void disconnect(P2PPeerID peerID);
    
    /**
     * 发送数据包
     * @param peerID 对端 ID
     * @param data 数据
     * @param size 大小
     * @return true 成功
     */
    bool sendPacket(P2PPeerID peerID, const void* data, uint32_t size);
    
    /**
     * 检查是否有数据包可读
     * @param peerID 对端 ID (0 表示任意)
     * @param outSize 输出包大小
     * @param outPeerID 输出发送方 PeerID
     * @return true 有数据包
     */
    bool isPacketAvailable(P2PPeerID peerID, uint32_t* outSize, P2PPeerID* outPeerID);
    
    /**
     * 读取数据包
     * @param peerID 对端 ID (0 表示任意)
     * @param buffer 缓冲区
     * @param bufferSize 缓冲区大小
     * @param outReadSize 输出实际读取大小
     * @param outPeerID 输出发送方 PeerID
     * @return true 成功
     */
    bool readPacket(P2PPeerID peerID, void* buffer, uint32_t bufferSize, 
                    uint32_t* outReadSize, P2PPeerID* outPeerID);
    
    /**
     * 处理网络事件
     */
    void processEvents();
    
    /**
     * 设置连接回调
     */
    void setConnectionCallback(ConnectionCallback callback);
    
    /**
     * 获取连接数量
     */
    uint32_t getPeerCount() const;
    
    /**
     * 获取所有已连接的对端
     */
    uint32_t getConnectedPeers(P2PPeerID* outPeerIDs, uint32_t maxCount) const;
    
    /**
     * 检查是否已初始化
     */
    bool isInitialized() const { return m_initialized; }

private:
    /**
     * 分配新的 PeerID
     */
    P2PPeerID allocatePeerID();
    
    /**
     * 接受新连接
     */
    void acceptNewConnections();
    
    /**
     * 处理连接的读取事件
     */
    void processRead(Connection& conn);
    
    /**
     * 处理连接的写入事件
     */
    void processWrite(Connection& conn);
    
    /**
     * 移除断开的连接
     */
    void removeDisconnected();
    
    /**
     * 尝试重连
     */
    void attemptReconnects();
    
    /**
     * 通知连接事件
     */
    void notifyConnectionEvent(P2PPeerID peerID, ConnectionEvent event);
    
    /**
     * 创建发送帧 (添加长度头)
     */
    static std::vector<uint8_t> createSendFrame(const void* data, uint32_t size);

    static uint64_t nowMs();

    static SocketHandle connectWithTimeout(const char* ip, uint16_t port, uint32_t timeoutMs);

private:
    bool m_initialized = false;
    SocketHandle m_listenSocket = INVALID_SOCKET_HANDLE;
    uint16_t m_listenPort = 0;
    
    P2PPeerID m_nextPeerID = 1;  // PeerID 从 1 开始
    std::unordered_map<P2PPeerID, Connection> m_connections;
    std::vector<P2PPeerID> m_pendingRemove;  // 待移除的连接
    
    ConnectionCallback m_connectionCallback;
    
    static constexpr size_t RECV_BUFFER_SIZE = 65536;  // 接收缓冲区大小
};

} // namespace p2p

#endif // CONNECTION_MANAGER_H
