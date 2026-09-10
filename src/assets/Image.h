#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace crate {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // width*height*4, row-major, top-left origin

    bool valid() const { return width > 0 && height > 0 && rgba.size() == size_t(width) * height * 4; }
};

// Load an image as 8-bit RGBA. PNG / JPEG / BMP / GIF / PSD / HDR go through
// stb_image; TIFF (.tif/.tiff) uses a baseline decoder here (uncompressed,
// PackBits and LZW; 8-bit grey/RGB/RGBA; either byte order; horizontal
// predictor). Returns an invalid Image on failure.
Image loadImage(const std::string& path);

// List of file extensions loadImage accepts (lowercase, no dot).
bool isSupportedImageExt(const std::string& lowerExt);

} // namespace crate
