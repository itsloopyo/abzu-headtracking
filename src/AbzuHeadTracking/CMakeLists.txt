set(ABZU_DLL_SOURCES
    DllMain.cpp
    Framework.cpp
    Mods.cpp
    Config.cpp
    builds/build_registry.cpp
    builds/steam_offsets.cpp
    hooks/D3D11Hook.cpp
    mods/HeadTracking.cpp
    mods/UEEngine.cpp
    mods/UnrealCamera.cpp
    utility/Logging.cpp
    ${CMAKE_SOURCE_DIR}/src/legacy_config/legacy_config.cpp
)

add_library(AbzuHeadTracking SHARED ${ABZU_DLL_SOURCES})

set_target_properties(AbzuHeadTracking PROPERTIES
    OUTPUT_NAME "AbzuHeadTracking"
    SUFFIX ".asi"
    PREFIX ""
)

target_include_directories(AbzuHeadTracking
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(AbzuHeadTracking
    PRIVATE
        cameraunlock
        cameraunlock_hooks
        minhook
        d3d11
        dxgi
        psapi
        version
)

if(MSVC)
    target_compile_options(AbzuHeadTracking PRIVATE
        /W4 /permissive- /EHa
        $<$<CONFIG:Release>:/O2 /GL>
    )
    set_target_properties(AbzuHeadTracking PROPERTIES
        LINK_FLAGS_RELEASE "/LTCG /OPT:REF /OPT:ICF"
    )
endif()
