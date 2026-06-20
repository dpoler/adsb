#include <Arduino.h>
#include "arrivals_view.h"
#include "views.h"
#include "detail_card.h"
#include "../data/storage.h"

#include "../pins_s3.h"
#include "../ui/geo.h"
#include "../ui/range.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

static AircraftList *_list = nullptr;
static lv_obj_t *_board_container = nullptr;
static lv_obj_t *_range_label = nullptr;

#define BOARD_W LCD_H_RES
#define BOARD_H (LCD_V_RES - 24)

// Smaller cells for 480px width
#define CELL_W 14
#define CELL_H 20
#define CELL_GAP 1
#define CELL_RADIUS 2
#define ROW_H (CELL_H + 3)
#define TITLE_H 24
#define COL_HEADER_H 16
#define HEADER_H (TITLE_H + COL_HEADER_H)
#define MAX_ROWS 8

// Sort modes
enum SortMode {
    SORT_NONE = 0,
    SORT_ASC,
    SORT_DESC,
};

// 5 columns (dropped ROUTE, SPD, HDG from P4's 8)
#define COL_FLIGHT 0
#define COL_TYPE   1
#define COL_ALT    2
#define COL_DIST   3
#define COL_STATUS 4

static int  _sort_col  = COL_DIST;
static SortMode _sort_dir = SORT_ASC;

// Colors
#define BOARD_BG      lv_color_hex(0x0c0c0c)
#define CELL_BG       lv_color_hex(0x1a1a1a)
#define CELL_TEXT     lv_color_hex(0xffdd00)
#define HEADER_TEXT   lv_color_hex(0xffffff)
#define HEADER_BG     lv_color_hex(0x222222)
#define EMERGENCY_CLR lv_color_hex(0xff3333)
#define MILITARY_CLR  lv_color_hex(0xffaa44)

struct Column {
    const char *name;
    int chars;
    int x;
    bool sortable;
};

static Column columns[] = {
    {"FLIGHT", 7,   8,   true},
    {"TYPE",   4,  120,  false},
    {"ALT",    4,  188,  true},
    {"DIST",   4,  260,  true},
    {"STATUS", 5,  332,  false},
};
#define NUM_COLS 5

struct FlipCell {
    lv_obj_t *label;
    char target;
    char current;
    int rolls_remaining;
    char buf[2];
};

struct BoardRow {
    FlipCell cells[24]; // max chars across all 5 columns
    int total_cells;
    char icao_hex[7];
    bool active;
};

static BoardRow _rows[MAX_ROWS];
static lv_obj_t *_header_labels[NUM_COLS];
static lv_obj_t *_title_label = nullptr;

static const char *status_from_vert_rate(int16_t vr, bool on_ground) {
    if (on_ground) return "GND  ";
    if (vr > 300) return "CLIMB";
    if (vr < -300) return "DESC ";
    return "CRUIS";
}

static FlipCell create_cell(lv_obj_t *parent, int x, int y) {
    FlipCell cell;
    cell.target = ' ';
    cell.current = ' ';
    cell.rolls_remaining = 0;
    cell.buf[0] = ' ';
    cell.buf[1] = '\0';

    cell.label = lv_label_create(parent);
    lv_label_set_text_static(cell.label, cell.buf);
    lv_obj_set_size(cell.label, CELL_W, CELL_H);
    lv_obj_set_pos(cell.label, x, y);
    lv_obj_set_style_bg_color(cell.label, CELL_BG, 0);
    lv_obj_set_style_bg_opa(cell.label, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cell.label, CELL_RADIUS, 0);
    lv_obj_set_style_text_font(cell.label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cell.label, CELL_TEXT, 0);
    lv_obj_set_style_text_align(cell.label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(cell.label, (CELL_H - 14) / 2, 0);
    lv_obj_clear_flag(cell.label, LV_OBJ_FLAG_CLICKABLE);

    return cell;
}

static void init_rows(lv_obj_t *parent) {
    for (int row = 0; row < MAX_ROWS; row++) {
        int y = HEADER_H + row * ROW_H + 2;
        int cell_idx = 0;
        _rows[row].active = false;
        memset(_rows[row].icao_hex, 0, sizeof(_rows[row].icao_hex));

        for (int col = 0; col < NUM_COLS; col++) {
            for (int ch = 0; ch < columns[col].chars; ch++) {
                int x = columns[col].x + ch * (CELL_W + CELL_GAP);
                _rows[row].cells[cell_idx] = create_cell(parent, x, y);
                cell_idx++;
            }
        }
        _rows[row].total_cells = cell_idx;
    }
}

static lv_color_t _row_colors[MAX_ROWS];

static void set_row_text(int row, const char *texts[], lv_color_t color, int rolls_base) {
    if (_row_colors[row].red != color.red || _row_colors[row].green != color.green ||
        _row_colors[row].blue != color.blue) {
        _row_colors[row] = color;
        int ci = 0;
        for (int col = 0; col < NUM_COLS; col++) {
            for (int ch = 0; ch < columns[col].chars; ch++) {
                lv_obj_set_style_text_color(_rows[row].cells[ci].label, color, 0);
                ci++;
            }
        }
    }

    int cell_idx = 0;
    for (int col = 0; col < NUM_COLS; col++) {
        const char *text = texts[col];
        int len = strlen(text);
        for (int ch = 0; ch < columns[col].chars; ch++) {
            char target = (ch < len) ? text[ch] : ' ';
            FlipCell &fc = _rows[row].cells[cell_idx];

            if (target != fc.current || rolls_base > 0) {
                fc.target = target;
                if (rolls_base > 0) {
                    fc.rolls_remaining = rolls_base + (rand() % 3);
                    fc.current = '\0';
                } else {
                    fc.rolls_remaining = 0;
                    fc.current = target;
                    fc.buf[0] = target;
                    lv_label_set_text_static(fc.label, fc.buf);
                }
            }
            cell_idx++;
        }
    }
}

#define FLIP_BUDGET 40

static void flip_animation_tick(lv_timer_t *t) {
    if (views_get_active_index() != VIEW_ARRIVALS) return;

    static const char flip_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -./";
    int updates = 0;

    for (int row = 0; row < MAX_ROWS && updates < FLIP_BUDGET; row++) {
        if (!_rows[row].active) continue;
        for (int c = 0; c < _rows[row].total_cells && updates < FLIP_BUDGET; c++) {
            FlipCell &fc = _rows[row].cells[c];
            if (fc.rolls_remaining > 0) {
                fc.buf[0] = flip_chars[rand() % (sizeof(flip_chars) - 1)];
                lv_label_set_text_static(fc.label, fc.buf);
                fc.rolls_remaining--;
                updates++;
            } else if (fc.current != fc.target) {
                fc.buf[0] = fc.target;
                lv_label_set_text_static(fc.label, fc.buf);
                fc.current = fc.target;
                updates++;
            }
        }
    }
}

struct SortEntry {
    int index;
    float dist_nm;
};

static int sort_compare(const void *a, const void *b) {
    const SortEntry *ea = (const SortEntry *)a;
    const SortEntry *eb = (const SortEntry *)b;
    if (_sort_dir == SORT_NONE) return 0;

    int cmp = 0;
    const Aircraft &aa = _list->aircraft[ea->index];
    const Aircraft &ab = _list->aircraft[eb->index];

    switch (_sort_col) {
        case COL_FLIGHT:
            cmp = strcasecmp(
                aa.callsign[0] ? aa.callsign : aa.icao_hex,
                ab.callsign[0] ? ab.callsign : ab.icao_hex);
            break;
        case COL_ALT:
            cmp = (aa.altitude > ab.altitude) - (aa.altitude < ab.altitude);
            break;
        case COL_DIST:
            if (ea->dist_nm < eb->dist_nm) cmp = -1;
            else if (ea->dist_nm > eb->dist_nm) cmp = 1;
            break;
    }

    return (_sort_dir == SORT_DESC) ? -cmp : cmp;
}

static bool _awaiting_data = true;

static void reset_board_gibberish() {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int row = 0; row < MAX_ROWS; row++) {
        _rows[row].active = true;
        memset(_rows[row].icao_hex, 0, sizeof(_rows[row].icao_hex));
        for (int c = 0; c < _rows[row].total_cells; c++) {
            FlipCell &fc = _rows[row].cells[c];
            char ch = chars[rand() % (sizeof(chars) - 1)];
            fc.buf[0] = ch;
            lv_label_set_text_static(fc.label, fc.buf);
            fc.current = ch;
            fc.target = ch;
            fc.rolls_remaining = 0;
        }
    }
    _awaiting_data = true;
}

static void update_board(lv_timer_t *t) {
    if (views_get_active_index() != VIEW_ARRIVALS) return;
    if (!_list->lock(pdMS_TO_TICKS(5))) return;

    bool first_data = _awaiting_data;
    if (_awaiting_data) _awaiting_data = false;

    SortEntry entries[MAX_AIRCRAFT];
    int n_entries = 0;
    for (int i = 0; i < _list->count && n_entries < MAX_AIRCRAFT; i++) {
        Aircraft &ac = _list->aircraft[i];
        if (ac.lat == 0 && ac.lon == 0) continue;
        float d = MapProjection::distance_nm(g_config.home_lat, g_config.home_lon, ac.lat, ac.lon);
        if (d > range_get_nm()) continue;
        entries[n_entries].index = i;
        entries[n_entries].dist_nm = d;
        n_entries++;
    }

    if (_sort_dir != SORT_NONE) {
        qsort(entries, n_entries, sizeof(SortEntry), sort_compare);
    }

    int row = 0;
    for (int e = 0; e < n_entries && row < MAX_ROWS; e++) {
        Aircraft &ac = _list->aircraft[entries[e].index];

        if (ac.stale_since != 0) {
            uint32_t now = millis();
            uint8_t opa = compute_aircraft_opacity(ac.stale_since, now);
            if (opa == 0) continue;
        }

        int rolls;
        if (first_data) {
            rolls = 3 + row;
        } else if (strcmp(_rows[row].icao_hex, ac.icao_hex) != 0) {
            rolls = 3;
        } else {
            rolls = 0;
        }

        _rows[row].active = true;
        strlcpy(_rows[row].icao_hex, ac.icao_hex, sizeof(_rows[row].icao_hex));

        // Format 5 columns
        char flight[8], type[5], alt[5], dist[5], status[6];
        snprintf(flight, sizeof(flight), "%-7s", ac.callsign[0] ? ac.callsign : ac.icao_hex);
        snprintf(type, sizeof(type), "%-4s", ac.type_code);

        if (ac.on_ground) snprintf(alt, sizeof(alt), " GND");
        else snprintf(alt, sizeof(alt), "%4d", ac.altitude / 100);

        float dist_nm = entries[e].dist_nm;
        if (dist_nm >= 99.95f) snprintf(dist, sizeof(dist), "%4.0f", dist_nm);
        else snprintf(dist, sizeof(dist), "%4.1f", dist_nm);

        snprintf(status, sizeof(status), "%-5s", status_from_vert_rate(ac.vert_rate, ac.on_ground));

        const char *texts[] = {flight, type, alt, dist, status};

        lv_color_t color = CELL_TEXT;
        if (ac.is_emergency) color = EMERGENCY_CLR;
        else if (ac.is_military) color = MILITARY_CLR;

        if (ac.stale_since != 0) {
            uint32_t now = millis();
            uint8_t opa = compute_aircraft_opacity(ac.stale_since, now);
            color = lv_color_make((color.red * opa) / 255,
                                  (color.green * opa) / 255,
                                  (color.blue * opa) / 255);
        }

        set_row_text(row, texts, color, rolls);
        row++;
    }

    int displayed_count = row;

    for (; row < MAX_ROWS; row++) {
        if (first_data || _rows[row].active) {
            const char *blanks[] = {"", "", "", "", ""};
            set_row_text(row, blanks, CELL_TEXT, 0);
            _rows[row].active = false;
            memset(_rows[row].icao_hex, 0, sizeof(_rows[row].icao_hex));
        }
    }

    lv_label_set_text_fmt(_title_label, "TRAFFIC <%s %d", range_label(), displayed_count);
    lv_label_set_text(_range_label, range_label());

    _list->unlock();
}

static void update_header_labels() {
    for (int i = 0; i < NUM_COLS; i++) {
        if (i == _sort_col && _sort_dir != SORT_NONE) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%s %c", columns[i].name,
                     _sort_dir == SORT_ASC ? '^' : 'v');
            lv_label_set_text(_header_labels[i], buf);
            lv_obj_set_style_text_color(_header_labels[i], lv_color_hex(0xffffff), 0);
        } else {
            lv_label_set_text(_header_labels[i], columns[i].name);
            lv_obj_set_style_text_color(_header_labels[i],
                columns[i].sortable ? lv_color_hex(0x888888) : lv_color_hex(0x666666), 0);
        }
    }
}

static void header_label_click_cb(lv_event_t *e) {
    int col = (int)(intptr_t)lv_event_get_user_data(e);
    if (col < 0 || col >= NUM_COLS || !columns[col].sortable) return;

    if (_sort_col == col) {
        if (_sort_dir == SORT_ASC) _sort_dir = SORT_DESC;
        else if (_sort_dir == SORT_DESC) { _sort_dir = SORT_NONE; _sort_col = -1; }
    } else {
        _sort_col = col;
        _sort_dir = SORT_ASC;
    }
    update_header_labels();
}

void arrivals_view_init(lv_obj_t *parent, AircraftList *list) {
    _list = list;

    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_bg_color(parent, BOARD_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    _board_container = lv_obj_create(parent);
    lv_obj_set_size(_board_container, BOARD_W, BOARD_H);
    lv_obj_set_pos(_board_container, 0, 0);
    lv_obj_set_style_bg_color(_board_container, BOARD_BG, 0);
    lv_obj_set_style_bg_opa(_board_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_board_container, 0, 0);
    lv_obj_set_style_radius(_board_container, 0, 0);
    lv_obj_set_style_pad_all(_board_container, 0, 0);
    lv_obj_clear_flag(_board_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_board_container, LV_OBJ_FLAG_SCROLL_CHAIN);

    // Tap row to show detail card
    lv_obj_add_event_cb(_board_container, [](lv_event_t *e) {
        if (views_get_active_index() != VIEW_ARRIVALS) return;

        if (detail_card_is_visible()) {
            detail_card_hide();
            return;
        }

        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        int ty = point.y - 24; // status bar offset

        int row = (ty - HEADER_H - 2) / ROW_H;
        if (row < 0 || row >= MAX_ROWS) return;
        if (!_rows[row].active) return;

        if (!_list->lock(pdMS_TO_TICKS(10))) return;
        for (int i = 0; i < _list->count; i++) {
            if (strcmp(_list->aircraft[i].icao_hex, _rows[row].icao_hex) == 0) {
                Aircraft ac_copy = _list->aircraft[i];
                _list->unlock();
                detail_card_show(&ac_copy);
                return;
            }
        }
        _list->unlock();
    }, LV_EVENT_CLICKED, nullptr);

    // Title bar
    lv_obj_t *title_bg = lv_obj_create(_board_container);
    lv_obj_set_size(title_bg, BOARD_W, TITLE_H);
    lv_obj_set_pos(title_bg, 0, 0);
    lv_obj_set_style_bg_color(title_bg, HEADER_BG, 0);
    lv_obj_set_style_bg_opa(title_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(title_bg, 0, 0);
    lv_obj_set_style_radius(title_bg, 0, 0);
    lv_obj_clear_flag(title_bg, LV_OBJ_FLAG_SCROLLABLE);

    _title_label = lv_label_create(title_bg);
    lv_label_set_text(_title_label, "TRAFFIC  Loading...");
    lv_obj_set_style_text_font(_title_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_title_label, HEADER_TEXT, 0);
    lv_obj_align(_title_label, LV_ALIGN_LEFT_MID, 8, 0);

    // Column header labels
    for (int i = 0; i < NUM_COLS; i++) {
        int col_w = (i < NUM_COLS - 1) ? (columns[i + 1].x - columns[i].x) : (BOARD_W - columns[i].x);

        if (columns[i].sortable) {
            lv_obj_t *btn = lv_obj_create(_board_container);
            lv_obj_set_size(btn, col_w, COL_HEADER_H);
            lv_obj_set_pos(btn, columns[i].x, TITLE_H);
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLL_CHAIN);
            lv_obj_add_event_cb(btn, header_label_click_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, columns[i].name);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
            _header_labels[i] = lbl;
        } else {
            lv_obj_t *lbl = lv_label_create(_board_container);
            lv_label_set_text(lbl, columns[i].name);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
            lv_obj_set_pos(lbl, columns[i].x, TITLE_H + 1);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            _header_labels[i] = lbl;
        }
    }

    update_header_labels();
    init_rows(_board_container);
    reset_board_gibberish();

    // Range label — bottom-right, tappable
    _range_label = lv_label_create(parent);
    lv_label_set_text(_range_label, range_label());
    lv_obj_set_style_text_font(_range_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_range_label, lv_color_hex(0xffdd00), 0);
    lv_obj_set_pos(_range_label, BOARD_W - 60, BOARD_H - 20);
    lv_obj_add_flag(_range_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_range_label, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(_range_label, [](lv_event_t *e) {
        range_cycle();
        lv_label_set_text(_range_label, range_label());
    }, LV_EVENT_CLICKED, nullptr);

    lv_timer_create(flip_animation_tick, 100, nullptr);
    lv_timer_create(update_board, 2000, nullptr);
}

void arrivals_view_update() {
    // Triggered externally if needed
}
