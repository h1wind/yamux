# CMakeLists.txt

cmake_minimum_required(VERSION 3.15.0)
project(yamux VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic -Werror -Wwrite-strings")

configure_file(
    config.h.in
    config.h
)

include_directories(
    ${PROJECT_BINARY_DIR}
    ${PROJECT_SOURCE_DIR}
)

set(sources
    bio.h
    bio.c
    debug.h
    debug.c
    event.h
    misc.h
    misc.c
    mux.h
    mux.c
    queue.h
    tree.h
)

if(LINUX)
    list(APPEND sources event-epoll.c)
endif()

add_library(yamux STATIC ${sources})

set(tests_srcs
    tests/main.c
)

set(tests_libs
    pthread
    yamux
)

add_executable(test ${tests_srcs})
target_link_libraries(test PRIVATE ${tests_libs})
