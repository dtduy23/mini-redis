cmake_minimum_required(VERSION 3.16)
project(mini_redis_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror)

add_subdirectory(src)
add_subdirectory(tests)
