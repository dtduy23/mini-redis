add_executable(test_resp_parser   test_resp_parser.cpp)
add_executable(test_data_store    test_data_store.cpp)
add_executable(test_expiry        test_expiry.cpp)
add_executable(test_config        test_config.cpp)
add_executable(test_rdb           test_rdb.cpp)
add_executable(test_bio           test_bio.cpp)

foreach(t test_resp_parser test_data_store test_expiry test_config test_rdb test_bio)
    target_link_libraries(${t} PRIVATE mini_redis_lib)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_server_regressions
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_server_regressions.py
                --server $<TARGET_FILE:mini_redis_cpp>)
    set_tests_properties(test_server_regressions PROPERTIES TIMEOUT 60)
endif()
