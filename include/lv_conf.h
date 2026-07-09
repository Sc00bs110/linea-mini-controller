#if 1  // Set to 1 to enable — guard required by LVGL

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// --- Colour depth ---
#define LV_COLOR_DEPTH 16       // DFR1092 (ILI9488 via LovyanGFX) uses RGB565

// --- Memory ---
// On PSRAM boards (S3), LVGL's heap lives in PSRAM (ps_malloc): the 48 KB
// static internal-RAM pool starved mbedtls, whose TLS buffers MUST be internal
// — the GitHub OTA check failed with "SSL - Memory allocation failed" at
// ~32 KB internal free (field-debugged 2026-07-09). Widget allocs from PSRAM
// are fine perf-wise on the S3's octal PSRAM; the 10-row draw buffer stays
// internal in display.cpp. The C6 (no PSRAM) keeps the static pool.
#ifdef BOARD_HAS_PSRAM
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE "esp32-hal-psram.h"
#define LV_MEM_CUSTOM_ALLOC   ps_malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_CUSTOM_REALLOC ps_realloc
#else
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE   (48U * 1024U)   // 48 KB heap for LVGL
#endif

// --- HAL tick ---
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE  "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

// --- Display resolution (max) ---
#define LV_HOR_RES_MAX 480      // DFR1092 landscape (setRotation(5) -> 480 wide)
#define LV_VER_RES_MAX 480      // >= tallest axis; actual ver_res (320) set in disp_drv

// --- Drawing ---
#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0
#define LV_IMG_CACHE_DEF_SIZE 0
#define LV_GRADIENT_MAX_STOPS 2
#define LV_GRAD_CACHE_DEF_SIZE 0

// --- GPU ---
#define LV_USE_GPU_STM32_DMA2D 0
#define LV_USE_GPU_NXP_PXP 0
#define LV_USE_GPU_NXP_VG_LITE 0
#define LV_USE_GPU_SDL 0

// --- Logging ---
#define LV_USE_LOG 0

// --- Asserts ---
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

// --- Fonts ---
#define LV_FONT_MONTSERRAT_12  0
#define LV_FONT_MONTSERRAT_14  0
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_MONTSERRAT_18  0
#define LV_FONT_MONTSERRAT_20  1
#define LV_FONT_MONTSERRAT_22  0
#define LV_FONT_MONTSERRAT_24  1
#define LV_FONT_MONTSERRAT_26  0
#define LV_FONT_MONTSERRAT_28  0
#define LV_FONT_MONTSERRAT_30  0
#define LV_FONT_MONTSERRAT_32  1
#define LV_FONT_MONTSERRAT_34  0
#define LV_FONT_MONTSERRAT_36  0
#define LV_FONT_MONTSERRAT_38  0
#define LV_FONT_MONTSERRAT_40  0
#define LV_FONT_MONTSERRAT_42  0
#define LV_FONT_MONTSERRAT_44  0
#define LV_FONT_MONTSERRAT_46  0
#define LV_FONT_MONTSERRAT_48  1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_SUBPX 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_BUILT_IN_FONT_SUBPX_BGR 0

// --- Text ---
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_COLOR_CMD "#"
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

// --- Widgets ---
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     0
#define LV_USE_CHECKBOX   0
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_ROLLER     0
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   0
#define LV_USE_TABLE      0

// --- Extra widgets ---
#define LV_USE_ANIMIMG    0
#define LV_USE_CALENDAR   0
#define LV_USE_CHART      0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN     0
#define LV_USE_KEYBOARD   0
#define LV_USE_LED        1
#define LV_USE_LIST       1
#define LV_USE_MENU       1
#define LV_USE_METER      0
#define LV_USE_MSGBOX     1
#define LV_USE_SPAN       0
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    1
#define LV_USE_TABVIEW    1
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0

// --- Layouts ---
#define LV_USE_FLEX  1
#define LV_USE_GRID  1

// --- Themes ---
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1     // dark theme by default
#define LV_THEME_DEFAULT_GROW 1
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0

// --- Misc ---
#define LV_USE_SNAPSHOT 0
#define LV_USE_MONKEY 0
#define LV_USE_GRIDNAV 0
#define LV_USE_FRAGMENT 0
#define LV_USE_IMGFONT 0
#define LV_USE_MSG 0
#define LV_USE_IME_PINYIN 0

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0
#define LV_USE_REFR_DEBUG   0

#define LV_USE_USER_DATA 1
#define LV_ENABLE_GC 0

#endif  // LV_CONF_H
#endif  // #if 1
