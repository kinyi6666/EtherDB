cmake_minimum_required(VERSION 3.10)
project(EtherDB_Shell CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 客户端自包含：不依赖服务器源码目录（base/net/rpc/query/etdb/dbnode/wal）
if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX)
else()
    add_definitions(-DNO_FSEEKO)
endif()
add_definitions(-D_CRT_SECURE_NO_DEPRECATE -D_CRT_NONSTDC_NO_DEPRECATE)

# MSVC：源码含 UTF-8 中文注释，指定 /utf-8 避免 C4819 警告
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 仅需要 client 目录自身
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Client sources
set(CLIENT_SRCS
    shell_main.cpp
    etdb.cpp
    EtDBClient.cpp
    tcpClient.cpp
)

add_executable(etherdb ${CLIENT_SRCS})

# Windows: ws2_32 (Winsock)；Linux: pthread
if(WIN32)
    target_link_libraries(etherdb ws2_32)
else()
    target_link_libraries(etherdb pthread)
endif()

set_target_properties(etherdb PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../bin
)
