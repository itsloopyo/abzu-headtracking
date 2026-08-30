cmake_minimum_required(VERSION 3.20)
project(AbzuHeadTracking VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "AbzuHeadTracking targets Windows only.")
endif()

add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Strip default /EHsc; we use /EHa per-target for SEH+C++ in UnrealCamera.
if(MSVC)
    foreach(flag_var
            CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        string(REGEX REPLACE "/EHsc" "" ${flag_var} "${${flag_var}}")
        string(REGEX REPLACE "/EHs" "" ${flag_var} "${${flag_var}}")
    endforeach()
endif()

set(ABZU_CAMERAUNLOCK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cameraunlock-core/cpp"
    CACHE PATH "Path to cameraunlock-core/cpp")
set(ABZU_MINHOOK_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/extern/minhook"
    CACHE PATH "Path to minhook source tree")

if(NOT EXISTS "${ABZU_CAMERAUNLOCK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "cameraunlock-core not found at ${ABZU_CAMERAUNLOCK_DIR}. "
        "Run: git submodule update --init --recursive")
endif()
if(NOT EXISTS "${ABZU_MINHOOK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "MinHook not found at ${ABZU_MINHOOK_DIR}. "
        "Run: git submodule update --init --recursive")
endif()

add_subdirectory(${ABZU_MINHOOK_DIR} ${CMAKE_BINARY_DIR}/_deps/minhook EXCLUDE_FROM_ALL)
if(NOT TARGET minhook AND TARGET MinHook)
    add_library(minhook ALIAS MinHook)
endif()

set(CAMERAUNLOCK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CAMERAUNLOCK_BUILD_HOOKS ON  CACHE BOOL "" FORCE)
add_subdirectory(${ABZU_CAMERAUNLOCK_DIR} ${CMAKE_BINARY_DIR}/_deps/cameraunlock EXCLUDE_FROM_ALL)

add_subdirectory(src/AbzuHeadTracking)

# Behaviour lock for the 6DOF lean boundary, driven by the shipped
# HeadTracking.ini rather than the struct defaults. A separate executable that
# links none of the hook or engine-walk machinery, so `pixi run test` runs
# anywhere the mod builds, with no game installed.
option(ABZU_BUILD_TESTS "Build the lean-budget behaviour tests" ON)
if(ABZU_BUILD_TESTS)
    enable_testing()
    add_executable(abzuht_lean_tests
        tests/lean_budget_tests.cpp
        src/AbzuHeadTracking/Config.cpp
        src/AbzuHeadTracking/utility/Logging.cpp
    )
    target_include_directories(abzuht_lean_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/AbzuHeadTracking
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(abzuht_lean_tests PRIVATE
        ABZUHT_SHIPPED_INI="${CMAKE_CURRENT_SOURCE_DIR}/HeadTracking.ini"
    )
    target_link_libraries(abzuht_lean_tests PRIVATE cameraunlock)
    if(MSVC)
        target_compile_options(abzuht_lean_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME lean_budget COMMAND abzuht_lean_tests)
endif()
