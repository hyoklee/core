project(hermes_shm)

#------------------------------------------------------------------------------
# Build Tests
#------------------------------------------------------------------------------

# Basic compression unit tests
add_executable(test_compress_exec
        ${TEST_MAIN}/main.cc
        test_init.cc
        test_compress.cc)

# Unified compression benchmark (main benchmark executable)
add_executable(benchmark_compression_unified_exec
        ${TEST_MAIN}/main.cc
        test_init.cc
        test_compression_benchmark.cc)

# Q-Table predictor test
add_executable(test_qtable_predictor
        test_qtable_predictor.cc)

# Compression contention benchmark (motivation section)
add_executable(benchmark_compression_contention_exec
        test_compression_contention.cc)

#------------------------------------------------------------------------------
# Link Dependencies
#------------------------------------------------------------------------------

add_dependencies(test_compress_exec hermes_shm_host)
target_link_libraries(test_compress_exec
        hermes_shm_host hshm::compress Catch2::Catch2)
target_compile_definitions(test_compress_exec
        PRIVATE HSHM_ENABLE_COMPRESS=1)

add_dependencies(benchmark_compression_unified_exec hermes_shm_host)

# Add conda library path to search paths if CONDA_PREFIX is set
if(DEFINED ENV{CONDA_PREFIX})
    list(APPEND CMAKE_PREFIX_PATH $ENV{CONDA_PREFIX})
    list(APPEND CMAKE_LIBRARY_PATH $ENV{CONDA_PREFIX}/lib)
endif()

# Find lossless compression libraries
find_library(ZSTD_LIBRARY NAMES zstd)
find_library(ZLIB_LIBRARY NAMES z)
find_library(LZMA_LIBRARY NAMES lzma)
find_library(LZ4_LIBRARY NAMES lz4)
find_library(BZIP2_LIBRARY NAMES bz2)
find_library(BROTLI_ENC_LIBRARY NAMES brotlienc)
find_library(BROTLI_DEC_LIBRARY NAMES brotlidec)
find_library(SNAPPY_LIBRARY NAMES snappy)
find_library(BLOSC2_LIBRARY NAMES blosc2)
find_library(LZO_LIBRARY NAMES lzo2)

set(LOSSLESS_LIBS "")
if(ZSTD_LIBRARY)
    list(APPEND LOSSLESS_LIBS ${ZSTD_LIBRARY})
endif()
if(ZLIB_LIBRARY)
    list(APPEND LOSSLESS_LIBS ${ZLIB_LIBRARY})
endif()
if(LZMA_LIBRARY)
    list(APPEND LOSSLESS_LIBS ${LZMA_LIBRARY})
endif()
if(LZ4_LIBRARY)
    list(APPEND LOSSLESS_LIBS ${LZ4_LIBRARY})
endif()
if(BZIP2_LIBRARY)
    list(APPEND LOSSLESS_LIBS ${BZIP2_LIBRARY})
endif()
if(BROTLI_ENC_LIBRARY AND BROTLI_DEC_LIBRARY)
    list(APPEND LOSSLESS_LIBS ${BROTLI_ENC_LIBRARY} ${BROTLI_DEC_LIBRARY})
endif()
if(SNAPPY_LIBRARY)
    list(APPEND LOSSLESS_LIBS ${SNAPPY_LIBRARY})
endif()
if(BLOSC2_LIBRARY)
    list(APPEND LOSSLESS_LIBS ${BLOSC2_LIBRARY})
endif()
if(LZO_LIBRARY)
    list(APPEND LOSSLESS_LIBS ${LZO_LIBRARY})
endif()

# Find lossy compression libraries
find_library(ZFP_LIBRARY NAMES zfp)
find_library(SZ3_LIBRARY NAMES SZ3c)
find_library(SZ_LIBRARY NAMES SZ)
find_library(FPZIP_LIBRARY NAMES fpzip)

set(LOSSY_LIBS "")
if(ZFP_LIBRARY)
    list(APPEND LOSSY_LIBS ${ZFP_LIBRARY})
    message(STATUS "Found ZFP library: ${ZFP_LIBRARY}")
endif()
if(SZ3_LIBRARY)
    list(APPEND LOSSY_LIBS ${SZ3_LIBRARY})
    message(STATUS "Found SZ3 library: ${SZ3_LIBRARY}")
endif()
if(SZ_LIBRARY)
    list(APPEND LOSSY_LIBS ${SZ_LIBRARY})
    message(STATUS "Found SZ library: ${SZ_LIBRARY}")
endif()
if(FPZIP_LIBRARY)
    list(APPEND LOSSY_LIBS ${FPZIP_LIBRARY})
    message(STATUS "Found FPZIP library: ${FPZIP_LIBRARY}")
endif()

target_link_libraries(benchmark_compression_unified_exec
        hermes_shm_host hshm::compress Catch2::Catch2 ${LOSSLESS_LIBS} ${LOSSY_LIBS})
target_compile_definitions(benchmark_compression_unified_exec
        PRIVATE HSHM_ENABLE_COMPRESS=1)

# Link Q-Table predictor test (requires nlohmann_json)
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND AND TARGET qtable_predictor)
    target_link_libraries(test_qtable_predictor
            qtable_predictor)
    set_property(TARGET test_qtable_predictor PROPERTY CXX_STANDARD 17)
    message(STATUS "Q-Table predictor test enabled")
else()
    message(STATUS "Q-Table predictor not available (requires nlohmann_json)")
    set_target_properties(test_qtable_predictor PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# Linear Regression Table predictor test (no external dependencies)
add_executable(test_linreg_table_predictor
        test_linreg_table_predictor.cc)
target_link_libraries(test_linreg_table_predictor
        linreg_table_predictor Catch2::Catch2WithMain)
set_property(TARGET test_linreg_table_predictor PROPERTY CXX_STANDARD 17)
message(STATUS "Linear Regression Table predictor test enabled")

# Distribution classifier test (mathematical approach, no ML)
add_executable(test_distribution_classifier
        test_distribution_classifier.cc)
target_link_libraries(test_distribution_classifier
        hermes_shm_host Catch2::Catch2WithMain)
set_property(TARGET test_distribution_classifier PROPERTY CXX_STANDARD 17)
message(STATUS "Distribution classifier test enabled")

# Link compression contention benchmark
add_dependencies(benchmark_compression_contention_exec hermes_shm_host)
target_link_libraries(benchmark_compression_contention_exec
        hermes_shm_host hshm::compress pthread)
target_compile_definitions(benchmark_compression_contention_exec
        PRIVATE HSHM_ENABLE_COMPRESS=1)

#------------------------------------------------------------------------------
# Tests
#------------------------------------------------------------------------------

add_test(NAME ctp_compress COMMAND
        ${CMAKE_BINARY_DIR}/bin/test_compress_exec "~[error=FatalError]")

add_test(NAME ctp_linreg_table_predictor COMMAND
        ${CMAKE_BINARY_DIR}/bin/test_linreg_table_predictor)

add_test(NAME ctp_distribution_classifier COMMAND
        ${CMAKE_BINARY_DIR}/bin/test_distribution_classifier)

#------------------------------------------------------------------------------
# Install Targets
#------------------------------------------------------------------------------

install(TARGETS
        test_compress_exec
        benchmark_compression_unified_exec
        benchmark_compression_contention_exec
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin)

#------------------------------------------------------------------------------
# Coverage
#------------------------------------------------------------------------------

if(HSHM_ENABLE_COVERAGE)
    set_coverage_flags(test_compress_exec)
endif()
