#pragma once
#include <string>

namespace camera {
// Takes a still photo and writes a JPEG. Returns "" on success or an error message.
// `upside_down` turns the picture through 180 degrees, for a sensor mounted inverted.
std::string capture(const std::string &jpeg_path, int width, int height, bool upside_down);
}
