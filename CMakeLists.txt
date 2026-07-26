############
# Versions #
############
cmake_minimum_required(VERSION 3.5.1)
project(ert_diametercomm LANGUAGES CXX)

set(MAIN_PROJECT OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(MAIN_PROJECT ON)
endif()

#############
# Variables #
#############
set(ERT_DIAMETERCOMM_TARGET_NAME       ${PROJECT_NAME})
set(ERT_DIAMETERCOMM_INCLUDE_BUILD_DIR "${PROJECT_SOURCE_DIR}/include")
option(ERT_DIAMETERCOMM_BuildTests "Build the unit tests." ${MAIN_PROJECT})

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

###########
# Modules #
###########
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/")

# Build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
message(STATUS "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")

# Build directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build/${CMAKE_BUILD_TYPE}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build/${CMAKE_BUILD_TYPE}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build/${CMAKE_BUILD_TYPE}/lib)
message(STATUS "CMAKE_RUNTIME_OUTPUT_DIRECTORY is ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "CMAKE_LIBRARY_OUTPUT_DIRECTORY is ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
message(STATUS "CMAKE_ARCHIVE_OUTPUT_DIRECTORY is ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")

# Compiler flags
add_compile_options(-Wall -Wextra -Wpedantic)
add_compile_options(
  $<$<CONFIG:Debug>:-O0>
  $<$<CONFIG:Debug>:-g3>
  $<$<CONFIG:Debug>:--coverage>
)
link_libraries(
  $<$<CONFIG:Debug>:--coverage>
)

# CMAKE_PREFIX_PATH
if(NOT CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH /usr/local)
endif()

################
# Dependencies #
################
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)

###########
# Install #
###########
include(GNUInstallDirs)
message(STATUS "CMAKE_INSTALL_PREFIX = ${CMAKE_INSTALL_PREFIX}")

##################
# Subdirectories #
##################
add_subdirectory(src)
if(ERT_DIAMETERCOMM_BuildTests)
  add_subdirectory(ut)
endif()

install(
  DIRECTORY ${ERT_DIAMETERCOMM_INCLUDE_BUILD_DIR}/ert/diametercomm
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ert
)

###########
# DOXYGEN #
###########
find_package(Doxygen)
if(Doxygen_FOUND)
  add_custom_target(doc
    ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/docs/doxygen/Doxyfile
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/docs/doxygen
    COMMENT "Generating API documentation with Doxygen" VERBATIM
  )
endif()
