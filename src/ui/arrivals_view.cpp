#include <Arduino.h>
#include "arrivals_view.h"
#include "views.h"
#include "detail_card.h"
#include "../pins_config.h"
#include "geo.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "range.h"
#include "filters.h"
#include "../data/storage.h"
#include "../data/locations.h"

static AircraftList *_list = nullptr;      // currently effective list
static AircraftList *_home_list = nullptr; // the list passed in at init
static lv_obj_t *_board_container = nullptr;
static lv_obj_t *_range_label = nullptr;
static lv_obj_t *_filter_btns[NUM_FILTERS] = {};
static lv_obj_t *_filter_lbls[NUM_FILTERS] = {};
static bool _filter_just_clicked = false; // suppress the row-tap handler right after a filter button tap

#define BOARD_W LCD_H_RES
#define BOARD_H (LCD_V_RES - 30)

#define CELL_H 28
#define ROW_H (CELL_H + 4)
#define TITLE_H 30
#define COL_HEADER_H 18
#define HEADER_H (TITLE_H + COL_HEADER_H)
#define MAX_ROWS 16

// Sort modes
enum SortMode {
    SORT_NONE = 0,
    SORT_ASC,
    SORT_DESC,
};

// Sortable column indices (into columns[])
#define COL_FLIGHT 0
#define COL_TYPE   1
#define COL_ALT    2
#define COL_SPD    3
#define COL_DIST   4
#define COL_HDG    5
#define COL_STATUS 6
#define COL_FPM    7

static int  _sort_col  = COL_DIST;   // which column is sorted
static SortMode _sort_dir = SORT_ASC; // ascending by default

// Colors
#define BOARD_BG      lv_color_hex(0x0c0c0c)
#define CELL_TEXT     lv_color_hex(0xffdd00)  // classic Solari yellow
#define HEADER_TEXT   lv_color_hex(0xffffff)
#define HEADER_BG     lv_color_hex(0x222222)
#define EMERGENCY_CLR lv_color_hex(0xff3333)
#define MILITARY_CLR  lv_color_hex(0xffaa44)

struct Column {
    const char *name;
    int x;
    bool sortable;
};

static Column columns[] = {
    {"FLIGHT",  10,  true},
    {"TYPE",   180, false},
    {"ALT",    280, true},
    {"SPD",    370, true},
    {"DIST",   450, true},
    {"HDG",    550, false},
    {"STATUS", 630, false},
    {"FPM",    800, false}, // spaced clear of STATUS's content and the filter buttons (x=952)
};
#define NUM_COLS 8

struct BoardRow {
    lv_obj_t *col_labels[NUM_COLS];
    char icao_hex[7];
    bool active;
};

static BoardRow _rows[MAX_ROWS];
static lv_obj_t *_header_labels[NUM_COLS];
static lv_obj_t *_title_label = nullptr;

static const char *active_location_label() {
    int idx = locations_active_index();
    if (idx == -1) return "HOME";
    const Location *loc = locations_get(idx);
    return (loc && loc->icao[0]) ? loc->icao : "?";
}

static const char *status_from_vert_rate(int16_t vr, bool on_ground) {
    if (on_ground) return "GROUND ";
    if (vr > 300) return "CLIMB  ";
    if (vr < -300) return "DESCEND";
    return "CRUISE ";
}

// Alternating row backgrounds -- a solid wall of CELL_TEXT yellow with no
// row separation was hard to scan; zebra-striping breaks it up without
// touching the text color itself.
#define ROW_BG_EVEN lv_color_hex(0x0c0c0c) // matches BOARD_BG
#define ROW_BG_ODD  lv_color_hex(0x1a1a1a)

static void init_rows(lv_obj_t *parent) {
    for (int row = 0; row < MAX_ROWS; row++) {
        int y = HEADER_H + row * ROW_H + 4;

        lv_obj_t *bg = lv_obj_create(parent);
        lv_obj_set_size(bg, BOARD_W, ROW_H);
        lv_obj_set_pos(bg, 0, HEADER_H + row * ROW_H);
        lv_obj_set_style_bg_color(bg, (row % 2) ? ROW_BG_ODD : ROW_BG_EVEN, 0);
        lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bg, 0, 0);
        lv_obj_set_style_radius(bg, 0, 0);
        lv_obj_set_style_pad_all(bg, 0, 0);
        lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);

        _rows[row].active = false;
        memset(_rows[row].icao_hex, 0, sizeof(_rows[row].icao_hex));
        for (int col = 0; col < NUM_COLS; col++) {
            lv_obj_t *lbl = lv_label_create(parent);
            lv_obj_set_pos(lbl, columns[col].x, y);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(lbl, CELL_TEXT, 0);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_label_set_text(lbl, "");
            _rows[row].col_labels[col] = lbl;
        }
    }
}

static lv_color_t _row_colors[MAX_ROWS];

static void set_row_text(int row, const char *texts[], lv_color_t color) {
    bool color_changed = (_row_colors[row].red != color.red ||
                          _row_colors[row].green != color.green ||
                          _row_colors[row].blue != color.blue);
    if (color_changed) {
        _row_colors[row] = color;
        for (int col = 0; col < NUM_COLS; col++) {
            lv_obj_set_style_text_color(_rows[row].col_labels[col], color, 0);
        }
    }
    for (int col = 0; col < NUM_COLS; col++) {
        if (strcmp(lv_label_get_text(_rows[row].col_labels[col]), texts[col]) != 0) {
            lv_label_set_text(_rows[row].col_labels[col], texts[col]);
        }
    }
}


// Temporary struct for sorting aircraft indices
struct SortEntry {
    int index;
    float dist_nm;  // cached for SORT_DIST
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
        case COL_SPD:
            cmp = (aa.speed > ab.speed) - (aa.speed < ab.speed);
            break;
        case COL_DIST:
            if (ea->dist_nm < eb->dist_nm) cmp = -1;
            else if (ea->dist_nm > eb->dist_nm) cmp = 1;
            break;
    }

    return (_sort_dir == SORT_DESC) ? -cmp : cmp;
}

// Filter button visuals -- same look/behavior as map_view.cpp/radar_view.cpp
static void update_filter_visuals() {
    unsigned active = filter_get_active();
    for (int i = 0; i < NUM_FILTERS; i++) {
        if (active & (1u << i)) {
            lv_obj_set_style_bg_color(_filter_btns[i], filter_defs[i].color, 0);
            lv_obj_set_style_bg_opa(_filter_btns[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(_filter_btns[i], lv_color_hex(0xffffff), 0);
            lv_obj_set_style_border_width(_filter_btns[i], 2, 0);
            lv_obj_set_style_border_opa(_filter_btns[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(_filter_lbls[i], lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(_filter_btns[i], lv_color_hex(0x0a0a1a), 0);
            lv_obj_set_style_bg_opa(_filter_btns[i], LV_OPA_70, 0);
            lv_obj_set_style_border_color(_filter_btns[i], filter_defs[i].color, 0);
            lv_obj_set_style_border_width(_filter_btns[i], 1, 0);
            lv_obj_set_style_border_opa(_filter_btns[i], LV_OPA_40, 0);
            lv_obj_set_style_text_color(_filter_lbls[i], lv_color_hex(0x666666), 0);
        }
    }
}

// Update board data from aircraft list
static void update_board(lv_timer_t *t) {
    if (views_get_active_index() != VIEW_ARRIVALS) return;

    // Sync filter button visuals if filter changed from another view
    static unsigned _last_synced_filter = ~0u; // impossible bitmask value, forces sync on first tick
    unsigned af = filter_get_active();
    if (af != _last_synced_filter) {
        _last_synced_filter = af;
        update_filter_visuals();
    }

    _list = locations_active_list(_home_list);
    float center_lat, center_lon;
    if (!locations_get_active_coords(&center_lat, &center_lon, nullptr)) {
        center_lat = g_config.home_lat;
        center_lon = g_config.home_lon;
    }

    if (!_list->lock(pdMS_TO_TICKS(5))) return;

    // Build sortable index of aircraft with valid positions
    SortEntry entries[MAX_AIRCRAFT];
    int n_entries = 0;
    for (int i = 0; i < _list->count && n_entries < MAX_AIRCRAFT; i++) {
        Aircraft &ac = _list->aircraft[i];
        if (ac.lat == 0 && ac.lon == 0) continue;
        if (g_config.hide_ground && ac.on_ground) continue;
        if (!aircraft_passes_filter(ac)) continue;
        float d = MapProjection::distance_nm(center_lat, center_lon, ac.lat, ac.lon);
        if (d > range_get_nm()) continue;
        entries[n_entries].index = i;
        entries[n_entries].dist_nm = d;
        n_entries++;
    }

    // Sort if a mode is active
    if (_sort_dir != SORT_NONE) {
        qsort(entries, n_entries, sizeof(SortEntry), sort_compare);
    }

    int row = 0;
    for (int e = 0; e < n_entries && row < MAX_ROWS; e++) {
        Aircraft &ac = _list->aircraft[entries[e].index];

        // Skip fully faded ghosts
        if (ac.stale_since != 0) {
            uint32_t now = millis();
            uint8_t opa = compute_aircraft_opacity(ac.stale_since, now);
            if (opa == 0) continue;
        }

        _rows[row].active = true;
        strlcpy(_rows[row].icao_hex, ac.icao_hex, sizeof(_rows[row].icao_hex));

        // Format each column
        char flight[9], type[5], alt[5], spd[4], dist[5], hdg[4], status[8], fpm[8];
        snprintf(flight, sizeof(flight), "%-8s", ac.callsign[0] ? ac.callsign : ac.icao_hex);
        snprintf(type, sizeof(type), "%-4s", ac.type_code);

        if (ac.on_ground) snprintf(alt, sizeof(alt), " GND");
        else snprintf(alt, sizeof(alt), "%4d", ac.altitude / 100);
        snprintf(spd, sizeof(spd), "%3d", ac.speed);

        float dist_nm = entries[e].dist_nm;
        if (dist_nm >= 99.95f) snprintf(dist, sizeof(dist), "%4.0f", dist_nm);
        else snprintf(dist, sizeof(dist), "%4.1f", dist_nm);
        snprintf(hdg, sizeof(hdg), "%03d", ac.heading);
        snprintf(status, sizeof(status), "%-7s", status_from_vert_rate(ac.vert_rate, ac.on_ground));

        // Own FPM column -- blank for GROUND/CRUISE (nothing meaningful to
        // show; this also covers the vert_rate_valid == false case for free,
        // since a missing baro_rate reading defaults to 0 in the parser,
        // which always classifies as CRUISE, never CLIMB/DESCEND).
        bool climbing_or_descending = !ac.on_ground && (ac.vert_rate > 300 || ac.vert_rate < -300);
        if (climbing_or_descending) snprintf(fpm, sizeof(fpm), "%+5d", ac.vert_rate);
        else fpm[0] = '\0';

        const char *texts[] = {flight, type, alt, spd, dist, hdg, status, fpm};

        lv_color_t color = CELL_TEXT;
        if (ac.is_emergency) color = EMERGENCY_CLR;
        else if (ac.is_military) color = MILITARY_CLR;

        // Dim stale (ghost) aircraft
        if (ac.stale_since != 0) {
            uint32_t now = millis();
            uint8_t opa = compute_aircraft_opacity(ac.stale_since, now);
            color = lv_color_make((color.red * opa) / 255,
                                  (color.green * opa) / 255,
                                  (color.blue * opa) / 255);
        }

        set_row_text(row, texts, color);
        row++;
    }

    int displayed_count = row;  // save before clear loop overwrites row

    // Clear rows below the displayed count
    for (; row < MAX_ROWS; row++) {
        if (_rows[row].active) {
            const char *blanks[] = {"", "", "", "", "", "", "", ""};
            set_row_text(row, blanks, CELL_TEXT);
            _rows[row].active = false;
            memset(_rows[row].icao_hex, 0, sizeof(_rows[row].icao_hex));
        }
    }

    // Update title with active location, range-filtered count, and active
    // filters (if any) -- was a fixed "OVERHEAD TRAFFIC" regardless of
    // location, which read oddly once the location picker existed.
    char filter_buf[128];
    const char *loc_label = active_location_label();
    if (filter_label_text(filter_buf, sizeof(filter_buf), nullptr) > 0) {
        lv_label_set_text_fmt(_title_label, "%s TRAFFIC  <%s    %d    %s",
                              loc_label, range_label(), displayed_count, filter_buf);
    } else {
        lv_label_set_text_fmt(_title_label, "%s TRAFFIC  <%s    %d", loc_label, range_label(), displayed_count);
    }
    lv_label_set_text(_range_label, range_label());

    _list->unlock();
}

void arrivals_view_on_show() {
    update_board(nullptr);
}

static void filter_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    _filter_just_clicked = true;
    filter_toggle(idx);
    update_filter_visuals();
    update_board(nullptr); // refresh immediately rather than waiting for the next 2s tick
}

// Update header label text to show sort indicator
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

// Per-label click handler for sort column headers
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
    _home_list = list;

    // Make tile fully opaque so radar/map don't bleed through
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
    views_attach_swipe(_board_container);

    lv_obj_add_event_cb(_board_container, [](lv_event_t *e) {
        if (views_get_active_index() != VIEW_ARRIVALS) return;
        if (views_swipe_active()) { views_clear_swipe(); return; }

        // Guard: skip if a filter button was just clicked -- the row-tap
        // logic below only checks Y, not X, so a tap on the filter column
        // would otherwise also be read as a row tap.
        if (_filter_just_clicked) {
            _filter_just_clicked = false;
            return;
        }

        if (detail_card_is_visible()) {
            detail_card_hide();
            return;
        }

        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        int ty = point.y - 30; // offset for status bar

        // Determine which row was tapped
        int row = (ty - HEADER_H - 4) / ROW_H;
        if (row < 0 || row >= MAX_ROWS) return;
        if (!_rows[row].active) return;

        // Look up aircraft by ICAO hex
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
    lv_label_set_text(_title_label, "OVERHEAD TRAFFIC  Loading...");
    lv_obj_set_style_text_font(_title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_title_label, HEADER_TEXT, 0);
    // Indented past the location picker button's footprint (top-left,
    // location_picker.cpp, ~4-74px) -- it renders on the shared top-level
    // screen above every tile, so the title text has to clear it instead.
    lv_obj_align(_title_label, LV_ALIGN_LEFT_MID, 84, 0);

    // Column header labels — sortable ones get wide clickable containers
    for (int i = 0; i < NUM_COLS; i++) {
        // Calculate column width (to next column, or to board edge)
        int col_w = (i < NUM_COLS - 1) ? (columns[i + 1].x - columns[i].x) : (BOARD_W - columns[i].x);

        if (columns[i].sortable) {
            // Clickable container spanning full column width for easy tap target
            lv_obj_t *btn = lv_obj_create(_board_container);
            lv_obj_set_size(btn, col_w, COL_HEADER_H);
            lv_obj_set_pos(btn, columns[i].x, TITLE_H);
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLL_CHAIN); // prevent tileview from stealing clicks
            lv_obj_add_event_cb(btn, header_label_click_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, columns[i].name);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
            _header_labels[i] = lbl;
        } else {
            // Non-sortable: plain label, no click handling
            lv_obj_t *lbl = lv_label_create(_board_container);
            lv_label_set_text(lbl, columns[i].name);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
            lv_obj_set_pos(lbl, columns[i].x, TITLE_H + 2);
            // Prevent non-sortable labels from eating touch events
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            _header_labels[i] = lbl;
        }
    }

    // Set initial sort indicator
    update_header_labels();

    init_rows(_board_container);

    // Range label — bottom-right, tappable
    _range_label = lv_label_create(parent);
    lv_label_set_text(_range_label, range_label());
    lv_obj_set_style_text_font(_range_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_range_label, lv_color_hex(0xffdd00), 0);
    lv_obj_set_pos(_range_label, BOARD_W - 80, BOARD_H - 28);
    lv_obj_add_flag(_range_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_range_label, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(_range_label, [](lv_event_t *e) {
        range_cycle();
        lv_label_set_text(_range_label, range_label());
    }, LV_EVENT_CLICKED, nullptr);

    // Filter toggle buttons — vertical stack on right edge, same layout as
    // map_view.cpp/radar_view.cpp (including the FILT_VERT divider). Fits in
    // the space freed up by the removed ROUTE column.
    {
        int btn_w = 64, btn_h = 48, btn_gap = 10, group_gap_extra = 14;
        int total_h = NUM_FILTERS * btn_h + (NUM_FILTERS - 1) * btn_gap + group_gap_extra;
        int btn_x = BOARD_W - btn_w - 8;
        int btn_y0 = (BOARD_H - total_h) / 2;
        for (int i = 0; i < NUM_FILTERS; i++) {
            int y = btn_y0 + i * (btn_h + btn_gap) + (i >= FILT_VERT ? group_gap_extra : 0);
            if (i == FILT_VERT) {
                lv_obj_t *div = lv_obj_create(parent);
                lv_obj_set_size(div, btn_w, 1);
                lv_obj_set_pos(div, btn_x, y - (btn_gap + group_gap_extra) / 2);
                lv_obj_set_style_bg_color(div, lv_color_hex(0x444466), 0);
                lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(div, 0, 0);
                lv_obj_set_style_radius(div, 0, 0);
                lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE);
            }
            lv_obj_t *btn = lv_obj_create(parent);
            lv_obj_set_size(btn, btn_w, btn_h);
            lv_obj_set_pos(btn, btn_x, y);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x0a0a1a), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
            lv_obj_set_style_border_color(btn, filter_defs[i].color, 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
            lv_obj_set_style_radius(btn, 6, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLL_CHAIN);
            lv_obj_add_event_cb(btn, filter_click_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, filter_defs[i].label);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
            lv_obj_center(lbl);

            _filter_btns[i] = btn;
            _filter_lbls[i] = lbl;
        }
        update_filter_visuals();
    }

    // Data update timer
    lv_timer_create(update_board, 2000, nullptr);
}

void arrivals_view_update() {
    // Triggered externally if needed
}
