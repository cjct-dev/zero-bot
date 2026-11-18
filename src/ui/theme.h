// Colours, fonts and small drawing helpers shared by every screen.
#pragma once
#include "lvgl.h"

namespace theme {

constexpr int kScreenW = 320;
constexpr int kScreenH = 170;
constexpr int kStatusH = 18;
constexpr int kFooterH = 18;

constexpr uint32_t kBg = 0x0B0E14;
constexpr uint32_t kPanel = 0x151B26;
constexpr uint32_t kPanelHi = 0x1F2937;
constexpr uint32_t kLine = 0x263041;
constexpr uint32_t kText = 0xE6EDF3;
constexpr uint32_t kDim = 0x8B95A5;
constexpr uint32_t kFaint = 0x586274;
constexpr uint32_t kAccent = 0x4FD1C5;     // teal: the assistant
constexpr uint32_t kUser = 0x1E3A5F;       // user bubble
constexpr uint32_t kListen = 0xF87171;     // red: microphone live
constexpr uint32_t kThink = 0xFBBF24;      // amber: working
constexpr uint32_t kOk = 0x86EFAC;

void init();                       // load fonts; call once after lv_init
const lv_font_t *body();           // 12 px text
const lv_font_t *small();          // 10 px captions and hints
const lv_font_t *bold();           // 12 px emphasis
const lv_font_t *large();          // 16 px bold, for what must be read at a glance (a timer's alarm)
const lv_font_t *icons();          // LVGL symbol font

lv_color_t color(uint32_t hex);
// A plain container with no padding, border or scrollbars.
lv_obj_t *box(lv_obj_t *parent, int w, int h, uint32_t bg, bool transparent = false);
lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t col);
// Sizes a label to its text, padding included, up to max_w; longer text wraps within max_w. Call
// after setting the text. LVGL's own sizing (width LV_SIZE_CONTENT with a max width) measured the
// text without the padding, so a line that fitted max_w but not max_w less the padding wrapped
// onto a second line the label left no room for, and its last word vanished.
void fit_width(lv_obj_t *l, int32_t max_w);
// True when a label is tall enough for every line its text wraps into at its current width.
bool label_fits(lv_obj_t *l);

}  // namespace theme
