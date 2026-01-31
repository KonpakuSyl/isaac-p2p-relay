#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <atomic>

#include "p2p_network.h"

// ============================================================================
// 日志系统 - 使用条件编译控制
// 定义 DEBUG 启用 ETW TraceLogging
// ============================================================================

#include <TraceLoggingProvider.h>
#include <evntrace.h>
    
// 定义 TraceLogging Provider
// GUID: {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
TRACELOGGING_DEFINE_PROVIDER(
    g_hTraceProvider,
    "P2PHookProvider",
    (0xa1b2c3d4, 0xe5f6, 0x7890, 0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90)
);

// ============================================================================
// 调试版本: 原样转发数据，对比原函数和TCP数据
// ============================================================================

// Steam类型定义
using uint32 = unsigned int;
using CSteamID = uint64_t;  // CSteamID 是 64 位值 (8字节)
using EP2PSend = int;

// 虚表偏移和大小
constexpr uintptr_t VTABLE_OFFSET = 0x7F7778;          // ISteamNetworking
constexpr uintptr_t ISTEAMUSER_OFFSET = 0x7F7760;      // ISteamUser
constexpr size_t VTABLE_SIZE = 48;

// 虚表函数索引 - ISteamNetworking
constexpr size_t VTABLE_INDEX_SendP2PPacket = 0;          // bool SendP2PPacket(CSteamID, const void*, uint32, EP2PSend, int)
constexpr size_t VTABLE_INDEX_IsP2PPacketAvailable = 1;   // bool IsP2PPacketAvailable(uint32*, int)
constexpr size_t VTABLE_INDEX_ReadP2PPacket = 2;          // bool ReadP2PPacket(void*, uint32, uint32*, CSteamID*, int)

// 虚表函数索引 - ISteamUser
constexpr size_t VTABLE_INDEX_GetSteamID = 2;             // CSteamID GetSteamID()

// 全局变量
static void** g_pOriginalVTable = nullptr;
static void** g_pNewVTable = nullptr;

// 配置

static std::string g_strRemoteIP = "127.0.0.1";
static uint16_t g_nPort = 27015;

// 当前连接的PeerID
static P2PPeerID g_connectedPeer = P2P_INVALID_PEER_ID;

static std::atomic<bool> g_stopPumpThread{false};
static HANDLE g_hPumpThread = NULL;

// 调试: 记录原函数读取的数据
static std::vector<uint8_t> g_originalReadData;
static uint32 g_originalReadSize = 0;
static CSteamID g_originalReadSteamID = 0;

// ============================================================================
// 日志函数
// ============================================================================

// 日志函数生成宏
#define DEFINE_TRACE_FUNC(funcName, eventName, level)           \
void funcName(const char* format, ...) {                        \
    char buffer[1024];                                          \
    va_list args;                                               \
    va_start(args, format);                                     \
    vsnprintf(buffer, sizeof(buffer), format, args);            \
    va_end(args);                                               \
    TraceLoggingWrite(g_hTraceProvider, eventName,              \
        TraceLoggingLevel(level),                               \
        TraceLoggingString(buffer, "Message"));                 \
}

DEFINE_TRACE_FUNC(TraceErrorImpl, "Error", TRACE_LEVEL_ERROR)
DEFINE_TRACE_FUNC(TraceInfoImpl, "Info", TRACE_LEVEL_INFORMATION)
DEFINE_TRACE_FUNC(TraceDebugImpl, "Debug", TRACE_LEVEL_VERBOSE)

#undef DEFINE_TRACE_FUNC

#define TraceError(fmt, ...) TraceErrorImpl(fmt, ##__VA_ARGS__)
#define TraceInfo(fmt, ...) TraceInfoImpl(fmt, ##__VA_ARGS__)

#ifdef DEBUG

#define TraceDebug(fmt, ...) TraceDebugImpl(fmt, ##__VA_ARGS__)

#else
// 禁用日志时，宏展开为空操作
#define TraceDebug(fmt, ...) ((void)0)

#endif

// 读取配置文件
bool LoadConfig() {
    // 获取DLL所在目录
    char dllPath[MAX_PATH];
    
    HMODULE hModule = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | 
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(LoadConfig), &hModule);
    GetModuleFileNameA(hModule, dllPath, MAX_PATH);
    
    // 获取目录部分
    std::string configPath = dllPath;
    auto lastSlash = configPath.rfind('\\');
    if (lastSlash != std::string::npos) {
        configPath = configPath.substr(0, lastSlash + 1) + "p2p_config.txt";
    } else {
        configPath = "p2p_config.txt";
    }
    
    std::ifstream file(configPath);
    if (!file.is_open()) {
        // 尝试当前目录
        file.open("p2p_config.txt");
        if (!file.is_open()) {
            return false;
        }
    }
    
    std::string line;
    while (std::getline(file, line)) {
        // 移除回车符
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        // 跳过空行和注释
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        
        // 解析 ip=xxx.xxx.xxx.xxx
        else if (line.compare(0, 3, "ip=") == 0) {
            g_strRemoteIP = line.substr(3);
        }
        // 解析 port=xxxxx
        else if (line.compare(0, 5, "port=") == 0) {
            g_nPort = static_cast<uint16_t>(std::stoi(line.substr(5)));
        }
    }
    
    return true;
}

// ============================================================================
// Hook的Steam函数实现
// 原函数: thiscall (this 通过 ECX 传递)
// Hook函数: fastcall 模拟 (ECX=this, EDX=unused)
// ============================================================================

#ifdef _WIN64
    #define HOOK_CALL
#else
    #define HOOK_CALL __fastcall
#endif

// 原函数类型定义 - ISteamNetworking
#ifdef _WIN64
using FnIsP2PPacketAvailable = bool(*)(void* thisptr, uint32* pcubMsgSize, int nChannel);
using FnReadP2PPacket = bool(*)(void* thisptr, void* pubDest, uint32 cubDest, uint32* pcubMsgSize, CSteamID* psteamIDRemote, int nChannel);
using FnSendP2PPacket = bool(*)(void* thisptr, CSteamID steamIDRemote, const void* pubData, uint32 cubData, int eP2PSendType, int nChannel);
#else
using FnIsP2PPacketAvailable = bool(__thiscall*)(void* thisptr, uint32* pcubMsgSize, int nChannel);
using FnReadP2PPacket = bool(__thiscall*)(void* thisptr, void* pubDest, uint32 cubDest, uint32* pcubMsgSize, CSteamID* psteamIDRemote, int nChannel);
using FnSendP2PPacket = bool(__thiscall*)(void* thisptr, CSteamID steamIDRemote, const void* pubData, uint32 cubData, int eP2PSendType, int nChannel);
#endif

// 原函数类型定义 - ISteamUser
#ifdef _WIN64
using FnGetSteamID = CSteamID*(*)(void* thisptr, CSteamID* psteamID);
#else
using FnGetSteamID = CSteamID*(__thiscall*)(void* thisptr, CSteamID* psteamID);
#endif

// TCP 接收缓冲区
static std::vector<uint8_t> g_tcpRecvBuffer;
static bool g_hasTcpData = false;
static CSteamID g_tcpRecvSteamID = 0;  // 从 TCP 数据中解析出的 CSteamID

// bool ISteamNetworking::IsP2PPacketAvailable(uint32 *pcubMsgSize, int nChannel = 0)
bool HOOK_CALL Hook_IsP2PPacketAvailable(void* thisptr, 
#ifndef _WIN64
    void* edx,
#endif
    uint32* pcubMsgSize, int nChannel) 
{
    // 调用原函数
    auto pfnOriginal = reinterpret_cast<FnIsP2PPacketAvailable>(g_pOriginalVTable[VTABLE_INDEX_IsP2PPacketAvailable]);
    bool originalResult = pfnOriginal(thisptr, pcubMsgSize, nChannel);
    
    if (originalResult && pcubMsgSize) {
        uint32 cubDest = *pcubMsgSize;
        g_originalReadData.resize(cubDest);
        auto xfnOriginal = reinterpret_cast<FnReadP2PPacket>(g_pOriginalVTable[VTABLE_INDEX_ReadP2PPacket]);
        xfnOriginal(thisptr, g_originalReadData.data(), cubDest, &g_originalReadSize, &g_originalReadSteamID, nChannel);
        g_originalReadData.clear();
        TraceDebug("IsP2PPacketAvailable Original Success Size: %u bytes %u", g_originalReadSize, g_originalReadSteamID);
    }
    
    P2P_RunCallbacks();
    
    // 检查 TCP 是否有数据
    if (!g_hasTcpData) {
        uint32_t size = 0;
        P2PPeerID peerID = P2P_INVALID_PEER_ID;
        
        if (P2P_IsPacketAvailable(P2P_INVALID_PEER_ID, &size, &peerID)) {
            std::vector<uint8_t> rawBuffer(size);
            uint32_t readSize = 0;
            
            if (P2P_ReadPacket(peerID, rawBuffer.data(), size, &readSize, &peerID) == P2P_OK) {
                // 数据格式: [CSteamID (8 bytes)] [原始数据]
                if (readSize >= sizeof(CSteamID)) {
                    // 读取 CSteamID
                    std::memcpy(&g_tcpRecvSteamID, rawBuffer.data(), sizeof(CSteamID));
                    
                    // 提取原始数据（去掉 CSteamID 头部）
                    uint32_t dataSize = readSize - sizeof(CSteamID);
                    g_tcpRecvBuffer.resize(dataSize);
                    if (dataSize > 0) {
                        std::memcpy(g_tcpRecvBuffer.data(), rawBuffer.data() + sizeof(CSteamID), dataSize);
                    }
                    g_hasTcpData = true;
                    
                    TraceDebug("IsP2PPacketAvailable TCP: CSteamID=%llu, DataSize=%u, PeerID=%u", 
                        g_tcpRecvSteamID, dataSize, peerID);
                } else {
                    TraceError("IsP2PPacketAvailable TCP: Invalid packet size %u (< %zu)", 
                        readSize, sizeof(CSteamID));
                }
            }
        }
    }
    
    // 如果有 TCP 数据，返回 TCP 数据的 size
    if (g_hasTcpData) {
        if (pcubMsgSize) {
            *pcubMsgSize = static_cast<uint32>(g_tcpRecvBuffer.size());
        }
        return true;
    }
    
    return false;
}

// bool ISteamNetworking::ReadP2PPacket(void *pubDest, uint32 cubDest, uint32 *pcubMsgSize, CSteamID *psteamIDRemote, int nChannel = 0)
bool HOOK_CALL Hook_ReadP2PPacket(void* thisptr,
#ifndef _WIN64
    void* edx,
#endif
    void* pubDest, uint32 cubDest, uint32* pcubMsgSize, CSteamID* psteamIDRemote, int nChannel)
{
    // 返回 TCP 数据
    if (g_hasTcpData) {
        uint32 copySize = static_cast<uint32>(g_tcpRecvBuffer.size());

        TraceDebug("ReadP2PPacket TCP Success Size: %u bytes, CSteamID: %llu %llu", copySize, g_tcpRecvSteamID, g_originalReadSteamID);
        
        if (pubDest && copySize > 0) {
            std::memcpy(pubDest, g_tcpRecvBuffer.data(), copySize);
        }
        
        if (pcubMsgSize) {
            *pcubMsgSize = copySize;
        }
        
        if (psteamIDRemote) { 
            *psteamIDRemote = g_tcpRecvSteamID;
        }
        
        g_hasTcpData = false;
        g_tcpRecvBuffer.clear();
        g_tcpRecvSteamID = 0;
        
        return true;
    }
    
    return false;
}

// 本地 CSteamID (从原函数调用中记录)
static CSteamID g_localSteamID = 0;

// bool ISteamNetworking::SendP2PPacket(CSteamID steamIDRemote, const void *pubData, uint32 cubData, EP2PSend eP2PSendType, int nChannel = 0)
bool HOOK_CALL Hook_SendP2PPacket(void* thisptr,
#ifndef _WIN64
    void* edx,
#endif
    CSteamID steamIDRemote, const void* pubData, uint32 cubData, int eP2PSendType, int nChannel)
{
    // 先调用原函数
    auto pfnOriginal = reinterpret_cast<FnSendP2PPacket>(g_pOriginalVTable[VTABLE_INDEX_SendP2PPacket]);
    bool originalResult = pfnOriginal(thisptr, steamIDRemote, pubData, 8, eP2PSendType, nChannel); // 只发送8个字节保持心跳
    
    P2PPeerID targetPeer = g_connectedPeer;
    
    // 通过 TCP 发送，数据格式: [目标SteamID (8 bytes)] [本地SteamID (8 bytes)] [原始数据]
    if (targetPeer != P2P_INVALID_PEER_ID) {
        uint32 totalSize = sizeof(CSteamID) + sizeof(CSteamID) + cubData;
        std::vector<uint8_t> sendBuffer(totalSize);
        
        // 写入目标 SteamID (offset 0)
        std::memcpy(sendBuffer.data(), &steamIDRemote, sizeof(CSteamID));
        // 写入本地 SteamID（发送方）(offset 8)
        std::memcpy(sendBuffer.data() + sizeof(CSteamID), &g_localSteamID, sizeof(CSteamID));
        // 写入原始数据 (offset 16)
        if (cubData > 0 && pubData) {
            std::memcpy(sendBuffer.data() + sizeof(CSteamID) + sizeof(CSteamID), pubData, cubData);
        }
        
        P2PResult tcpResult = P2P_SendPacket(targetPeer, sendBuffer.data(), totalSize);
        
        if (tcpResult != P2P_OK) {
            TraceError("SendP2PPacket TCP Error: send failed, Size=%u, Error=%d",
                totalSize, tcpResult);
        }
        TraceDebug("SendP2PPacket TCP Success: CSteamID=%llu, DataSize=%u, TotalSize=%u, PeerID=%u", 
            steamIDRemote, cubData, totalSize, targetPeer);
            
    } else {
        TraceError("SendP2PPacket No Connection: PeerID=%u, CSteamID=%llu, Size=%u",
            targetPeer, steamIDRemote, cubData);
    }
    
    return originalResult;
}

// ============================================================================
// 连接回调
// ============================================================================

void ConnectionCallback(P2PPeerID peerID, bool connected, void* userData) {
    if (connected) {
        g_connectedPeer = peerID;  // 保存连接的 PeerID
        TraceInfo("P2P Connection Connected PeerID: %u Global PeerID: %u", peerID, g_connectedPeer);
    } else {
        TraceInfo("P2P Connection Disconnected PeerID: %u", peerID);
        if (g_connectedPeer == peerID) {
            g_connectedPeer = P2P_INVALID_PEER_ID;
        }
    }
}

// ============================================================================
// 获取本地 SteamID
// ============================================================================

CSteamID GetLocalSteamID() {
    HMODULE hBase = GetModuleHandle(nullptr);
    if (!hBase) return 0;
    
    // 获取 ISteamUser 对象指针
    auto ppSteamUser = reinterpret_cast<void***>(
        reinterpret_cast<uintptr_t>(hBase) + ISTEAMUSER_OFFSET);
    
    void** pSteamUser = *ppSteamUser;
    if (!pSteamUser) return 0;
    
    // 获取虚表
    void** vtable = reinterpret_cast<void**>(pSteamUser[0]);
    if (!vtable) return 0;
    
    // 调用 GetSteamID()
    auto pfnGetSteamID = reinterpret_cast<FnGetSteamID>(vtable[VTABLE_INDEX_GetSteamID]);
    CSteamID steamID = 0;
    pfnGetSteamID(pSteamUser, &steamID);
    return steamID;
}

// ============================================================================
// 虚表Hook
// ============================================================================

// 保存对象指针，用于恢复
static void** g_pObject = nullptr;

bool HookVTable() {
    HMODULE hBase = GetModuleHandle(nullptr);
    if (!hBase) return false;
    
    // hBase + VTABLE_OFFSET 存储的是对象指针
    auto ppObject = reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(hBase) + VTABLE_OFFSET);
    g_pObject = *ppObject;
    if (!g_pObject) return false;
    
    // 对象的第一个成员是虚表指针
    g_pOriginalVTable = reinterpret_cast<void**>(g_pObject[0]);
    if (!g_pOriginalVTable) return false;
    
    constexpr size_t vtableBytes = VTABLE_SIZE * sizeof(void*);
    
    // 创建新虚表并复制原虚表内容
    g_pNewVTable = new void*[VTABLE_SIZE];
    std::memcpy(g_pNewVTable, g_pOriginalVTable, vtableBytes);
    
    // 替换虚表函数
    g_pNewVTable[VTABLE_INDEX_SendP2PPacket] = reinterpret_cast<void*>(Hook_SendP2PPacket);
    g_pNewVTable[VTABLE_INDEX_IsP2PPacketAvailable] = reinterpret_cast<void*>(Hook_IsP2PPacketAvailable);
    g_pNewVTable[VTABLE_INDEX_ReadP2PPacket] = reinterpret_cast<void*>(Hook_ReadP2PPacket);
    
    // 替换对象的虚表指针
    g_pObject[0] = g_pNewVTable;
    
    return true;
}

void UnhookVTable() {
    if (!g_pOriginalVTable || !g_pNewVTable || !g_pObject) return;
    
    // 恢复对象的虚表指针
    g_pObject[0] = g_pOriginalVTable;
    
    delete[] g_pNewVTable;
    g_pNewVTable = nullptr;
    g_pObject = nullptr;
}

// 注入线程函数
DWORD WINAPI InjectionThread(LPVOID lpParam) {
    HMODULE hBase = GetModuleHandle(nullptr);
    if (!hBase) return 0;
    auto ppObject = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hBase) + VTABLE_OFFSET);
    auto intPtr = reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(hBase) + VTABLE_OFFSET - 4);
    while (1)
    {
        if (*intPtr > 0 && *ppObject != nullptr) {
            HookVTable();
            break;
        }
        Sleep(1000);
    }
    TraceInfo("HookVTable Success");
    
    // 获取本地 SteamID
    g_localSteamID = GetLocalSteamID();
    TraceInfo("Local SteamID: %llu", g_localSteamID);

    const size_t dataSize = sizeof(CSteamID);
    uint8_t data[dataSize] = {0};
    std::memcpy(data, &g_localSteamID, sizeof(CSteamID));
    P2P_SendPacket(g_connectedPeer, data, dataSize);
    
    return 0;
}

static DWORD WINAPI PumpThread(LPVOID) {
    while (!g_stopPumpThread.load()) {
        P2P_RunCallbacks();
        Sleep(10);
    }
    return 0;
}

// ============================================================================
// DLL入口点
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    static HANDLE hInjectionThread = NULL;
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        
        // 注册 TraceLogging Provider
        TraceLoggingRegister(g_hTraceProvider);
        
        LoadConfig();
        
        if (P2P_Init() != P2P_OK) {
            TraceError("P2P Init Error P2P_Init failed");
            return FALSE;
        }
        
        P2P_SetConnectionCallback(ConnectionCallback, nullptr);

        if (P2P_Connect(g_strRemoteIP.c_str(), g_nPort, &g_connectedPeer) != P2P_OK) {
            TraceError("P2P Connect Error Connect to %s:%u failed", g_strRemoteIP.c_str(), g_nPort);
        } else {
            TraceInfo("P2P Debug Mode Client mode\nConnected to: %s:%u\nPeerID: %u", 
                g_strRemoteIP.c_str(), g_nPort, g_connectedPeer);

            P2P_SetAutoReconnect(g_connectedPeer, true, 500, 10 * 1000);
        }

        g_stopPumpThread.store(false);
        g_hPumpThread = CreateThread(NULL, 0, PumpThread, NULL, 0, NULL);
        
        // 创建线程，初始化完成后注入 VTable
        hInjectionThread = CreateThread(NULL, 0, InjectionThread, NULL, 0, NULL);
        break;
        
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
        
    case DLL_PROCESS_DETACH:
        g_stopPumpThread.store(true);
        if (g_hPumpThread) {
            WaitForSingleObject(g_hPumpThread, 1000);
            CloseHandle(g_hPumpThread);
            g_hPumpThread = NULL;
        }

        if (hInjectionThread) {
            // 等待注入线程结束（最多等1秒）
            WaitForSingleObject(hInjectionThread, 1000);
            CloseHandle(hInjectionThread);
        }
        UnhookVTable();
        P2P_Shutdown();        
        
        // 注销 TraceLogging Provider
        TraceLoggingUnregister(g_hTraceProvider);
        break;
    }
    return TRUE;
}
