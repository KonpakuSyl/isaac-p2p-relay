# Isaac P2P Network

[中文](README.md) | English

A TCP P2P network relay solution for *The Binding of Isaac*, enabling multiplayer functionality by hooking Steam P2P API.

## Features

- **TCP P2P Relay** - Bypass Steam P2P network limitations with a self-hosted relay server
- **Steam API Hook** - Transparently intercept `ISteamNetworking` interface without modifying game files
- **Cross-platform Server** - Linux server with high-performance non-blocking I/O based on epoll
- **DLL Proxy Injection** - Automatic loading via winmm.dll proxy

## Project Structure

```
isaac/
├── include/
│   └── p2p_network.h       # P2P network library public header
├── src/
│   ├── p2p_network.cpp     # P2P network library implementation
│   ├── connection_manager.cpp/h  # Connection manager
│   └── packet_queue.cpp/h  # Packet queue
├── hook/
│   ├── hook.cpp            # Steam API Hook implementation
│   ├── p2p_config.txt      # Client configuration file
│   └── load.js             # Helper script
├── server/
│   ├── main.cpp            # TCP relay server (Linux)
│   └── Makefile            # Server build configuration
├── winmm/
│   ├── winmm.cpp           # WinMM proxy DLL
│   ├── CMakeLists.txt      # WinMM build configuration
│   └── x86.def             # Export definitions
└── CMakeLists.txt          # Main CMake configuration
```

## System Requirements

### Client (Windows)
- Windows 10/11
- CMake 3.15+
- Visual Studio 2019+ or MinGW-w64
- The Binding of Isaac: Repentance

### Server (Linux)
- Linux kernel 2.6+ (epoll support)
- GCC 7+ or Clang 5+

## Building

### Windows Client

```bash
# Create build directory
mkdir build && cd build

# 32-bit (game is 32-bit)
cmake -A Win32 ..
cmake --build . --config Release

# Or 64-bit
cmake -A x64 ..
cmake --build . --config Release
```

Build outputs are located in `build/bin/x86/Release/` or `build/bin/x64/Release/`:
- `p2p_network.dll` - P2P network library
- `p2p_hook.dll` - Steam API Hook DLL
- `p2p_config.txt` - Configuration file

### Linux Server

```bash
cd server
make

# Enable debug logging
make DEBUG=1
```

## Usage

### 1. Deploy Server

Run on a public server:

```bash
./relay_server 27015
```

### 2. Configure Client

Edit `p2p_config.txt`:

```ini
# P2P Network Configuration
mode=client

# Server IP address
ip=YOUR_SERVER_IP

# Port
port=27015
```

### 3. Install to Game

Copy the following files to the game directory:
- `p2p_network.dll`
- `p2p_hook.dll`
- `p2p_config.txt`
- `winmm.dll` (proxy DLL for automatic loading)

### 4. Launch Game

Launch the game normally, the hook will take effect automatically.

## Technical Details

### Steam API Hook

Intercepts the following methods of `ISteamNetworking` interface using VTable hooking:
- `SendP2PPacket` - Send packets
- `IsP2PPacketAvailable` - Check for available packets
- `ReadP2PPacket` - Read packets

All P2P communications are transparently redirected to the TCP relay server.

### Packet Protocol

```
Packet format: [4-byte length (little-endian)] + [data]

Registration packet (first connection):
  [4-byte length=8] + [8-byte SteamID]

Forward packet:
  [4-byte length] + [8-byte target SteamID] + [payload]
```

### ETW Logging

The Hook DLL uses Windows ETW TraceLogging for debugging:
- Provider: `P2PHookProvider`
- GUID: `{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}`

Use Windows Performance Recorder (WPR) or tracelog tools to view logs.

## Configuration

| Parameter | Description | Default |
|-----------|-------------|---------|
| `mode` | Operation mode (client/server) | `client` |
| `ip` | Server IP address | `127.0.0.1` |
| `port` | Port number | `27015` |

## Troubleshooting

### Connection Failed
1. Verify the server is running
2. Check if firewall allows the port
3. Verify IP and port in configuration file

### Hook Not Working
1. Ensure all DLL files are in the game directory
2. Check if DLL architecture matches (32-bit game requires 32-bit DLLs)

### View Debug Logs
Use ETW tools to capture `P2PHookProvider` log events.

## TODO

- [ ] **Client Reconnection** - Currently, the client does not automatically reconnect after disconnection, requiring a game restart. Planned features:
  - Connection loss detection
  - Exponential backoff reconnection strategy
  - Packet buffering during reconnection

## License

This project is for educational and research purposes only. Using this tool to modify game behavior may violate the game's terms of service. Use at your own risk.

## Disclaimer

- This project is not affiliated with Edmund McMillen, Nicalis, or Valve
- For technical research and private use only
- Compatibility with game updates is not guaranteed
