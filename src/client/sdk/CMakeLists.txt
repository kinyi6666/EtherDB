# ============================================================================
# EtherDB Client SDK —— 独立于 src/client/CMakeLists.txt 的库构建
#
# 说明：
#   - 原 src/client/CMakeLists.txt 保持不变（shell 直接编译源码，便于调试）。
#   - 本文件仅构建供数据库使用者链接的 etherdb_client 库，
#     并打包 SDK 到 src/bin/sdk/{include,lib,README.md}。
#
# 用法（也可直接用 scripts/build_sdk.bat / build_sdk.sh）：
#   cmake -S src/client/sdk -B build_win_sdk -A x64
#   cmake --build build_win_sdk --config Release --target etherdb_client
#
# 默认静态库；共享库：cmake -DETHERDB_BUILD_SHARED=ON
# ============================================================================
cmake_minimum_required(VERSION 3.10)
project(EtherDB_ClientSDK CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

# 客户端源码目录（本文件位于 src/client/sdk/）
set(CLIENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ============================================================================
# 客户端库 etherdb_client —— 供数据库使用者链接
#   公共接口（随库发布到 SDK include/）：
#     C   接口 : etdb.h              (extern "C"，纯 C 可直接调用)
#     C++ 接口 : EtDBClient.h        (及其依赖头文件)
#   产物：
#     Windows: src/bin/etherdb_client.lib  (静态) 或 etherdb_client.dll+.lib (共享)
#     Linux  : src/bin/libetherdb_client.a (静态) 或 libetherdb_client.so     (共享)
# ============================================================================
set(CLIENT_LIB_SRCS
    ${CLIENT_DIR}/EtDBClient.cpp
    ${CLIENT_DIR}/etdb.cpp
    ${CLIENT_DIR}/tcpClient.cpp
)

# 公共接口头文件（打包进 SDK include/）
set(CLIENT_PUBLIC_HEADERS
    ${CLIENT_DIR}/etdb.h
    ${CLIENT_DIR}/EtDBClient.h
    ${CLIENT_DIR}/EtDBClientProtocol.h
    ${CLIENT_DIR}/EtDBMeta.h
    ${CLIENT_DIR}/EtDBLog.h
    ${CLIENT_DIR}/tcpClient.h
)

# 默认构建静态库；需要共享库时：cmake -DETHERDB_BUILD_SHARED=ON
option(ETHERDB_BUILD_SHARED "Build shared library instead of static" OFF)
if(ETHERDB_BUILD_SHARED)
    add_library(etherdb_client SHARED ${CLIENT_LIB_SRCS})
else()
    add_library(etherdb_client STATIC ${CLIENT_LIB_SRCS})
endif()

target_include_directories(etherdb_client PUBLIC ${CLIENT_DIR})

# Windows: ws2_32 (Winsock)；Linux: pthread
if(WIN32)
    target_link_libraries(etherdb_client PUBLIC ws2_32)
else()
    target_link_libraries(etherdb_client PUBLIC pthread)
endif()

# 库输出到 src/bin（与 shell 输出保持一致）
set_target_properties(etherdb_client PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CLIENT_DIR}/../bin
    LIBRARY_OUTPUT_DIRECTORY ${CLIENT_DIR}/../bin
    RUNTIME_OUTPUT_DIRECTORY ${CLIENT_DIR}/../bin
)

# ============================================================================
# SDK 打包：src/bin/sdk/{include,lib,README.md}
# 数据库使用者只需拿到 src/bin/sdk 目录即可开发。
# ============================================================================
set(SDK_DIR ${CLIENT_DIR}/../bin/sdk)
add_custom_command(TARGET etherdb_client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SDK_DIR}/include
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SDK_DIR}/lib
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CLIENT_PUBLIC_HEADERS} ${SDK_DIR}/include
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:etherdb_client> ${SDK_DIR}/lib
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_LINKER_FILE:etherdb_client> ${SDK_DIR}/lib
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CLIENT_DIR}/README.md ${SDK_DIR}/README.md
    COMMENT "EtherDB SDK -> ${SDK_DIR}"
)
