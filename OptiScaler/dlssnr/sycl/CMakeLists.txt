cmake_minimum_required(VERSION 3.25)

project(arc_nr_sycl_provider LANGUAGES CXX)

set(SNR_RUNTIME_DIR "" CACHE PATH "Path to the SNR/runtime source directory")
if(NOT EXISTS "${SNR_RUNTIME_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set SNR_RUNTIME_DIR to the SNR/runtime source directory")
endif()

add_subdirectory("${SNR_RUNTIME_DIR}" "${CMAKE_BINARY_DIR}/snr-runtime")
set_target_properties(arc_nr_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(arc_nr_sycl SHARED ArcNrSyclProvider.cpp)
target_compile_features(arc_nr_sycl PRIVATE cxx_std_20)
target_include_directories(arc_nr_sycl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(arc_nr_sycl PRIVATE arc_nr_runtime)
set_target_properties(arc_nr_sycl PROPERTIES OUTPUT_NAME "arc_nr_sycl")

add_executable(arc_nr_sycl_provider_smoke ArcNrSyclProviderSmoke.cpp)
target_compile_features(arc_nr_sycl_provider_smoke PRIVATE cxx_std_20)
target_link_libraries(arc_nr_sycl_provider_smoke PRIVATE arc_nr_sycl)
