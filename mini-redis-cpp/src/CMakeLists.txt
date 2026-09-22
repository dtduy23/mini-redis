find_package(Threads REQUIRED)

# ── Library chứa toàn bộ logic (để tests có thể link vào) ──────────────────
add_library(mini_redis_lib STATIC
    server/listener.cpp
    server/event_loop.cpp
    server/connection.cpp
    protocol/resp_parser.cpp
    protocol/resp_serializer.cpp
    store/data_store.cpp
    store/expiry.cpp
    commands/dispatcher.cpp
    commands/handlers.cpp
    config/config.cpp
    store/rdb.cpp
    server/bio.cpp
)

target_link_libraries(mini_redis_lib PUBLIC Threads::Threads)

target_include_directories(mini_redis_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ── Executable — chỉ chứa main() ───────────────────────────────────────────
add_executable(mini_redis_cpp main.cpp)

target_link_libraries(mini_redis_cpp PRIVATE mini_redis_lib)

target_include_directories(mini_redis_cpp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ── Benchmark Executable ──────────────────────────────────────────────────────
add_executable(mini_redis_benchmark benchmark/benchmark.cpp)

target_link_libraries(mini_redis_benchmark PRIVATE Threads::Threads)

