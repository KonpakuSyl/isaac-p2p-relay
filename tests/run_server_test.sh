#!/bin/bash

# 启动服务器并运行压力测试

echo "Building server..."
cd ../server && make clean && make

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

echo "Build successful!"

# 获取一个可用端口
PORT=8889
echo "Using port: $PORT"

# 启动服务器
echo "Starting server on port $PORT..."
cd ../server
./relay_server $PORT > server.log 2>&1 &
SERVER_PID=$!

# 等待服务器启动
sleep 1

if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "Failed to start server"
    exit 1
fi

echo "Server started with PID: $SERVER_PID"

# 运行压力测试
echo "Running pressure tests..."
cd ../tests
( ./p2p_tests \[stress\] & ) & TEST_PID=$!
sleep 60  # 等待60秒后停止测试

# 检查测试是否仍在运行
if kill -0 $TEST_PID 2>/dev/null; then
    echo "Test still running after 60 seconds, stopping it..."
    kill $TEST_PID 2>/dev/null
    sleep 2
    # 确保进程终止
    kill -9 $TEST_PID 2>/dev/null
fi

# 检查测试结果
if wait $TEST_PID 2>/dev/null; then
    TEST_RESULT=0
else
    TEST_RESULT=1
fi

# 停止服务器
kill $SERVER_PID 2>/dev/null
sleep 1
# 确保服务器进程终止
kill -9 $SERVER_PID 2>/dev/null

if [ $TEST_RESULT -eq 0 ]; then
    echo "Pressure tests completed successfully"
else
    echo "Pressure tests failed or timed out"
fi

exit $TEST_RESULT