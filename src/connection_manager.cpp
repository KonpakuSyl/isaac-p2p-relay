#include "connection_manager.h"
#include <cstring>
#include <algorithm>
#include <chrono>
#include <limits>

namespace p2p {

uint64_t ConnectionManager::nowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

SocketHandle ConnectionManager::connectWithTimeout(const char* ip, uint16_t port, uint32_t timeoutMs) {
    if (!ip) {
        return INVALID_SOCKET_HANDLE;
    }

    SocketHandle sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET_HANDLE) {
        return INVALID_SOCKET_HANDLE;
    }

    setNoDelay(sock);
    setNonBlocking(sock);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        closeSocket(sock);
        return INVALID_SOCKET_HANDLE;
    }

    int rc = ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        return sock;
    }

    if (!wouldBlock()) {
        closeSocket(sock);
        return INVALID_SOCKET_HANDLE;
    }

    fd_set writeSet;
    fd_set exceptSet;
    FD_ZERO(&writeSet);
    FD_ZERO(&exceptSet);
    FD_SET(sock, &writeSet);
    FD_SET(sock, &exceptSet);

    timeval timeout{};
    timeout.tv_sec = static_cast<long>(timeoutMs / 1000);
    timeout.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);

    int sel = select(static_cast<int>(sock + 1), nullptr, &writeSet, &exceptSet, &timeout);
    if (sel <= 0) {
        closeSocket(sock);
        return INVALID_SOCKET_HANDLE;
    }

    if (FD_ISSET(sock, &exceptSet)) {
        closeSocket(sock);
        return INVALID_SOCKET_HANDLE;
    }

    int soError = 0;
    socklen_t soErrorLen = sizeof(soError);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soErrorLen) != 0) {
        closeSocket(sock);
        return INVALID_SOCKET_HANDLE;
    }

    if (soError != 0) {
        closeSocket(sock);
        return INVALID_SOCKET_HANDLE;
    }

    return sock;
}

ConnectionManager::ConnectionManager() = default;

ConnectionManager::~ConnectionManager() {
    shutdown();
}

bool ConnectionManager::initialize() {
    if (m_initialized) {
        return true;
    }

    if (!initializeSockets()) {
        return false;
    }

    m_initialized = true;
    return true;
}

void ConnectionManager::shutdown() {
    if (!m_initialized) {
        return;
    }

    stopListen();

    for (auto& [peerID, conn] : m_connections) {
        closeSocket(conn.socket);
    }
    m_connections.clear();
    m_pendingRemove.clear();

    cleanupSockets();
    m_initialized = false;
}

bool ConnectionManager::listen(uint16_t port) {
    if (!m_initialized) {
        return false;
    }
    
    // 如果已经在监听，先停止
    stopListen();
    
    // 创建监听 socket
    m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET_HANDLE) {
        return false;
    }
    
    // 设置 socket 选项
    setReuseAddr(m_listenSocket);
    setNonBlocking(m_listenSocket);
    
    // 绑定地址
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(m_listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET_HANDLE;
        return false;
    }
    
    // 开始监听
    if (::listen(m_listenSocket, SOMAXCONN) != 0) {
        closeSocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET_HANDLE;
        return false;
    }
    
    m_listenPort = port;
    return true;
}

void ConnectionManager::stopListen() {
    if (m_listenSocket != INVALID_SOCKET_HANDLE) {
        closeSocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET_HANDLE;
        m_listenPort = 0;
    }
}

bool ConnectionManager::connect(const char* ip, uint16_t port, P2PPeerID& outPeerID) {
    if (!m_initialized || !ip) {
        return false;
    }

    SocketHandle sock = connectWithTimeout(ip, port, 2000);
    if (sock == INVALID_SOCKET_HANDLE) {
        return false;
    }
     
    // 分配 PeerID 并创建连接
    P2PPeerID peerID = allocatePeerID();
    
    Connection conn;
    conn.socket = sock;
    conn.peerID = peerID;
    conn.remoteIP = ip;
    conn.remotePort = port;
    conn.connected = true;
    
    m_connections.emplace(peerID, std::move(conn));
    
    outPeerID = peerID;
    
    // 通知连接事件
    notifyConnectionEvent(peerID, ConnectionEvent::Connected);
     
    return true;
}

void ConnectionManager::setAutoReconnect(P2PPeerID peerID, bool enable, uint32_t baseDelayMs, uint32_t maxDelayMs) {
    auto it = m_connections.find(peerID);
    if (it == m_connections.end()) {
        return;
    }

    Connection& conn = it->second;
    conn.autoReconnect = enable;
    conn.reconnectBaseDelayMs = baseDelayMs;
    conn.reconnectMaxDelayMs = maxDelayMs;
    conn.reconnectAttempt = 0;
    conn.nextReconnectAtMs = 0;
}

void ConnectionManager::disconnect(P2PPeerID peerID) {
    auto it = m_connections.find(peerID);
    if (it == m_connections.end()) {
        return;
    }

    Connection& conn = it->second;
    closeSocket(conn.socket);
    conn.socket = INVALID_SOCKET_HANDLE;
    conn.connected = false;
    conn.recvBuffer.clear();
    conn.receiveQueue.clear();
    conn.sendBuffer.clear();
    notifyConnectionEvent(peerID, ConnectionEvent::Disconnected);

    if (conn.autoReconnect) {
        conn.reconnectAttempt = 0;
        conn.nextReconnectAtMs = nowMs() + conn.reconnectBaseDelayMs;
        return;
    }

    m_connections.erase(it);
}

bool ConnectionManager::sendPacket(P2PPeerID peerID, const void* data, uint32_t size) {
    if (!data || size == 0) {
        return false;
    }

    auto it = m_connections.find(peerID);
    if (it == m_connections.end() || !it->second.connected) {
        return false;
    }

    Connection& conn = it->second;

    std::vector<uint8_t> frame = createSendFrame(data, size);

    int sent = send(conn.socket, reinterpret_cast<const char*>(frame.data()),
                    static_cast<int>(frame.size()), 0);

    if (sent < 0) {
        if (wouldBlock()) {
            conn.sendBuffer.insert(conn.sendBuffer.end(), frame.begin(), frame.end());
            return true;
        }
        conn.connected = false;
        m_pendingRemove.push_back(peerID);
        return false;
    }

    if (static_cast<size_t>(sent) < frame.size()) {
        conn.sendBuffer.insert(conn.sendBuffer.end(), frame.begin() + sent, frame.end());
    }

    return true;
}

bool ConnectionManager::isPacketAvailable(P2PPeerID peerID, uint32_t* outSize, P2PPeerID* outPeerID) {
    if (peerID != P2P_INVALID_PEER_ID) {
        auto it = m_connections.find(peerID);
        if (it == m_connections.end()) {
            return false;
        }

        uint32_t size = 0;
        if (it->second.receiveQueue.peek(size)) {
            if (outSize) {
                *outSize = size;
            }
            if (outPeerID) {
                *outPeerID = peerID;
            }
            return true;
        }
        return false;
    }

    for (auto& [pid, conn] : m_connections) {
        uint32_t size = 0;
        if (conn.receiveQueue.peek(size)) {
            if (outSize) {
                *outSize = size;
            }
            if (outPeerID) {
                *outPeerID = pid;
            }
            return true;
        }
    }

    return false;
}

bool ConnectionManager::readPacket(P2PPeerID peerID, void* buffer, uint32_t bufferSize,
                                  uint32_t* outReadSize, P2PPeerID* outPeerID) {
    if (!buffer || bufferSize == 0) {
        return false;
    }

    Connection* conn = nullptr;
    P2PPeerID sourcePeerID = P2P_INVALID_PEER_ID;

    if (peerID != P2P_INVALID_PEER_ID) {
        auto it = m_connections.find(peerID);
        if (it == m_connections.end()) {
            return false;
        }
        conn = &it->second;
        sourcePeerID = peerID;
    } else {
        for (auto& [pid, c] : m_connections) {
            uint32_t size = 0;
            if (c.receiveQueue.peek(size)) {
                conn = &c;
                sourcePeerID = pid;
                break;
            }
        }
    }

    if (!conn) {
        return false;
    }

    Packet packet;
    if (!conn->receiveQueue.pop(packet)) {
        return false;
    }

    if (packet.size() > bufferSize) {
        return false;
    }

    std::memcpy(buffer, packet.data.data(), packet.size());

    if (outReadSize) {
        *outReadSize = packet.size();
    }
    if (outPeerID) {
        *outPeerID = sourcePeerID;
    }

    return true;
}

void ConnectionManager::processEvents() {
    if (!m_initialized) {
        return;
    }

    attemptReconnects();
     
    // 接受新连接
    acceptNewConnections();
    
    // 处理现有连接
    fd_set readSet, writeSet, exceptSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&exceptSet);
    
    SocketHandle maxFd = 0;
    
    for (auto& [peerID, conn] : m_connections) {
        if (conn.socket != INVALID_SOCKET_HANDLE && conn.connected) {
            FD_SET(conn.socket, &readSet);
            FD_SET(conn.socket, &exceptSet);
            
            // 如果有待发送数据，监听写事件
            if (!conn.sendBuffer.empty()) {
                FD_SET(conn.socket, &writeSet);
            }
            
            if (conn.socket > maxFd) {
                maxFd = conn.socket;
            }
        }
    }
     
    if (maxFd == 0) {
        removeDisconnected();
        attemptReconnects();
        return;
    }
    
    // 非阻塞 select
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    
    int result = select(static_cast<int>(maxFd + 1), &readSet, &writeSet, &exceptSet, &timeout);
    
    if (result > 0) {
        for (auto& [peerID, conn] : m_connections) {
            if (conn.socket == INVALID_SOCKET_HANDLE || !conn.connected) {
                continue;
            }
            
            // 检查异常
            if (FD_ISSET(conn.socket, &exceptSet)) {
                conn.connected = false;
                m_pendingRemove.push_back(peerID);
                continue;
            }
            
            // 处理读取
            if (FD_ISSET(conn.socket, &readSet)) {
                processRead(conn);
            }
            
            // 处理写入
            if (FD_ISSET(conn.socket, &writeSet)) {
                processWrite(conn);
            }
        }
    }
    
    // 移除断开的连接
    removeDisconnected();

    attemptReconnects();
}

void ConnectionManager::setConnectionCallback(ConnectionCallback callback) {
    m_connectionCallback = std::move(callback);
}

uint32_t ConnectionManager::getPeerCount() const {
    return static_cast<uint32_t>(m_connections.size());
}

uint32_t ConnectionManager::getConnectedPeers(P2PPeerID* outPeerIDs, uint32_t maxCount) const {
    if (!outPeerIDs || maxCount == 0) {
        return 0;
    }
    
    uint32_t count = 0;
    for (const auto& [peerID, conn] : m_connections) {
        if (conn.connected && count < maxCount) {
            outPeerIDs[count++] = peerID;
        }
    }
    
    return count;
}

P2PPeerID ConnectionManager::allocatePeerID() {
    return m_nextPeerID++;
}

void ConnectionManager::acceptNewConnections() {
    if (m_listenSocket == INVALID_SOCKET_HANDLE) {
        return;
    }
    
    while (true) {
        sockaddr_in clientAddr{};
        int addrLen = sizeof(clientAddr);
        
        SocketHandle clientSocket = accept(m_listenSocket, 
                                           reinterpret_cast<sockaddr*>(&clientAddr),
                                           &addrLen);
        
        if (clientSocket == INVALID_SOCKET_HANDLE) {
            break;  // 没有更多连接
        }
        
        // 设置 socket 选项
        setNonBlocking(clientSocket);
        setNoDelay(clientSocket);
        
        // 获取客户端 IP
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        
        // 分配 PeerID
        P2PPeerID peerID = allocatePeerID();
        
        Connection conn;
        conn.socket = clientSocket;
        conn.peerID = peerID;
        conn.remoteIP = ipStr;
        conn.remotePort = ntohs(clientAddr.sin_port);
        conn.connected = true;
        
        m_connections.emplace(peerID, std::move(conn));
        
        // 通知新连接
        notifyConnectionEvent(peerID, ConnectionEvent::Connected);
    }
}

void ConnectionManager::processRead(Connection& conn) {
    char buffer[RECV_BUFFER_SIZE];
    
    while (true) {
        int received = recv(conn.socket, buffer, sizeof(buffer), 0);
        
        if (received > 0) {
            // 追加到接收缓冲区
            conn.recvBuffer.append(buffer, static_cast<uint32_t>(received));
            
            // 尝试解析完整数据包
            Packet packet;
            while (conn.recvBuffer.tryParsePacket(packet)) {
                conn.receiveQueue.push(std::move(packet));
            }
        } else if (received == 0) {
            // 连接正常关闭
            conn.connected = false;
            m_pendingRemove.push_back(conn.peerID);
            break;
        } else {
            if (wouldBlock()) {
                // 没有更多数据
                break;
            }
            // 读取错误
            conn.connected = false;
            m_pendingRemove.push_back(conn.peerID);
            break;
        }
    }
}

void ConnectionManager::processWrite(Connection& conn) {
    if (conn.sendBuffer.empty()) {
        return;
    }
    
    int sent = send(conn.socket, 
                    reinterpret_cast<const char*>(conn.sendBuffer.data()),
                    static_cast<int>(conn.sendBuffer.size()), 0);
    
    if (sent > 0) {
        conn.sendBuffer.erase(conn.sendBuffer.begin(), 
                              conn.sendBuffer.begin() + sent);
    } else if (sent < 0 && !wouldBlock()) {
        // 发送错误
        conn.connected = false;
        m_pendingRemove.push_back(conn.peerID);
    }
}

void ConnectionManager::removeDisconnected() {
    // 去重
    std::sort(m_pendingRemove.begin(), m_pendingRemove.end());
    m_pendingRemove.erase(std::unique(m_pendingRemove.begin(), m_pendingRemove.end()),
                          m_pendingRemove.end());
    
    for (P2PPeerID peerID : m_pendingRemove) {
        auto it = m_connections.find(peerID);
        if (it != m_connections.end()) {
            Connection& conn = it->second;
            closeSocket(conn.socket);
            conn.socket = INVALID_SOCKET_HANDLE;
            conn.connected = false;
            conn.recvBuffer.clear();
            conn.receiveQueue.clear();
            conn.sendBuffer.clear();
            notifyConnectionEvent(peerID, ConnectionEvent::Disconnected);

            if (conn.autoReconnect) {
                conn.reconnectAttempt = 0;
                conn.nextReconnectAtMs = nowMs() + conn.reconnectBaseDelayMs;
                continue;
            }

            m_connections.erase(it);
        }
    }
    
    m_pendingRemove.clear();
}

void ConnectionManager::attemptReconnects() {
    const uint64_t now = nowMs();

    for (auto& [peerID, conn] : m_connections) {
        if (!conn.autoReconnect) {
            continue;
        }
        if (conn.connected) {
            continue;
        }
        if (conn.remoteIP.empty() || conn.remotePort == 0) {
            continue;
        }
        if (conn.nextReconnectAtMs == 0 || now < conn.nextReconnectAtMs) {
            continue;
        }

        SocketHandle sock = connectWithTimeout(conn.remoteIP.c_str(), conn.remotePort, 2000);
        if (sock != INVALID_SOCKET_HANDLE) {
            conn.socket = sock;
            conn.connected = true;
            conn.reconnectAttempt = 0;
            conn.nextReconnectAtMs = 0;
            notifyConnectionEvent(peerID, ConnectionEvent::Connected);
            continue;
        }

        const uint32_t attempt = conn.reconnectAttempt;
        conn.reconnectAttempt = (attempt == (std::numeric_limits<uint32_t>::max)()) ? attempt : (attempt + 1);

        uint64_t delay = static_cast<uint64_t>(conn.reconnectBaseDelayMs);
        for (uint32_t i = 0; i < attempt; i++) {
            delay *= 2;
            if (delay >= conn.reconnectMaxDelayMs) {
                delay = conn.reconnectMaxDelayMs;
                break;
            }
        }

        conn.nextReconnectAtMs = now + delay;
    }
}

void ConnectionManager::notifyConnectionEvent(P2PPeerID peerID, ConnectionEvent event) {
    if (m_connectionCallback) {
        m_connectionCallback(peerID, event);
    }
}

std::vector<uint8_t> ConnectionManager::createSendFrame(const void* data, uint32_t size) {
    std::vector<uint8_t> frame;
    frame.reserve(4 + size);
    
    // 写入长度头 (小端序)
    frame.push_back(static_cast<uint8_t>(size & 0xFF));
    frame.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>((size >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((size >> 24) & 0xFF));
    
    // 写入数据
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    frame.insert(frame.end(), bytes, bytes + size);
    
    return frame;
}

} // namespace p2p
