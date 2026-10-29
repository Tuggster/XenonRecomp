cmake_minimum_required (VERSION 3.8)

project("XenonRecomp")

# The recompiler itself is a static library so other hosts (the XenonTests/vmx
# generator) can drive Recompiler without going through the CLI in main.cpp.
add_library(LibXenonRecomp STATIC
    "recompiler.cpp"
    "test_recompiler.cpp"
    "recompiler_config.cpp")

target_include_directories(LibXenonRecomp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_precompile_headers(LibXenonRecomp PUBLIC "pch.h")

target_link_libraries(LibXenonRecomp PUBLIC
    LibXenonAnalyse
    XenonUtils
    fmt::fmt
    tomlplusplus::tomlplusplus
    xxHash::xxhash)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(LibXenonRecomp PUBLIC -Wno-switch -Wno-unused-variable -Wno-null-arithmetic)

    # alias attribute not supported on Apple.
    if (NOT APPLE)
        target_compile_definitions(LibXenonRecomp PRIVATE XENON_RECOMP_USE_ALIAS)
    endif()
endif()

target_compile_definitions(LibXenonRecomp PUBLIC _CRT_SECURE_NO_WARNINGS)

add_executable(XenonRecomp "main.cpp")

target_link_libraries(XenonRecomp PRIVATE LibXenonRecomp)
