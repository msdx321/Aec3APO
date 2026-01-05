cmake_minimum_required(VERSION 3.20)
project(speexdsp C)

get_filename_component(SPEEXDSP_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/speexdsp"
    ABSOLUTE)

set(SPEEXDSP_SOURCES
    "${SPEEXDSP_ROOT}/libspeexdsp/buffer.c"
    "${SPEEXDSP_ROOT}/libspeexdsp/fftwrap.c"
    "${SPEEXDSP_ROOT}/libspeexdsp/filterbank.c"
    "${SPEEXDSP_ROOT}/libspeexdsp/jitter.c"
    "${SPEEXDSP_ROOT}/libspeexdsp/kiss_fft.c"
    "${SPEEXDSP_ROOT}/libspeexdsp/kiss_fftr.c"
    "${SPEEXDSP_ROOT}/libspeexdsp/mdf.c"
    "${SPEEXDSP_ROOT}/libspeexdsp/preprocess.c"
    "${SPEEXDSP_ROOT}/libspeexdsp/resample.c"
    "${SPEEXDSP_ROOT}/libspeexdsp/scal.c"
    "${SPEEXDSP_ROOT}/libspeexdsp/smallft.c"
)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include/speex")
set(INCLUDE_STDINT "#include <stdint.h>")
set(SIZE16 "int16_t")
set(USIZE16 "uint16_t")
set(SIZE32 "int32_t")
set(USIZE32 "uint32_t")
configure_file(
    "${SPEEXDSP_ROOT}/include/speex/speexdsp_config_types.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/speex/speexdsp_config_types.h"
    @ONLY)

add_library(speexdsp STATIC ${SPEEXDSP_SOURCES})
target_include_directories(speexdsp
    PUBLIC
        "${SPEEXDSP_ROOT}/include"
        "${SPEEXDSP_ROOT}/libspeexdsp"
        "${CMAKE_CURRENT_BINARY_DIR}/include")
target_compile_definitions(speexdsp
    PRIVATE
        FLOATING_POINT
        USE_KISS_FFT
        EXPORT=)

if (CMAKE_GENERATOR_PLATFORM MATCHES "Win32|x64")
    target_compile_definitions(speexdsp
        PRIVATE
            USE_SSE
            USE_SSE2)
endif ()
