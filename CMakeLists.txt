cmake_minimum_required(VERSION 3.20)
project(iowarp-core VERSION 1.0.0 LANGUAGES C CXX)

#------------------------------------------------------------------------------
# Project Options
#------------------------------------------------------------------------------
option(WRP_CORE_ENABLE_RUNTIME "Enable runtime component" ON)
option(WRP_CORE_ENABLE_CTE "Enable context-transfer-engine component" ON)
option(WRP_CORE_ENABLE_CAE "Enable context-assimilation-engine component" ON)
option(WRP_CORE_ENABLE_CEE "Enable context-exploration-engine component" ON)
option(IOWARP_ENABLE_TESTS "Enable testing for all components" ON)
option(IOWARP_ENABLE_CMAKE_DOTENV "Enable reading environment variables from .env.cmake" ON)
option(IOWARP_ENABLE_ASAN "Enable AddressSanitizer for debugging" OFF)

# Read environment variables from .env.cmake if enabled
if(IOWARP_ENABLE_CMAKE_DOTENV)
  if(EXISTS "${CMAKE_SOURCE_DIR}/.env.cmake")
    include("${CMAKE_SOURCE_DIR}/.env.cmake")
  endif()
endif()

#------------------------------------------------------------------------------
# Global Settings
#------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Enable RPATH
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")

# Platform-specific RPATH configuration
if(APPLE)
    set(CMAKE_MACOSX_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH "@loader_path/../lib:@loader_path:${CMAKE_INSTALL_PREFIX}/lib")
elseif(UNIX)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib:$ORIGIN:${CMAKE_INSTALL_PREFIX}/lib")
endif()

#------------------------------------------------------------------------------
# Compiler Flags
#------------------------------------------------------------------------------
# Add compiler flags following Google C++ style guide
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
# Disable some problematic warnings for external dependencies
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-parameter -Wno-unused-variable")

# Debug configuration
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0 -DDEBUG")

# Release configuration
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")

# AddressSanitizer configuration
if(IOWARP_ENABLE_ASAN)
    message(STATUS "AddressSanitizer (ASAN) enabled for iowarp-core")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

#------------------------------------------------------------------------------
# Binary Output Directories
#------------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# For multi-config generators (Visual Studio, Xcode)
foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/bin)
endforeach()

#------------------------------------------------------------------------------
# Testing
#------------------------------------------------------------------------------
if(IOWARP_ENABLE_TESTS)
    enable_testing()
    include(CTest)
endif()

#------------------------------------------------------------------------------
# Component Subdirectories
#------------------------------------------------------------------------------

# Context Transport Primitives (formerly cte-hermes-shm) - Required by runtime
message(STATUS "Building context-transport-primitives component")
add_subdirectory(context-transport-primitives)

# Runtime component
if(WRP_CORE_ENABLE_RUNTIME)
    message(STATUS "Building runtime component")
    add_subdirectory(runtime)
else()
    message(STATUS "Runtime component disabled")
endif()

# Context Transfer Engine
if(WRP_CORE_ENABLE_CTE)
    message(STATUS "Building context-transfer-engine component")
    add_subdirectory(context-transfer-engine)
else()
    message(STATUS "Context-transfer-engine component disabled")
endif()

# Context Assimilation Engine
if(WRP_CORE_ENABLE_CAE)
    message(STATUS "Building context-assimilation-engine component")
    add_subdirectory(context-assimilation-engine)
else()
    message(STATUS "Context-assimilation-engine component disabled")
endif()

# Context Exploration Engine
if(WRP_CORE_ENABLE_CEE)
    message(STATUS "Building context-exploration-engine component")
    add_subdirectory(context-exploration-engine)
else()
    message(STATUS "Context-exploration-engine component disabled")
endif()

#------------------------------------------------------------------------------
# Summary
#------------------------------------------------------------------------------
message(STATUS "")
message(STATUS "IOWarp Core Configuration Summary:")
message(STATUS "  Runtime:                      ${WRP_CORE_ENABLE_RUNTIME}")
message(STATUS "  Context Transfer Engine:      ${WRP_CORE_ENABLE_CTE}")
message(STATUS "  Context Assimilation Engine:  ${WRP_CORE_ENABLE_CAE}")
message(STATUS "  Context Exploration Engine:   ${WRP_CORE_ENABLE_CEE}")
message(STATUS "  Enable Tests:                 ${IOWARP_ENABLE_TESTS}")
message(STATUS "  AddressSanitizer:             ${IOWARP_ENABLE_ASAN}")
message(STATUS "")
