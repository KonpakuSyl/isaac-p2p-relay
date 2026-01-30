# TCP P2P Relay Server Makefile

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
DEBUG_FLAGS = -std=c++17 -Wall -Wextra -g -O0 -DDEBUG_MODE
TARGET = relay_server
SRC = main.cpp

.PHONY: all clean debug run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

# Debug模式 - 启用DEBUG_MODE宏，输出debug级别日志
# 使用: make debug
debug: clean
	$(CXX) $(DEBUG_FLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

# 安装到 /usr/local/bin (需要sudo)
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

# 运行示例 (端口8888)
run: $(TARGET)
	./$(TARGET) 8888
