add_library(zlib_bundled STATIC
    adler32.c compress.c crc32.c deflate.c
    gzclose.c gzlib.c gzread.c gzwrite.c
    infback.c inffast.c inflate.c inftrees.c
    trees.c uncompr.c zutil.c
)
target_include_directories(zlib_bundled PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(zlib_bundled PRIVATE _CRT_SECURE_NO_WARNINGS)
# Silence unused-parameter warnings in zlib
target_compile_options(zlib_bundled PRIVATE -w)
