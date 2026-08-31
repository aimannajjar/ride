cmake_minimum_required(VERSION 4.1)
project(ride 
  LANGUAGES C CXX
  VERSION 0.0.1
  DESCRIPTION "RIDES - Real-time Intrusion Detection Events System"
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -Wall -Werror")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -g -DUSERSPACE_DEBUG")

# use official blake3 implementation
include(FetchContent)
FetchContent_Declare(
  blake3
  GIT_REPOSITORY https://github.com/BLAKE3-team/BLAKE3
  GIT_TAG        1.8.7
  SOURCE_SUBDIR  c

)
FetchContent_MakeAvailable(blake3)


file(GLOB_RECURSE SOURCE_FILES 
    src/main.c
    src/ride.c
    src/queue.c
    src/worker.c
    src/hasher.c
)

add_executable(ride ${SOURCE_FILES})
target_include_directories(ride PRIVATE ${CMAKE_SOURCE_DIR}/build)
target_link_libraries(ride PRIVATE jemalloc uring blake3 bpf)

