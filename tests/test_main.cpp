#define CATCH_CONFIG_MAIN
#include "catch2/catch.hpp"
#include "test_helpers.h"
#include <functional>

// 声明测试函数
extern bool test_two_clients_forwarding();
extern bool test_socket_forwarding();
extern bool test_two_clients_high_throughput();
extern bool test_three_clients_high_throughput();

TEST_CASE("Two Clients Forwarding Test", "[p2p]") {
    REQUIRE(test_two_clients_forwarding() == true);
}

TEST_CASE("Socket Forwarding Test", "[p2p]") {
    REQUIRE(test_socket_forwarding() == true);
}

TEST_CASE("Two Clients High Throughput Test", "[stress]") {
    REQUIRE(test_two_clients_high_throughput() == true);
}

TEST_CASE("Three Clients High Throughput Test", "[stress]") {
    REQUIRE(test_three_clients_high_throughput() == true);
}