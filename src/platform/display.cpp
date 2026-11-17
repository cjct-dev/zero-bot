#include "platform/display.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include <png.h>

#include "lvgl.h"

namespace display {

namespace {
Mode g_mode = Mode::Headless;
std::vector<uint8_t> g_headless_buf;

uint32_t tick_cb() { return static_cast<uint32_t>(now_ms()); }

void headless_flush(lv_display_t *disp, const lv_area_t *, uint8_t *) { lv_display_flush_ready(disp); }
}  // namespace

void init(Mode mode) {
    g_mode = mode;
    lv_init();
    lv_tick_set_cb(tick_cb);

#if ZB_DESKTOP
    if (mode == Mode::Window) {
        lv_display_t *disp = lv_sdl_window_create(kWidth, kHeight);
        lv_sdl_window_set_zoom(disp, 3);
        lv_sdl_window_set_title(disp, "Zero Bot simulator");
        return;
    }
#else
    if (mode == Mode::Device) {
        const char *fb = getenv("APPLAUNCH_LINUX_FBDEV_DEVICE");
        lv_display_t *disp = lv_linux_fbdev_create();
        lv_linux_fbdev_set_file(disp, fb && *fb ? fb : "/dev/fb0");
        return;
    }
#endif
    // Headless: render into memory only, used for scripted screenshots.
    g_mode = Mode::Headless;
    lv_display_t *disp = lv_display_create(kWidth, kHeight);
    g_headless_buf.resize(static_cast<size_t>(kWidth) * kHeight * 4);
    lv_display_set_buffers(disp, g_headless_buf.data(), nullptr, g_headless_buf.size(),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, headless_flush);
}

bool save_png(const std::string &path) {
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888);
    if (!snap) return false;
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    img.width = snap->header.w;
    img.height = snap->header.h;
    img.format = PNG_FORMAT_BGRA;  // LVGL ARGB8888 is B,G,R,A in memory
    // Force opaque alpha so viewers do not treat the frame as translucent.
    for (uint32_t y = 0; y < snap->header.h; ++y) {
        uint8_t *row = snap->data + static_cast<size_t>(y) * snap->header.stride;
        for (uint32_t x = 0; x < snap->header.w; ++x) row[x * 4 + 3] = 0xff;
    }
    const bool ok = png_image_write_to_file(&img, path.c_str(), 0, snap->data,
                                            static_cast<png_int_32>(snap->header.stride), nullptr) != 0;
    lv_draw_buf_destroy(snap);
    return ok;
}

}  // namespace display
