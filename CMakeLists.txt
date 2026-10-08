cmake_minimum_required(VERSION 3.24)

project(mhw_cpuopt VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "64-bit Windows only")
endif()

# Build identity for the start event. Configure-time: reconfigure after
# committing (tools/build.py does) so the commit is current.
find_package(Git QUIET)
set(CPUOPT_COMMIT "unknown")
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse --short=12 HEAD
                    OUTPUT_VARIABLE CPUOPT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" status --porcelain --untracked-files=no
                    OUTPUT_VARIABLE CPUOPT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(CPUOPT_DIRTY)
        set(CPUOPT_COMMIT "${CPUOPT_COMMIT}-dirty")
    endif()
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CPUOPT_WARNINGS /W4 /WX /permissive- /EHsc /utf-8)
set(CPUOPT_DEFINES WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# Pure logic, unit tested outside the game.
add_library(cpuopt_core STATIC
    src/core/control.cpp
    src/core/control.hpp
    src/core/histogram.hpp
    src/core/json.hpp
    src/core/signature.cpp
    src/core/signature.hpp
    src/core/toggles.hpp
)
target_compile_features(cpuopt_core PUBLIC cxx_std_20)
target_compile_options(cpuopt_core PRIVATE ${CPUOPT_WARNINGS} /guard:cf)

set(CPUOPT_PLUGIN_SOURCES
    src/plugin/frame_clock.cpp
    src/plugin/frame_clock.hpp
    src/plugin/log.cpp
    src/plugin/log.hpp
    src/plugin/runtime.cpp
    src/plugin/runtime.hpp
    src/plugin/sampler.cpp
    src/plugin/sampler.hpp
    src/plugin/system_info.cpp
    src/plugin/system_info.hpp
    src/plugin/thread_census.cpp
    src/plugin/thread_census.hpp
)

# Loaded by Stracker's Loader from nativePC\plugins (an MO2 mod folder).
add_library(mhw_cpuopt SHARED src/plugin/dllmain.cpp ${CPUOPT_PLUGIN_SOURCES})
target_compile_features(mhw_cpuopt PRIVATE cxx_std_20)
target_compile_definitions(mhw_cpuopt PRIVATE ${CPUOPT_DEFINES}
    CPUOPT_VERSION="${PROJECT_VERSION}" CPUOPT_COMMIT="${CPUOPT_COMMIT}")
target_compile_options(mhw_cpuopt PRIVATE ${CPUOPT_WARNINGS} /guard:cf)
target_link_libraries(mhw_cpuopt PRIVATE cpuopt_core bcrypt)
# The map file gives the report symbols for the plugin's own frames.
target_link_options(mhw_cpuopt PRIVATE /guard:cf /DEBUG /MAP /OPT:REF /OPT:NOICF)
set_target_properties(mhw_cpuopt PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

enable_testing()

add_executable(core_tests tests/core_tests.cpp)
target_compile_definitions(core_tests PRIVATE ${CPUOPT_DEFINES})
target_compile_options(core_tests PRIVATE ${CPUOPT_WARNINGS})
target_link_libraries(core_tests PRIVATE cpuopt_core)
set_target_properties(core_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_test(NAME core_tests COMMAND core_tests)

# The plugin's in-process parts against a stand-in process with known threads.
add_executable(plugin_tests tests/plugin_tests.cpp ${CPUOPT_PLUGIN_SOURCES})
target_compile_definitions(plugin_tests PRIVATE ${CPUOPT_DEFINES})
target_compile_options(plugin_tests PRIVATE ${CPUOPT_WARNINGS})
target_link_libraries(plugin_tests PRIVATE cpuopt_core bcrypt)
set_target_properties(plugin_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_test(NAME plugin_tests COMMAND plugin_tests)

# The built DLL in a stand-in process: log, control file, fail-closed frame clock.
add_executable(runtime_smoke tests/runtime_smoke.cpp)
target_compile_features(runtime_smoke PRIVATE cxx_std_20)
target_compile_definitions(runtime_smoke PRIVATE ${CPUOPT_DEFINES})
target_compile_options(runtime_smoke PRIVATE ${CPUOPT_WARNINGS})
set_target_properties(runtime_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_dependencies(runtime_smoke mhw_cpuopt)
add_test(NAME runtime_smoke COMMAND runtime_smoke $<TARGET_FILE:mhw_cpuopt> "${CMAKE_BINARY_DIR}/smoke")
