cmake_minimum_required(VERSION 3.20)
project(rinha_backend_2026 C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPT_FLAGS "-Ofast -DNDEBUG -march=haswell -mtune=haswell -fomit-frame-pointer -flto -fno-rtti -fno-plt")
set(CMAKE_CXX_FLAGS_RELEASE "${OPT_FLAGS}")
set(CMAKE_C_FLAGS_RELEASE   "${OPT_FLAGS}")
add_link_options(-flto)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


option(ENABLE_APM "Enable APM latency histograms and metrics endpoint" OFF)
if(ENABLE_APM)
    add_compile_definitions(ENABLE_APM)
    message(STATUS "APM ENABLED")
else()
    message(STATUS "APM DISABLED")
endif()

# ── converter: build the IVF index at image-build time ───────────────────────
add_executable(converter src/converter.cpp)
target_include_directories(converter PRIVATE src)
target_compile_options(converter PRIVATE -fno-exceptions)

# ── lb: tiny SCM_RIGHTS load balancer (pure C) ───────────────────────────────
add_executable(lb src/lb.cpp)
set_target_properties(lb PROPERTIES LINKER_LANGUAGE C)
target_compile_options(lb PRIVATE -fno-exceptions)

# ── debug_query: brute-force exact search for a hardcoded query ─────────────
add_executable(debug_query src/debug_query.cpp)
target_include_directories(debug_query PRIVATE src)

# ── api_server: epoll HTTP server with IVF search ────────────────────────────
add_executable(api_server src/server.cpp)
target_link_libraries(api_server PRIVATE pthread)
target_include_directories(api_server PRIVATE src)
target_compile_options(api_server PRIVATE -fno-exceptions)
