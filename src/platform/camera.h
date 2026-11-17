#pragma once
#include <string>

namespace camera {
// Takes a still photo and writes a JPEG. Returns "" on success or an error message.
std::string capture(const std::string &jpeg_path, int width = 1280, int height = 960);
}
