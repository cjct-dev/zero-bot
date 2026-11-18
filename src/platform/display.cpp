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
lv_obj_t *g_cover = nullptr;      // black, over everything, while the screen is off
bool g_off = false;
unsigned long g_refreshes = 0;

uint32_t tick_cb() { return static_cast<uint32_t>(now_ms()); }

void headless_flush(lv_display_t *disp, const lv_area_t *, uint8_t *) { lv_display_flush_ready(disp); }

// While the screen is off the display's refresh timer runs this instead of LVGL's refresh: it only
// pauses itself again. (A change of size or layout asks for a refresh even with nothing to draw.)
void refresh_nothing(lv_timer_t *t) { lv_timer_pause(t); }

// Counts refresh cycles (LVGL sends this one even when there is nothing to redraw).
void watch(lv_display_t *disp) {
    lv_display_add_event_cb(disp, [](lv_event_t *) { ++g_refreshes; }, LV_EVENT_REFR_START, nullptr);
}
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
        watch(disp);
        return;
    }
#else
    if (mode == Mode::Device) {
        const char *fb = getenv("APPLAUNCH_LINUX_FBDEV_DEVICE");
        lv_display_t *disp = lv_linux_fbdev_create();
        lv_linux_fbdev_set_file(disp, fb && *fb ? fb : "/dev/fb0");
        watch(disp);
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
    watch(disp);
}

// The panel's own blanking (FBIOBLANK) is not used: its driver (panel-mipi-dbi) only switches the
// backlight off for it, and, as the panel cannot be read back, resets and sets up the panel again
// on every wake. The backlight is switched off the way the launcher does it (brightness 0).
void screen_off() {
    if (g_off) return;
    g_off = true;
    if (!g_cover) {
        g_cover = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(g_cover);
        lv_obj_set_size(g_cover, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(g_cover, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(g_cover, LV_OPA_COVER, 0);
        lv_obj_remove_flag(g_cover, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(g_cover, [](lv_event_t *) { g_cover = nullptr; }, LV_EVENT_DELETE, nullptr);
    }
    lv_obj_remove_flag(g_cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_cover);
    lv_display_t *disp = lv_display_get_default();
    lv_refr_now(disp);                                // black on the panel (liquid crystal at rest)...
    // ...and nothing more: a change marks nothing for redrawing, and no refresh runs (not even to
    // lay out what changed), so no pixel goes over SPI. Changes are laid out and drawn on waking.
    lv_display_enable_invalidation(disp, false);
    if (lv_timer_t *t = lv_display_get_refr_timer(disp)) {
        lv_timer_set_cb(t, refresh_nothing);
        lv_timer_pause(t);
    }
    backlight_off();
}

void screen_on(int percent) {
    if (g_off) {
        g_off = false;
        lv_display_t *disp = lv_display_get_default();
        lv_display_enable_invalidation(disp, true);
        // LVGL's own refresh again (the fbdev, SDL and headless displays all use it unchanged).
        if (lv_timer_t *t = lv_display_get_refr_timer(disp)) lv_timer_set_cb(t, lv_display_refr_timer);
        if (g_cover) lv_obj_add_flag(g_cover, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(lv_screen_active());        // whatever changed meanwhile was not marked
        lv_refr_now(disp);                            // drawn before the light comes on
    }
    set_brightness(percent);
}

bool screen_is_off() { return g_off; }

unsigned long refreshes() { return g_refreshes; }

bool frame_black() {
    if (g_mode != Mode::Headless || g_headless_buf.empty()) return false;
    for (size_t i = 0; i < g_headless_buf.size(); i += 4)
        if (g_headless_buf[i] || g_headless_buf[i + 1] || g_headless_buf[i + 2]) return false;
    return true;
}

bool frame_current() {
    if (g_mode != Mode::Headless || g_headless_buf.empty()) return false;
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_XRGB8888);
    if (!snap) return false;
    bool same = snap->header.w == kWidth && snap->header.h == kHeight;
    for (uint32_t y = 0; same && y < snap->header.h; ++y) {
        const uint8_t *a = snap->data + static_cast<size_t>(y) * snap->header.stride;
        const uint8_t *b = g_headless_buf.data() + static_cast<size_t>(y) * kWidth * 4;
        for (uint32_t x = 0; same && x < snap->header.w; ++x)
            same = memcmp(a + x * 4, b + x * 4, 3) == 0;   // blue, green, red
    }
    lv_draw_buf_destroy(snap);
    return same;
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
