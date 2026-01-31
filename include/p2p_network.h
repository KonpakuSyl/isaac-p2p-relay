#ifndef P2P_NETWORK_H
#define P2P_NETWORK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
    #ifdef P2P_NETWORK_EXPORTS
        #define P2P_API __declspec(dllexport)
    #else
        #define P2P_API __declspec(dllimport)
    #endif
#else
    #define P2P_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 类型定义
typedef uint32_t P2PPeerID;

// 无效的 PeerID
#define P2P_INVALID_PEER_ID ((P2PPeerID)0)

// 错误码
typedef enum P2PResult {
    P2P_OK = 0,
    P2P_ERROR_NOT_INITIALIZED = -1,
    P2P_ERROR_ALREADY_INITIALIZED = -2,
    P2P_ERROR_INVALID_PEER = -3,
    P2P_ERROR_CONNECTION_FAILED = -4,
    P2P_ERROR_SEND_FAILED = -5,
    P2P_ERROR_BUFFER_TOO_SMALL = -6,
    P2P_ERROR_NO_PACKET = -7,
    P2P_ERROR_SOCKET_ERROR = -8,
    P2P_ERROR_INVALID_PARAM = -9,
    P2P_ERROR_LISTEN_FAILED = -10
} P2PResult;

// 连接事件回调类型
typedef void (*P2PConnectionCallback)(P2PPeerID peerID, bool connected, void* userData);

/**
 * 初始化 P2P 网络库
 * @return P2P_OK 成功, 其他值表示错误
 */
P2P_API P2PResult P2P_Init(void);

/**
 * 关闭 P2P 网络库，断开所有连接
 */
P2P_API void P2P_Shutdown(void);

/**
 * 开始监听指定端口，接受传入连接
 * @param port 监听端口
 * @return P2P_OK 成功
 */
P2P_API P2PResult P2P_Listen(uint16_t port);

/**
 * 停止监听
 */
P2P_API void P2P_StopListen(void);

/**
 * 连接到指定对端
 * @param ip 对端 IP 地址 (如 "192.168.1.100")
 * @param port 对端端口
 * @param outPeerID 输出连接成功后的 PeerID
 * @return P2P_OK 成功
 */
P2P_API P2PResult P2P_Connect(const char* ip, uint16_t port, P2PPeerID* outPeerID);

/**
 * 启用/禁用指定连接的自动重连
 * @param peerID 对端 ID
 * @param enable 是否启用
 * @param baseDelayMs 初始重连延迟
 * @param maxDelayMs 最大重连延迟
 */
P2P_API void P2P_SetAutoReconnect(P2PPeerID peerID, bool enable, uint32_t baseDelayMs, uint32_t maxDelayMs);

/**
 * 断开与指定对端的连接
 * @param peerID 对端 ID
 */
P2P_API void P2P_Disconnect(P2PPeerID peerID);

/**
 * 发送数据包到指定对端
 * @param peerID 对端 ID
 * @param data 数据指针
 * @param size 数据大小 (字节)
 * @return P2P_OK 成功
 */
P2P_API P2PResult P2P_SendPacket(P2PPeerID peerID, const void* data, uint32_t size);

/**
 * 检查是否有来自指定对端的数据包可读
 * @param peerID 对端 ID, 如果为 P2P_INVALID_PEER_ID 则检查所有连接
 * @param outSize 输出可用数据包大小 (可以为 NULL)
 * @param outPeerID 输出发送方 PeerID (可以为 NULL, 仅当 peerID 为 P2P_INVALID_PEER_ID 时有效)
 * @return true 有数据包可读, false 无数据包
 */
P2P_API bool P2P_IsPacketAvailable(P2PPeerID peerID, uint32_t* outSize, P2PPeerID* outPeerID);

/**
 * 读取来自指定对端的数据包
 * @param peerID 对端 ID, 如果为 P2P_INVALID_PEER_ID 则从任意连接读取
 * @param buffer 接收缓冲区
 * @param bufferSize 缓冲区大小
 * @param outReadSize 输出实际读取的字节数
 * @param outPeerID 输出发送方 PeerID (可以为 NULL)
 * @return P2P_OK 成功
 */
P2P_API P2PResult P2P_ReadPacket(P2PPeerID peerID, void* buffer, uint32_t bufferSize, 
                                  uint32_t* outReadSize, P2PPeerID* outPeerID);

/**
 * 处理网络事件，需要定期调用 (如每帧)
 * 此函数会接收新连接、读取数据、检测断开等
 */
P2P_API void P2P_RunCallbacks(void);

/**
 * 设置连接状态回调
 * @param callback 回调函数, 当有新连接或断开时调用
 * @param userData 用户数据，会传递给回调函数
 */
P2P_API void P2P_SetConnectionCallback(P2PConnectionCallback callback, void* userData);

/**
 * 获取已连接的对端数量
 * @return 连接数
 */
P2P_API uint32_t P2P_GetPeerCount(void);

/**
 * 获取所有已连接的对端 ID
 * @param outPeerIDs 输出数组
 * @param maxCount 数组最大容量
 * @return 实际填充的数量
 */
P2P_API uint32_t P2P_GetConnectedPeers(P2PPeerID* outPeerIDs, uint32_t maxCount);

#ifdef __cplusplus
}
#endif

#endif // P2P_NETWORK_H
