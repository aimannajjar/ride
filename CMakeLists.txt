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

# Pull libquiche via corrosion
FetchContent_Declare(
  corrosion
  GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
  GIT_TAG v0.6.1
)
FetchContent_MakeAvailable(corrosion)

FetchContent_Declare(
  quiche
  GIT_REPOSITORY https://github.com/cloudflare/quiche.git
  GIT_TAG 0.29.3
)
FetchContent_MakeAvailable(quiche)

corrosion_import_crate(MANIFEST_PATH ${quiche_SOURCE_DIR}/Cargo.toml
  FEATURES ffi pkg-config-meta
  CRATES quiche)


file(GLOB_RECURSE SOURCE_FILES 
    src/main.c
    src/ride.c
    src/queue.c
    src/worker.c
    src/hasher.c
    src/client.c
)

add_executable(ride ${SOURCE_FILES})
target_include_directories(ride PRIVATE ${CMAKE_SOURCE_DIR}/build
  ${quiche_SOURCE_DIR}/quiche/include)
target_link_libraries(ride PRIVATE jemalloc uring blake3 bpf quiche)

