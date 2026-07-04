cmake_minimum_required(VERSION 3.15)
project(ql-atmoforge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(ql-atmoforge
  src/main.cpp
  src/util.cpp
  src/config.cpp
  src/sampler.cpp
  src/params.cpp
  src/tape5.cpp
  src/subprocess.cpp
  src/parse.cpp
  src/shard.cpp
  src/npy.cpp
  src/datalink.cpp
  src/worker.cpp
)

target_include_directories(ql-atmoforge PRIVATE src third_party)

find_package(Threads REQUIRED)
target_link_libraries(ql-atmoforge PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(ql-atmoforge PRIVATE /W3 /utf-8)
  target_compile_definitions(ql-atmoforge PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(ql-atmoforge PRIVATE -Wall -Wextra)
endif()
