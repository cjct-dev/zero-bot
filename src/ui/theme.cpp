#include "ui/theme.h"

#include <initializer_list>

#include <unistd.h>

namespace theme {

namespace {
lv_font_t *g_body = nullptr, *g_small = nullptr, *g_bold = nullptr;

const char *first_existing(std::initializer_list<const char *> paths) {
    for (const char *p : paths)
        if (access(p, R_OK) == 0) return p;
    return nullptr;
}

lv_font_t *load(const char *path, int size, const lv_font_t *fallback) {
    if (!path) return nullptr;
    lv_font_t *f = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, static_cast<uint32_t>(size),
                                           LV_FREETYPE_FONT_STYLE_NORMAL);
    if (f) f->fallback = fallback;
    return f;
}
}  // namespace

void init() {
    const char *regular = first_existing({"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                          "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                          "/usr/share/fonts/dejavu/DejaVuSans.ttf"});
    const char *strong = first_existing({"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                         "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                                         "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"});
    // Chinese, Japanese and Korean glyphs come from Noto when it is installed, so
    // translations display properly.
    const char *cjk = first_existing({"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                      "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"});
    lv_font_t *cjk12 = load(cjk, 12, &lv_font_montserrat_12);
    lv_font_t *cjk10 = load(cjk, 10, &lv_font_montserrat_10);
    g_body = load(regular, 12, cjk12 ? cjk12 : &lv_font_montserrat_12);
    g_small = load(regular, 10, cjk10 ? cjk10 : &lv_font_montserrat_10);
    g_bold = load(strong, 12, cjk12 ? cjk12 : &lv_font_montserrat_12);
}

const lv_font_t *body() { return g_body ? g_body : &lv_font_montserrat_12; }
const lv_font_t *small() { return g_small ? g_small : &lv_font_montserrat_10; }
const lv_font_t *bold() { return g_bold ? g_bold : body(); }
const lv_font_t *icons() { return &lv_font_montserrat_12; }

lv_color_t color(uint32_t hex) { return lv_color_hex(hex); }

lv_obj_t *box(lv_obj_t *parent, int w, int h, uint32_t bg, bool transparent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, color(bg), 0);
    lv_obj_set_style_bg_opa(o, transparent ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t col) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color(col), 0);
    lv_label_set_text(l, text);
    return l;
}

}  // namespace theme
