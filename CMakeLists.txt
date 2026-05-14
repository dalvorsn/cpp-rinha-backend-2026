cmake_minimum_required(VERSION 3.20)
project(rinha_backend_2026 C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMMON_OPTIMIZATIONS "-O3 -march=haswell -mtune=haswell -ffast-math -fomit-frame-pointer -flto")
set(CMAKE_CXX_FLAGS_RELEASE "${COMMON_OPTIMIZATIONS}")
set(CMAKE_C_FLAGS_RELEASE "${COMMON_OPTIMIZATIONS}")

add_link_options(-flto)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v3.9.1
)
FetchContent_MakeAvailable(simdjson)

set(UWS_WITH_PROXY OFF CACHE BOOL "" FORCE)
set(UWS_WITH_LIBUV OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    uWebSockets
    GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
    GIT_TAG v20.48.0
)
FetchContent_MakeAvailable(uWebSockets)

set(UWS_INCLUDE_DIRS 
    ${uwebsockets_SOURCE_DIR}/src 
    ${uwebsockets_SOURCE_DIR}/uSockets/src
)

add_executable(converter src/converter.cpp)
target_link_libraries(converter PRIVATE simdjson::simdjson)

file(GLOB USOCKETS_SOURCES 
    "${uwebsockets_SOURCE_DIR}/uSockets/src/*.c"
    "${uwebsockets_SOURCE_DIR}/uSockets/src/eventing/*.c"
)

add_executable(api_server src/server.cpp ${USOCKETS_SOURCES})
target_compile_definitions(api_server PRIVATE LIBUS_USE_EPOLL LIBUS_NO_SSL UWS_NO_ZLIB UWS_NO_SSL)
target_include_directories(api_server PRIVATE ${UWS_INCLUDE_DIRS})
target_link_libraries(api_server PRIVATE simdjson::simdjson pthread uv)

add_executable(lb src/lb.cpp)
target_include_directories(lb PRIVATE ${UWS_INCLUDE_DIRS})
target_link_libraries(lb PRIVATE uv pthread simdjson::simdjson)