# Benchmark directory CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

# Create wrp_cte_bench executable
add_executable(wrp_cte_bench
  wrp_cte_bench.cc
)

# Link required libraries (MPI no longer required - uses worker threads instead)
target_link_libraries(wrp_cte_bench
  wrp_cte::core_client
  chimaera::cxx
)

# Set C++ standard
target_compile_features(wrp_cte_bench PRIVATE cxx_std_17)

# Install the benchmark executable
install(TARGETS wrp_cte_bench
  RUNTIME DESTINATION bin
)

# ------------------------------------------------------------------------------
# Compression Benchmark (conditional on WRP_CORE_ENABLE_COMPRESS)
# ------------------------------------------------------------------------------
if(WRP_CORE_ENABLE_COMPRESS)
    message(STATUS "CTE Benchmark: Compression benchmark ENABLED")

    # Create wrp_cte_compress_bench executable
    add_executable(wrp_cte_compress_bench
        wrp_cte_compress_bench.cc
    )

    # Add compression compile definition
    target_compile_definitions(wrp_cte_compress_bench PRIVATE WRP_CORE_ENABLE_COMPRESS)

    # Link required libraries
    target_link_libraries(wrp_cte_compress_bench
        wrp_cte::core_client          # CTE core client library
        wrp_cte::core_runtime         # CTE core runtime (for compression context)
        chimaera::cxx                 # Core Chimaera library
        hshm::compress                # Compression library
        ${CMAKE_THREAD_LIBS_INIT}     # Threading support (for pthread_setaffinity_np)
    )

    # Set C++ standard
    target_compile_features(wrp_cte_compress_bench PRIVATE cxx_std_17)

    # Install the compression benchmark executable
    install(TARGETS wrp_cte_compress_bench
        RUNTIME DESTINATION bin
    )
else()
    message(STATUS "CTE Benchmark: Compression benchmark DISABLED (WRP_CORE_ENABLE_COMPRESS=OFF)")
endif()
