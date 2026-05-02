cmake_minimum_required(VERSION 3.20)
project(LibDeploy VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Output directory: put exe + DLLs together ────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ── Third-party dependencies ─────────────────────────────────────────────────
add_subdirectory(third_party/zlib)
add_subdirectory(third_party/minizip)

include(cmake/BundledWxWidgets.cmake)

# ── nlohmann/json (header-only) ───────────────────────────────────────────────
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    "${CMAKE_SOURCE_DIR}/third_party"
)

# ── Source files ─────────────────────────────────────────────────────────────
file(GLOB ENGINE_SRC "engine/*.cpp")
file(GLOB CONFIG_SRC "config/*.cpp")
file(GLOB APP_SRC    "app/*.cpp")

# ── Executable ───────────────────────────────────────────────────────────────
add_executable(LibDeploy WIN32
    ${ENGINE_SRC}
    ${CONFIG_SRC}
    ${APP_SRC}
    app/app.rc          # Windows manifest (Common Controls v6)
)

target_include_directories(LibDeploy PRIVATE
    "${CMAKE_SOURCE_DIR}"          # allows #include "engine/..." etc.
    "${CMAKE_SOURCE_DIR}/third_party"
)

target_link_libraries(LibDeploy PRIVATE
    wx::wx
    minizip_bundled
    nlohmann_json
)
if(WIN32)
    target_link_libraries(LibDeploy PRIVATE version)
endif()

target_compile_options(LibDeploy PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
)

# ── Post-build: copy wx DLLs and MinGW runtime DLLs ─────────────────────────
libdeploy_copy_wx_dlls(LibDeploy)

set(RUNTIME_DLLS
    "${CMAKE_SOURCE_DIR}/runtime/libgcc_s_seh-1.dll"
    "${CMAKE_SOURCE_DIR}/runtime/libstdc++-6.dll"
    "${CMAKE_SOURCE_DIR}/runtime/libwinpthread-1.dll"
)
foreach(_dll ${RUNTIME_DLLS})
    add_custom_command(TARGET LibDeploy POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_dll}" "$<TARGET_FILE_DIR:LibDeploy>"
        COMMENT "Copying runtime DLL: ${_dll}"
    )
endforeach()

# ── i18n: compile .po → .mo and copy to bin/locale/<lang>/LibDeploy.mo ───────
find_program(PYTHON_EXE python HINTS "D:/install/miniforg3")
if(PYTHON_EXE)
    set(LOCALE_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/compile_po.py")
    foreach(_lang en_US zh_CN)
        set(_po  "${CMAKE_SOURCE_DIR}/locale/${_lang}/LibDeploy.po")
        set(_mo  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/locale/${_lang}/LibDeploy.mo")
        add_custom_command(
            OUTPUT  "${_mo}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/locale/${_lang}"
            COMMAND "${PYTHON_EXE}" "${LOCALE_SCRIPT}" "${_po}" "${_mo}"
            DEPENDS "${_po}" "${LOCALE_SCRIPT}"
            COMMENT "Compiling locale: ${_lang}"
        )
        list(APPEND LOCALE_MO_FILES "${_mo}")
    endforeach()
    add_custom_target(LibDeploy_locale ALL DEPENDS ${LOCALE_MO_FILES})
    add_dependencies(LibDeploy LibDeploy_locale)
    message(STATUS "i18n: locales will be compiled to ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/locale/")
else()
    message(WARNING "Python not found – locale .mo files will not be compiled")
endif()

# ── Bundled NSIS: copy tools/nsis → build/bin/nsis ──────────────────────────
add_custom_command(TARGET LibDeploy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/tools/nsis"
        "$<TARGET_FILE_DIR:LibDeploy>/nsis"
    COMMENT "Copying bundled NSIS"
)

# ── Install (optional: cmake --install) ──────────────────────────────────────
install(TARGETS LibDeploy RUNTIME DESTINATION bin)
install(FILES
    ${WX_DLLS}
    ${RUNTIME_DLLS}
    DESTINATION bin
)

message(STATUS "LibDeploy configured. Generator: ${CMAKE_GENERATOR}")
message(STATUS "  Build dir : ${CMAKE_BINARY_DIR}")
message(STATUS "  Output    : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
