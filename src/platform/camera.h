#pragma once
#include <atomic>
#include <string>

namespace camera {
// Takes a still photo and writes a JPEG. Returns "" on success or an error message.
// `upside_down` turns the picture through 180 degrees, for a sensor mounted inverted. Setting
// `cancel` stops the camera program. On failure no photo is left at jpeg_path, so an earlier one
// can never be taken for the new one.
std::string capture(const std::string &jpeg_path, int width, int height, bool upside_down,
                    const std::atomic<bool> *cancel = nullptr);
}
