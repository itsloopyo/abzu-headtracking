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

# Behaviour lock for the 6DOF lean boundary, the canonical config tests and the
# config differential test. Separate executables that link none of the hook or
# engine-walk machinery, so `pixi run test` runs anywhere the mod builds, with
# no game installed.
option(ABZU_BUILD_TESTS "Build the behaviour and config tests" ON)
if(ABZU_BUILD_TESTS)
    enable_testing()
    add_executable(abzuht_lean_tests
        tests/lean_budget_tests.cpp
        src/AbzuHeadTracking/Config.cpp
        src/AbzuHeadTracking/utility/Logging.cpp
        src/legacy_config/legacy_config.cpp
    )
    target_include_directories(abzuht_lean_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/AbzuHeadTracking
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(abzuht_lean_tests PRIVATE cameraunlock)
    if(MSVC)
        target_compile_options(abzuht_lean_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME lean_budget COMMAND abzuht_lean_tests)

    # The committed HeadTracking.ini against the table, what the owner creates on
    # a first start and on the first start after an update, and what a toggle's
    # save changes. `--render-config <path>` is pixi run render-config.
    add_executable(abzuht_canonical_config_tests
        tests/canonical_config_tests.cpp
        src/AbzuHeadTracking/Config.cpp
        src/AbzuHeadTracking/utility/Logging.cpp
        src/legacy_config/legacy_config.cpp
    )
    target_include_directories(abzuht_canonical_config_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/AbzuHeadTracking
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(abzuht_canonical_config_tests PRIVATE
        ABZU_COMMITTED_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/HeadTracking.ini"
        ABZU_DIFFERENTIAL_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/config_differential/data"
        ABZU_DEFAULTS_FIXTURE="${ABZU_CAMERAUNLOCK_DIR}/../data/fixtures/canonical-ini/global/Defaults.ini")
    target_link_libraries(abzuht_canonical_config_tests PRIVATE cameraunlock)
    if(MSVC)
        target_compile_options(abzuht_canonical_config_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME canonical_config COMMAND abzuht_canonical_config_tests)

    # The config differential test. The oracle is the newest published build's reader, with
    # ParseVk and ParseMode and the core sources they compiled, copied verbatim into
    # tests/config_differential/oracle (provenance.txt). It builds as a library with `ueht` and
    # `cameraunlock` renamed, so it links beside the current core and the current mod.
    set(ABZU_ORACLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_differential/oracle)
    add_library(abzuht_config_oracle STATIC
        ${ABZU_ORACLE_DIR}/src/Config.cpp
        ${ABZU_ORACLE_DIR}/src/utility/Logging.cpp
        ${ABZU_ORACLE_DIR}/core/src/config/ini_reader.cpp
        tests/config_differential/oracle_adapter.cpp
    )
    target_include_directories(abzuht_config_oracle PRIVATE
        ${ABZU_ORACLE_DIR}/src
        ${ABZU_ORACLE_DIR}/include
        ${ABZU_ORACLE_DIR}/core/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_differential)
    target_compile_definitions(abzuht_config_oracle PRIVATE
        cameraunlock=abzuht_oracle_core
        ueht=abzuht_oracle)
    if(MSVC)
        target_compile_options(abzuht_config_oracle PRIVATE /EHsc)
    endif()

    add_executable(abzuht_config_differential_tests
        tests/config_differential/differential_tests.cpp
        src/AbzuHeadTracking/Config.cpp
        src/AbzuHeadTracking/utility/Logging.cpp
        src/legacy_config/legacy_config.cpp
    )
    target_include_directories(abzuht_config_differential_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/AbzuHeadTracking
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_differential
    )
    target_compile_definitions(abzuht_config_differential_tests PRIVATE
        ABZU_DIFFERENTIAL_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/config_differential/data")
    target_link_libraries(abzuht_config_differential_tests PRIVATE cameraunlock abzuht_config_oracle)
    if(MSVC)
        target_compile_options(abzuht_config_differential_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME config_differential COMMAND abzuht_config_differential_tests)
    # Every corpus input is migrated three times through real files, which takes longer than
    # ctest's default 1500 seconds on a slow disk.
    set_tests_properties(config_differential PROPERTIES TIMEOUT 7200)
endif()
