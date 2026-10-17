cmake_minimum_required(VERSION 3.20)
project(kvstore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")

option(KV_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(KV_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(kvcore STATIC
  src/aof.cpp
  src/commands.cpp
  src/log.cpp
  src/resp.cpp
  src/server.cpp
  src/store.cpp
)
target_include_directories(kvcore PUBLIC src)
target_compile_options(kvcore PUBLIC -Wall -Wextra -Wpedantic)

add_executable(kvstore src/main.cpp)
target_link_libraries(kvstore PRIVATE kvcore)

enable_testing()
foreach(t test_resp test_store test_commands)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE kvcore)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
