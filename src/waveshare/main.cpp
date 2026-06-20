#include <Arduino.h>
#include "lvgl.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "pins.h"

#include "rgb_lcd.h"
#include "../hal/gt911_touch.h"
#include "../data/aircraft.h"
#include "../data/fetcher.h"
#include "../ui/status_bar.h"
#include "../ui/views.h"
#include "../ui/detail_card.h"
#include "../ui/alerts.h"
#include "../ui/settings.h"
#include "../ui/map_view.h"
#include "../ui/radar_view.h"
#include "../ui/arrivals_view.h"
#include "../data/storage.h"
#include "../data/error_log.h"
#include "../hal/encoder_input.h"
#include "../ui/input_nav.h"
#include "../data/enrichment.h"

// Global touch state — read by view timers to defer heavy rendering during interaction
volatile bool touch_active = false;

// Hardware drivers
static rgb_lcd lcd;
static gt911_touch touch(I2C_SDA, I2C_SCL, -1, TP_INT);  // RST via IO expander, not direct GPIO

// Aircraft data
AircraftList aircraft_list;

// LVGL display
static lv_display_t *disp;
static uint16_t *buf0;
static uint16_t *buf1;

// I2C bus handle (shared by IO expander and GT911)
static i2c_master_bus_handle_t i2c_handle = NULL;

// Use UART for serial debug
#define DBG Serial0

// ---- IO Expander (CH422G / CH32V003 at 0x24) ----
// Simple I2C control for setting EXIO output pins

static i2c_master_dev_handle_t io_exp_dev = NULL;

static bool io_exp_init() {
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = IO_EXP_ADDR;
    dev_cfg.scl_speed_hz = 100000;
    esp_err_t err = i2c_master_bus_add_device(i2c_handle, &dev_cfg, &io_exp_dev);
    if (err != ESP_OK) {
        DBG.printf("IO Expander: failed to add device (0x%02X): %d\n", IO_EXP_ADDR, err);
        return false;
    }
    DBG.println("IO Expander initialized");
    return true;
}

static void io_exp_set_pin(uint8_t pin, bool value) {
    if (!io_exp_dev) return;
    // CH422G/CH32V003 protocol: write [register, data]
    // Output register at 0x03, set bit for pin
    // Read current state, modify bit, write back
    uint8_t read_cmd = 0x03;
    uint8_t current = 0;
    // Try to read current output state
    i2c_master_transmit_receive(io_exp_dev, &read_cmd, 1, &current, 1, 100);
    if (value) {
        current |= (1 << pin);
    } else {
        current &= ~(1 << pin);
    }
    uint8_t data[2] = { 0x03, current };
    i2c_master_transmit(io_exp_dev, data, 2, 100);
}

// Display flush callback — draw partial area to RGB panel framebuffer
static void disp_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *color_map) {
    lcd.lcd_draw_bitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, (uint16_t *)color_map);
    lv_display_flush_ready(d);
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
    Serial0.begin(115200);
    delay(200);
    DBG.println("ADS-B Display (Waveshare S3-7B) starting...");

    DBG.printf("Heap free: %lu  PSRAM free: %lu\n",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Init I2C bus for IO expander + GT911 touch
    i2c_master_bus_config_t i2c_bus_conf = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = (gpio_num_t)I2C_SDA,
        .scl_io_num = (gpio_num_t)I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    i2c_new_master_bus(&i2c_bus_conf, &i2c_handle);
    DBG.println("I2C bus initialized");

    // Init IO expander — needed for backlight and touch reset
    if (io_exp_init()) {
        // Enable backlight
        io_exp_set_pin(EXIO_DISP, true);
        DBG.println("Backlight enabled via IO expander");

        // Pulse touch reset via IO expander
        io_exp_set_pin(EXIO_TP_RST, false);
        vTaskDelay(pdMS_TO_TICKS(10));
        io_exp_set_pin(EXIO_TP_RST, true);
        vTaskDelay(pdMS_TO_TICKS(50));
        DBG.println("GT911 reset via IO expander");
    }

    // Init display hardware
    lcd.begin();
    DBG.println("LCD initialized");

    // GT911 touch — skip if init fails
    bool touch_ok = false;
    {
        touch.begin_safe();
        touch_ok = touch.is_ready();
        if (touch_ok) {
            DBG.println("Touch initialized");
        } else {
            DBG.println("WARNING: Touch init failed — display will work without touch");
        }
    }

    // Init LVGL
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return (uint32_t)millis(); });

    // Render buffers in PSRAM — 1024×30 lines double-buffered for PARTIAL mode
    uint32_t buf_size = LCD_H_RES * 30;
    buf0 = (uint16_t *)heap_caps_malloc(buf_size * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    buf1 = (uint16_t *)heap_caps_malloc(buf_size * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    assert(buf0 && buf1);

    // Create LVGL display — PARTIAL mode with double-buffered LCD for reduced tearing
    disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, buf0, buf1, buf_size * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Create LVGL touch input device
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, disp);
    lv_indev_set_scroll_limit(indev, 10);

    // Poll touch at 10ms
    lv_timer_set_period(lv_indev_get_read_timer(indev), 10);

    // --- Boot splash: military LCD style ---
    {
        lv_obj_t *splash = lv_screen_active();
        lv_obj_set_style_bg_color(splash, lv_color_hex(0x0a1a0a), 0);
        lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);

        // Outer border — phosphor green scanline feel
        lv_obj_t *border = lv_obj_create(splash);
        lv_obj_set_size(border, 500, 200);
        lv_obj_center(border);
        lv_obj_set_style_bg_color(border, lv_color_hex(0x0a1a0a), 0);
        lv_obj_set_style_bg_opa(border, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(border, lv_color_hex(0x33ff33), 0);
        lv_obj_set_style_border_width(border, 2, 0);
        lv_obj_set_style_radius(border, 0, 0);
        lv_obj_set_style_pad_all(border, 20, 0);
        lv_obj_clear_flag(border, LV_OBJ_FLAG_SCROLLABLE);

        // Title
        lv_obj_t *title = lv_label_create(border);
        lv_label_set_text(title, "ADS-B DISPLAY");
        lv_obj_set_style_text_color(title, lv_color_hex(0x33ff33), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

        // Model line
        lv_obj_t *model = lv_label_create(border);
        lv_label_set_text(model, "MODEL BR549");
        lv_obj_set_style_text_color(model, lv_color_hex(0x22cc22), 0);
        lv_obj_set_style_text_font(model, &lv_font_montserrat_20, 0);
        lv_obj_align(model, LV_ALIGN_CENTER, 0, -5);

        // Loading text
        lv_obj_t *loading = lv_label_create(border);
        lv_label_set_text(loading, "LOADING . . .");
        lv_obj_set_style_text_color(loading, lv_color_hex(0x55ff55), 0);
        lv_obj_set_style_text_font(loading, &lv_font_montserrat_16, 0);
        lv_obj_align(loading, LV_ALIGN_BOTTOM_MID, 0, -10);

        // Render the splash screen
        lv_timer_handler();
        lv_timer_handler();
    }

    // Init aircraft data
    aircraft_list.init();

    // Create UI — LVGL must be fully set up before background tasks
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clean(screen);  // remove splash

    DBG.println("Creating status bar...");
    status_bar_create(screen);
    DBG.println("Status bar OK");

    DBG.println("views_init...");
    views_init(screen, &aircraft_list);
    DBG.println("views OK");

    DBG.println("detail_card_init...");
    detail_card_init(screen);
    DBG.println("detail_card OK");

    DBG.println("alerts_init...");
    alerts_init(screen);
    DBG.println("alerts OK");

    DBG.println("settings_init...");
    settings_init(screen);
    DBG.println("settings OK");

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

    // Init rotary encoder + back button for non-touch navigation
    encoder_input_init(ENC_CLK, ENC_DT, ENC_SW, BTN_BACK);
    input_nav_init(&aircraft_list);
    lv_timer_create([](lv_timer_t *t) { input_nav_tick(); }, 20, nullptr);
    DBG.println("Encoder input initialized");

    DBG.println("LVGL initialized - UI ready");
    DBG.printf("Heap free: %lu  PSRAM free: %lu\n",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    error_log_init();
    enrichment_init();
    fetcher_init(&aircraft_list);
}

void loop() {
    lv_timer_handler();

    // Heap monitor — log every 10s for crash diagnosis
    static uint32_t last_heap_log = 0;
    uint32_t now = millis();
    if (now - last_heap_log >= 10000) {
        last_heap_log = now;
        DBG.printf("HEAP int=%lu min=%lu  PSRAM=%lu\n",
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    vTaskDelay(pdMS_TO_TICKS(1));
}
