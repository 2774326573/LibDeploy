add_library(minizip_bundled STATIC
    ioapi.c
    iowin32.c
    zip.c
    unzip.c
    mztools.c
)
target_include_directories(minizip_bundled PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(minizip_bundled PUBLIC zlib_bundled)
target_compile_definitions(minizip_bundled PRIVATE _CRT_SECURE_NO_WARNINGS USE_FILE32API)
target_compile_options(minizip_bundled PRIVATE -w)
