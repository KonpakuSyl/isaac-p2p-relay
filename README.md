# Isaac P2P Network

中文 | [English](README_EN.md)

为《以撒的结合》(The Binding of Isaac) 提供的 TCP P2P 网络中继解决方案，通过 Hook Steam P2P API 实现游戏联机功能。

## 功能特性

- **TCP P2P 中继** - 绕过 Steam P2P 网络限制，通过自建服务器实现稳定联机
- **Steam API Hook** - 透明拦截 `ISteamNetworking` 接口，无需修改游戏文件
- **跨平台服务端** - Linux 服务端基于 epoll 实现高性能非阻塞 I/O
- **DLL 代理注入** - 通过 winmm.dll 代理实现自动加载

## 项目结构

```
isaac/
├── include/
│   └── p2p_network.h       # P2P 网络库公共头文件
├── src/
│   ├── p2p_network.cpp     # P2P 网络库实现
│   ├── connection_manager.cpp/h  # 连接管理器
│   └── packet_queue.cpp/h  # 数据包队列
├── hook/
│   ├── hook.cpp            # Steam API Hook 实现
│   ├── p2p_config.txt      # 客户端配置文件
│   └── load.js             # 辅助脚本
├── server/
│   ├── main.cpp            # TCP 中继服务器 (Linux)
│   └── Makefile            # 服务端编译配置
├── winmm/
│   ├── winmm.cpp           # WinMM 代理 DLL
│   ├── CMakeLists.txt      # WinMM 编译配置
│   └── x86.def             # 导出定义
└── CMakeLists.txt          # 主 CMake 配置
```

## 系统要求

### 客户端 (Windows)
- Windows 10/11
- CMake 3.15+
- Visual Studio 2019+ 或 MinGW-w64
- The Binding of Isaac: Repentance

### 服务端 (Linux)
- Linux 内核 2.6+（支持 epoll）
- GCC 7+ 或 Clang 5+

## 编译

### Windows 客户端

```bash
# 创建构建目录
mkdir build && cd build

# 32位 (游戏为32位)
cmake -A Win32 ..
cmake --build . --config Release

# 或 64位
cmake -A x64 ..
cmake --build . --config Release
```

编译产物位于 `build/bin/x86/Release/` 或 `build/bin/x64/Release/`:
- `p2p_network.dll` - P2P 网络库
- `p2p_hook.dll` - Steam API Hook DLL
- `p2p_config.txt` - 配置文件

### Linux 服务端

```bash
cd server
make

# 启用调试日志
make DEBUG=1
```

## 使用方法

### 1. 部署服务端

在公网服务器上运行：

```bash
./relay_server 27015
```

### 2. 配置客户端

编辑 `p2p_config.txt`：

```ini
# P2P Network Configuration
mode=client

# 服务端 IP 地址
ip=你的服务器IP

# 端口号
port=27015
```

### 3. 安装到游戏

将以下文件复制到游戏目录：
- `p2p_network.dll`
- `p2p_hook.dll`
- `p2p_config.txt`
- `winmm.dll` (代理 DLL，用于自动加载)

### 4. 启动游戏

正常启动游戏即可，Hook 会自动生效。

## 技术原理

### Steam API Hook

通过虚表 (VTable) Hook 技术拦截 `ISteamNetworking` 接口的以下方法：
- `SendP2PPacket` - 发送数据包
- `IsP2PPacketAvailable` - 检查可用数据包
- `ReadP2PPacket` - 读取数据包

所有 P2P 通信被透明重定向到 TCP 中继服务器。

### 数据包协议

```
包格式: [4字节长度 (小端序)] + [数据]

注册包 (首次连接):
  [4字节长度=8] + [8字节 SteamID]

转发包:
  [4字节长度] + [8字节目标 SteamID] + [实际数据]
```

### ETW 日志

Hook DLL 使用 Windows ETW TraceLogging 进行调试：
- Provider: `P2PHookProvider`
- GUID: `{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}`

使用 Windows Performance Recorder (WPR) 或 tracelog 工具查看日志。

## 配置说明

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `mode` | 运行模式 (client/server) | `client` |
| `ip` | 服务端 IP 地址 | `127.0.0.1` |
| `port` | 端口号 | `27015` |

## 故障排除

### 连接失败
1. 确认服务端正在运行
2. 检查防火墙是否开放端口
3. 验证配置文件中的 IP 和端口

### Hook 未生效
1. 确认所有 DLL 文件位于游戏目录
2. 检查 DLL 架构是否匹配（32位游戏需要32位 DLL）

### 查看调试日志
使用 ETW 工具捕获 `P2PHookProvider` 的日志事件。

## TODO

- [ ] **客户端重连机制** - 当前客户端在连接断开后不会自动重连，需要重启游戏。计划添加自动重连功能，包括：
  - 检测连接断开
  - 指数退避重连策略
  - 重连期间的数据包缓存

## 许可证

本项目仅供学习和研究目的。使用本工具修改游戏行为可能违反游戏服务条款，请自行承担风险。

## 免责声明

- 本项目与 Edmund McMillen、Nicalis 或 Valve 无关
- 仅供技术研究和私人使用
- 不保证与游戏更新的兼容性
