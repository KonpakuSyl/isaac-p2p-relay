#include "test_helpers.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <vector>
#include <cstring>
#include <iostream>
#include <atomic>
#include <future>
#include <functional>
#include <random>
#include <algorithm>
#include <sys/select.h>

// 生成随机数据
std::vector<uint8_t> generate_random_data(size_t size) {
    std::vector<uint8_t> data(size);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    for (size_t i = 0; i < size; i++) {
        data[i] = static_cast<uint8_t>(dis(gen));
    }
    
    return data;
}

// 高吞吐量测试：2个客户端大量数据传输
bool test_two_clients_high_throughput() {
    std::cout << "Testing high throughput with 2 clients..." << std::endl;

    // 使用固定的测试端口
    const int port = 8889;
    
    // 连接两个客户端进行高吞吐量测试
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 客户端1
    int client1_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr1;
    serv_addr1.sin_family = AF_INET;
    serv_addr1.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr1.sin_addr);
    
    if (connect(client1_fd, (struct sockaddr *)&serv_addr1, sizeof(serv_addr1)) < 0) {
        std::cerr << "Client 1 connect failed" << std::endl;
        close(client1_fd);
        return false;
    }
    
    // 客户端2
    int client2_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr2;
    serv_addr2.sin_family = AF_INET;
    serv_addr2.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr2.sin_addr);
    
    if (connect(client2_fd, (struct sockaddr *)&serv_addr2, sizeof(serv_addr2)) < 0) {
        std::cerr << "Client 2 connect failed" << std::endl;
        close(client1_fd);
        close(client2_fd);
        return false;
    }
    
    // 注册客户端1
    uint8_t reg_header[4];
    write_packet_length(reg_header, 8);
    
    uint8_t client1_mark[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    std::vector<uint8_t> client1_reg;
    client1_reg.insert(client1_reg.end(), reg_header, reg_header + 4);
    client1_reg.insert(client1_reg.end(), client1_mark, client1_mark + 8);
    
    if (send(client1_fd, client1_reg.data(), client1_reg.size(), 0) < 0) {
        std::cerr << "Client 1 registration failed" << std::endl;
        close(client1_fd);
        close(client2_fd);
        return false;
    }
    
    // 注册客户端2
    uint8_t client2_mark[8] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    std::vector<uint8_t> client2_reg;
    client2_reg.insert(client2_reg.end(), reg_header, reg_header + 4);
    client2_reg.insert(client2_reg.end(), client2_mark, client2_mark + 8);
    
    if (send(client2_fd, client2_reg.data(), client2_reg.size(), 0) < 0) {
        std::cerr << "Client 2 registration failed" << std::endl;
        close(client1_fd);
        close(client2_fd);
        return false;
    }
    
    // 等待注册完成
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // 开始高吞吐量测试
    const int num_messages = 500;  // 发送500条消息
    const size_t message_size = 256; // 每条消息256B
    
    std::atomic<int> received_count(0);
    std::atomic<int> sent_count(0);
    
    // 客户端1发送大量数据到客户端2
    std::thread sender_thread([&]() {
        for (int i = 0; i < num_messages; i++) {
            std::vector<uint8_t> random_data = generate_random_data(message_size);
            
            // 构造消息包 - 遵循服务器协议: [4字节长度] + [8字节目标标记] + [实际数据]
            uint8_t msg_header[4];
            write_packet_length(msg_header, 8 + message_size); // 总长度是标记+数据
            
            std::vector<uint8_t> msg_to_client2;
            msg_to_client2.insert(msg_to_client2.end(), msg_header, msg_header + 4);
            msg_to_client2.insert(msg_to_client2.end(), client2_mark, client2_mark + 8);  // 目标标记
            msg_to_client2.insert(msg_to_client2.end(), random_data.begin(), random_data.end());
            
            if (send(client1_fd, msg_to_client2.data(), msg_to_client2.size(), 0) <= 0) {
                std::cerr << "Sending message " << i << " failed" << std::endl;
                break;
            }
            
            sent_count++;
            
            if (i % 10 == 0) {
                std::cout << "Sent " << i << " messages from client 1 to client 2" << std::endl;
            }
            
            // 短暂延迟以避免过快发送导致缓冲区溢出
            std::this_thread::sleep_for(std::chrono::microseconds(1000));
        }
    });
    
    // 客户端2接收数据
    std::thread receiver_thread([&]() {
        for (int i = 0; i < num_messages; i++) {
            // 首先读取4字节长度（服务器转发时会保留长度前缀）
            uint8_t len_buf[4];
            ssize_t total_len = 0;
            while (total_len < 4) {
                ssize_t r = recv(client2_fd, len_buf + total_len, 4 - total_len, 0);
                if (r <= 0) {
                    std::cerr << "Failed to read length header" << std::endl;
                    break;
                }
                total_len += r;
            }
            
            if (total_len < 4) break;
            
            uint32_t data_len = read_packet_length_impl(len_buf);
            
            // 分配缓冲区并读取数据（服务器转发时去掉了8字节目标标记）
            std::vector<uint8_t> response(data_len);
            total_len = 0;
            while (total_len < static_cast<ssize_t>(data_len)) {
                ssize_t r = recv(client2_fd, response.data() + total_len, 
                                data_len - total_len, 0);
                if (r <= 0) {
                    std::cerr << "Failed to read message data" << std::endl;
                    break;
                }
                total_len += r;
            }
            
            if (total_len == static_cast<ssize_t>(data_len)) {
                received_count++;
            } else {
                std::cerr << "Incomplete message received: expected " << data_len 
                          << " bytes, got " << total_len << " bytes" << std::endl;
            }
            
            if (i % 10 == 0) {
                std::cout << "Received " << i << " messages at client 2" << std::endl;
            }
        }
    });
    
    // 等待发送完成
    sender_thread.join();
    
    // 等待接收完成，最多等待10秒
    auto start_time = std::chrono::steady_clock::now();
    while (received_count.load() < num_messages) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time);
        if (elapsed.count() > 10) {
            std::cout << "Timeout waiting for all messages to be received" << std::endl;
            break;
        }
    }
    
    // 等待接收线程结束（而不是分离）
    receiver_thread.join();
    
    std::cout << "High throughput test: Sent " << sent_count.load() 
              << " messages, received " << received_count.load() << " messages" << std::endl;
    
    bool success = received_count.load() >= num_messages * 0.8; // 接收80%以上就算成功
    
    // 清理
    close(client1_fd);
    close(client2_fd);
    
    if (success) {
        std::cout << "High throughput test with 2 clients PASSED" << std::endl;
    } else {
        std::cout << "High throughput test with 2 clients FAILED" << std::endl;
        std::cout << "Expected: " << num_messages << ", Sent: " << sent_count.load() 
                  << ", Received: " << received_count.load() << std::endl;
    }
    
    return success;
}

// 高吞吐量测试：3个客户端大量数据传输
bool test_three_clients_high_throughput() {
    std::cout << "Testing high throughput with 3 clients..." << std::endl;

    // 使用固定的测试端口
    const int port = 8889;
    
    // 连接三个客户端进行高吞吐量测试
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    int client_fds[3];
    struct sockaddr_in serv_addrs[3];
    
    // 连接所有客户端
    for (int i = 0; i < 3; i++) {
        client_fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        serv_addrs[i].sin_family = AF_INET;
        serv_addrs[i].sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &serv_addrs[i].sin_addr);
        
        if (connect(client_fds[i], (struct sockaddr *)&serv_addrs[i], sizeof(serv_addrs[i])) < 0) {
            std::cerr << "Client " << (i+1) << " connect failed" << std::endl;
            for (int j = 0; j <= i; j++) {
                close(client_fds[j]);
            }
            return false;
        }
    }
    
    // 准备客户端标记
    uint8_t client_marks[3][8] = {
        {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08},
        {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01},
        {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}
    };
    
    // 注册所有客户端
    uint8_t reg_header[4];
    write_packet_length(reg_header, 8);
    
    for (int i = 0; i < 3; i++) {
        std::vector<uint8_t> client_reg;
        client_reg.insert(client_reg.end(), reg_header, reg_header + 4);
        client_reg.insert(client_reg.end(), client_marks[i], client_marks[i] + 8);
        
        if (send(client_fds[i], client_reg.data(), client_reg.size(), 0) < 0) {
            std::cerr << "Client " << (i+1) << " registration failed" << std::endl;
            for (int j = 0; j < 3; j++) {
                close(client_fds[j]);
            }
            return false;
        }
    }
    
    // 等待注册完成
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // 开始高吞吐量测试 - 三个客户端互相发送数据
    const int num_messages_per_client = 200;  // 每客户端200条消息
    const size_t message_size = 128; // 每条消息128B
    
    std::atomic<int> total_sent(0);
    std::atomic<int> total_received(0);
    
    // 创建三个发送线程，每个客户端向其他两个客户端发送数据
    std::vector<std::thread> send_threads;
    
    for (int sender = 0; sender < 3; sender++) {
        send_threads.emplace_back([&, sender]() {
            for (int i = 0; i < num_messages_per_client; i++) {
                // 计算接收方 (round-robin)
                int receiver = (sender + 1 + i % 2) % 3;  // 发送给另外两个客户端之一
                
                std::vector<uint8_t> random_data = generate_random_data(message_size);
                
                // 构造消息包 - 遵循服务器协议: [4字节长度] + [8字节目标标记] + [实际数据]
                uint8_t msg_header[4];
                write_packet_length(msg_header, 8 + message_size); // 总长度是标记+数据
                
                std::vector<uint8_t> msg_to_receiver;
                msg_to_receiver.insert(msg_to_receiver.end(), msg_header, msg_header + 4);
                msg_to_receiver.insert(msg_to_receiver.end(), client_marks[receiver], client_marks[receiver] + 8);  // 目标标记
                msg_to_receiver.insert(msg_to_receiver.end(), random_data.begin(), random_data.end());
                
                if (send(client_fds[sender], msg_to_receiver.data(), msg_to_receiver.size(), 0) <= 0) {
                    std::cerr << "Sending message from client " << (sender+1) 
                              << " to client " << (receiver+1) << " failed" << std::endl;
                    break;
                }
                
                total_sent.fetch_add(1);
                
                if (i % 10 == 0) {
                    std::cout << "Client " << (sender+1) << " sent " << i 
                              << " messages to client " << (receiver+1) << std::endl;
                }
                
                // 添加短暂延迟以避免过载
                std::this_thread::sleep_for(std::chrono::microseconds(1000));
            }
        });
    }
    
    // 创建接收线程，接收来自其他客户端的数据
    std::vector<std::thread> recv_threads;
    
    for (int receiver = 0; receiver < 3; receiver++) {
        recv_threads.emplace_back([&, receiver]() {
            int expected_messages = num_messages_per_client;  // 从其他两个客户端接收（对方各发送一半）
            int received = 0;
            
            for (int i = 0; i < expected_messages; i++) {
                // 首先读取4字节长度（服务器转发时会保留长度前缀）
                uint8_t len_buf[4];
                ssize_t total_len = 0;
                while (total_len < 4) {
                    ssize_t r = recv(client_fds[receiver], len_buf + total_len, 4 - total_len, 0);
                    if (r <= 0) {
                        std::cerr << "Failed to read length header at client " << (receiver+1) << std::endl;
                        goto cleanup;  // 跳出循环
                    }
                    total_len += r;
                }
                
                if (total_len < 4) break;
                
                uint32_t data_len = read_packet_length_impl(len_buf);
                
                // 分配缓冲区并读取数据（服务器转发时去掉了8字节目标标记）
                std::vector<uint8_t> response(data_len);
                total_len = 0;
                while (total_len < static_cast<ssize_t>(data_len)) {
                    ssize_t r = recv(client_fds[receiver], response.data() + total_len, 
                                    data_len - total_len, 0);
                    if (r <= 0) {
                        std::cerr << "Failed to read message data at client " << (receiver+1) << std::endl;
                        goto cleanup;  // 跳出循环
                    }
                    total_len += r;
                }
                
                if (total_len == static_cast<ssize_t>(data_len)) {
                    total_received.fetch_add(1);
                    received++;
                } else {
                    std::cerr << "Incomplete message received at client " << (receiver+1) 
                              << ": expected " << data_len << " bytes, got " << total_len << " bytes" << std::endl;
                }
                
                if (i % 10 == 0) {
                    std::cout << "Client " << (receiver+1) << " received " << i << " messages" << std::endl;
                }
            }
            
cleanup:
            std::cout << "Client " << (receiver+1) << " finished receiving, got " << received << " messages" << std::endl;
        });
    }
    
    // 等待所有发送完成
    for (auto& t : send_threads) {
        t.join();
    }
    
    // 等待所有接收完成
    for (auto& t : recv_threads) {
        t.join();
    }
    
    int expected_total = num_messages_per_client * 3;  // 每个发送者发送num_messages_per_client条，总计3倍
    
    std::cout << "High throughput test with 3 clients: Expected " << expected_total 
              << " messages, sent " << total_sent.load()
              << ", received " << total_received.load() << " messages" << std::endl;
    
    bool success = total_received.load() >= expected_total * 0.8; // 接收80%以上就算成功
    
    // 清理
    for (int i = 0; i < 3; i++) {
        close(client_fds[i]);
    }
    
    if (success) {
        std::cout << "High throughput test with 3 clients PASSED" << std::endl;
    } else {
        std::cout << "High throughput test with 3 clients FAILED" << std::endl;
        std::cout << "Expected: " << expected_total << ", Sent: " << total_sent.load() 
                  << ", Received: " << total_received.load() << " (Success rate: " 
                  << (double)total_received.load()/(expected_total)*100 << "%)" << std::endl;
    }
    
    return success;
}