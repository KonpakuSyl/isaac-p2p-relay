#include <p2p_network.h>
#include "connection_manager.h"

#include <memory>

// 全局连接管理器实例
static std::unique_ptr<p2p::ConnectionManager> g_manager;

// 用户连接回调
static P2PConnectionCallback g_userCallback = nullptr;
static void* g_userCallbackData = nullptr;

// 内部连接回调，转发给用户回调
static void internalConnectionCallback(P2PPeerID peerID, p2p::ConnectionEvent event) {
    if (g_userCallback) {
        bool connected = (event == p2p::ConnectionEvent::Connected);
        g_userCallback(peerID, connected, g_userCallbackData);
    }
}

P2PResult P2P_Init(void) {
    if (g_manager) {
        return P2P_ERROR_ALREADY_INITIALIZED;
    }
    
    g_manager = std::make_unique<p2p::ConnectionManager>();
    
    if (!g_manager->initialize()) {
        g_manager.reset();
        return P2P_ERROR_SOCKET_ERROR;
    }
    
    g_manager->setConnectionCallback(internalConnectionCallback);
    
    return P2P_OK;
}

void P2P_Shutdown(void) {
    if (g_manager) {
        g_manager->shutdown();
        g_manager.reset();
    }
    
    g_userCallback = nullptr;
    g_userCallbackData = nullptr;
}

P2PResult P2P_Listen(uint16_t port) {
    if (!g_manager || !g_manager->isInitialized()) {
        return P2P_ERROR_NOT_INITIALIZED;
    }
    
    if (!g_manager->listen(port)) {
        return P2P_ERROR_LISTEN_FAILED;
    }
    
    return P2P_OK;
}

void P2P_StopListen(void) {
    if (g_manager) {
        g_manager->stopListen();
    }
}

P2PResult P2P_Connect(const char* ip, uint16_t port, P2PPeerID* outPeerID) {
    if (!g_manager || !g_manager->isInitialized()) {
        return P2P_ERROR_NOT_INITIALIZED;
    }
    
    if (!ip || !outPeerID) {
        return P2P_ERROR_INVALID_PARAM;
    }
    
    P2PPeerID peerID = P2P_INVALID_PEER_ID;
    if (!g_manager->connect(ip, port, peerID)) {
        return P2P_ERROR_CONNECTION_FAILED;
    }
    
    *outPeerID = peerID;
    return P2P_OK;
}

void P2P_SetAutoReconnect(P2PPeerID peerID, bool enable, uint32_t baseDelayMs, uint32_t maxDelayMs) {
    if (!g_manager || !g_manager->isInitialized() || peerID == P2P_INVALID_PEER_ID) {
        return;
    }

    g_manager->setAutoReconnect(peerID, enable, baseDelayMs, maxDelayMs);
}

void P2P_Disconnect(P2PPeerID peerID) {
    if (g_manager && peerID != P2P_INVALID_PEER_ID) {
        g_manager->disconnect(peerID);
    }
}

P2PResult P2P_SendPacket(P2PPeerID peerID, const void* data, uint32_t size) {
    if (!g_manager || !g_manager->isInitialized()) {
        return P2P_ERROR_NOT_INITIALIZED;
    }
    
    if (peerID == P2P_INVALID_PEER_ID) {
        return P2P_ERROR_INVALID_PEER;
    }
    
    if (!data || size == 0) {
        return P2P_ERROR_INVALID_PARAM;
    }
    
    if (!g_manager->sendPacket(peerID, data, size)) {
        return P2P_ERROR_SEND_FAILED;
    }
    
    return P2P_OK;
}

bool P2P_IsPacketAvailable(P2PPeerID peerID, uint32_t* outSize, P2PPeerID* outPeerID) {
    if (!g_manager || !g_manager->isInitialized()) {
        return false;
    }
    
    return g_manager->isPacketAvailable(peerID, outSize, outPeerID);
}

P2PResult P2P_ReadPacket(P2PPeerID peerID, void* buffer, uint32_t bufferSize, 
                          uint32_t* outReadSize, P2PPeerID* outPeerID) {
    if (!g_manager || !g_manager->isInitialized()) {
        return P2P_ERROR_NOT_INITIALIZED;
    }
    
    if (!buffer || bufferSize == 0) {
        return P2P_ERROR_INVALID_PARAM;
    }
    
    // 先检查是否有数据包
    uint32_t packetSize = 0;
    if (!g_manager->isPacketAvailable(peerID, &packetSize, nullptr)) {
        return P2P_ERROR_NO_PACKET;
    }
    
    // 检查缓冲区大小
    if (packetSize > bufferSize) {
        return P2P_ERROR_BUFFER_TOO_SMALL;
    }
    
    // 读取数据包
    if (!g_manager->readPacket(peerID, buffer, bufferSize, outReadSize, outPeerID)) {
        return P2P_ERROR_NO_PACKET;
    }
    
    return P2P_OK;
}

void P2P_RunCallbacks(void) {
    if (g_manager && g_manager->isInitialized()) {
        g_manager->processEvents();
    }
}

void P2P_SetConnectionCallback(P2PConnectionCallback callback, void* userData) {
    g_userCallback = callback;
    g_userCallbackData = userData;
}

uint32_t P2P_GetPeerCount(void) {
    if (!g_manager || !g_manager->isInitialized()) {
        return 0;
    }
    
    return g_manager->getPeerCount();
}

uint32_t P2P_GetConnectedPeers(P2PPeerID* outPeerIDs, uint32_t maxCount) {
    if (!g_manager || !g_manager->isInitialized()) {
        return 0;
    }
    
    return g_manager->getConnectedPeers(outPeerIDs, maxCount);
}
