cmake_minimum_required(VERSION 3.10)
project(WEBSERVER)  #该指定隐式指定了两个变量<projectname>_BINARY_DIR SOLDIERFIRE_BINARY_DIR和SOLDIERFIRE_SOURCE_DIR
message(STATUS "This is BINARY dir" ${PROJECT_BINARY_DIR})  #用户自定义信息，STATUS信息前带--
message(STATUS "This is SOURCE dir" ${PROJECT_SOURCE_DIR})

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -g -pthread") #追加其他选项
set(CMAKE_BUILD_TYPE Debug)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
include_directories(include)
add_subdirectory(src)