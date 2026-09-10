#include "assets/Image.h"
#include "core/Log.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include <cstdio>
#include <cstring>

namespace crate {

static std::string lowerExt(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return {};
    std::string e = path.substr(dot + 1);
    for (char& c : e)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return e;
}

bool isSupportedImageExt(const std::string& e) {
    return e == "png" || e == "jpg" || e == "jpeg" || e == "bmp" || e == "gif" || e == "tga" ||
           e == "psd" || e == "hdr" || e == "pic" || e == "ppm" || e == "pgm" || e == "tif" ||
           e == "tiff";
}

// --------------------------------------------------------------------------
// Baseline TIFF decoder
// --------------------------------------------------------------------------
namespace {

struct Reader {
    const uint8_t* p = nullptr;
    size_t n = 0;
    bool be = false;

    uint16_t u16(size_t o) const {
        if (o + 2 > n) return 0;
        return be ? (uint16_t(p[o]) << 8 | p[o + 1]) : (uint16_t(p[o + 1]) << 8 | p[o]);
    }
    uint32_t u32(size_t o) const {
        if (o + 4 > n) return 0;
        return be ? (uint32_t(p[o]) << 24 | uint32_t(p[o + 1]) << 16 | uint32_t(p[o + 2]) << 8 |
                     p[o + 3])
                  : (uint32_t(p[o + 3]) << 24 | uint32_t(p[o + 2]) << 16 | uint32_t(p[o + 1]) << 8 |
                     p[o]);
    }
};

// PackBits (RLE) decompression.
std::vector<uint8_t> unpackBits(const uint8_t* src, size_t len, size_t expected) {
    std::vector<uint8_t> out;
    out.reserve(expected);
    size_t i = 0;
    while (i < len && out.size() < expected) {
        int8_t n = static_cast<int8_t>(src[i++]);
        if (n >= 0) {
            int count = n + 1;
            for (int k = 0; k < count && i < len; ++k)
                out.push_back(src[i++]);
        } else if (n != -128) {
            int count = 1 - n;
            if (i < len) {
                uint8_t v = src[i++];
                for (int k = 0; k < count; ++k)
                    out.push_back(v);
            }
        }
    }
    return out;
}

// TIFF LZW (MSB-first, variable width 9..12, early change, clear=256, eoi=257).
std::vector<uint8_t> lzwDecode(const uint8_t* src, size_t len, size_t expected) {
    std::vector<uint8_t> out;
    out.reserve(expected);
    std::vector<std::vector<uint8_t>> table;
    auto reset = [&]() {
        table.clear();
        table.resize(258);
        for (int i = 0; i < 256; ++i)
            table[i] = {static_cast<uint8_t>(i)};
    };
    reset();

    size_t bitpos = 0;
    int codeWidth = 9;
    auto getCode = [&]() -> int {
        int code = 0;
        for (int i = 0; i < codeWidth; ++i) {
            size_t byte = (bitpos + i) >> 3;
            if (byte >= len)
                return 257; // EOI
            int bit = 7 - int((bitpos + i) & 7);
            code = (code << 1) | ((src[byte] >> bit) & 1);
        }
        bitpos += codeWidth;
        return code;
    };

    int prev = -1;
    while (out.size() < expected) {
        int code = getCode();
        if (code == 257)
            break;
        if (code == 256) {
            reset();
            codeWidth = 9;
            prev = -1;
            continue;
        }
        std::vector<uint8_t> entry;
        if (code < int(table.size()) && !table[code].empty()) {
            entry = table[code];
        } else if (prev >= 0) {
            entry = table[prev];
            entry.push_back(table[prev][0]);
        } else {
            break;
        }
        out.insert(out.end(), entry.begin(), entry.end());
        if (prev >= 0) {
            std::vector<uint8_t> newEntry = table[prev];
            newEntry.push_back(entry[0]);
            table.push_back(std::move(newEntry));
        }
        prev = code;
        // Early-change: widen one code before the table actually fills.
        if (table.size() + 1 >= (size_t(1) << codeWidth) && codeWidth < 12)
            ++codeWidth;
    }
    return out;
}

Image decodeTiff(const std::vector<uint8_t>& buf) {
    Image img;
    Reader r{buf.data(), buf.size(), false};
    if (buf.size() < 8)
        return img;
    if (buf[0] == 'I' && buf[1] == 'I')
        r.be = false;
    else if (buf[0] == 'M' && buf[1] == 'M')
        r.be = true;
    else
        return img;
    if (r.u16(2) != 42)
        return img;

    uint32_t ifd = r.u32(4);
    if (ifd + 2 > buf.size())
        return img;
    uint16_t count = r.u16(ifd);

    uint32_t width = 0, height = 0, compression = 1, photometric = 1, samples = 1;
    uint32_t rowsPerStrip = 0xFFFFFFFF, predictor = 1, bits = 8;
    std::vector<uint32_t> stripOffsets, stripCounts;

    auto readArray = [&](size_t entryOff, uint16_t type, uint32_t cnt) {
        std::vector<uint32_t> v;
        size_t elemSize = (type == 3) ? 2 : 4;
        size_t total = elemSize * cnt;
        size_t src = (total <= 4) ? entryOff + 8 : r.u32(entryOff + 8);
        for (uint32_t i = 0; i < cnt; ++i)
            v.push_back(type == 3 ? r.u16(src + i * 2) : r.u32(src + i * 4));
        return v;
    };

    for (uint16_t i = 0; i < count; ++i) {
        size_t e = ifd + 2 + i * 12;
        if (e + 12 > buf.size())
            break;
        uint16_t tag = r.u16(e);
        uint16_t type = r.u16(e + 2);
        uint32_t cnt = r.u32(e + 4);
        uint32_t val = (type == 3) ? r.u16(e + 8) : r.u32(e + 8);
        switch (tag) {
            case 256: width = val; break;
            case 257: height = val; break;
            case 258: bits = (cnt >= 1) ? readArray(e, type, 1)[0] : val; break;
            case 259: compression = val; break;
            case 262: photometric = val; break;
            case 273: stripOffsets = readArray(e, type, cnt); break;
            case 277: samples = val; break;
            case 278: rowsPerStrip = val; break;
            case 279: stripCounts = readArray(e, type, cnt); break;
            case 317: predictor = val; break;
            default: break;
        }
    }

    if (width == 0 || height == 0 || bits != 8 || samples < 1 || samples > 4 ||
        stripOffsets.empty() || stripOffsets.size() != stripCounts.size()) {
        CR_WARN("assets", "TIFF: unsupported layout (bits/samples/strips)");
        return img;
    }
    if (compression != 1 && compression != 5 && compression != 32773) {
        CR_WARN("assets", "TIFF: unsupported compression " + std::to_string(compression));
        return img;
    }
    if (rowsPerStrip == 0xFFFFFFFF)
        rowsPerStrip = height;

    const size_t rowBytes = size_t(width) * samples;
    std::vector<uint8_t> pixels;
    pixels.reserve(rowBytes * height);

    for (size_t s = 0; s < stripOffsets.size(); ++s) {
        uint32_t off = stripOffsets[s], len = stripCounts[s];
        if (off + len > buf.size())
            break;
        size_t rows = std::min<size_t>(rowsPerStrip, height - s * rowsPerStrip);
        size_t expect = rowBytes * rows;
        std::vector<uint8_t> strip;
        if (compression == 1)
            strip.assign(buf.begin() + off, buf.begin() + off + len);
        else if (compression == 32773)
            strip = unpackBits(buf.data() + off, len, expect);
        else
            strip = lzwDecode(buf.data() + off, len, expect);

        if (predictor == 2) {
            for (size_t row = 0; row < rows; ++row)
                for (size_t x = samples; x < rowBytes; ++x)
                    strip[row * rowBytes + x] =
                        static_cast<uint8_t>(strip[row * rowBytes + x] + strip[row * rowBytes + x - samples]);
        }
        pixels.insert(pixels.end(), strip.begin(), strip.end());
    }
    if (pixels.size() < rowBytes * height)
        pixels.resize(rowBytes * height, 0);

    img.width = int(width);
    img.height = int(height);
    img.rgba.resize(size_t(width) * height * 4, 255);
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        const uint8_t* src = &pixels[i * samples];
        uint8_t* dst = &img.rgba[i * 4];
        if (samples == 1) {
            uint8_t g = photometric == 0 ? uint8_t(255 - src[0]) : src[0];
            dst[0] = dst[1] = dst[2] = g;
            dst[3] = 255;
        } else {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = samples == 4 ? src[3] : 255;
        }
    }
    return img;
}

} // namespace

Image loadImage(const std::string& path) {
    const std::string ext = lowerExt(path);

    if (ext == "tif" || ext == "tiff") {
        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "rb");
        if (!f) {
            CR_WARN("assets", "Cannot open " + path);
            return {};
        }
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> buf(sz > 0 ? size_t(sz) : 0);
        if (!buf.empty())
            (void)std::fread(buf.data(), 1, buf.size(), f);
        std::fclose(f);
        Image img = decodeTiff(buf);
        if (!img.valid())
            CR_WARN("assets", "TIFF decode failed: " + path);
        return img;
    }

    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!px) {
        CR_WARN("assets", "Image load failed: " + path + " (" + stbi_failure_reason() + ")");
        return {};
    }
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.assign(px, px + size_t(w) * h * 4);
    stbi_image_free(px);
    return img;
}

} // namespace crate
