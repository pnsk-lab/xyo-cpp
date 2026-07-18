SKIA="$PWD/.deps/skia/out/Static"

make all \
    SKIA_ROOT="$PWD/.deps/skia" \
    SKIA_OUT="$SKIA" \
    SKIA_EXTRA_LDFLAGS="-Wl,--start-group \
    $SKIA/liballocator_core.a \
    $SKIA/liballocator_base.a \
    $SKIA/liballocator_shim.a \
    $SKIA/libraw_ptr.a \
    $SKIA/libharfbuzz.a \
    $SKIA/libexpat.a \
    $SKIA/libpng.a \
    $SKIA/libzlib.a \
    $SKIA/libjpeg.a \
    $SKIA/libwebp.a \
    $SKIA/libwuffs.a \
    $SKIA/libdng_sdk.a \
    $SKIA/libpiex.a \
    -Wl,--end-group $(pkg-config --libs freetype2) -lGL"