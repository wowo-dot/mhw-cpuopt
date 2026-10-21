cmake_minimum_required(VERSION 3.24)

project(mhw_cpuopt VERSION 0.18.0 LANGUAGES CXX)

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
    src/core/bench.cpp
    src/core/bench.hpp
    src/core/control.cpp
    src/core/control.hpp
    src/core/histogram.hpp
    src/core/json.hpp
    src/core/limit.cpp
    src/core/limit.hpp
    src/core/settings.cpp
    src/core/settings.hpp
    src/core/signature.cpp
    src/core/signature.hpp
    src/core/toggles.hpp
)
target_compile_features(cpuopt_core PUBLIC cxx_std_20)
target_compile_options(cpuopt_core PRIVATE ${CPUOPT_WARNINGS} /guard:cf)

set(CPUOPT_PLUGIN_SOURCES
    src/plugin/cpu_limit.cpp
    src/plugin/cpu_limit.hpp
    src/plugin/contact_guard.cpp
    src/plugin/contact_guard.hpp
    src/plugin/contacts.cpp
    src/plugin/contacts.hpp
    src/plugin/frame_budget.cpp
    src/plugin/frame_budget.hpp
    src/plugin/frame_cap.cpp
    src/plugin/frame_cap.hpp
    src/plugin/limiter.cpp
    src/plugin/limiter.hpp
    src/plugin/menu.cpp
    src/plugin/menu.hpp
    src/plugin/frame_clock.cpp
    src/plugin/frame_clock.hpp
    src/plugin/frame_events.cpp
    src/plugin/frame_events.hpp
    src/plugin/image_scan.cpp
    src/plugin/image_scan.hpp
    src/plugin/job_cap.cpp
    src/plugin/job_cap.hpp
    src/plugin/job_timing.cpp
    src/plugin/job_timing.hpp
    src/plugin/log.cpp
    src/plugin/multi_hit.cpp
    src/plugin/multi_hit.hpp
    src/plugin/log.hpp
    src/plugin/prim_stores.cpp
    src/plugin/prim_stores.hpp
    src/plugin/runtime.cpp
    src/plugin/runtime.hpp
    src/plugin/sampler.cpp
    src/plugin/sampler.hpp
    src/plugin/server_time.cpp
    src/plugin/server_time.hpp
    src/plugin/spin_probe.cpp
    src/plugin/spin_probe.hpp
    src/plugin/system_info.cpp
    src/plugin/system_info.hpp
    src/plugin/thread_census.cpp
    src/plugin/thread_census.hpp
    src/plugin/unit_lines.cpp
    src/plugin/unit_lines.hpp
)

# An Astera Loader plugin: <game>\AsteraLoader\plugins\cpuopt\cpuopt.dll (an MO2
# mod folder). The plugin API header is vendored (third_party/astera_loader).
add_library(mhw_cpuopt SHARED src/plugin/dllmain.cpp src/plugin/al_entry.cpp ${CPUOPT_PLUGIN_SOURCES})
target_compile_features(mhw_cpuopt PRIVATE cxx_std_20)
target_include_directories(mhw_cpuopt PRIVATE third_party/astera_loader/include third_party/cmfm/include)
target_compile_definitions(mhw_cpuopt PRIVATE ${CPUOPT_DEFINES}
    CPUOPT_VERSION="${PROJECT_VERSION}" CPUOPT_COMMIT="${CPUOPT_COMMIT}")
target_compile_options(mhw_cpuopt PRIVATE ${CPUOPT_WARNINGS} /guard:cf)
target_link_libraries(mhw_cpuopt PRIVATE cpuopt_core bcrypt)
# The map file gives the report symbols for the plugin's own frames.
# PDBALTPATH: the DLL names its symbol file without the build folder's path.
target_link_options(mhw_cpuopt PRIVATE /guard:cf /DEBUG /MAP /OPT:REF /OPT:NOICF "/PDBALTPATH:%_PDB%")
set_target_properties(mhw_cpuopt PROPERTIES OUTPUT_NAME cpuopt RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

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
target_include_directories(plugin_tests PRIVATE third_party/cmfm/include)
target_link_libraries(plugin_tests PRIVATE cpuopt_core bcrypt)
set_target_properties(plugin_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_test(NAME plugin_tests COMMAND plugin_tests)

# The built DLL under a stand-in Astera Loader: frames, Presents, control file,
# the frame counter found through the host's scanner, EXITING.
add_executable(al_host_smoke tests/al_host_smoke.cpp)
target_compile_definitions(al_host_smoke PRIVATE ${CPUOPT_DEFINES})
target_compile_options(al_host_smoke PRIVATE ${CPUOPT_WARNINGS})
target_include_directories(al_host_smoke PRIVATE third_party/astera_loader/include third_party/cmfm/include)
target_link_libraries(al_host_smoke PRIVATE cpuopt_core)
set_target_properties(al_host_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_dependencies(al_host_smoke mhw_cpuopt)
add_test(NAME al_host_smoke COMMAND al_host_smoke $<TARGET_FILE:mhw_cpuopt> "${CMAKE_BINARY_DIR}/smoke")
add_test(NAME al_host_player COMMAND al_host_smoke $<TARGET_FILE:mhw_cpuopt> "${CMAKE_BINARY_DIR}/smoke" player)

# The Python tools' pure logic (job round simulator), when Python 3 is there.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME tools_tests COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/tools_tests.py")
endif()
