// Image loader tests: baseline TIFF (uncompressed + PackBits) and the stb path
// via the bundled BMP. Data path injected by CMake.

#include "assets/Image.h"

#include <cstdio>
#include <string>

using namespace crate;

#ifndef CRATE_TEST_DATA
#define CRATE_TEST_DATA "."
#endif

static int g_checks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(c)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);              \
            return 1;                                                             \
        }                                                                         \
    } while (0)

static Image load(const char* name) {
    return loadImage(std::string(CRATE_TEST_DATA) + "/" + name);
}

int main() {
    CHECK(isSupportedImageExt("png"));
    CHECK(isSupportedImageExt("tiff"));
    CHECK(!isSupportedImageExt("fbx"));

    Image bmp = load("uv_check.bmp");
    CHECK(bmp.valid());
    CHECK(bmp.width == 64 && bmp.height == 64);

    Image rgb = load("rgb_uncompressed.tif");
    CHECK(rgb.valid());
    CHECK(rgb.width == 8 && rgb.height == 8);
    CHECK(rgb.rgba[3] == 255); // opaque

    Image gray = load("gray_packbits.tif");
    CHECK(gray.valid());
    CHECK(gray.width == 8 && gray.height == 8);
    CHECK(gray.rgba[0] == gray.rgba[1] && gray.rgba[1] == gray.rgba[2]); // grey expands equal

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
