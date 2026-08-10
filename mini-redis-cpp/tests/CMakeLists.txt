add_executable(test_resp_parser   test_resp_parser.cpp)
add_executable(test_data_store    test_data_store.cpp)
add_executable(test_expiry        test_expiry.cpp)

foreach(t test_resp_parser test_data_store test_expiry)
    target_link_libraries(${t} PRIVATE mini_redis_lib)
endforeach()
