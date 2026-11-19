#include "platform/camera.h"

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include "agent/experiment.h"
#include "core/util.h"
#include "platform/proc.h"

namespace camera {

std::string capture(const std::string &jpeg_path, int width, int height, bool upside_down, const std::atomic<bool> *cancel) {
    // The last photo stays on disk between calls; a capture that fails without writing must not
    // leave it there to be read as this one.
    unlink(jpeg_path.c_str());
    // ZB_FAKE_CAMERA=<file.jpg> lets the simulator and tests supply a picture.
    if (const char *fake = getenv("ZB_FAKE_CAMERA"); fake && *fake) {
        std::string data;
        if (!util::read_file(fake, data)) return "camera test image not found";
        return util::write_file_atomic(jpeg_path, data) ? "" : "could not save the photo";
    }
    // A test run (ZB_EXP_NO_HARDWARE, experiment builds only) never takes a real photo.
    if (std::string off = experiment::no_camera(); !off.empty()) return off;
    if (!proc::have("rpicam-jpeg")) return "this device has no camera";
    // The CardputerZero's sensor is mounted the other way up from how the device is held.
    const auto res = proc::run({"rpicam-jpeg", "-n", "-t", "700", "--width", std::to_string(width), "--height",
                                std::to_string(height), "--rotation", upside_down ? "180" : "0", "-q", "82", "-o", jpeg_path},
                               "", 15000, 4096, true, cancel);
    struct stat st;
    if (res.cancelled) { unlink(jpeg_path.c_str()); return "the photo was cancelled"; }
    if (res.timed_out) { unlink(jpeg_path.c_str()); return "the camera did not respond"; }
    if (res.code != 0 || stat(jpeg_path.c_str(), &st) != 0 || st.st_size < 1000) {
        unlink(jpeg_path.c_str());
        return "the camera failed to take a picture";
    }
    return "";
}

}  // namespace camera
