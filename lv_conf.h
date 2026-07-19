#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Memory — custom allocator routes to PSRAM (internal RAM exhausted by MIPI DSI) */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CUSTOM
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

/* Tick — provided via lv_tick_set_cb() in code */

/* Display */
#define LV_USE_LOG 0

/* Fonts - enable the ones we need */
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1

/* Widgets */
#define LV_USE_LABEL 1
#define LV_USE_BTN 1
#define LV_USE_IMG 1
#define LV_USE_LINE 1
#define LV_USE_ARC 1
#define LV_USE_TABLE 1
#define LV_USE_CANVAS 1
#define LV_USE_TILEVIEW 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_TEXTAREA 1
#define LV_USE_KEYBOARD 1
#define LV_USE_LIST 1
#define LV_USE_MSGBOX 1
#define LV_USE_ROLLER 1
#define LV_USE_DROPDOWN 1
#define LV_USE_CHECKBOX 1
#define LV_USE_SPINNER 1

/* Animations */
#define LV_USE_ANIM 1

/* Drawing */
#define LV_DRAW_SW_DRAW_UNIT_CNT 1

/* Image decoders */
#define LV_USE_LODEPNG 1

#endif /* LV_CONF_H */
