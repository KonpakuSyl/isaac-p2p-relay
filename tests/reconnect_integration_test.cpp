#include <p2p_network.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

static bool recvAll(SOCKET s, void* buf, int len) {
    char* p = static_cast<char*>(buf);
    int got = 0;
    while (got < len) {
        int r = recv(s, p + got, len - got, 0);
        if (r <= 0) {
            return false;
        }
        got += r;
    }
    return true;
}

static bool sendAll(SOCKET s, const void* buf, int len) {
    const char* p = static_cast<const char*>(buf);
    int sent = 0;
    while (sent < len) {
        int r = send(s, p + sent, len - sent, 0);
        if (r <= 0) {
            return false;
        }
        sent += r;
    }
    return true;
}

static uint32_t readLe32(const uint8_t* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return 2;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        WSACleanup();
        return 2;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(listenSock);
        WSACleanup();
        return 2;
    }

    if (listen(listenSock, SOMAXCONN) != 0) {
        closesocket(listenSock);
        WSACleanup();
        return 2;
    }

    int addrLen = sizeof(addr);
    if (getsockname(listenSock, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
        closesocket(listenSock);
        WSACleanup();
        return 2;
    }

    const uint16_t port = ntohs(addr.sin_port);

    std::atomic<bool> serverOk{false};
    std::thread serverThread([&]() {
        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            return;
        }

        uint8_t hdr[4];
        if (!recvAll(client, hdr, 4)) {
            closesocket(client);
            return;
        }
        const uint32_t len1 = readLe32(hdr);
        std::vector<uint8_t> payload1(len1);
        if (!recvAll(client, payload1.data(), (int)len1)) {
            closesocket(client);
            return;
        }
        if (len1 != 8) {
            closesocket(client);
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        closesocket(client);

        client = accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            return;
        }

        if (!recvAll(client, hdr, 4)) {
            closesocket(client);
            return;
        }
        const uint32_t len2 = readLe32(hdr);
        std::vector<uint8_t> payload2(len2);
        if (!recvAll(client, payload2.data(), (int)len2)) {
            closesocket(client);
            return;
        }
        if (len2 != 8) {
            closesocket(client);
            return;
        }

        if (!recvAll(client, hdr, 4)) {
            closesocket(client);
            return;
        }
        const uint32_t msgLen = readLe32(hdr);
        std::vector<uint8_t> msg(msgLen);
        if (!recvAll(client, msg.data(), (int)msgLen)) {
            closesocket(client);
            return;
        }

        serverOk.store(true);
        closesocket(client);
    });

    if (P2P_Init() != P2P_OK) {
        closesocket(listenSock);
        serverThread.join();
        WSACleanup();
        return 2;
    }

    std::atomic<int> connectedCount{0};
    std::atomic<int> disconnectedCount{0};

    struct CbState {
        std::atomic<int>* connected;
        std::atomic<int>* disconnected;
        P2PPeerID peerID;
    } cbState{&connectedCount, &disconnectedCount, P2P_INVALID_PEER_ID};

    P2P_SetConnectionCallback(
        [](P2PPeerID peerID, bool connected, void* userData) {
            auto* st = static_cast<CbState*>(userData);
            if (connected) {
                st->peerID = peerID;
                st->connected->fetch_add(1);
            } else {
                st->disconnected->fetch_add(1);
            }
        },
        &cbState);

    P2PPeerID peerID = P2P_INVALID_PEER_ID;
    if (P2P_Connect("127.0.0.1", port, &peerID) != P2P_OK) {
        P2P_Shutdown();
        closesocket(listenSock);
        serverThread.join();
        WSACleanup();
        return 2;
    }

    P2P_SetAutoReconnect(peerID, true, 100, 1000);

    const uint64_t mark = 0x0807060504030201ULL;
    if (P2P_SendPacket(peerID, &mark, sizeof(mark)) != P2P_OK) {
        P2P_Shutdown();
        closesocket(listenSock);
        serverThread.join();
        WSACleanup();
        return 2;
    }

    const auto start = std::chrono::steady_clock::now();
    bool sentAfterReconnect = false;

    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        P2P_RunCallbacks();

        if (connectedCount.load() >= 2 && !sentAfterReconnect) {
            if (P2P_SendPacket(peerID, &mark, sizeof(mark)) == P2P_OK) {
                const char msg[] = "post-reconnect";
                if (P2P_SendPacket(peerID, msg, (uint32_t)sizeof(msg)) == P2P_OK) {
                    sentAfterReconnect = true;
                }
            }
        }

        if (serverOk.load()) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    P2P_Shutdown();
    closesocket(listenSock);
    serverThread.join();
    WSACleanup();

    if (!sentAfterReconnect) {
        return 1;
    }
    if (!serverOk.load()) {
        return 1;
    }

    return 0;
#else
    return 0;
#endif
}
