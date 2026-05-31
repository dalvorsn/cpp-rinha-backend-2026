cmake_minimum_required(VERSION 3.20)
project(rinha_backend_2026 C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPT_FLAGS "-Ofast -DNDEBUG -march=haswell -mtune=haswell -fomit-frame-pointer -flto -fno-rtti")
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

find_package(OpenMP REQUIRED)

# ── converter: build the IVF index at image-build time ───────────────────────
add_executable(converter src/converter.cpp)
target_include_directories(converter PRIVATE src)
target_link_libraries(converter PRIVATE OpenMP::OpenMP_CXX)

# ── lb: tiny SCM_RIGHTS load balancer (pure C) ───────────────────────────────
add_executable(lb src/lb.cpp)
set_target_properties(lb PROPERTIES LINKER_LANGUAGE C)
target_compile_options(lb PRIVATE -fno-exceptions)
target_link_libraries(lb PRIVATE pthread)
target_include_directories(lb PRIVATE src)

# ── api_server: epoll HTTP server with IVF search ────────────────────────────
add_executable(api_server src/server.cpp)
target_link_libraries(api_server PRIVATE pthread)
target_include_directories(api_server PRIVATE src)

# ── bench: offline IVF benchmark across configs ───────────────────────────────
add_executable(bench src/bench.cpp)
target_include_directories(bench PRIVATE src)
