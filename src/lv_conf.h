/* LVGL 9.5 configuration for Zero Bot (device + desktop simulator). */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 32

#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_NONE
/* LVGL's redraw and animation timers: 30 a second at most. The panel is fed over SPI by the kernel,
 * which costs about five times what drawing does, so an animation (a scroll, a long title moving in the
 * status bar while music plays) costs what it sends: half at 33 ms of what it was at 16. */
#define LV_DEF_REFR_PERIOD 33
#define LV_DPI_DEF 130

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

#define LV_USE_FLOAT 1
#define LV_USE_OBSERVER 0
#define LV_USE_SNAPSHOT 1
#define LV_USE_SYSMON 0

#define LV_DRAW_SW_COMPLEX 1
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#define LV_DRAW_BUF_ALIGN 4
#define LV_DRAW_BUF_STRIDE_ALIGN 4

/* Fonts: FreeType renders the system TTFs; Montserrat is the fallback. */
#define LV_USE_FREETYPE 1
#define LV_FREETYPE_CACHE_FT_GLYPH_CNT 512
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 0             /* LVGL's default is 1: unused (theme.cpp falls back on 10, 12 and 16) */
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_DEFAULT &lv_font_montserrat_12
#define LV_USE_FONT_PLACEHOLDER 1
#define LV_TXT_ENC LV_TXT_ENC_UTF8

/* Images: PNG only (a photo reaches the screen as a PNG thumbnail: App::wire_agent) */
#define LV_USE_LIBPNG 1
#define LV_USE_FS_STDIO 1
#define LV_FS_STDIO_LETTER 'A'
#define LV_FS_STDIO_PATH ""

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_USE_FLEX 1
#define LV_USE_GRID 0                       /* LVGL's default is 1: no screen lays out in a grid */

#if ZB_DESKTOP
#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH <SDL2/SDL.h>
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT 1
#define LV_SDL_FULLSCREEN 0
#define LV_SDL_DIRECT_EXIT 0
#define LV_SDL_MOUSEWHEEL_MODE LV_SDL_MOUSEWHEEL_MODE_ENCODER
#else
#define LV_USE_LINUX_FBDEV 1
#define LV_LINUX_FBDEV_BSD 0
#define LV_LINUX_FBDEV_RENDER_MODE LV_DISPLAY_RENDER_MODE_PARTIAL
#define LV_LINUX_FBDEV_BUFFER_COUNT 0
#define LV_LINUX_FBDEV_BUFFER_SIZE 60
#define LV_LINUX_FBDEV_MMAP 1
#endif

#endif /* LV_CONF_H */
