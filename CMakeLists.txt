cmake_minimum_required(VERSION 3.10)
project(telegramshell C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")

# Vendored JSON parser
set(YYJSON_DIR ${CMAKE_SOURCE_DIR}/vendor/yyjson)
include_directories(${CMAKE_SOURCE_DIR}/src ${YYJSON_DIR})
find_path(CURL_INCLUDE_DIR NAMES curl/curl.h PATHS /usr/include/x86_64-linux-gnu)
if(CURL_INCLUDE_DIR)
    include_directories(${CURL_INCLUDE_DIR})
endif()

add_executable(telegramshell
    src/main.c
    src/curl_uv.c
    src/telegram.c
    src/shell.c
    src/bot.c
    ${YYJSON_DIR}/yyjson.c
)

find_library(LIBUV uv)
find_library(LIBCURL curl)
find_library(LIBUTIL util)

target_link_libraries(telegramshell ${LIBUV} ${LIBCURL} ${LIBUTIL})
