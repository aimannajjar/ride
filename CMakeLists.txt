cmake_minimum_required(VERSION 4.1)
project(ride 
  LANGUAGES C CXX
  VERSION 0.0.1
  DESCRIPTION "RIDE - Real-time Intrusion Detection Events"
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -Wall -Werror")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Wall -Werror")

option(DEBUG_QUIET "Make a debug build without logging")
if (DEBUG_QUIET)
  set(_log_level LOG_INFO)
else()
  set(_log_level LOG_DEBUG)
endif()
add_library(build_flags INTERFACE)

target_compile_options(build_flags INTERFACE 
  -Wall -Werror
  $<$<CONFIG:Debug>:-Og -g>
)

target_compile_definitions(build_flags INTERFACE
  $<$<CONFIG:Debug>:DEBUG;${_log_level}>
)

include(FetchContent)

# blake3
FetchContent_Declare(
  blake3
  GIT_REPOSITORY https://github.com/BLAKE3-team/BLAKE3
  GIT_TAG        1.8.7
  SOURCE_SUBDIR  c

)
FetchContent_MakeAvailable(blake3)

# aws-lc (TLS for ngtcp2)
FetchContent_Declare(
  aws-lc
  GIT_REPOSITORY https://github.com/aws/aws-lc
  GIT_TAG        v1.61.4
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(aws-lc)

# ngtcp2
set(HAVE_SSL_SET_QUIC_EARLY_DATA_CONTEXT TRUE CACHE INTERNAL "" FORCE)
set(BORINGSSL_SSL_LIBRARY     ssl    CACHE STRING "" FORCE)
set(BORINGSSL_CRYPTO_LIBRARY  crypto CACHE STRING "" FORCE)
set(ENABLE_BORINGSSL          ON     CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL            OFF    CACHE BOOL "" FORCE)
set(ENABLE_SHARED_LIB         OFF    CACHE BOOL "" FORCE)
set(ENABLE_STATIC_LIB         ON     CACHE BOOL "" FORCE)

set(BORINGSSL_INCLUDE_DIR  "${aws-lc_SOURCE_DIR}/include")
set(BORINGSSL_INCLUDE_DIRS "${aws-lc_SOURCE_DIR}/include")

if (TARGET jitterentropy)
  target_compile_options(jitterentropy PRIVATE -Wno-error=cpp)
endif()

foreach(tgt crypto_objects ssl crypto jitterentropy) 
  # getting false positives, see https://github.com/aws/aws-lc/pull/191
  target_compile_options(${tgt} PRIVATE -Wno-error=stringop-overflow)
endforeach()

if (TARGET ngtcp2_static)
  target_compile_options(ngtcp2_static PRIVATE -Wno-error=inline)
endif()

FetchContent_Declare(
  ngtcp2
  GIT_REPOSITORY https://github.com/ngtcp2/ngtcp2
  GIT_TAG        main
)
FetchContent_MakeAvailable(ngtcp2)

## Enable static analyzer
option(ENABLE_FANALYZER "Enable -fanalyzer")
if (ENABLE_FANALYZER)
  add_compile_options(-fanalyzer)

  # cppcheck
  find_program(CPPCHECK cppcheck)
  set(CMAKE_C_CPPCHECK ${CPPCHECK}
    --enable=all
    --inconclusive
    --quiet
    -i ${CMAKE_BINARY_DIR} # skip auto-generated ride.skel.h
    --suppress=missingIncludeSystem
  )
endif()

set(SOURCE_FILES 
    src/main.c
    src/ride.c
    src/queue.c
    src/hasher.c
    src/processor.c
    src/ev.c
    src/verifier.c
)

add_executable(ride ${SOURCE_FILES})
target_include_directories(ride PRIVATE ${CMAKE_SOURCE_DIR}/build)
target_link_libraries(ride PRIVATE build_flags jemalloc uring blake3 bpf ngtcp2_static ngtcp2_crypto_boringssl_static ssl crypto)

