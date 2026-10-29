cmake_minimum_required (VERSION 3.20)

set(THIRDPARTY_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)

set(CMAKE_CXX_STANDARD 17)
set(BUILD_SHARED_LIBS OFF)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(${THIRDPARTY_ROOT})
set(XENONANALYSE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/XenonAnalyse)
set(XENONUTILS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/XenonUtils)
set(XENONRECOMP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/XenonRecomp)

project ("XenonRecomp-ALL")

add_subdirectory(${XENONANALYSE_ROOT})
add_subdirectory(${XENONRECOMP_ROOT})
add_subdirectory(${XENONUTILS_ROOT})

# Only tests if this is the top level project
if (${CMAKE_CURRENT_SOURCE_DIR} STREQUAL ${CMAKE_SOURCE_DIR})
    add_subdirectory(XenonTests)
endif()

# Instruction-level tests driven by raw instruction words (no PPC toolchain
# needed). Registered with ctest; the test binary is built on demand by a ctest
# fixture, so it never affects the default build.
option(XENON_RECOMP_VMX_TESTS "Register the XenonTests/vmx suite with ctest" ON)
if (XENON_RECOMP_VMX_TESTS)
    enable_testing()
    add_subdirectory(XenonTests/vmx)
endif()
