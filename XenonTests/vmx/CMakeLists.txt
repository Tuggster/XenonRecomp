project("XenonVmxTests")

# Instruction-level tests fed by raw instruction words. See README.md.
#
# Nothing here is part of the default build: XenonTestGen and XenonVmxTests are
# EXCLUDE_FROM_ALL and get built by the `vmx.build` ctest fixture, so a broken
# test never breaks the normal XenonRecomp/XenonAnalyse build.

include(ProcessorCount)
ProcessorCount(XENON_VMX_JOBS)
if (XENON_VMX_JOBS EQUAL 0)
    set(XENON_VMX_JOBS 4)
endif()

# Generator: drives Recompiler on each case file and writes one C++ file per group.
add_executable(XenonTestGen EXCLUDE_FROM_ALL gen/XenonTestGen.cpp)
target_link_libraries(XenonTestGen PRIVATE LibXenonRecomp)

# Every cases/*.toml is a group. CONFIGURE_DEPENDS re-globs on each build, so a
# new file is picked up by `cmake --build` with no edits here.
file(GLOB XENON_VMX_CASE_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cases/*.toml)

set(XENON_VMX_GENERATED)
set(XENON_VMX_GROUPS)
foreach(case_file IN LISTS XENON_VMX_CASE_FILES)
    get_filename_component(group ${case_file} NAME_WE)
    set(gen ${CMAKE_CURRENT_BINARY_DIR}/gen/${group}.cpp)
    add_custom_command(OUTPUT ${gen}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
        COMMAND XenonTestGen ${case_file} ${gen}
        DEPENDS XenonTestGen ${case_file}
        COMMENT "XenonTestGen ${group}.toml"
        VERBATIM)
    list(APPEND XENON_VMX_GENERATED ${gen})
    list(APPEND XENON_VMX_GROUPS ${group})
endforeach()

add_executable(XenonVmxTests EXCLUDE_FROM_ALL
    harness/main.cpp
    harness/harness.cpp
    ${XENON_VMX_GENERATED})

target_include_directories(XenonVmxTests PRIVATE harness)
target_link_libraries(XenonVmxTests PRIVATE XenonUtils fmt::fmt)
target_compile_options(XenonVmxTests PRIVATE
    -Wno-unused-label
    -Wno-unused-variable
    -Wno-unused-but-set-variable)

# The generated code is the same SSE-via-simde code the game gets. On x86 match
# upstream XenonTests' baseline; on arm64 simde lowers to NEON with no flags.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(XenonVmxTests PRIVATE -march=sandybridge)
endif()

# ctest: `vmx.build` builds the suite (fixture), `vmx.all` runs every group in
# one go, `vmx.<group>` runs one group.
#
# vmx.all exists because ctest reads the test list written at configure time:
# a cases/*.toml added since then is compiled in by vmx.build (the re-glob
# reconfigures) and runs under vmx.all right away, but only gets its own
# vmx.<group> entry on the next ctest invocation.
add_test(NAME vmx.build
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG>
            --target XenonVmxTests --parallel ${XENON_VMX_JOBS})
set_tests_properties(vmx.build PROPERTIES FIXTURES_SETUP vmx_build)

add_test(NAME vmx.all COMMAND XenonVmxTests)
set_tests_properties(vmx.all PROPERTIES FIXTURES_REQUIRED vmx_build)

foreach(group IN LISTS XENON_VMX_GROUPS)
    add_test(NAME vmx.${group} COMMAND XenonVmxTests --group ${group})
    set_tests_properties(vmx.${group} PROPERTIES FIXTURES_REQUIRED vmx_build)
endforeach()
