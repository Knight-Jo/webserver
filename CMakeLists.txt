#顶层Cmake，设置全局链接库以及添加子目录，和项目的头文件.h位置
#设置最低版本和项目名称
cmake_minimum_required(VERSION 3.0)
project(kama-webserver)

#设置全局的c++标准
set (CMAKE_CXX_STANDARD 11)
set (CMAKE_CXX_STANDARD_REQUIRED True)

#设置头文件目录，供所有子项目使用
include_directories(${CMAKE_SOURCE_DIR}/include)


# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)

add_compile_options(-g)

#设置全局链接库
set(LIBS
    pthread 
)

# Optional gperftools (CPU profiler)
find_path(GPERFTOOLS_INCLUDE_DIR gperftools/profiler.h)
find_library(GPERFTOOLS_LIB profiler)
if (GPERFTOOLS_INCLUDE_DIR AND GPERFTOOLS_LIB)
    include_directories(${GPERFTOOLS_INCLUDE_DIR})
    add_compile_definitions(USE_GPERFTOOLS)
    list(APPEND LIBS ${GPERFTOOLS_LIB})
else()
    message(STATUS "gperftools not found: CPU profiling disabled")
endif()

#添加子目录使用
add_subdirectory(src)
add_subdirectory(memory)
add_subdirectory(log)
add_subdirectory(benchmarks)
add_subdirectory(tests)
