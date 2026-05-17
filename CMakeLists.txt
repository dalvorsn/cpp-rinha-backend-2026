cmake_minimum_required(VERSION 3.20)
project(rinha_backend_2026 C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPT_FLAGS "-O3 -march=haswell -mtune=haswell -ffast-math -fomit-frame-pointer -flto")
set(CMAKE_CXX_FLAGS_RELEASE "${OPT_FLAGS}")
set(CMAKE_C_FLAGS_RELEASE   "${OPT_FLAGS}")
add_link_options(-flto)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DEBUG_LOG "Enable verbose debug logging" OFF)
if(DEBUG_LOG)
    add_compile_definitions(DEBUG)
    message(STATUS "Debug logging ENABLED")
endif()

include(FetchContent)

FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        v3.9.1
)
FetchContent_MakeAvailable(simdjson)

# ── converter: build the ball-tree index at image-build time ─────────────────
add_executable(converter src/converter.cpp)
target_link_libraries(converter PRIVATE simdjson::simdjson)
target_include_directories(converter PRIVATE src)

# ── lb: tiny SCM_RIGHTS load balancer (pure C) ───────────────────────────────
add_executable(lb src/lb.cpp)
set_target_properties(lb PROPERTIES LINKER_LANGUAGE C)

# ── api_server: epoll HTTP server with ball-tree KNN ─────────────────────────
add_executable(api_server src/server.cpp)
target_link_libraries(api_server PRIVATE simdjson::simdjson pthread)
target_include_directories(api_server PRIVATE src)
