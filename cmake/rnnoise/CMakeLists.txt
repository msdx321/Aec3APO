cmake_minimum_required(VERSION 3.20)
project(rnnoise C)

get_filename_component(RNNOISE_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/rnnoise"
    ABSOLUTE)
get_filename_component(RNNOISE_MODEL_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../rnnoise_model"
    ABSOLUTE)

set(RNNOISE_SOURCES
    "${RNNOISE_ROOT}/src/denoise.c"
    "${RNNOISE_ROOT}/src/rnn.c"
    "${RNNOISE_ROOT}/src/pitch.c"
    "${RNNOISE_ROOT}/src/kiss_fft.c"
    "${RNNOISE_ROOT}/src/celt_lpc.c"
    "${RNNOISE_ROOT}/src/nnet.c"
    "${RNNOISE_ROOT}/src/nnet_default.c"
    "${RNNOISE_ROOT}/src/parse_lpcnet_weights.c"
    "${RNNOISE_MODEL_ROOT}/rnnoise_data.c"
    "${RNNOISE_ROOT}/src/rnnoise_tables.c"
)

add_library(rnnoise STATIC ${RNNOISE_SOURCES})
target_include_directories(rnnoise
    PUBLIC
        "${RNNOISE_ROOT}/include"
        "${RNNOISE_ROOT}/src"
        "${RNNOISE_MODEL_ROOT}")
target_compile_definitions(rnnoise
    PRIVATE
        RNNOISE_BUILD)

if (CMAKE_GENERATOR_PLATFORM MATCHES "Win32|x64")
    target_sources(rnnoise
        PRIVATE
            "${RNNOISE_ROOT}/src/x86/x86_dnn_map.c"
            "${RNNOISE_ROOT}/src/x86/x86cpu.c"
            "${RNNOISE_ROOT}/src/x86/nnet_sse4_1.c"
            "${RNNOISE_ROOT}/src/x86/nnet_avx2.c")
    target_compile_definitions(rnnoise
        PRIVATE
            RNN_ENABLE_X86_RTCD
            OPUS_X86_MAY_HAVE_SSE2
            OPUS_X86_MAY_HAVE_AVX2)
    set_source_files_properties(
        "${RNNOISE_ROOT}/src/x86/nnet_sse4_1.c"
        PROPERTIES
            COMPILE_OPTIONS "/arch:AVX"
            COMPILE_DEFINITIONS "__SSE4_1__")
    set_source_files_properties(
        "${RNNOISE_ROOT}/src/x86/nnet_avx2.c"
        PROPERTIES
            COMPILE_OPTIONS "/arch:AVX2")
endif ()
