cmake_minimum_required(VERSION 3.25)

project(arc_nr_sycl_provider LANGUAGES CXX)

set(SNR_RUNTIME_DIR "" CACHE PATH "Path to the SNR/runtime source directory")
if(NOT EXISTS "${SNR_RUNTIME_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set SNR_RUNTIME_DIR to the SNR/runtime source directory")
endif()

add_subdirectory("${SNR_RUNTIME_DIR}" "${CMAKE_BINARY_DIR}/snr-runtime")
set_target_properties(arc_nr_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
option(ARC_NR_SYCL_FAST_DEFAULT
       "Enable measured approximate Arc SNR fast paths, including 720p alignment" OFF)
option(ARC_NR_SYCL_EXACT_FUSED_FFN_DEFAULT
       "Fuse full FFN arithmetic without enabling approximate model paths" OFF)
if(ARC_NR_SYCL_FAST_DEFAULT AND ARC_NR_SYCL_EXACT_FUSED_FFN_DEFAULT)
    message(FATAL_ERROR "Choose one Arc SNR default profile")
endif()
if(ARC_NR_SYCL_FAST_DEFAULT)
    target_compile_definitions(arc_nr_runtime PRIVATE SNR_GAME_FAST_DEFAULT=1)
elseif(ARC_NR_SYCL_EXACT_FUSED_FFN_DEFAULT)
    target_compile_definitions(arc_nr_runtime PRIVATE SNR_EXACT_FUSED_FFN_DEFAULT=1)
endif()

add_library(arc_nr_sycl SHARED ArcNrSyclProvider.cpp)
target_compile_features(arc_nr_sycl PRIVATE cxx_std_20)
target_include_directories(arc_nr_sycl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(arc_nr_sycl PRIVATE arc_nr_runtime)
set_target_properties(arc_nr_sycl PROPERTIES OUTPUT_NAME "arc_nr_sycl")

add_executable(arc_nr_sycl_provider_smoke ArcNrSyclProviderSmoke.cpp)
target_compile_features(arc_nr_sycl_provider_smoke PRIVATE cxx_std_20)
target_link_libraries(arc_nr_sycl_provider_smoke PRIVATE arc_nr_sycl)

if(WIN32)
    add_executable(arc_nr_sycl_provider_gpu_smoke ArcNrSyclProviderGpuSmoke.cpp)
    target_compile_features(arc_nr_sycl_provider_gpu_smoke PRIVATE cxx_std_20)
    target_link_libraries(arc_nr_sycl_provider_gpu_smoke PRIVATE d3d12 dxgi)
endif()
