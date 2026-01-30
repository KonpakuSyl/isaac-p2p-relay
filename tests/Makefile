# P2P Relay Tests Makefile

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -I../include
DEBUG_FLAGS = -std=c++17 -Wall -Wextra -g -O0 -DDEBUG_MODE -I../include
TARGET = p2p_tests
SOURCES = test_main.cpp test_p2p_forwarding.cpp test_stress_forwarding.cpp

.PHONY: all clean debug run stress

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Debug模式
debug: clean
	$(CXX) $(DEBUG_FLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)

# 运行所有测试
run: $(TARGET)
	./$(TARGET)

# 运行详细测试
verbose: $(TARGET)
	./$(TARGET) -s

# 只运行压力测试
stress: $(TARGET)
	./$(TARGET) [stress]