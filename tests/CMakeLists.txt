add_executable(http_tests http_tests.cc)

target_link_libraries(http_tests src_lib memory_lib log_lib ${LIBS})
