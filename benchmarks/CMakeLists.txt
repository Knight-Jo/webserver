# Benchmark and stress test executables

add_executable(log_benchmark log_benchmark.cc)
add_executable(log_stress log_stress.cc)

target_link_libraries(log_benchmark src_lib log_lib ${LIBS})
target_link_libraries(log_stress src_lib log_lib ${LIBS})
