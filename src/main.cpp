#include <Arduino.h>
#include "lvgl.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_heap_caps.h"
#include "pins_config.h"
#include "config.h"
#include "hal/jd9165_lcd.h"
#include "hal/gt911_touch.h"
#include "data/aircraft.h"
#include "data/fetcher.h"
#include "ui/status_bar.h"
#include "ui/views.h"
#include "ui/detail_card.h"
#include "ui/alerts.h"
#include "ui/settings.h"
#include "ui/range.h"
#include "ui/tile_cache.h"
#include "ui/map_view.h"
#include "ui/radar_view.h"
#include "ui/arrivals_view.h"
#include "data/storage.h"
#include "data/error_log.h"
#include "data/enrichment.h"

// Global touch state — read by view timers to defer heavy rendering during interaction
volatile bool touch_active = false;

// Hardware drivers
static jd9165_lcd lcd(LCD_RST);
static gt911_touch touch(TP_I2C_SDA, TP_I2C_SCL, TP_RST, TP_INT);

// Aircraft data
AircraftList aircraft_list;

// LVGL display
static lv_display_t *disp;
static uint16_t *buf0;
static uint16_t *buf1;

// Display flush callback
static void disp_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *color_map) {
    lcd.lcd_draw_bitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, (uint16_t *)color_map);
}

// Vsync callback — signals LVGL that flush is complete
static bool flush_ready_cb(esp_lcd_panel_handle_t panel,
    esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx) {
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

// Touch read callback
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    uint16_t x, y;
    if (touch.getTouch(&x, &y)) {
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
    Serial.println("ADS-B Display starting...");

    Serial.printf("Heap free: %lu  PSRAM free: %lu\n",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Init I2C bus 1 (MUST be before touch.begin())
    i2c_master_bus_handle_t i2c_handle = NULL;
    i2c_master_bus_config_t i2c_bus_conf = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = (gpio_num_t)TP_I2C_SDA,
        .scl_io_num = (gpio_num_t)TP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    i2c_new_master_bus(&i2c_bus_conf, &i2c_handle);
    Serial.println("I2C bus initialized");

    // Init display hardware
    lcd.begin();
    Serial.println("LCD initialized");

    // Init touch hardware
    touch.begin();
    Serial.println("Touch initialized");

    // Init LVGL
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return (uint32_t)millis(); });

    // Allocate render buffers in PSRAM — 1/2 screen for PARTIAL mode
    // Larger buffers = fewer render passes per frame = better touch response
    uint32_t buf_size = LCD_H_RES * LCD_V_RES / 2;
    buf0 = (uint16_t *)heap_caps_malloc(buf_size * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    buf1 = (uint16_t *)heap_caps_malloc(buf_size * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    assert(buf0 && buf1);

    // Create LVGL display — PARTIAL mode only redraws dirty regions
    disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, buf0, buf1, buf_size * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Register vsync callback for proper flush synchronization
    bsp_lcd_handles_t lcd_handles;
    lcd.get_handle(&lcd_handles);
    esp_lcd_dpi_panel_event_callbacks_t cbs = {};
    cbs.on_color_trans_done = flush_ready_cb;
    esp_lcd_dpi_panel_register_event_callbacks(lcd_handles.panel, &cbs, disp);

    // Create LVGL touch input device
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, disp);
    lv_indev_set_scroll_limit(indev, 10);

    // Poll touch at 10ms (vs 30ms default) — catches fast taps between render frames
    lv_timer_set_period(lv_indev_get_read_timer(indev), 10);

    // Init aircraft data
    aircraft_list.init();

    // Load config before UI so views init with the correct range
    g_config = storage_load_config();

    // Create UI — LVGL must be fully set up before background tasks
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    Serial.println("Creating status bar...");
    status_bar_create(screen);
    Serial.println("Status bar OK");

    Serial.println("views_init...");
    range_set_default(g_config.radius_nm);
    views_init(screen, &aircraft_list);
    Serial.println("views OK");

    Serial.println("detail_card_init...");
    detail_card_init(screen);
    Serial.println("detail_card OK");

    Serial.println("alerts_init...");
    alerts_init(screen);
    Serial.println("alerts OK");

    Serial.println("settings_init...");
    settings_init(screen);
    Serial.println("settings OK");

    status_bar_set_gear_callback([](lv_event_t *e) {
        settings_show();
    });

    settings_set_change_callback([](const UserConfig *cfg) {
        g_config = *cfg;
        range_set_default(cfg->radius_nm);
    });

    // Periodic status bar update — count matches what's drawn on the map
    lv_timer_create([](lv_timer_t *timer) {
        status_bar_update(fetcher_wifi_connected(), map_view_drawn_count(), fetcher_last_update());
    }, 1000, nullptr);

    Serial.println("LVGL initialized - UI ready");
    Serial.printf("Heap free: %lu  PSRAM free: %lu\n",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    error_log_init();
    enrichment_init();
    fetcher_init(&aircraft_list);
    // tile_cache_init(); // disabled: lv_draw_image broken on ESP32-P4 PPA
}

void loop() {
    lv_timer_handler();

    // Heap monitor — log every 10s for crash diagnosis
    static uint32_t last_heap_log = 0;
    uint32_t now = millis();
    if (now - last_heap_log >= 10000) {
        last_heap_log = now;
        Serial.printf("HEAP int=%lu min=%lu  PSRAM=%lu\n",
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    // Yield briefly to FreeRTOS — 1ms instead of 5ms for better touch responsiveness
    vTaskDelay(pdMS_TO_TICKS(1));
}
