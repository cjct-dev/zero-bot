#include "platform/camera.h"

#include <cstdlib>
#include <sys/stat.h>

#include "core/util.h"
#include "platform/proc.h"

namespace camera {

std::string capture(const std::string &jpeg_path, int width, int height) {
    // ZB_FAKE_CAMERA=<file.jpg> lets the simulator and tests supply a picture.
    if (const char *fake = getenv("ZB_FAKE_CAMERA"); fake && *fake) {
        std::string data;
        if (!util::read_file(fake, data)) return "camera test image not found";
        return util::write_file_atomic(jpeg_path, data) ? "" : "could not save the photo";
    }
    if (!proc::have("rpicam-jpeg")) return "this device has no camera";
    const auto res = proc::run({"rpicam-jpeg", "-n", "-t", "700", "--width", std::to_string(width), "--height",
                                std::to_string(height), "-q", "82", "-o", jpeg_path},
                               "", 15000, 4096, true);
    struct stat st;
    if (res.timed_out) return "the camera did not respond";
    if (stat(jpeg_path.c_str(), &st) != 0 || st.st_size < 1000) return "the camera failed to take a picture";
    return "";
}

}  // namespace camera
