#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <vector>
#include <cstdint>

// 获取可用端口的函数
int get_available_port();

// 写入包长度（小端序）
void write_packet_length(uint8_t* buf, uint32_t len);

// 读取包长度（小端序）
uint32_t read_packet_length(const uint8_t* buf);

// 实现包长度读取
uint32_t read_packet_length_impl(const uint8_t* buf);

#endif // TEST_HELPERS_H