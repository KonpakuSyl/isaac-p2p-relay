cmake_minimum_required(VERSION 3.15)
project(P2PNetwork VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 检测架构
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(P2P_ARCH "x64")
else()
    set(P2P_ARCH "x86")
endif()

message(STATUS "Building for architecture: ${P2P_ARCH}")

# 动态库源文件
set(P2P_SOURCES
    src/p2p_network.cpp
    src/connection_manager.cpp
    src/packet_queue.cpp
)

# 创建动态库
add_library(p2p_network SHARED ${P2P_SOURCES})

# 包含目录
target_include_directories(p2p_network
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Windows 平台链接 Winsock2
if(WIN32)
    target_link_libraries(p2p_network PRIVATE ws2_32)
    target_compile_definitions(p2p_network PRIVATE P2P_NETWORK_EXPORTS)
endif()

# MSVC UTF-8 编码支持 (修复 C4819 警告)
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 设置输出目录 (按架构区分)
set_target_properties(p2p_network PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/${P2P_ARCH}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${P2P_ARCH}
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${P2P_ARCH}
)

# 示例程序
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/example/main.cpp")
    add_executable(p2p_example example/main.cpp)
    target_link_libraries(p2p_example PRIVATE p2p_network)
    target_include_directories(p2p_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    set_target_properties(p2p_example PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/${P2P_ARCH}
    )
endif()

# Hook DLL
add_library(p2p_hook SHARED hook/hook.cpp)
target_link_libraries(p2p_hook PRIVATE p2p_network advapi32)
target_include_directories(p2p_hook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(p2p_hook PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/${P2P_ARCH}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${P2P_ARCH}
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${P2P_ARCH}
)

# 复制配置文件到输出目录
add_custom_command(TARGET p2p_hook POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/hook/p2p_config.txt
        ${CMAKE_BINARY_DIR}/bin/${P2P_ARCH}/$<CONFIG>/p2p_config.txt
    COMMENT "Copying p2p_config.txt to output directory"
)