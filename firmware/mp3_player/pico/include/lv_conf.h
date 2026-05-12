#if 1  /* Guard: set to 1 to enable this file */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH     16
#define LV_COLOR_16_SWAP   1   /* ILI9341 uses big-endian; TFT_eSPI swaps for us */

#define LV_MEM_CUSTOM      0
#define LV_MEM_SIZE        (48U * 1024U)  /* 48 KB from SRAM */

#define LV_DISP_DEF_REFR_PERIOD  33  /* ~30 fps */
#define LV_INDEV_DEF_READ_PERIOD 30

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0

/* Font: only embed what we need to save flash */
#define LV_FONT_MONTSERRAT_12  1
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_18  1
#define LV_FONT_MONTSERRAT_24  1
#define LV_FONT_DEFAULT        &lv_font_montserrat_14

/* Widgets */
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  0
#define LV_USE_CANVAS     1   /* album art */
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1   /* album art */
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_ROLLER     0
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1
#define LV_USE_TABLE      0
#define LV_USE_LIST       1
#define LV_USE_METER      0
#define LV_USE_MSGBOX     0
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    1
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0
#define LV_USE_SPAN       0

/* Layout */
#define LV_USE_FLEX  1
#define LV_USE_GRID  0

/* Logging */
#define LV_USE_LOG     0
#define LV_USE_ASSERT  0

#endif /* LV_CONF_H */
#endif /* Guard */
