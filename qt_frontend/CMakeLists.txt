cmake_minimum_required(VERSION 3.20)
project(LibDeployQt VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

set(LIBDEPLOY_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(Qt5 REQUIRED COMPONENTS Widgets Concurrent LinguistTools)

add_subdirectory("${LIBDEPLOY_ROOT}/third_party/zlib" "${CMAKE_BINARY_DIR}/third_party/zlib")
add_subdirectory("${LIBDEPLOY_ROOT}/third_party/minizip" "${CMAKE_BINARY_DIR}/third_party/minizip")

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    "${LIBDEPLOY_ROOT}/third_party"
)

file(GLOB ENGINE_SRC "${LIBDEPLOY_ROOT}/engine/*.cpp")
file(GLOB CONFIG_SRC "${LIBDEPLOY_ROOT}/config/*.cpp")

add_executable(LibDeployQt WIN32
    src/main.cpp
    src/main_window.h
    src/main_window.cpp
    src/app.rc
    ${ENGINE_SRC}
    ${CONFIG_SRC}
)

set(LIBDEPLOY_QT_TS
    translations/LibDeployQt_zh_CN.ts
)
qt5_add_translation(LIBDEPLOY_QT_QM ${LIBDEPLOY_QT_TS})
add_custom_target(LibDeployQt_translations ALL DEPENDS ${LIBDEPLOY_QT_QM})
add_dependencies(LibDeployQt LibDeployQt_translations)

target_include_directories(LibDeployQt PRIVATE
    "${LIBDEPLOY_ROOT}"
    "${LIBDEPLOY_ROOT}/third_party"
)

target_link_libraries(LibDeployQt PRIVATE
    Qt5::Widgets
    Qt5::Concurrent
    minizip_bundled
    nlohmann_json
)
if(WIN32)
    target_link_libraries(LibDeployQt PRIVATE version)
endif()

target_compile_definitions(LibDeployQt PRIVATE
    QT_DISABLE_DEPRECATED_BEFORE=0x050C00
)

if(MSVC)
    target_compile_options(LibDeployQt PRIVATE /W4 /permissive- /utf-8)
    set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
    include(InstallRequiredSystemLibraries)
else()
    target_compile_options(LibDeployQt PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

add_custom_command(TARGET LibDeployQt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LIBDEPLOY_ROOT}/libdeploy_config.json"
        "$<TARGET_FILE_DIR:LibDeployQt>/libdeploy_config.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${LIBDEPLOY_ROOT}/tools/nsis"
        "$<TARGET_FILE_DIR:LibDeployQt>/nsis"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:LibDeployQt>/translations"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${LIBDEPLOY_QT_QM}
        "$<TARGET_FILE_DIR:LibDeployQt>/translations"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LIBDEPLOY_ROOT}/assets/libdeploy.ico"
        "$<TARGET_FILE_DIR:LibDeployQt>/libdeploy.ico"
    COMMENT "Copying LibDeploy runtime config and bundled NSIS"
)

if(MSVC AND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    add_custom_command(TARGET LibDeployQt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
            "$<TARGET_FILE_DIR:LibDeployQt>"
        COMMENT "Copying MSVC runtime DLLs"
    )
endif()

find_program(WINDEPLOYQT_EXE windeployqt HINTS "$ENV{QTDIR}/bin")
if(WIN32 AND WINDEPLOYQT_EXE)
    add_custom_command(TARGET LibDeployQt POST_BUILD
        COMMAND "${WINDEPLOYQT_EXE}" "$<TARGET_FILE:LibDeployQt>"
        COMMENT "Running windeployqt"
    )
endif()

install(TARGETS LibDeployQt RUNTIME DESTINATION bin)
