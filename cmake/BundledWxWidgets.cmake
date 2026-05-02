# BundledWxWidgets.cmake
# Creates imported targets for the pre-built wxWidgets 3.3.1 (x64-mingw-dynamic)
# bundled in third_party/wxwidgets/.
# Designed to work without vcpkg on any machine that has a compatible MinGW g++.

set(WX_ROOT "${CMAKE_SOURCE_DIR}/third_party/wxwidgets")

# ── Verify bundle exists ──────────────────────────────────────────────────────
if(NOT EXISTS "${WX_ROOT}/include/wx/wx.h")
    message(FATAL_ERROR
        "Bundled wxWidgets not found at ${WX_ROOT}/include/wx/wx.h.\n"
        "Run the setup script or check third_party/wxwidgets/.")
endif()

# ── Collect library paths ─────────────────────────────────────────────────────
set(_WX_LIB_DIR "${WX_ROOT}/lib")
set(_WX_SETUP_INCLUDE "${_WX_LIB_DIR}/mswu")  # contains wx/setup.h

foreach(_lib base core adv)
    set(_path "${_WX_LIB_DIR}/libwxmsw33u_${_lib}.a")
    if(_lib STREQUAL "base")
        set(_path "${_WX_LIB_DIR}/libwxbase33u.a")
    endif()
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "Bundled wx lib not found: ${_path}")
    endif()
    list(APPEND _WX_LIBS "${_path}")
endforeach()

# ── System libs wxWidgets requires on Windows (MinGW) ────────────────────────
set(_WX_SYS_LIBS
    gdi32 comdlg32 winspool winmm shell32 shlwapi comctl32
    ole32 oleaut32 uuid rpcrt4 advapi32 wsock32 uxtheme
)

# ── Create imported interface target wx::wx ───────────────────────────────────
add_library(wx::wx INTERFACE IMPORTED GLOBAL)
target_include_directories(wx::wx INTERFACE
    "${WX_ROOT}/include"
    "${_WX_SETUP_INCLUDE}"
)
target_compile_definitions(wx::wx INTERFACE
    WXUSINGDLL
    __WXMSW__
    wxUSE_UNICODE=1
    _UNICODE
    UNICODE
)
target_link_libraries(wx::wx INTERFACE
    ${_WX_LIBS}
    ${_WX_SYS_LIBS}
)

# ── Copy wx DLLs to the build output directory post-build ────────────────────
set(WX_DLLS
    "${WX_ROOT}/bin/wxbase331u_gcc_custom.dll"
    "${WX_ROOT}/bin/wxmsw331u_core_gcc_custom.dll"
    "${WX_ROOT}/bin/wxmsw331u_adv_gcc_custom.dll"
    # Indirect dependencies of wxWidgets (MinGW-built, from vcpkg x64-mingw-dynamic)
    "${WX_ROOT}/bin/libpcre2-16.dll"
    "${WX_ROOT}/bin/libjpeg-62.dll"
    "${WX_ROOT}/bin/libzlib1.dll"
    # Image format DLLs (clean vcpkg MinGW builds – no MSVCR120 dependency)
    "${WX_ROOT}/bin/libpng16.dll"
    "${WX_ROOT}/bin/libtiff.dll"
    "${WX_ROOT}/bin/libwebp.dll"
    "${WX_ROOT}/bin/libwebpdecoder.dll"
    "${WX_ROOT}/bin/libwebpdemux.dll"
    "${WX_ROOT}/bin/libsharpyuv.dll"
    "${WX_ROOT}/bin/liblzma.dll"
)

function(libdeploy_copy_wx_dlls target)
    foreach(_dll ${WX_DLLS})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Copying ${_dll}"
        )
    endforeach()
endfunction()

message(STATUS "BundledWxWidgets: found ${WX_ROOT}  (wxWidgets 3.3.1 x64-mingw-dynamic)")
