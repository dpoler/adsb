#include "../platform/platform.h" // millis()/strlcpy compatibility shims on non-Arduino builds
#include "stats_view.h"
#include "stats.h"
#include "views.h"
#include "status_bar.h"
#include "../pins_config.h"
#include "../data/locations.h"
#include "../data/metar.h"
#include "../data/atis.h"
#include "geo.h" // altitude_color()
#include <cstdio>
#include <cstring>

#define STATS_W LCD_H_RES
#define STATS_H (LCD_V_RES - STATUS_BAR_HEIGHT)
#define BG_COLOR lv_color_hex(0x0a0a1a)
#define DIM_COLOR lv_color_hex(0x9999bb) // brightened from 0x666688 -- low contrast against BG_COLOR was hard to read
#define ACCENT_COLOR lv_color_hex(0x4488ff)
// Column-identity accent for the "LOCATION" (session) column -- lets
// UNIQUE/PEAK and the column header itself read as visually distinct from
// the "CURRENT TRAFFIC" (live) column's ACCENT_COLOR, reinforcing the reset-on-
// switch grouping without needing a caption to explain it every time.
#define SESSION_COLOR lv_color_hex(0xaa88ff)

static AircraftList *_list = nullptr;      // the one aircraft list -- fetch_task always fetches for whichever location is currently active
static lv_obj_t *_container = nullptr;
static lv_obj_t *_traffic_total_lbl = nullptr; // "Total: N" caption under CURRENT TRAFFIC
static lv_obj_t *_metar_lbl = nullptr; // nearest-station METAR readout, top band (see metar.h)
#if LCD_H_RES >= 1280
static lv_obj_t *_atis_panel = nullptr; // scrollable D-ATIS column (right)
static lv_obj_t *_atis_lbl = nullptr;
#endif

// Category rows
struct BarRow {
    lv_obj_t *name_lbl;
    lv_obj_t *count_lbl;
    lv_obj_t *bar;
};

static BarRow _cat_rows[5];
static const char *CAT_NAMES[] = {"JETS", "GA", "HELI", "MIL", "EMRG"};
static const uint32_t CAT_COLORS[] = {0x4488ff, 0x88aacc, 0x44ddaa, 0xffaa00, 0xff3333};

// Altitude rows -- colors sourced from altitude_color() (geo.h) at a
// representative altitude in each band, rather than a second hardcoded
// palette, so this can't drift from what the trails on Map/Radar draw.
// GND dropped -- ground traffic is excluded from this whole screen (see
// stats.cpp), so that bucket would always read zero.
static BarRow _alt_rows[5];
static const char *ALT_NAMES[] = {"<5k", "<15k", "<25k", "<35k", "35k+"};
static const int32_t ALT_SAMPLES[] = {2500, 10000, 20000, 30000, 45000};

// Speed rows -- GND dropped, same reason as altitude above
static BarRow _spd_rows[5];
static const char *SPD_NAMES[] = {"<200", "<300", "<400", "<500", "500+"};
static const uint32_t SPD_COLORS[] = {0x4488cc, 0x4488ff, 0x8844ff, 0xcc44ff, 0xff44aa};

// Records
static lv_obj_t *_fastest_val = nullptr;
static lv_obj_t *_slowest_val = nullptr;
static lv_obj_t *_highest_val = nullptr;
static lv_obj_t *_lowest_val = nullptr;
static lv_obj_t *_closest_val = nullptr;

// Session stats -- per-location (see stats.cpp)
static lv_obj_t *_unique_val = nullptr;
static lv_obj_t *_peak_val = nullptr;

// Top airlines
static lv_obj_t *_airline_labels[5] = {};

// Top types
static lv_obj_t *_type_labels[5] = {};

#define BAR_MAX_W 220 // widened from 160 -- see stats_view_init()'s LEFT COLUMN comment
#define BAR_H 8 // thinner bars, tightened row pitch below to match

// Uniform vertical rhythm for the whole screen -- every section (bars or
// plain text) now uses one of these two pitches instead of each section
// having picked its own spacing independently.
#define ROW_H 18      // line pitch for 14pt rows (text or bars)
#define ROW_H_WIDE 20 // line pitch for 16pt rows (center column distributions)
#define SECTION_GAP 14 // gap from a section's last row to the next header
#define COL_HEADER_GAP 40 // gap from a column's own header down to its first subsection header -- deliberately more breathing room than SECTION_GAP, since the column header is now larger (20pt) and needs to read as a clear step above the subsection headers below it, not just another row

static lv_obj_t *create_bar(lv_obj_t *parent, int x, int y, lv_color_t color) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 0, BAR_H);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_style_bg_color(bar, color, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    return bar;
}

static void create_bar_row(lv_obj_t *parent, BarRow *row, const char *name,
                           uint32_t color_hex, int x, int y,
                           const lv_font_t *font = &lv_font_montserrat_14,
                           int name_off = 42, int bar_off = 70) {
    lv_color_t color = lv_color_hex(color_hex);

    row->name_lbl = lv_label_create(parent);
    lv_label_set_text(row->name_lbl, name);
    lv_obj_set_style_text_font(row->name_lbl, font, 0);
    lv_obj_set_style_text_color(row->name_lbl, color, 0);
    lv_obj_set_pos(row->name_lbl, x, y + 1);
    lv_obj_clear_flag(row->name_lbl, LV_OBJ_FLAG_CLICKABLE);

    row->count_lbl = lv_label_create(parent);
    lv_label_set_text(row->count_lbl, "0");
    lv_obj_set_style_text_font(row->count_lbl, font, 0);
    lv_obj_set_style_text_color(row->count_lbl, lv_color_hex(0xccccdd), 0);
    lv_obj_set_pos(row->count_lbl, x + name_off, y + 1);
    lv_obj_clear_flag(row->count_lbl, LV_OBJ_FLAG_CLICKABLE);

    // +3 keeps the now-thinner bar vertically centered against the label's
    // text line instead of hugging its top.
    row->bar = create_bar(parent, x + bar_off, y + 3, color);
}

static void update_bar(BarRow *row, int count, int total) {
    lv_label_set_text_fmt(row->count_lbl, "%d", count);
    int w = (total > 0) ? (count * BAR_MAX_W / total) : 0;
    if (w < 2 && count > 0) w = 2;
    lv_obj_set_width(row->bar, w);
}

// Helper for an inline "HEADER  value" row -- header and value share one line.
static lv_obj_t *create_inline_row(lv_obj_t *parent, const char *header, int x, int y,
                                    lv_color_t val_color, int val_off) {
    lv_obj_t *h = lv_label_create(parent);
    lv_label_set_text(h, header);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h, DIM_COLOR, 0);
    lv_obj_set_pos(h, x, y);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *v = lv_label_create(parent);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(v, val_color, 0);
    lv_obj_set_pos(v, x + val_off, y);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
    return v;
}

static void refresh_stats(lv_timer_t *t) {
    // Deliberately not gated on "is Stats the active tab" -- UNIQUE/PEAK are
    // meant to describe the whole time a location has been selected, not
    // just however long you happen to have had the Stats screen open. Label
    // updates on an inactive tileview tile are cheap (nothing to redraw
    // until it's actually shown), so there's no real cost to always running.
    stats_update(_list);
    const SessionStats *s = stats_get();

    // METAR readout, top band -- metar_poll() (fetcher.cpp's location_poll_task)
    // updates the shared metar_status/metar_raw globals on its own slow
    // cadence (metar.h); this just reflects whatever they currently say.
    //
    // Blank the label immediately on a location change, in lockstep with
    // stats_update() resetting everything else above -- rather than trust
    // metar_poll() (a different task, own timer) to have already cleared
    // metar_raw by the time this runs. Without this, the OLD location's
    // METAR stayed on screen for however long the new fetch took after
    // switching (reported: switching away from KJFK left it showing for a
    // bit, reading as a stale/wrong value rather than "still loading").
    // metar.cpp clears its own globals synchronously the instant *it*
    // notices the same change too, so once this tick's forced blank passes,
    // later ticks reading METAR_FETCHING here are showing a real "nothing
    // yet" state, not stale data -- not a race between the two, just two
    // independent belt-and-suspenders blanks of the same underlying switch.
    static int _metar_last_loc = -2; // sentinel so the very first call syncs
    int metar_loc = locations_active_index();
    if (metar_loc != _metar_last_loc) {
        _metar_last_loc = metar_loc;
        lv_label_set_text(_metar_lbl, "");
    } else {
        // FETCHING/ERROR are otherwise left alone rather than overwriting
        // the label -- both are transient (a routine fetch every ~10min,
        // self-healing on the next poll) *for the same location*, so
        // keeping the last good reading up while one's in flight reads
        // better than flashing a special state for it.
        switch (metar_status) {
            case METAR_OK:
                lv_label_set_text(_metar_lbl, metar_raw);
                lv_obj_set_style_text_color(_metar_lbl, DIM_COLOR, 0);
                break;
            case METAR_NO_STATION:
                lv_label_set_text(_metar_lbl, "NO STATIONS IN RANGE");
                lv_obj_set_style_text_color(_metar_lbl, lv_color_hex(0x666688), 0);
                break;
            case METAR_IDLE:
                lv_label_set_text(_metar_lbl, "");
                break;
            case METAR_FETCHING:
            case METAR_ERROR:
                break;
        }
    }

#if LCD_H_RES >= 1280
    // D-ATIS column — blank on location change; keep last text through
    // transient FETCHING/ERROR for the same airport (same as METAR).
    static int _atis_last_loc = -2;
    int atis_loc = locations_active_index();
    if (atis_loc != _atis_last_loc) {
        _atis_last_loc = atis_loc;
        if (_atis_lbl) lv_label_set_text(_atis_lbl, "");
    } else if (_atis_lbl) {
        switch (atis_status) {
            case ATIS_OK: {
                // Build ARR/DEP or combined text. Buffer sized for two
                // reports + headers (atis texts are up to ~1.6KB each).
                static char buf[ATIS_MAX_REPORTS * (ATIS_TEXT_LEN + 48)];
                buf[0] = '\0';
                size_t used = 0;
                for (int i = 0; i < atis_report_count && i < ATIS_MAX_REPORTS; i++) {
                    const char *hdr = "ATIS";
                    if (strcmp(atis_reports[i].type, "arr") == 0) hdr = "ARR ATIS";
                    else if (strcmp(atis_reports[i].type, "dep") == 0) hdr = "DEP ATIS";
                    int n = snprintf(buf + used, sizeof(buf) - used,
                                     "%s%s %s%s\n%s",
                                     (used > 0) ? "\n\n" : "",
                                     hdr,
                                     atis_reports[i].code[0] ? "INFO " : "",
                                     atis_reports[i].code,
                                     atis_reports[i].text);
                    if (n < 0) break;
                    used += (size_t)n;
                    if (used >= sizeof(buf)) break;
                }
                lv_label_set_text(_atis_lbl, buf);
                lv_obj_set_style_text_color(_atis_lbl, DIM_COLOR, 0);
                break;
            }
            case ATIS_NONE:
                lv_label_set_text(_atis_lbl, "NO D-ATIS");
                lv_obj_set_style_text_color(_atis_lbl, lv_color_hex(0x666688), 0);
                break;
            case ATIS_IDLE:
                lv_label_set_text(_atis_lbl, "");
                break;
            case ATIS_FETCHING:
            case ATIS_ERROR:
                break;
        }
    }
#endif

    lv_label_set_text_fmt(_traffic_total_lbl, "Total: %d", s->current_count);

    // Category bars
    int cat_counts[] = {s->jets, s->ga, s->heli, s->military, s->emergency};
    int cat_total = s->current_count > 0 ? s->current_count : 1;
    for (int i = 0; i < 5; i++) {
        update_bar(&_cat_rows[i], cat_counts[i], cat_total);
    }

    // Altitude bars (ground traffic excluded entirely -- see stats.cpp)
    int alt_counts[] = {s->alt_low, s->alt_med_low,
                        s->alt_med, s->alt_high, s->alt_very_high};
    int alt_max = 1;
    for (int i = 0; i < 5; i++) {
        if (alt_counts[i] > alt_max) alt_max = alt_counts[i];
    }
    for (int i = 0; i < 5; i++) {
        update_bar(&_alt_rows[i], alt_counts[i], alt_max);
    }

    // Speed bars (ground traffic excluded entirely -- see stats.cpp)
    int spd_counts[] = {s->spd_slow, s->spd_med,
                        s->spd_fast, s->spd_very_fast, s->spd_extreme};
    int spd_max = 1;
    for (int i = 0; i < 5; i++) {
        if (spd_counts[i] > spd_max) spd_max = spd_counts[i];
    }
    for (int i = 0; i < 5; i++) {
        update_bar(&_spd_rows[i], spd_counts[i], spd_max);
    }

    // Records
    if (s->fastest_callsign[0]) {
        lv_label_set_text_fmt(_fastest_val, "%s  %dkt", s->fastest_callsign, s->fastest_speed);
    } else {
        lv_label_set_text(_fastest_val, "--");
    }
    if (s->slowest_callsign[0] && s->slowest_speed < 99999) {
        lv_label_set_text_fmt(_slowest_val, "%s  %dkt", s->slowest_callsign, s->slowest_speed);
    } else {
        lv_label_set_text(_slowest_val, "--");
    }
    if (s->highest_callsign[0] && s->highest_alt > -9999) {
        if (s->highest_alt >= 18000) {
            lv_label_set_text_fmt(_highest_val, "%s  FL%d", s->highest_callsign, s->highest_alt / 100);
        } else {
            lv_label_set_text_fmt(_highest_val, "%s  %dft", s->highest_callsign, s->highest_alt);
        }
    } else {
        lv_label_set_text(_highest_val, "--");
    }
    if (s->lowest_callsign[0] && s->lowest_alt < 999999) {
        lv_label_set_text_fmt(_lowest_val, "%s  %dft", s->lowest_callsign, s->lowest_alt);
    } else {
        lv_label_set_text(_lowest_val, "--");
    }
    if (s->closest_callsign[0] && s->closest_dist < 9999.0f) {
        lv_label_set_text_fmt(_closest_val, "%s  %.1fnm", s->closest_callsign, (double)s->closest_dist);
    } else {
        lv_label_set_text(_closest_val, "--");
    }

    // Session stats
    lv_label_set_text_fmt(_unique_val, "%d", s->unique_seen);
    lv_label_set_text_fmt(_peak_val, "%d", s->peak_count);

    // Top airlines
    for (int i = 0; i < 5; i++) {
        if (s->top_airlines[i].code[0]) {
            lv_label_set_text_fmt(_airline_labels[i], "%-3s %d", s->top_airlines[i].code, s->top_airlines[i].count);
        } else {
            lv_label_set_text(_airline_labels[i], "");
        }
    }

    // Top types
    for (int i = 0; i < 5; i++) {
        if (s->top_types[i].type[0]) {
            lv_label_set_text_fmt(_type_labels[i], "%-4s %d", s->top_types[i].type, s->top_types[i].count);
        } else {
            lv_label_set_text(_type_labels[i], "");
        }
    }
}

static void create_section_header(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, DIM_COLOR, 0);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

void stats_view_init(lv_obj_t *parent, AircraftList *list) {
    _list = list;

    _container = lv_obj_create(parent);
    lv_obj_set_size(_container, STATS_W, STATS_H);
    lv_obj_set_pos(_container, 0, 0);
    lv_obj_set_style_bg_color(_container, BG_COLOR, 0);
    lv_obj_set_style_bg_opa(_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_container, 0, 0);
    lv_obj_set_style_radius(_container, 0, 0);
    lv_obj_set_style_pad_all(_container, 0, 0);
    lv_obj_clear_flag(_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_container, LV_OBJ_FLAG_SCROLL_CHAIN);
    views_attach_swipe(_container);

    // METAR readout, top band -- the 80px top_y reserved below was left for
    // exactly this. Centered, full-width single line -- a raw METAR string
    // ("METAR KDEN 281453Z 25005KT 10SM FEW100 FEW150 SCT220 26/15 A3014
    // RMK AO2 SLP112 T02610150 51000") comfortably fits one line at this
    // width/font. See metar.h/metar.cpp for the fetch side (nearest
    // reporting station within 20nm of the active location, via
    // aviationweather.gov's public Data API) and refresh_stats() above for
    // how this label's text/color get set from metar_status.
    _metar_lbl = lv_label_create(_container);
    lv_label_set_text(_metar_lbl, "");
    lv_obj_set_style_text_font(_metar_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_metar_lbl, DIM_COLOR, 0);
    lv_obj_set_style_text_align(_metar_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_metar_lbl, STATS_W - 80);
    lv_obj_set_pos(_metar_lbl, 40, 40);
    lv_obj_clear_flag(_metar_lbl, LV_OBJ_FLAG_CLICKABLE);

    // Both columns share one vertical rhythm: column header at top_y, first
    // subsection header at top_y+COL_HEADER_GAP(+ROW_H to clear a caption
    // line), subsections spaced SECTION_GAP(14) apart thereafter.
    //
    // top_y reserves 80px above the columns for the METAR band above.
    int top_y = 88;

    // ============================================================
    // LEFT COLUMN: "CURRENT TRAFFIC" -- everything here is recalculated
    // from scratch on every refresh_stats() tick (~2s) from whichever
    // aircraft are currently visible. Nothing in this column accumulates
    // over time or remembers anything from a previous tick -- that's the
    // "LOCATION" column (right) below, kept deliberately separate.
    //
    // Widened from the original 3-column layout (lx was 15, bars capped at
    // 160px) now that the former DEVICE column (moved to Settings' STATUS
    // tab) freed up the right third of the screen -- BAR_MAX_W grew to 220
    // and both columns shifted outward to actually use the reclaimed width,
    // rather than leaving it as dead space in the middle.
    // ============================================================
    int lx = 40;

    lv_obj_t *now_header = lv_label_create(_container);
    lv_label_set_text(now_header, "CURRENT TRAFFIC");
    lv_obj_set_style_text_font(now_header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(now_header, ACCENT_COLOR, 0);
    lv_obj_set_pos(now_header, lx, top_y);
    lv_obj_clear_flag(now_header, LV_OBJ_FLAG_CLICKABLE);

    // "Total: N" caption, same position/style/purpose as LOCATION's "since
    // last switch" caption below -- lines the two column headers up with
    // matching header-plus-caption shapes instead of one having a caption
    // and the other just floating alone above TYPE.
    _traffic_total_lbl = lv_label_create(_container);
    lv_label_set_text(_traffic_total_lbl, "Total: 0");
    lv_obj_set_style_text_font(_traffic_total_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_traffic_total_lbl, DIM_COLOR, 0);
    lv_obj_set_pos(_traffic_total_lbl, lx, top_y + 24);
    lv_obj_clear_flag(_traffic_total_lbl, LV_OBJ_FLAG_CLICKABLE);

    // Category breakdown -- type_y matches LOCATION's rc_y (COL_HEADER_GAP +
    // ROW_H, not just COL_HEADER_GAP) so TYPE and RECORDS line up now that
    // both columns have a caption line under their header.
    int type_y = top_y + COL_HEADER_GAP + ROW_H;
    create_section_header(_container, "TYPE", lx, type_y);
    for (int i = 0; i < 5; i++) {
        // EMRG's wide "M" runs into the count digit at the default 42px
        // offset even though HELI/JETS (same 4 chars) don't -- give it a
        // few extra px.
        int name_off = (i == 4) ? 48 : 42;
        create_bar_row(_container, &_cat_rows[i], CAT_NAMES[i], CAT_COLORS[i],
                       lx, type_y + ROW_H + i * ROW_H, &lv_font_montserrat_14, name_off);
    }

    // Altitude distribution
    int alt_y = type_y + 6 * ROW_H + SECTION_GAP;
    create_section_header(_container, "ALTITUDE", lx, alt_y);
    for (int i = 0; i < 5; i++) {
        create_bar_row(_container, &_alt_rows[i], ALT_NAMES[i],
                       lv_color_to_u32(altitude_color(ALT_SAMPLES[i])),
                       lx, alt_y + ROW_H_WIDE + i * ROW_H_WIDE, &lv_font_montserrat_16, 52, 88);
    }

    // Speed distribution
    int spd_y = alt_y + 6 * ROW_H_WIDE + SECTION_GAP;
    create_section_header(_container, "SPEED", lx, spd_y);
    for (int i = 0; i < 5; i++) {
        create_bar_row(_container, &_spd_rows[i], SPD_NAMES[i], SPD_COLORS[i],
                       lx, spd_y + ROW_H_WIDE + i * ROW_H_WIDE, &lv_font_montserrat_16, 52, 88);
    }

    // ============================================================
    // RIGHT COLUMN: "LOCATION (since last switch)" -- everything here is
    // accumulated since the active location was last switched (see
    // stats.cpp: reset happens once, the moment locations_active_index()
    // changes -- leaving location A and arriving at location B are the same
    // event, not two separate resets). RECORDS in particular changed
    // meaning from earlier versions of this screen: FASTEST/SLOWEST/
    // HIGHEST/LOWEST/CLOSEST used to be recalculated from scratch every
    // tick just like the CURRENT TRAFFIC column, which read as "session records"
    // but was actually "whatever's true this instant" -- misleading, and
    // part of why this screen was reorganized. They are now genuine running
    // extremes: the most extreme value seen since this location became
    // active, same as PEAK already was.
    //
    // Was the center column (cx=340) when DEVICE occupied the right third;
    // now the second of two, moved out to cx=540 to split the screen's full
    // width with CURRENT TRAFFIC rather than leaving a gap where DEVICE used
    // to sit.
    // ============================================================
    int cx = 540;

    lv_obj_t *loc_header = lv_label_create(_container);
    lv_label_set_text(loc_header, "LOCATION");
    lv_obj_set_style_text_font(loc_header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(loc_header, SESSION_COLOR, 0);
    lv_obj_set_pos(loc_header, cx, top_y);
    lv_obj_clear_flag(loc_header, LV_OBJ_FLAG_CLICKABLE);

    // "(since last switch)" split onto its own smaller line rather than
    // appended to the big 20pt header -- at that size the full phrase runs
    // long enough to risk crowding the right edge of the screen.
    lv_obj_t *loc_caption = lv_label_create(_container);
    lv_label_set_text(loc_caption, "since last switch");
    lv_obj_set_style_text_font(loc_caption, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(loc_caption, DIM_COLOR, 0);
    lv_obj_set_pos(loc_caption, cx, top_y + 24);
    lv_obj_clear_flag(loc_caption, LV_OBJ_FLAG_CLICKABLE);

    // Records — compact rows with inline header + value
    int rc_y = top_y + COL_HEADER_GAP + ROW_H; // +ROW_H to clear the caption line above
    create_section_header(_container, "RECORDS", cx, rc_y);

    lv_color_t rec_hdr = DIM_COLOR;
    lv_color_t rec_val = lv_color_hex(0xccccdd);
    int rr = rc_y + ROW_H; // first row
    int rh = ROW_H;        // row height

    auto make_rec_row = [&](const char *hdr, int y) -> lv_obj_t* {
        lv_obj_t *h = lv_label_create(_container);
        lv_label_set_text(h, hdr);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(h, rec_hdr, 0);
        lv_obj_set_pos(h, cx, y);
        lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *v = lv_label_create(_container);
        lv_label_set_text(v, "--");
        lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(v, rec_val, 0);
        // 100px, not 80 -- a bit more breathing room now that the column
        // itself has more room to give (freed up by DEVICE's removal).
        lv_obj_set_pos(v, cx + 100, y);
        lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
        return v;
    };

    _fastest_val = make_rec_row("FASTEST", rr);
    lv_obj_set_style_text_color(_fastest_val, lv_color_hex(0xff66cc), 0);
    _slowest_val = make_rec_row("SLOWEST", rr + rh);
    lv_obj_set_style_text_color(_slowest_val, lv_color_hex(0x66aaff), 0);
    _highest_val = make_rec_row("HIGHEST", rr + rh * 2);
    lv_obj_set_style_text_color(_highest_val, altitude_color(45000), 0); // high end of the altitude gradient
    _lowest_val  = make_rec_row("LOWEST",  rr + rh * 3);
    lv_obj_set_style_text_color(_lowest_val, altitude_color(1000), 0); // low end of the altitude gradient
    _closest_val = make_rec_row("CLOSEST", rr + rh * 4);
    lv_obj_set_style_text_color(_closest_val, lv_color_hex(0x44ddaa), 0);

    // AIRCRAFT SEEN -- UPTIME deliberately lives on Settings' STATUS tab now
    // (settings.cpp), not here, despite looking like a natural fit -- it
    // does not reset when you switch locations (genuinely device-global),
    // so grouping it with UNIQUE/PEAK would misrepresent it as
    // location-scoped.
    int ss_y = rr + rh * 5 + SECTION_GAP;
    create_section_header(_container, "AIRCRAFT SEEN", cx, ss_y);
    // Stacked vertically (one inline "HEADER  value" row per stat), matching
    // RECORDS above, rather than side by side. SESSION_COLOR (not
    // ACCENT_COLOR) -- these are LOCATION data, not CURRENT TRAFFIC data.
    _unique_val = create_inline_row(_container, "UNIQUE", cx, ss_y + ROW_H, SESSION_COLOR, 100);
    _peak_val = create_inline_row(_container, "PEAK", cx, ss_y + ROW_H * 2, SESSION_COLOR, 100);

    // Top airlines -- AIRCRAFT SEEN is 2 rows deep (ROW_H * 2) plus its own
    // header pitch (ROW_H), then the usual gap before the next header.
    // Grid spacing widened 80->100 -- same 3-per-row shape, just using more
    // of the column's reclaimed width instead of leaving it unused.
    int al_y = ss_y + ROW_H * 3 + SECTION_GAP;
    create_section_header(_container, "AIRLINES SEEN", cx, al_y);
    for (int i = 0; i < 5; i++) {
        _airline_labels[i] = lv_label_create(_container);
        lv_label_set_text(_airline_labels[i], "");
        lv_obj_set_style_text_font(_airline_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(_airline_labels[i], lv_color_hex(0xccccdd), 0);
        lv_obj_set_pos(_airline_labels[i], cx + (i % 3) * 100, al_y + ROW_H + (i / 3) * ROW_H);
        lv_obj_clear_flag(_airline_labels[i], LV_OBJ_FLAG_CLICKABLE);
    }

    // Top aircraft types -- same reasoning as above (2 rows of airlines)
    int ty_y = al_y + ROW_H * 3 + SECTION_GAP;
    create_section_header(_container, "TYPES SEEN", cx, ty_y);
    for (int i = 0; i < 5; i++) {
        _type_labels[i] = lv_label_create(_container);
        lv_label_set_text(_type_labels[i], "");
        lv_obj_set_style_text_font(_type_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(_type_labels[i], lv_color_hex(0xccccdd), 0);
        lv_obj_set_pos(_type_labels[i], cx + (i % 3) * 100, ty_y + ROW_H + (i / 3) * ROW_H);
        lv_obj_clear_flag(_type_labels[i], LV_OBJ_FLAG_CLICKABLE);
    }

#if LCD_H_RES >= 1280
    // ============================================================
    // FAR-RIGHT COLUMN: D-ATIS for the active airport (datis.clowd.io).
    // Scrollable — ATIS text is often longer than the panel. Separate
    // ARR/DEP reports (e.g. KDEN) are stacked with headers.
    // ============================================================
    {
        const int ax = 880;
        const int atis_w = STATS_W - ax - 24;
        const int atis_h = STATS_H - top_y - 16;

        lv_obj_t *atis_header = lv_label_create(_container);
        lv_label_set_text(atis_header, "ATIS");
        lv_obj_set_style_text_font(atis_header, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(atis_header, ACCENT_COLOR, 0);
        lv_obj_set_pos(atis_header, ax, top_y);
        lv_obj_clear_flag(atis_header, LV_OBJ_FLAG_CLICKABLE);

        _atis_panel = lv_obj_create(_container);
        lv_obj_set_size(_atis_panel, atis_w, atis_h - 28);
        lv_obj_set_pos(_atis_panel, ax, top_y + 28);
        lv_obj_set_style_bg_opa(_atis_panel, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(_atis_panel, 0, 0);
        lv_obj_set_style_pad_all(_atis_panel, 0, 0);
        lv_obj_set_style_radius(_atis_panel, 0, 0);
        lv_obj_set_scroll_dir(_atis_panel, LV_DIR_VER);
        lv_obj_add_flag(_atis_panel, LV_OBJ_FLAG_SCROLLABLE);
        // Keep vertical ATIS scroll from stealing the tileview swipe.
        lv_obj_clear_flag(_atis_panel, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_set_scrollbar_mode(_atis_panel, LV_SCROLLBAR_MODE_AUTO);

        _atis_lbl = lv_label_create(_atis_panel);
        lv_label_set_text(_atis_lbl, "");
        lv_label_set_long_mode(_atis_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_atis_lbl, atis_w - 4);
        lv_obj_set_style_text_font(_atis_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(_atis_lbl, DIM_COLOR, 0);
        lv_obj_set_pos(_atis_lbl, 0, 0);
        lv_obj_clear_flag(_atis_lbl, LV_OBJ_FLAG_CLICKABLE);
    }
#endif

    // Refresh timer -- stats_update() just reads whatever is currently in
    // the live aircraft list, so polling it more often than the ~20s fetch
    // cadence doesn't "recount stale data" (the reasoning this used to be
    // pinned to the fetch interval for); it just redraws the same numbers
    // an extra few times between fetches, which is cheap. The real cost of
    // the old 20s interval showed up right after a location switch: the
    // aircraft-list clear (fetcher.cpp's fetcher_request_immediate_fetch())
    // and the fresh fetch landing could both complete within a couple of
    // seconds, but the visible reset/repopulation of UNIQUE/PEAK/CLOSEST
    // stayed invisible for up to 20s until this timer's next tick happened
    // to fire -- reported as "stats do update but it takes a while."
    lv_timer_create(refresh_stats, 2000, nullptr);
}

void stats_view_update() {
    // triggered externally if needed
}
