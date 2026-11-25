// The art's pictures: the PNG files under assets/art/ (8-bit indexed, each with its own palette, index 0
// clear: tests/art_check.py checks every one), decoded once with libpng and kept while a scene uses them.
// The app reads the copies built into the program (ArtView); the unit tests read the folder.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "ui/art/canvas.h"

namespace art {

// The bytes of the file at `path` under assets/art/ ("radio/sky-dusk.png"); empty when there is none.
using Source = std::function<std::string(const std::string &path)>;
Source folder(const std::string &dir);   // the files in a folder

// Decodes an 8-bit indexed PNG into `out`: its indices as stored, its palette in RGB565. False, with why,
// for anything else (another kind of PNG, a pixel outside the palette, a broken or cut-short file).
bool decode(const std::string &png, Image &out, std::string &err);

class Assets {
public:
    explicit Assets(Source source) : source_(std::move(source)) {}
    // The picture at `path`, decoded the first time it is asked for; nullptr when there is no such file, or
    // it cannot be decoded (said on stderr). It stays until clear().
    const Image *image(const std::string &path);
    void clear();   // lets go of every picture: none may be on a canvas any more

private:
    Source source_;
    std::map<std::string, std::unique_ptr<Image>> images_;   // nullptr: asked for, and there is none
};

}  // namespace art
