# Benchmark and stress test executables

add_executable(log_benchmark log_benchmark.cc)
add_executable(log_stress log_stress.cc)
add_executable(log_sync_vs_async log_sync_vs_async.cc)
add_executable(http_benchmark http_benchmark.cc)

target_link_libraries(log_benchmark src_lib log_lib ${LIBS})
target_link_libraries(log_stress src_lib log_lib ${LIBS})
target_link_libraries(log_sync_vs_async src_lib log_lib ${LIBS})
target_link_libraries(http_benchmark ${LIBS})
