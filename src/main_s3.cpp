#include <Arduino.h>
#include "lvgl.h"
#include <TFT_eSPI.h>
#include "esp_heap_caps.h"
#include "pins_s3.h"

#include "data/aircraft.h"
#include "data/fetcher.h"
#include "data/storage.h"
#include "data/error_log.h"
#include "data/enrichment.h"
#include "ui_s3/status_bar.h"
#include "ui_s3/views.h"
#include "ui_s3/detail_card.h"
#include "ui_s3/alerts.h"
#include "ui_s3/settings.h"

// Global touch state — read by view timers to defer heavy rendering during interaction
volatile bool touch_active = false;

// TFT display driver
static TFT_eSPI tft = TFT_eSPI();

// Aircraft data
AircraftList aircraft_list;

// LVGL display
static lv_display_t *disp;
static uint16_t *buf0;
static uint16_t *buf1;

// Display flush callback for TFT_eSPI
static void disp_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *color_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)color_map, w * h, true);
    tft.endWrite();
    lv_display_flush_ready(d);
}

// Touch read callback (XPT2046 via TFT_eSPI)
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    uint16_t x, y;
    if (tft.getTouch(&x, &y, 40)) {  // threshold 40 for resistive noise
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
        touch_active = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        touch_active = false;
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("ADS-B Display (S3) starting...");

    Serial.printf("Heap free: %lu  PSRAM free: %lu\n",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Init TFT display
    tft.init();
    tft.setRotation(1);  // landscape: 480x320
    tft.fillScreen(TFT_BLACK);
    Serial.println("TFT initialized");

    // Touch calibration — adjust after hardware testing
    // Format: {x_min, x_max, y_min, y_max} for landscape rotation=1
    uint16_t cal[5] = {300, 3600, 300, 3600, 1};
    tft.setTouch(cal);
    Serial.println("Touch initialized (default calibration)");

    // Init LVGL
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return (uint32_t)millis(); });

    // Render buffers in PSRAM — 480x32 lines (2 x ~30KB) for PARTIAL mode
    uint32_t buf_size = LCD_H_RES * 32;
    buf0 = (uint16_t *)heap_caps_malloc(buf_size * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    buf1 = (uint16_t *)heap_caps_malloc(buf_size * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    assert(buf0 && buf1);

    // Create LVGL display
    disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, buf0, buf1, buf_size * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Create LVGL touch input device
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, disp);
    lv_indev_set_scroll_limit(indev, 20);  // higher for resistive touch jitter

    // Poll touch at 10ms
    lv_timer_set_period(lv_indev_get_read_timer(indev), 10);

    // Init aircraft data
    aircraft_list.init();

    // Create UI
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    Serial.println("Creating status bar...");
    status_bar_create(screen);

    Serial.println("views_init...");
    views_init(screen, &aircraft_list);

    Serial.println("detail_card_init...");
    detail_card_init(screen);

    Serial.println("alerts_init...");
    alerts_init(screen);

    Serial.println("settings_init...");
    settings_init(screen);

    // Load runtime config
    g_config = storage_load_config();

    status_bar_set_gear_callback([](lv_event_t *e) {
        settings_show();
    });

    settings_set_change_callback([](const UserConfig *cfg) {
        g_config = *cfg;
    });

    // Periodic status bar update
    lv_timer_create([](lv_timer_t *timer) {
        status_bar_update(fetcher_wifi_connected(), aircraft_list.count, fetcher_last_update());
    }, 1000, nullptr);

    Serial.println("LVGL initialized - UI ready");
    Serial.printf("Heap free: %lu  PSRAM free: %lu\n",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    error_log_init();
    enrichment_init();
    fetcher_init(&aircraft_list);
}

void loop() {
    lv_timer_handler();

    // Heap monitor — log every 10s
    static uint32_t last_heap_log = 0;
    uint32_t now = millis();
    if (now - last_heap_log >= 10000) {
        last_heap_log = now;
        Serial.printf("HEAP int=%lu min=%lu  PSRAM=%lu\n",
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    vTaskDelay(pdMS_TO_TICKS(1));
}
