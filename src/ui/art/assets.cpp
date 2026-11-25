#include "ui/art/assets.h"

#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <png.h>

namespace art {

namespace {

constexpr png_uint_32 kMaxSide = 4096;   // larger than any art; a bigger header is a broken file

// What read_png works on, outside its own frame: libpng reports an error with a longjmp back into read_png,
// which then holds nothing that needs destroying.
struct Decoding {
    const std::string *bytes;
    size_t at;
    Image *out;
    std::string error;
};

void read_bytes(png_structp png, png_bytep to, png_size_t n) {
    Decoding *d = static_cast<Decoding *>(png_get_io_ptr(png));
    if (n > d->bytes->size() - d->at) png_error(png, "the file ends too early");
    memcpy(to, d->bytes->data() + d->at, n);
    d->at += n;
}

void on_error(png_structp png, png_const_charp message) {
    static_cast<Decoding *>(png_get_error_ptr(png))->error = message;
    png_longjmp(png, 1);
}

void on_warning(png_structp, png_const_charp) {}   // a warning leaves the picture readable

uint16_t rgb565(const png_color &c) {
    return static_cast<uint16_t>(((c.red >> 3) << 11) | ((c.green >> 2) << 5) | (c.blue >> 3));
}

// The picture into d->out; false with d->error set when libpng refuses it.
bool read_png(Decoding *d) {
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, d, on_error, on_warning);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        d->error = "out of memory";
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }
    png_set_read_fn(png, d, read_bytes);
    png_read_info(png, info);
    png_uint_32 w = 0, h = 0;
    int depth = 0, type = 0, interlace = 0;
    png_get_IHDR(png, info, &w, &h, &depth, &type, &interlace, nullptr, nullptr);
    if (type != PNG_COLOR_TYPE_PALETTE || depth != 8 || interlace != PNG_INTERLACE_NONE)
        png_error(png, "not an 8-bit indexed picture");
    if (w == 0 || h == 0 || w > kMaxSide || h > kMaxSide) png_error(png, "an impossible size");
    png_colorp plte = nullptr;
    int colours = 0;
    if (!png_get_PLTE(png, info, &plte, &colours)) png_error(png, "no palette");
    d->out->w = static_cast<int>(w);
    d->out->h = static_cast<int>(h);
    d->out->pal.resize(static_cast<size_t>(colours));
    for (int i = 0; i < colours; ++i) d->out->pal[static_cast<size_t>(i)] = rgb565(plte[i]);
    d->out->px.resize(static_cast<size_t>(w) * h);
    for (png_uint_32 y = 0; y < h; ++y) png_read_row(png, &d->out->px[static_cast<size_t>(y) * w], nullptr);
    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &info, nullptr);
    return true;
}

}  // namespace

Source folder(const std::string &dir) {
    return [dir](const std::string &path) {
        std::ifstream f(dir + "/" + path, std::ios::binary);
        std::ostringstream bytes;
        bytes << f.rdbuf();
        return f ? bytes.str() : std::string();
    };
}

bool decode(const std::string &png, Image &out, std::string &err) {
    if (png.size() < 8 || png_sig_cmp(reinterpret_cast<png_const_bytep>(png.data()), 0, 8) != 0) {
        err = "not a PNG";
        return false;
    }
    Decoding d{&png, 0, &out, ""};
    if (!read_png(&d)) {
        err = d.error.empty() ? "cannot be read" : d.error;
        return false;
    }
    for (const uint8_t index : out.px)
        if (index >= out.pal.size()) {
            err = "a pixel outside the palette";
            return false;
        }
    return true;
}

const Image *Assets::image(const std::string &path) {
    const auto known = images_.find(path);
    if (known != images_.end()) return known->second.get();
    std::unique_ptr<Image> img;
    const std::string bytes = source_(path);
    if (!bytes.empty()) {
        img = std::make_unique<Image>();
        std::string err;
        if (!decode(bytes, *img, err)) {
            fprintf(stderr, "art: %s: %s\n", path.c_str(), err.c_str());
            img.reset();
        }
    }
    return (images_[path] = std::move(img)).get();
}

void Assets::clear() { images_.clear(); }

}  // namespace art
