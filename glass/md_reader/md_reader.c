/*
 * MD Reader glasses plugin.
 *
 * The paired phone plugin owns the notes: it renders Markdown to display
 * text, resolves links and keeps the back history. This plugin owns layout
 * and input: it paginates bounded text windows with the firmware font, draws
 * link underlines and the selected link, and turns head motion into paging
 * (pitch) and link selection (yaw) locally, so scrolling never waits on
 * Bluetooth.
 */
#include "gm_plugin_lvgl_api.h"
#include "gm_plugin_libc.h"

#define MD_CHANNEL UINT16_C(0x4D44)
#define MD_EVENT_CHANNEL UINT16_C(0x4D45)
#define PROTOCOL_VERSION 1U

#define COMMAND_OPEN 1U
#define COMMAND_WINDOW 2U
#define COMMAND_SETTINGS 3U
#define COMMAND_CLOSE 4U
#define COMMAND_NOTICE 5U
#define COMMAND_AUTOSCROLL 6U

#define EVENT_NEED_WINDOW 1U
#define EVENT_PAGE 2U
#define EVENT_FOLLOW 3U
#define EVENT_BACK 4U
#define EVENT_HELLO 5U
#define EVENT_AUTOSCROLL 6U
#define EVENT_DIAG 7U

#define GLASSES_VERSION 7U
#define DIAG_BYTES 120U

#define OPEN_HEADER_BYTES 21U
#define WINDOW_HEADER_BYTES 13U
#define WINDOW_LINK_BYTES 10U
#define SETTINGS_BYTES 14U
#define SETTINGS_V1_BYTES 12U
#define AUTOSCROLL_BYTES 9U
#define MIN_AUTOSCROLL_MS 500U

#define WINDOW_BYTES 8192U
#define WINDOW_GUARD_BYTES 512U
#define PAGE_BYTES 4096U
#define MAX_PAGE_LINES 16U
#define MAX_PAGES 2048U
#define MAX_WINDOW_LINKS 128U
#define MAX_PAGE_LINKS 32U
#define MAX_UNDERLINES 40U
#define MAX_SELECT_BOXES 4U
#define BOX_PAD_X 3
#define BOX_PAD_Y 2
#define TITLE_BYTES 160U
#define NOTICE_BYTES 96U
#define MEASURE_BYTES 512U
#define NO_LINK UINT16_C(0xFFFF)
#define NO_PAGE UINT16_C(0xFFFF)
#define SEEK_STEPS_PER_CALL 64U

#define BUILD_OK 0
#define BUILD_NEED_WINDOW 1
#define BUILD_ERROR 2

#define PENDING_NONE 0U
#define PENDING_SHOW 1U
#define PENDING_SEEK 2U

#define FONT_DEFAULT 0U
#define FONT_LARGE 1U

#define FLAG_INVERT_PITCH 0x01U
#define FLAG_INVERT_YAW 0x02U
#define FLAG_TILT_GUIDE 0x04U
#define FLAG_GESTURE_SLEEP 0x08U
#define FLAG_NOD_OPEN 0x10U
#define FLAG_INVERTED_LINK 0x20U

/* Shake "no" this many times to turn the display off, nod to turn it on. */
#define GESTURE_COUNT 3U
#define GESTURE_WINDOW_MS 4000U
/* The same, measured from raw gyro swings: a "no" three times is about six
 * alternating left/right swings, a triple nod six up/down ones. Five are
 * enough, each within SWING_GAP_MS of the last. */
#define SHAKE_SWINGS 5U
#define NOD_SWINGS 5U
#define SWING_GAP_MS 700U
/* Two nods (down, up, down, up) open the selected link. While a link is
 * selected, a page turn waits at least NOD_GUARD_MS so a nod is not
 * mistaken for a look down. */
#define OPEN_NOD_SWINGS 4U
#define NOD_GUARD_MS 700U

#define DEFAULT_PITCH_THRESHOLD 12U
#define DEFAULT_REPEAT_MS 0U
#define MIN_REPEAT_MS 500U
#define DEFAULT_DWELL_MS 300U
#define DEFAULT_TURN_THRESHOLD 120U
#define PITCH_HYSTERESIS 3
#define IMU_SAMPLE_MS 40U
#define CALIBRATION_SAMPLES 8U
#define PITCH_SCALE 256
#define DRIFT_TAU_MS 5000U
#define SETTLE_MS 600U
#define SETTLE_TAU_MS 150U
#define HOLD_RECENTRE_MS 2500U
#define RETURN_WINDOW_MS 6000U
#define YAW_CONFIRM_SAMPLES 2U
#define YAW_MIN_LOCK_MS 350U
#define YAW_QUIET_MS 200U

#define WINDOW_RETRY_MS 2500U
#define NOTICE_DURATION_MS 1500U
#define EXIT_HOLD_MS 3000U

#define SIDE_MARGIN 20
#define TOP_MARGIN 6
#define BOTTOM_MARGIN 4
#define LINE_SPACE 5
#define HEADER_GAP 10
#define ARROW_HALF_WIDTH 32
#define ARROW_HEIGHT 5
#define ARROW_BRIGHT_MS 2000U
#define GUIDE_WIDTH 14
/* The tilt guide's track spans +/- this many degrees from neutral. */
#define GUIDE_RANGE_DEG 40

#define COLOR_TEXT 0xFFU
#define COLOR_DIM 0xDBU
#define COLOR_FAINT 0x92U
#define COLOR_BLACK 0x00U

typedef struct {
    uint32_t start;
    uint32_t end;
    uint16_t id;
} md_link_t;

typedef struct {
    const gm_plugin_host_api_t *host;
    const gm_plugin_lvgl_api_t *ui;
    const gm_plugin_libc_extension_api_t *libc;

    gm_plugin_lvgl_obj_t *root;
    gm_plugin_lvgl_obj_t *title_label;
    gm_plugin_lvgl_obj_t *viewport;
    gm_plugin_lvgl_obj_t *page_label;
    gm_plugin_lvgl_obj_t *hint_label;
    gm_plugin_lvgl_obj_t *footer_label;
    gm_plugin_lvgl_obj_t *exit_label;
    gm_plugin_lvgl_obj_t *exit_arc;
    gm_plugin_lvgl_obj_t *underline[MAX_UNDERLINES];
    /* The selected link: an outline box per line, or (inverted) a filled box
     * with the link text redrawn dark inside it. */
    gm_plugin_lvgl_obj_t *select_box[MAX_SELECT_BOXES];
    gm_plugin_lvgl_obj_t *select_text[MAX_SELECT_BOXES];
    gm_plugin_lvgl_obj_t *guide_track;
    gm_plugin_lvgl_obj_t *guide_tick[2];
    gm_plugin_lvgl_obj_t *guide_marker;
    gm_plugin_lvgl_obj_t *scroll_arrow;
    gm_plugin_lvgl_point_t underline_points[MAX_UNDERLINES][2];
    gm_plugin_lvgl_point_t track_points[2];
    gm_plugin_lvgl_point_t tick_points[2][2];
    gm_plugin_lvgl_point_t arrow_points[3];
    int8_t arrow_pending;
    int8_t arrow_shown;
    uint32_t arrow_elapsed;

    /* Current text window, NUL-terminated for the font measurement calls. */
    char window[WINDOW_BYTES + 1U];
    uint32_t window_offset;
    uint32_t window_length;
    md_link_t window_links[MAX_WINDOW_LINKS];
    uint16_t window_link_count;
    uint8_t window_valid;
    uint8_t window_final;

    /* The page most recently laid out by build_page(). */
    char page_text[PAGE_BYTES];
    uint32_t line_offset[MAX_PAGE_LINES];
    uint16_t line_count;
    uint16_t built_page;
    uint32_t built_end;
    md_link_t page_links[MAX_PAGE_LINKS];
    uint16_t page_link_count;
    int16_t selected;

    /* Page starts discovered so far; pages are always laid out from 0. */
    uint32_t page_starts[MAX_PAGES];
    uint16_t page_count;
    uint8_t doc_complete;
    uint16_t current_page;
    uint8_t displayed;

    uint8_t pending;
    uint16_t pending_page;
    uint32_t seek_offset;
    uint16_t seek_link;

    uint8_t waiting;
    uint32_t requested_offset;
    uint32_t wait_elapsed;

    uint32_t session;
    uint32_t doc_id;
    uint32_t total_bytes;
    uint8_t history_depth;
    uint8_t connected;
    char title[TITLE_BYTES + 1U];
    char measure[MEASURE_BYTES];

    /* Settings pushed by the phone. */
    uint8_t head_enabled;
    uint8_t pitch_threshold;
    uint16_t repeat_ms;
    uint16_t dwell_ms;
    uint16_t turn_threshold;
    uint8_t flags;
    uint8_t font_mode;
    uint8_t settings_received;
    uint8_t last_settings[SETTINGS_BYTES];
    uint8_t brightness_lock;
    uint8_t brightness_level;

    /* Display control: holding brightness blocks automatic changes. */
    uint8_t display_control_available;
    uint8_t brightness_blocked;

    /* Auto-scroll: a page every auto_interval_ms, ignoring head tilt. */
    uint8_t auto_scroll;
    uint32_t auto_interval_ms;
    uint32_t auto_elapsed;

    /* Head tracking. */
    uint8_t imu_available;
    uint8_t imu_running;
    uint8_t gestures_available;
    gm_plugin_imu_modes_t imu_modes;

    /* Display power, switched by head gestures. */
    uint8_t display_power_available;
    uint8_t screen_off;
    uint8_t shake_count;
    uint8_t nod_count;
    uint32_t shake_first_ms;
    uint32_t nod_first_ms;
    int8_t shake_sign;
    uint8_t shake_swings;
    uint32_t shake_gap;
    int8_t nod_sign;
    uint8_t nod_swings;
    uint32_t nod_gap;
    uint32_t imu_elapsed;
    uint8_t calibration_count;
    int32_t calibration_sum;
    /* Neutral pose in 1/PITCH_SCALE degrees; it follows the head (see
     * update_pitch) so a calibration taken while looking at the phone heals. */
    int32_t neutral_fp;
    int32_t previous_neutral_fp;
    int8_t recentred_zone;
    uint32_t return_window;
    uint8_t settling;
    uint32_t settle_elapsed;
    /* Set once the user has shown where neutral is (a tap, a return from a
     * page turn, or a re-centre). Until then a long hold re-centres even in
     * continuous mode, so a start taken while looking at the phone heals. */
    uint8_t neutral_confirmed;
    uint8_t confirm_on_calibration;
    int8_t tilt_zone;
    uint8_t tilt_fired;
    uint32_t zone_elapsed;
    uint32_t repeat_elapsed;
    int8_t guide_zone;
    int16_t guide_y;
    uint8_t yaw_locked;
    int8_t yaw_candidate;
    uint8_t yaw_hits;
    uint32_t yaw_lock_elapsed;
    uint32_t yaw_quiet_elapsed;

    /* Layout. */
    uint16_t display_width;
    uint16_t display_height;
    int16_t viewport_x;
    int16_t viewport_y;
    int16_t viewport_width;
    int16_t viewport_height;
    int16_t font_height;
    int16_t line_height;
    uint16_t lines_per_page;

    uint32_t notice_elapsed;
    uint8_t exit_active;
    uint8_t back_sent;
    uint32_t exit_elapsed;
    uint8_t exit_seconds;
} md_reader_t;

static md_reader_t reader;

static gm_plugin_lvgl_style_value_t number(int32_t value)
{
    return gm_plugin_lvgl_style_number(value);
}

static gm_plugin_lvgl_style_value_t color(uint8_t value)
{
    return gm_plugin_lvgl_style_color(value);
}

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_u32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static void write_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static const gm_plugin_lvgl_font_t *body_font(const md_reader_t *self)
{
    return self->font_mode == FONT_LARGE ? self->ui->font_large
                                         : self->ui->font_default;
}

static void set_hidden(md_reader_t *self, gm_plugin_lvgl_obj_t *object,
                       bool hidden)
{
    if (object == 0) return;
    if (hidden) self->ui->obj_add_flag(object, GM_PLUGIN_LVGL_FLAG_HIDDEN);
    else self->ui->obj_clear_flag(object, GM_PLUGIN_LVGL_FLAG_HIDDEN);
}

static void clear_style_box(md_reader_t *self, gm_plugin_lvgl_obj_t *object)
{
    self->ui->style_set(object, GM_PLUGIN_LVGL_STYLE_PAD_TOP, number(0),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->style_set(object, GM_PLUGIN_LVGL_STYLE_PAD_BOTTOM, number(0),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->style_set(object, GM_PLUGIN_LVGL_STYLE_PAD_LEFT, number(0),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->style_set(object, GM_PLUGIN_LVGL_STYLE_PAD_RIGHT, number(0),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->style_set(object, GM_PLUGIN_LVGL_STYLE_BORDER_WIDTH, number(0),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->style_set(object, GM_PLUGIN_LVGL_STYLE_OUTLINE_WIDTH, number(0),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->style_set(object, GM_PLUGIN_LVGL_STYLE_RADIUS, number(0),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
}

/* ------------------------------------------------------------------ */
/* Phone messaging                                                     */
/* ------------------------------------------------------------------ */

static void send_hello(md_reader_t *self)
{
    uint8_t data[7];
    data[0] = PROTOCOL_VERSION;
    data[1] = EVENT_HELLO;
    write_u32(data + 2, self->session);
    /* Lets the phone show which glasses build is running. */
    data[6] = (uint8_t)GLASSES_VERSION;
    (void)self->host->bt_send(MD_EVENT_CHANNEL, data, sizeof(data));
}

/* One line for the phone plugin's Glasses log (and the Host log). */
static void diag(md_reader_t *self, const char *format, ...)
{
    uint8_t data[2U + DIAG_BYTES];
    va_list arguments;
    int length;
    va_start(arguments, format);
    length = self->libc->vsnprintf((char *)data + 2, DIAG_BYTES, format, arguments);
    va_end(arguments);
    if (length < 0) return;
    if (length >= (int)DIAG_BYTES) length = (int)DIAG_BYTES - 1;
    if (self->host->log != 0) self->host->log("md_reader: %s", (char *)data + 2);
    data[0] = PROTOCOL_VERSION;
    data[1] = EVENT_DIAG;
    (void)self->host->bt_send(MD_EVENT_CHANNEL, data, (uint32_t)(2 + length));
}

static void send_autoscroll_state(md_reader_t *self)
{
    uint8_t data[7];
    if (self->session == 0U) return;
    data[0] = PROTOCOL_VERSION;
    data[1] = EVENT_AUTOSCROLL;
    write_u32(data + 2, self->session);
    data[6] = self->auto_scroll;
    (void)self->host->bt_send(MD_EVENT_CHANNEL, data, sizeof(data));
}

static void request_window(md_reader_t *self, uint32_t offset)
{
    uint8_t data[12];
    if (self->session == 0U) return;
    if (self->waiting != 0U && self->requested_offset == offset) return;
    data[0] = PROTOCOL_VERSION;
    data[1] = EVENT_NEED_WINDOW;
    write_u32(data + 2, self->session);
    write_u32(data + 6, offset);
    write_u16(data + 10, WINDOW_BYTES);
    self->requested_offset = offset;
    self->wait_elapsed = 0U;
    self->waiting = self->host->bt_send(MD_EVENT_CHANNEL, data,
                                        sizeof(data)) == GM_PLUGIN_OK
        ? 1U : 0U;
    /* A failed send is retried by the loop timer like a lost reply. */
    if (self->waiting == 0U) {
        self->waiting = 1U;
        self->wait_elapsed = WINDOW_RETRY_MS;
    }
}

static uint16_t selected_link_id(const md_reader_t *self)
{
    if (self->selected < 0 || (uint16_t)self->selected >= self->page_link_count)
        return NO_LINK;
    return self->page_links[self->selected].id;
}

static void send_page(md_reader_t *self)
{
    uint8_t data[20];
    if (self->session == 0U || self->displayed == 0U) return;
    data[0] = PROTOCOL_VERSION;
    data[1] = EVENT_PAGE;
    write_u32(data + 2, self->session);
    write_u32(data + 6, self->page_starts[self->current_page]);
    write_u32(data + 10, self->built_end);
    write_u16(data + 14, self->current_page);
    write_u16(data + 16, self->doc_complete != 0U ? self->page_count : 0U);
    write_u16(data + 18, selected_link_id(self));
    (void)self->host->bt_send(MD_EVENT_CHANNEL, data, sizeof(data));
}

static bool send_follow(md_reader_t *self, uint16_t link_id)
{
    uint8_t data[12];
    if (self->session == 0U) return false;
    data[0] = PROTOCOL_VERSION;
    data[1] = EVENT_FOLLOW;
    write_u32(data + 2, self->session);
    write_u16(data + 6, link_id);
    write_u32(data + 8, self->page_starts[self->current_page]);
    return self->host->bt_send(MD_EVENT_CHANNEL, data, sizeof(data)) ==
        GM_PLUGIN_OK;
}

static bool send_back(md_reader_t *self)
{
    uint8_t data[6];
    if (self->session == 0U) return false;
    data[0] = PROTOCOL_VERSION;
    data[1] = EVENT_BACK;
    write_u32(data + 2, self->session);
    return self->host->bt_send(MD_EVENT_CHANNEL, data, sizeof(data)) ==
        GM_PLUGIN_OK;
}

/* ------------------------------------------------------------------ */
/* Status line                                                         */
/* ------------------------------------------------------------------ */

static void show_notice(md_reader_t *self, const char *text)
{
    if (self->hint_label == 0) return;
    self->ui->label_set_text(self->hint_label, text);
    self->notice_elapsed = NOTICE_DURATION_MS;
}

static void update_hint(md_reader_t *self)
{
    char text[32];
    if (self->hint_label == 0 || self->notice_elapsed != 0U) return;
    if (self->displayed == 0U) {
        self->ui->label_set_text(self->hint_label, "");
    } else if (self->auto_scroll != 0U) {
        self->ui->label_set_text(self->hint_label, "Auto-scroll  (tap to stop)");
    } else if (self->head_enabled == 0U && self->imu_available != 0U) {
        self->ui->label_set_text(self->hint_label, "Head scroll paused");
    } else if (self->page_link_count == 0U) {
        self->ui->label_set_text(self->hint_label,
                                 self->history_depth != 0U ? "< hold for back"
                                                           : "");
    } else {
        self->libc->snprintf(text, sizeof(text), "%u link%s",
                             (unsigned int)self->page_link_count,
                             self->page_link_count == 1U ? "" : "s");
        self->ui->label_set_text(self->hint_label, text);
    }
}

static void update_footer(md_reader_t *self)
{
    char text[32];
    if (self->footer_label == 0) return;
    if (self->displayed == 0U) {
        self->ui->label_set_text(self->footer_label, "");
        return;
    }
    if (self->doc_complete != 0U) {
        self->libc->snprintf(text, sizeof(text), "%u / %u",
                             (unsigned int)self->current_page + 1U,
                             (unsigned int)self->page_count);
    } else {
        /* 32-bit only: the RV32 target has no 64-bit division helper. */
        uint32_t start = self->page_starts[self->current_page];
        uint32_t percent = self->total_bytes < 100U ? 0U :
            start < UINT32_MAX / 100U ? start * 100U / self->total_bytes
                                      : start / (self->total_bytes / 100U);
        self->libc->snprintf(text, sizeof(text), "%u  %u%%",
                             (unsigned int)self->current_page + 1U,
                             (unsigned int)percent);
    }
    self->ui->label_set_text(self->footer_label, text);
}

static void show_arrow(md_reader_t *self, int8_t direction);

static void show_message(md_reader_t *self, const char *text)
{
    self->displayed = 0U;
    self->page_link_count = 0U;
    self->selected = -1;
    if (self->page_label != 0) self->ui->label_set_text(self->page_label, text);
    for (uint16_t index = 0U; index < MAX_SELECT_BOXES; ++index)
        set_hidden(self, self->select_box[index], true);
    for (uint16_t index = 0U; index < MAX_UNDERLINES; ++index)
        set_hidden(self, self->underline[index], true);
    show_arrow(self, 0);
    update_footer(self);
    update_hint(self);
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/* Pixels from the middle of the tilt guide for a tilt of `degrees`. */
static int32_t guide_offset(const md_reader_t *self, int32_t degrees)
{
    int32_t half = self->viewport_height / 2;
    int32_t offset = degrees * half / GUIDE_RANGE_DEG;
    if (offset < -half) offset = -half;
    if (offset > half) offset = half;
    return offset;
}

/* Ticks at +/- the tilt threshold, on the same scale as the marker, so a
 * smaller threshold draws them closer together. */
static void layout_guide_ticks(md_reader_t *self)
{
    int16_t x = (int16_t)(self->display_width - SIDE_MARGIN - GUIDE_WIDTH / 2);
    int16_t middle = (int16_t)(self->viewport_y + self->viewport_height / 2);
    int16_t reach = (int16_t)guide_offset(self, self->pitch_threshold);
    if (self->guide_tick[0] == 0) return;
    for (uint8_t tick = 0U; tick < 2U; ++tick) {
        int16_t y = (int16_t)(tick == 0U ? middle - reach : middle + reach);
        self->tick_points[tick][0].x = (int16_t)(x - 5);
        self->tick_points[tick][0].y = y;
        self->tick_points[tick][1].x = (int16_t)(x + 5);
        self->tick_points[tick][1].y = y;
        self->ui->line_set_points(self->guide_tick[tick], self->tick_points[tick], 2U);
    }
}

static void calculate_layout(md_reader_t *self)
{
    gm_plugin_lvgl_style_value_t font = {0};
    int16_t chrome_height = (int16_t)self->ui->font_get_line_height(
        self->ui->font_default);
    int16_t footer_y;
    int16_t lines;
    self->font_height = (int16_t)self->ui->font_get_line_height(body_font(self));
    if (self->font_height < 1) self->font_height = 1;
    self->line_height = (int16_t)(self->font_height + LINE_SPACE);
    self->viewport_x = SIDE_MARGIN;
    self->viewport_y = (int16_t)(TOP_MARGIN + chrome_height + HEADER_GAP);
    self->viewport_width = (int16_t)(self->display_width - SIDE_MARGIN * 2 -
                                     GUIDE_WIDTH);
    footer_y = (int16_t)(self->display_height - BOTTOM_MARGIN - chrome_height);
    lines = (int16_t)((footer_y - HEADER_GAP - self->viewport_y + LINE_SPACE) /
                      self->line_height);
    if (lines < 2) lines = 2;
    if (lines > (int16_t)MAX_PAGE_LINES) lines = MAX_PAGE_LINES;
    self->lines_per_page = (uint16_t)lines;
    self->viewport_height = (int16_t)(lines * self->line_height);

    self->ui->obj_set_pos(self->title_label, SIDE_MARGIN, TOP_MARGIN);
    self->ui->obj_set_size(self->title_label,
                           (int16_t)(self->display_width - SIDE_MARGIN * 2),
                           chrome_height);
    self->ui->obj_set_pos(self->viewport, self->viewport_x, self->viewport_y);
    self->ui->obj_set_size(self->viewport, self->viewport_width,
                           self->viewport_height);
    self->ui->obj_set_pos(self->page_label, 0, 0);
    self->ui->obj_set_size(self->page_label, self->viewport_width,
                           self->viewport_height);
    self->ui->obj_set_pos(self->hint_label, SIDE_MARGIN, footer_y);
    self->ui->obj_set_size(self->hint_label,
                           (int16_t)(self->display_width - SIDE_MARGIN * 2 - 140),
                           chrome_height);
    self->ui->obj_set_pos(self->footer_label,
                          (int16_t)(self->display_width - SIDE_MARGIN - 140),
                          footer_y);
    self->ui->obj_set_size(self->footer_label, 140, chrome_height);

    font.ptr = body_font(self);
    self->ui->style_set(self->page_label, GM_PLUGIN_LVGL_STYLE_TEXT_FONT, font,
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->style_set(self->page_label, GM_PLUGIN_LVGL_STYLE_TEXT_LINE_SPACE,
                        number(LINE_SPACE), GM_PLUGIN_LVGL_SELECTOR_MAIN);
    for (uint16_t index = 0U; index < MAX_SELECT_BOXES; ++index) {
        if (self->select_text[index] != 0)
            self->ui->style_set(self->select_text[index], GM_PLUGIN_LVGL_STYLE_TEXT_FONT,
                                font, GM_PLUGIN_LVGL_SELECTOR_MAIN);
    }

    /* Tilt guide: a track in the right margin with ticks at the thresholds. */
    {
        int16_t x = (int16_t)(self->display_width - SIDE_MARGIN - GUIDE_WIDTH / 2);
        self->track_points[0].x = x;
        self->track_points[0].y = self->viewport_y;
        self->track_points[1].x = x;
        self->track_points[1].y = (int16_t)(self->viewport_y + self->viewport_height);
        self->ui->line_set_points(self->guide_track, self->track_points, 2U);
        layout_guide_ticks(self);
        self->ui->obj_set_size(self->guide_marker, 8, 8);
        self->guide_y = INT16_MIN;
    }
}

/* ------------------------------------------------------------------ */
/* Pagination                                                          */
/* ------------------------------------------------------------------ */

/* Forgets the page layout but keeps the received text window. */
static void reset_pagination(md_reader_t *self)
{
    self->page_starts[0] = 0U;
    self->page_count = 1U;
    self->doc_complete = 0U;
    self->current_page = 0U;
    self->built_page = NO_PAGE;
    self->built_end = 0U;
    self->line_count = 0U;
    self->page_link_count = 0U;
    self->selected = -1;
    self->displayed = 0U;
    self->pending = PENDING_NONE;
    self->waiting = 0U;
}

static void reset_document(md_reader_t *self)
{
    self->window_valid = 0U;
    self->window_offset = 0U;
    self->window_length = 0U;
    self->window_link_count = 0U;
    self->window_final = 0U;
    self->window[0] = '\0';
    reset_pagination(self);
}

static void wipe_private_data(md_reader_t *self)
{
    reset_document(self);
    self->libc->memset(self->window, 0, sizeof(self->window));
    self->libc->memset(self->page_text, 0, sizeof(self->page_text));
    self->libc->memset(self->title, 0, sizeof(self->title));
    self->session = 0U;
    self->doc_id = 0U;
    self->total_bytes = 0U;
    self->history_depth = 0U;
    self->auto_scroll = 0U;
}

static void record_next_start(md_reader_t *self, uint16_t index, uint32_t end)
{
    /* Skip blank lines at the top of the following page. */
    while (end < self->total_bytes && end >= self->window_offset &&
           end < self->window_offset + self->window_length &&
           self->window[end - self->window_offset] == '\n')
        ++end;
    if (end >= self->total_bytes) {
        self->doc_complete = 1U;
        return;
    }
    if (index + 1U != self->page_count) return;
    if (self->page_count >= MAX_PAGES) {
        self->doc_complete = 1U;
        return;
    }
    self->page_starts[self->page_count++] = end;
}

static int build_page(md_reader_t *self, uint16_t index)
{
    uint32_t start;
    uint32_t cursor;
    uint32_t output = 0U;
    uint16_t lines = 0U;
    if (index >= self->page_count) return BUILD_ERROR;
    if (self->built_page == index) return BUILD_OK;
    start = self->page_starts[index];
    if (start >= self->total_bytes) return BUILD_ERROR;
    if (self->window_valid == 0U || start < self->window_offset ||
        start >= self->window_offset + self->window_length)
        return BUILD_NEED_WINDOW;
    cursor = start - self->window_offset;
    while (lines < self->lines_per_page && cursor < self->window_length) {
        uint32_t step;
        if (self->window_final == 0U &&
            self->window_length - cursor < WINDOW_GUARD_BYTES)
            return start == self->window_offset ? BUILD_ERROR
                                                : BUILD_NEED_WINDOW;
        step = self->ui->text_get_next_line(
            self->window + cursor, body_font(self), 0, self->viewport_width,
            0, GM_PLUGIN_LVGL_TEXT_FLAG_NONE);
        if (step == 0U || cursor + step > self->window_length) break;
        if (output + step >= PAGE_BYTES) break;
        self->line_offset[lines] = self->window_offset + cursor;
        self->libc->memcpy(self->page_text + output, self->window + cursor, step);
        output += step;
        cursor += step;
        ++lines;
    }
    if (lines == 0U) return BUILD_ERROR;
    /* A trailing newline would render as an empty extra line. */
    if (output > 0U && self->page_text[output - 1U] == '\n') --output;
    self->page_text[output] = '\0';
    self->line_count = lines;
    self->built_page = index;
    self->built_end = self->window_offset + cursor;
    record_next_start(self, index, self->built_end);
    return BUILD_OK;
}

static void apply_selection(md_reader_t *self);

static int16_t measure_prefix(md_reader_t *self, uint32_t from, uint32_t to)
{
    gm_plugin_lvgl_point_t size = {0};
    uint32_t length = to - from;
    if (length == 0U) return 0;
    if (length >= MEASURE_BYTES) length = MEASURE_BYTES - 1U;
    self->libc->memcpy(self->measure, self->page_text + from, length);
    self->measure[length] = '\0';
    self->ui->text_get_size(&size, self->measure, body_font(self), 0, 0,
                            INT16_MAX, GM_PLUGIN_LVGL_TEXT_FLAG_NONE);
    return (int16_t)size.x;
}

/* The part of `link` on page line `line`, as page-text byte offsets and
 * pixel x positions. False when the link is not on that line. */
static bool link_segment(md_reader_t *self, const md_link_t *link, uint16_t line,
                         uint32_t *from_out, uint32_t *to_out,
                         int16_t *x0_out, int16_t *x1_out)
{
    uint32_t page_start = self->page_starts[self->current_page];
    uint32_t text_end = page_start + self->libc->strlen(self->page_text);
    uint32_t line_start = self->line_offset[line];
    uint32_t line_end = line + 1U < self->line_count
        ? self->line_offset[line + 1U] : text_end;
    uint32_t from = link->start < line_start ? line_start : link->start;
    uint32_t to = link->end > line_end ? line_end : link->end;
    /* Not the line break, nor the space before a wrap. */
    while (to > from && (self->page_text[to - 1U - page_start] == '\n' ||
                         self->page_text[to - 1U - page_start] == ' '))
        --to;
    if (to <= from) return false;
    *x0_out = measure_prefix(self, line_start - page_start, from - page_start);
    *x1_out = measure_prefix(self, line_start - page_start, to - page_start);
    *from_out = from - page_start;
    *to_out = to - page_start;
    return *x1_out > *x0_out;
}

static void apply_selection(md_reader_t *self)
{
    uint16_t used = 0U;
    bool inverted = (self->flags & FLAG_INVERTED_LINK) != 0U;
    if (self->selected >= 0 && (uint16_t)self->selected < self->page_link_count) {
        const md_link_t *link = &self->page_links[self->selected];
        for (uint16_t line = 0U; line < self->line_count && used < MAX_SELECT_BOXES; ++line) {
            uint32_t from;
            uint32_t to;
            int16_t x0;
            int16_t x1;
            gm_plugin_lvgl_obj_t *box = self->select_box[used];
            gm_plugin_lvgl_obj_t *text = self->select_text[used];
            if (!link_segment(self, link, line, &from, &to, &x0, &x1)) continue;
            self->ui->obj_set_pos(box, x0 - BOX_PAD_X,
                                  (int16_t)(line * self->line_height - BOX_PAD_Y));
            self->ui->obj_set_size(box, x1 - x0 + BOX_PAD_X * 2,
                                   self->font_height + BOX_PAD_Y * 2);
            self->ui->style_set(box, GM_PLUGIN_LVGL_STYLE_BG_OPA,
                                number(inverted ? GM_PLUGIN_LVGL_OPA_COVER
                                                : GM_PLUGIN_LVGL_OPA_TRANSPARENT),
                                GM_PLUGIN_LVGL_SELECTOR_MAIN);
            self->ui->style_set(box, GM_PLUGIN_LVGL_STYLE_BORDER_WIDTH,
                                number(inverted ? 0 : 2), GM_PLUGIN_LVGL_SELECTOR_MAIN);
            if (inverted) {
                uint32_t length = to - from;
                if (length >= MEASURE_BYTES) length = MEASURE_BYTES - 1U;
                self->libc->memcpy(self->measure, self->page_text + from, length);
                self->measure[length] = '\0';
                self->ui->label_set_text(text, self->measure);
            }
            set_hidden(self, text, !inverted);
            set_hidden(self, box, false);
            ++used;
        }
    }
    for (; used < MAX_SELECT_BOXES; ++used) set_hidden(self, self->select_box[used], true);
    update_hint(self);
}

static void layout_underlines(md_reader_t *self)
{
    uint16_t used = 0U;
    for (uint16_t link = 0U; link < self->page_link_count; ++link) {
        for (uint16_t line = 0U; line < self->line_count; ++line) {
            uint32_t from;
            uint32_t to;
            int16_t x0;
            int16_t x1;
            int16_t y = (int16_t)(line * self->line_height + self->font_height);
            if (used >= MAX_UNDERLINES ||
                !link_segment(self, &self->page_links[link], line, &from, &to, &x0, &x1))
                continue;
            self->underline_points[used][0].x = x0;
            self->underline_points[used][0].y = y;
            self->underline_points[used][1].x = x1;
            self->underline_points[used][1].y = y;
            self->ui->line_set_points(self->underline[used],
                                      self->underline_points[used], 2U);
            set_hidden(self, self->underline[used], false);
            ++used;
        }
    }
    for (; used < MAX_UNDERLINES; ++used)
        set_hidden(self, self->underline[used], true);
}

static void collect_page_links(md_reader_t *self)
{
    uint32_t page_start = self->page_starts[self->current_page];
    uint32_t page_end = page_start + self->libc->strlen(self->page_text);
    self->page_link_count = 0U;
    for (uint16_t index = 0U; index < self->window_link_count &&
         self->page_link_count < MAX_PAGE_LINKS; ++index) {
        md_link_t link = self->window_links[index];
        if (link.end <= page_start || link.start >= page_end) continue;
        if (link.start < page_start) link.start = page_start;
        if (link.end > page_end) link.end = page_end;
        self->page_links[self->page_link_count++] = link;
    }
}

/* A skinny chevron above the text (scrolled up) or below it (scrolled down),
 * bright for a moment, then dim until the next page turn. */
static void show_arrow(md_reader_t *self, int8_t direction)
{
    int16_t centre = (int16_t)(self->display_width / 2);
    int16_t top;
    int16_t tip;
    int16_t base;
    self->arrow_shown = direction;
    if (self->scroll_arrow == 0) return;
    if (direction == 0) {
        set_hidden(self, self->scroll_arrow, true);
        return;
    }
    if (direction > 0) {
        top = (int16_t)(self->viewport_y + self->viewport_height +
                        (HEADER_GAP - ARROW_HEIGHT) / 2);
        base = top;
        tip = (int16_t)(top + ARROW_HEIGHT);
    } else {
        top = (int16_t)(self->viewport_y - HEADER_GAP + (HEADER_GAP - ARROW_HEIGHT) / 2);
        base = (int16_t)(top + ARROW_HEIGHT);
        tip = top;
    }
    self->arrow_points[0].x = centre - ARROW_HALF_WIDTH;
    self->arrow_points[0].y = base;
    self->arrow_points[1].x = centre;
    self->arrow_points[1].y = tip;
    self->arrow_points[2].x = centre + ARROW_HALF_WIDTH;
    self->arrow_points[2].y = base;
    self->ui->line_set_points(self->scroll_arrow, self->arrow_points, 3U);
    self->ui->style_set(self->scroll_arrow, GM_PLUGIN_LVGL_STYLE_LINE_COLOR,
                        color(COLOR_TEXT), GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->arrow_elapsed = ARROW_BRIGHT_MS;
    set_hidden(self, self->scroll_arrow, false);
}

/* Requires build_page(index) to have returned BUILD_OK. */
static void render_page(md_reader_t *self, uint16_t index, uint16_t select_id)
{
    self->current_page = index;
    self->displayed = 1U;
    self->ui->label_set_text(self->page_label, self->page_text);
    collect_page_links(self);
    self->selected = -1;
    for (uint16_t link = 0U; link < self->page_link_count; ++link) {
        if (select_id != NO_LINK && self->page_links[link].id == select_id) {
            self->selected = (int16_t)link;
            break;
        }
    }
    layout_underlines(self);
    apply_selection(self);
    /* Only a page turn gets an arrow; opening or jumping clears it. */
    show_arrow(self, self->arrow_pending);
    self->arrow_pending = 0;
    self->auto_elapsed = 0U;
    update_footer(self);
    update_hint(self);
    send_page(self);
}

static void show_page(md_reader_t *self, uint16_t index, uint16_t select_id)
{
    int result = build_page(self, index);
    if (result == BUILD_OK) {
        self->pending = PENDING_NONE;
        render_page(self, index, select_id);
        return;
    }
    if (result == BUILD_NEED_WINDOW) {
        self->pending = PENDING_SHOW;
        self->pending_page = index;
        self->seek_link = select_id;
        request_window(self, self->page_starts[index]);
        return;
    }
    self->pending = PENDING_NONE;
    show_notice(self, "Layout error");
}

/* Lays pages out from the start until the one containing seek_offset. */
static void continue_seek(md_reader_t *self)
{
    for (uint16_t step = 0U; step < SEEK_STEPS_PER_CALL; ++step) {
        uint16_t index = self->page_count - 1U;
        int result;
        while (index > 0U && self->page_starts[index] > self->seek_offset) --index;
        if (index + 1U < self->page_count || self->doc_complete != 0U) {
            show_page(self, index, self->seek_link);
            return;
        }
        result = build_page(self, index);
        if (result == BUILD_NEED_WINDOW) {
            self->pending = PENDING_SEEK;
            request_window(self, self->page_starts[index]);
            return;
        }
        if (result != BUILD_OK) {
            self->pending = PENDING_NONE;
            show_page(self, 0U, NO_LINK);
            return;
        }
    }
    /* Yield to the display task; the loop resumes the seek next frame. */
    self->pending = PENDING_SEEK;
}

static void seek(md_reader_t *self, uint32_t offset, uint16_t select_id)
{
    self->seek_offset = offset;
    self->seek_link = select_id;
    self->pending = PENDING_SEEK;
    self->arrow_pending = 0;
    continue_seek(self);
}

static void next_page(md_reader_t *self)
{
    if (self->displayed == 0U || self->pending != PENDING_NONE) return;
    if (self->current_page + 1U >= self->page_count) {
        show_notice(self, "End of note");
        return;
    }
    self->arrow_pending = 1;
    show_page(self, (uint16_t)(self->current_page + 1U), NO_LINK);
}

static void previous_page(md_reader_t *self)
{
    if (self->displayed == 0U || self->pending != PENDING_NONE) return;
    if (self->current_page == 0U) {
        show_notice(self, "Top of note");
        return;
    }
    self->arrow_pending = -1;
    show_page(self, (uint16_t)(self->current_page - 1U), NO_LINK);
}

static void step_link(md_reader_t *self, int8_t direction)
{
    if (self->displayed == 0U) return;
    if (self->page_link_count == 0U) {
        show_notice(self, "No links on this page");
        return;
    }
    if (self->selected < 0) {
        self->selected = direction > 0 ? 0
            : (int16_t)(self->page_link_count - 1U);
    } else {
        int16_t next = (int16_t)(self->selected + direction);
        if (next < 0) next = (int16_t)(self->page_link_count - 1U);
        if (next >= (int16_t)self->page_link_count) next = 0;
        self->selected = next;
    }
    apply_selection(self);
    send_page(self);
}

static void follow_selected(md_reader_t *self)
{
    uint16_t id = selected_link_id(self);
    if (id == NO_LINK) {
        show_notice(self, self->page_link_count == 0U ? "No links on this page"
                                                      : "Turn head to pick a link");
        return;
    }
    if (send_follow(self, id)) show_notice(self, "Opening...");
}

/* ------------------------------------------------------------------ */
/* Exit countdown (long press with no history)                        */
/* ------------------------------------------------------------------ */

static void show_exit(md_reader_t *self)
{
    char text[24];
    uint8_t seconds = (uint8_t)((EXIT_HOLD_MS - self->exit_elapsed + 999U) / 1000U);
    self->ui->arc_set_value(self->exit_arc, (int16_t)self->exit_elapsed);
    if (seconds == self->exit_seconds) return;
    self->exit_seconds = seconds;
    self->libc->snprintf(text, sizeof(text), "EXIT IN %u", (unsigned int)seconds);
    self->ui->label_set_text(self->exit_label, text);
    set_hidden(self, self->exit_label, false);
    set_hidden(self, self->exit_arc, false);
}

static void set_exit(md_reader_t *self, bool active)
{
    self->exit_active = active ? 1U : 0U;
    self->exit_elapsed = 0U;
    self->exit_seconds = 0xFFU;
    if (active) {
        show_exit(self);
    } else {
        set_hidden(self, self->exit_label, true);
        set_hidden(self, self->exit_arc, true);
    }
}

static void exit_app(md_reader_t *self)
{
    wipe_private_data(self);
    self->host->app_exit();
}

static void go_back(md_reader_t *self)
{
    if (self->history_depth != 0U && send_back(self)) show_notice(self, "Back");
}

/* ------------------------------------------------------------------ */
/* Head tracking                                                       */
/* ------------------------------------------------------------------ */

static void reset_head_state(md_reader_t *self)
{
    self->calibration_count = 0U;
    self->calibration_sum = 0;
    self->tilt_zone = 0;
    self->tilt_fired = 0U;
    self->zone_elapsed = 0U;
    self->repeat_elapsed = 0U;
    self->settling = 0U;
    self->settle_elapsed = 0U;
    self->neutral_confirmed = 0U;
    self->return_window = 0U;
    self->recentred_zone = 0;
    self->yaw_locked = 0U;
    self->yaw_hits = 0U;
    self->yaw_candidate = 0;
    self->imu_elapsed = 0U;
}

static void update_guide_visibility(md_reader_t *self)
{
    bool hidden = self->head_enabled == 0U || self->imu_running == 0U ||
        self->auto_scroll != 0U ||
        (self->flags & FLAG_TILT_GUIDE) == 0U ||
        self->calibration_count < CALIBRATION_SAMPLES;
    set_hidden(self, self->guide_track, hidden);
    set_hidden(self, self->guide_tick[0], hidden);
    set_hidden(self, self->guide_tick[1], hidden);
    set_hidden(self, self->guide_marker, hidden);
}

/* Enables what the settings need: raw samples for head control (not while
 * the display is off) and firmware nod/shake events for display power, which
 * stay on while the display is off so a nod can wake it. `active` false is
 * suspend/stop. */
static void set_imu(md_reader_t *self, bool active)
{
    gm_plugin_imu_modes_t wanted = GM_PLUGIN_IMU_ENABLE_NONE;
    if (self->imu_available == 0U && self->gestures_available == 0U) return;
    /* Raw samples drive head control and the shake/nod detector. */
    if (active && self->imu_available != 0U &&
        ((self->head_enabled != 0U && self->screen_off == 0U) ||
         (self->flags & FLAG_GESTURE_SLEEP) != 0U))
        wanted |= GM_PLUGIN_IMU_ENABLE_RAW;
    if (self->gestures_available != 0U &&
        (self->flags & FLAG_GESTURE_SLEEP) != 0U &&
        (active || self->screen_off != 0U))
        wanted |= GM_PLUGIN_IMU_ENABLE_GESTURES;
    if (wanted != self->imu_modes) {
        gm_plugin_result_t result = self->host->imu_enable(wanted);
        diag(self, "imu_enable(%u) -> %d", (unsigned int)wanted, (int)result);
        if (result == GM_PLUGIN_OK) self->imu_modes = wanted;
    }
    self->imu_running = (self->imu_modes & GM_PLUGIN_IMU_ENABLE_RAW) != 0U ? 1U : 0U;
    reset_head_state(self);
    update_guide_visibility(self);
}

static void update_guide(md_reader_t *self, int32_t delta_fp)
{
    /* Looking up moves the marker up; it crosses a tick at the threshold.
     * delta_fp keeps fractions of a degree so the marker lines up exactly. */
    int32_t half = self->viewport_height / 2;
    int32_t offset = delta_fp * half / (GUIDE_RANGE_DEG * PITCH_SCALE);
    int16_t y;
    if (offset < -half) offset = -half;
    if (offset > half) offset = half;
    y = (int16_t)(self->viewport_y + half - offset - 4);
    if (y != self->guide_y) {
        self->guide_y = y;
        self->ui->obj_set_pos(self->guide_marker,
            (int16_t)(self->display_width - SIDE_MARGIN - GUIDE_WIDTH / 2 - 4), y);
    }
    if (self->guide_zone != self->tilt_zone) {
        self->guide_zone = self->tilt_zone;
        self->ui->style_set(self->guide_marker, GM_PLUGIN_LVGL_STYLE_BG_COLOR,
                            color(self->tilt_zone != 0 ? COLOR_TEXT : COLOR_FAINT),
                            GM_PLUGIN_LVGL_SELECTOR_MAIN);
    }
}

/* Moves the neutral pose toward the current pitch with time constant tau. */
static void track_neutral(md_reader_t *self, int16_t pitch, uint32_t dt,
                          uint32_t tau)
{
    int32_t error = (int32_t)pitch * PITCH_SCALE - self->neutral_fp;
    if (dt >= tau) self->neutral_fp += error;
    else self->neutral_fp += error * (int32_t)dt / (int32_t)tau;
}

static void leave_zone(md_reader_t *self, bool settle)
{
    self->tilt_zone = 0;
    self->tilt_fired = 0U;
    self->zone_elapsed = 0U;
    self->repeat_elapsed = 0U;
    self->settling = settle ? 1U : 0U;
    self->settle_elapsed = 0U;
    /* Every settle captures a pose the user chose to hold. */
    if (settle) self->neutral_confirmed = 1U;
}

static void turn_page(md_reader_t *self, int8_t zone)
{
    /* Looking down reads further down the note; looking up goes back. */
    if (zone < 0) next_page(self);
    else previous_page(self);
}

/*
 * Pitch paging. A tilt past the threshold, held for the dwell, turns one page;
 * in continuous mode (repeat_ms != 0) it keeps turning a page every repeat_ms
 * while held. Looking down pages down. The neutral pose is not fixed:
 *  - coming back from a page turn, wherever the head settles is the new
 *    neutral, so the return swing never pages the other way;
 *  - outside a tilt, neutral slowly follows posture changes;
 *  - a tilt held well past its page turn is taken as the new reading pose
 *    (this heals a calibration taken while looking down at the phone), and
 *    a quick return to the old pose soon after restores it without paging.
 *    Continuous mode skips this once neutral is confirmed.
 */
static bool nod_opens_link(const md_reader_t *self);

static void update_pitch(md_reader_t *self, int16_t pitch, uint32_t dt)
{
    int16_t enter = (int16_t)self->pitch_threshold;
    int16_t leave = (int16_t)(enter - PITCH_HYSTERESIS);
    int16_t delta;
    uint32_t hold_limit;
    if (leave < 1) leave = 1;
    if (self->settling != 0U) {
        track_neutral(self, pitch, dt, SETTLE_TAU_MS);
        self->settle_elapsed += dt;
        if (self->settle_elapsed >= SETTLE_MS) self->settling = 0U;
        return;
    }
    self->return_window = self->return_window > dt ? self->return_window - dt : 0U;
    if (self->tilt_zone == 0) track_neutral(self, pitch, dt, DRIFT_TAU_MS);
    delta = (int16_t)(((int32_t)pitch * PITCH_SCALE - self->neutral_fp) /
                      PITCH_SCALE);
    if (self->tilt_zone == 0) {
        int8_t zone = delta >= enter ? 1 : (delta <= -enter ? -1 : 0);
        int32_t from_previous =
            (int32_t)pitch * PITCH_SCALE - self->previous_neutral_fp;
        if (zone == 0) return;
        if (self->return_window != 0U && zone == -self->recentred_zone &&
            from_previous < enter * PITCH_SCALE &&
            from_previous > -enter * PITCH_SCALE) {
            self->neutral_fp = self->previous_neutral_fp;
            self->return_window = 0U;
            leave_zone(self, true);
            return;
        }
        self->tilt_zone = zone;
        self->tilt_fired = 0U;
        self->zone_elapsed = 0U;
        self->repeat_elapsed = 0U;
    } else if ((self->tilt_zone > 0 && delta < leave) ||
               (self->tilt_zone < 0 && delta > -leave)) {
        /* A glance that never turned a page needs no recentring. */
        leave_zone(self, self->tilt_fired != 0U);
        return;
    }
    self->zone_elapsed += dt;
    if (self->tilt_fired == 0U) {
        uint32_t dwell = self->dwell_ms;
        /* With a link selected a nod may be starting: wait a little longer,
         * and never turn the page in the middle of nodding. */
        if (nod_opens_link(self)) {
            if (dwell < NOD_GUARD_MS) dwell = NOD_GUARD_MS;
            if (self->nod_swings >= 2U) return;
        }
        if (self->zone_elapsed < dwell) return;
        self->tilt_fired = 1U;
        self->repeat_elapsed = 0U;
        turn_page(self, self->tilt_zone);
        return;
    }
    /* Continuous mode keeps paging for as long as the tilt is held, once
     * neutral is known to be right. */
    hold_limit = self->repeat_ms != 0U && self->neutral_confirmed != 0U
        ? UINT32_MAX : HOLD_RECENTRE_MS;
    if (self->zone_elapsed >= hold_limit) {
        self->previous_neutral_fp = self->neutral_fp;
        self->recentred_zone = self->tilt_zone;
        self->return_window = RETURN_WINDOW_MS;
        leave_zone(self, true);
        return;
    }
    if (self->repeat_ms == 0U) return;
    self->repeat_elapsed += dt;
    if (self->repeat_elapsed < self->repeat_ms) return;
    self->repeat_elapsed = 0U;
    turn_page(self, self->tilt_zone);
}

static void update_yaw(md_reader_t *self, const gm_plugin_imu_sample_t *sample,
                       uint32_t dt)
{
    int32_t horizontal = (int32_t)sample->gyro_raw[0] - sample->gyro_raw[1];
    int32_t vertical = sample->gyro_raw[2];
    int32_t abs_horizontal;
    int32_t abs_vertical = vertical < 0 ? -vertical : vertical;
    if ((self->flags & FLAG_INVERT_YAW) != 0U) horizontal = -horizontal;
    abs_horizontal = horizontal < 0 ? -horizontal : horizontal;
    if (self->yaw_locked != 0U) {
        /* Ignore the return swing until the head has settled. */
        self->yaw_lock_elapsed += dt;
        if (abs_horizontal < (int32_t)self->turn_threshold / 3)
            self->yaw_quiet_elapsed += dt;
        else
            self->yaw_quiet_elapsed = 0U;
        if (self->yaw_lock_elapsed >= YAW_MIN_LOCK_MS &&
            self->yaw_quiet_elapsed >= YAW_QUIET_MS) {
            self->yaw_locked = 0U;
            self->yaw_hits = 0U;
        }
        return;
    }
    if (abs_horizontal >= (int32_t)self->turn_threshold &&
        abs_horizontal > abs_vertical) {
        int8_t direction = horizontal < 0 ? -1 : 1;
        if (direction != self->yaw_candidate) {
            self->yaw_candidate = direction;
            self->yaw_hits = 0U;
        }
        if (++self->yaw_hits < YAW_CONFIRM_SAMPLES) return;
        self->yaw_locked = 1U;
        self->yaw_lock_elapsed = 0U;
        self->yaw_quiet_elapsed = 0U;
        step_link(self, direction);
    } else {
        self->yaw_hits = 0U;
    }
}

static void set_screen(md_reader_t *self, bool on);

/* Counts direction reversals of a gyro rate past `threshold`; true when a
 * new swing was counted. A pause longer than SWING_GAP_MS starts over. */
static bool count_swing(int8_t *sign, uint8_t *swings, uint32_t *gap,
                        int32_t rate, int32_t threshold, uint32_t dt)
{
    int8_t now;
    *gap += dt;
    if (*swings != 0U && *gap > SWING_GAP_MS) {
        *swings = 0U;
        *sign = 0;
    }
    if (rate < threshold && rate > -threshold) return false;
    now = rate > 0 ? 1 : -1;
    if (now == *sign) return false;
    *sign = now;
    *gap = 0U;
    if (*swings < UINT8_MAX) ++*swings;
    return true;
}

static bool nod_opens_link(const md_reader_t *self)
{
    return (self->flags & FLAG_NOD_OPEN) != 0U && self->head_enabled != 0U &&
        self->screen_off == 0U && self->selected >= 0;
}

/* Gyro gestures: shake to turn the display off, nod to turn it on, and two
 * nods to open the selected link. */
static void update_head_gestures(md_reader_t *self,
                                 const gm_plugin_imu_sample_t *sample,
                                 uint32_t dt)
{
    int32_t horizontal = (int32_t)sample->gyro_raw[0] - sample->gyro_raw[1];
    int32_t vertical = sample->gyro_raw[2];
    int32_t threshold = (int32_t)self->turn_threshold;
    bool sleep = (self->flags & FLAG_GESTURE_SLEEP) != 0U;
    char text[24];
    if (sleep && self->screen_off == 0U &&
        count_swing(&self->shake_sign, &self->shake_swings, &self->shake_gap,
                    horizontal, threshold, dt)) {
        if (self->shake_swings >= SHAKE_SWINGS) {
            self->shake_swings = 0U;
            self->shake_sign = 0;
            diag(self, "shake detected from gyro");
            set_screen(self, false);
            return;
        }
        if (self->shake_swings >= 2U) {
            /* A link flick and its return make two swings; show from three. */
            if (self->shake_swings >= 3U) {
                self->libc->snprintf(text, sizeof(text), "Shake %u/%u",
                                     (unsigned int)self->shake_swings,
                                     (unsigned int)SHAKE_SWINGS);
                show_notice(self, text);
            }
            diag(self, "shake swing %u/%u (rate %ld)", (unsigned int)self->shake_swings,
                 (unsigned int)SHAKE_SWINGS, (long)horizontal);
        }
    }
    if (!(sleep && self->screen_off != 0U) && !nod_opens_link(self)) {
        self->nod_swings = 0U;
        self->nod_sign = 0;
        return;
    }
    if (!count_swing(&self->nod_sign, &self->nod_swings, &self->nod_gap,
                     vertical, threshold, dt))
        return;
    diag(self, "nod swing %u (rate %ld)", (unsigned int)self->nod_swings, (long)vertical);
    if (self->screen_off != 0U) {
        if (self->nod_swings < NOD_SWINGS) return;
        self->nod_swings = 0U;
        self->nod_sign = 0;
        diag(self, "nod detected from gyro");
        set_screen(self, true);
        return;
    }
    if (self->nod_swings == 2U) show_notice(self, "Nod again to open");
    if (self->nod_swings < OPEN_NOD_SWINGS) return;
    self->nod_swings = 0U;
    self->nod_sign = 0;
    diag(self, "double nod: open link");
    follow_selected(self);
}

static void update_head(md_reader_t *self, uint32_t elapsed_ms)
{
    gm_plugin_imu_sample_t sample;
    uint32_t dt;
    int16_t pitch;
    if (self->imu_running == 0U) return;
    self->imu_elapsed += elapsed_ms;
    if (self->imu_elapsed < IMU_SAMPLE_MS) return;
    dt = self->imu_elapsed > 250U ? 250U : self->imu_elapsed;
    self->imu_elapsed = 0U;
    if (self->host->imu_read(&sample) != GM_PLUGIN_OK) return;
    update_head_gestures(self, &sample, dt);
    if (self->head_enabled == 0U || self->screen_off != 0U) return;
    pitch = sample.pitch_degrees;
    if ((self->flags & FLAG_INVERT_PITCH) != 0U) pitch = (int16_t)-pitch;
    if (self->calibration_count < CALIBRATION_SAMPLES) {
        self->calibration_sum += pitch;
        if (++self->calibration_count == CALIBRATION_SAMPLES) {
            self->neutral_fp = self->calibration_sum * PITCH_SCALE /
                (int32_t)CALIBRATION_SAMPLES;
            self->previous_neutral_fp = self->neutral_fp;
            self->neutral_confirmed = self->confirm_on_calibration;
            self->confirm_on_calibration = 0U;
            self->guide_zone = 2;
            update_guide_visibility(self);
        }
        return;
    }
    /* Auto-scroll pages on a timer "regardless of where I'm looking". */
    if (self->auto_scroll == 0U) update_pitch(self, pitch, dt);
    if ((self->flags & FLAG_TILT_GUIDE) != 0U)
        update_guide(self, (int32_t)pitch * PITCH_SCALE - self->neutral_fp);
    if (self->tilt_zone == 0 && self->settling == 0U)
        update_yaw(self, &sample, dt);
}

/* ------------------------------------------------------------------ */
/* Message handlers                                                    */
/* ------------------------------------------------------------------ */

static void apply_brightness(md_reader_t *self)
{
    const gm_plugin_display_control_api_t *display = &self->host->display_control;
    if (self->display_control_available == 0U) return;
    if (self->brightness_lock != 0U) {
        if (self->brightness_blocked == 0U) {
            gm_plugin_result_t result = display->auto_brightness_block(true);
            diag(self, "auto_brightness_block(1) -> %d", (int)result);
            if (result == GM_PLUGIN_OK) self->brightness_blocked = 1U;
        }
        /* Level 0 keeps whatever brightness the glasses have now. */
        if (self->brightness_blocked != 0U && self->brightness_level >= 1U &&
            self->brightness_level <= 10U)
            (void)display->brightness_set(
                (gm_plugin_display_brightness_t)self->brightness_level);
    } else if (self->brightness_blocked != 0U &&
               display->auto_brightness_block(false) == GM_PLUGIN_OK) {
        self->brightness_blocked = 0U;
    }
}

static void release_brightness(md_reader_t *self)
{
    if (self->display_control_available != 0U && self->brightness_blocked != 0U &&
        self->host->display_control.auto_brightness_block(false) == GM_PLUGIN_OK)
        self->brightness_blocked = 0U;
}

static void set_autoscroll(md_reader_t *self, bool on, const char *notice)
{
    self->auto_scroll = on ? 1U : 0U;
    self->auto_elapsed = 0U;
    if (on) leave_zone(self, false);
    update_guide_visibility(self);
    if (notice != 0) show_notice(self, notice);
    update_hint(self);
    send_autoscroll_state(self);
}

static void update_autoscroll(md_reader_t *self, uint32_t elapsed_ms)
{
    if (self->auto_scroll == 0U || self->displayed == 0U ||
        self->pending != PENDING_NONE || self->screen_off != 0U)
        return;
    self->auto_elapsed += elapsed_ms;
    if (self->auto_elapsed < self->auto_interval_ms) return;
    self->auto_elapsed = 0U;
    if (self->current_page + 1U >= self->page_count) {
        set_autoscroll(self, false, "End of note");
        return;
    }
    next_page(self);
}

static bool handle_autoscroll(md_reader_t *self, const uint8_t *data,
                              uint32_t length)
{
    if (length != AUTOSCROLL_BYTES || read_u32(data + 2) != self->session)
        return false;
    self->auto_interval_ms = read_u16(data + 7);
    if (self->auto_interval_ms < MIN_AUTOSCROLL_MS)
        self->auto_interval_ms = MIN_AUTOSCROLL_MS;
    if ((data[6] != 0U) == (self->auto_scroll != 0U)) {
        /* Same state: only the interval changed. */
        send_autoscroll_state(self);
        return true;
    }
    set_autoscroll(self, data[6] != 0U,
                   data[6] != 0U ? "Auto-scroll on" : "Auto-scroll off");
    return true;
}

static bool handle_open(md_reader_t *self, const uint8_t *data, uint32_t length)
{
    uint32_t session;
    uint32_t doc_id;
    uint32_t total;
    uint32_t target;
    uint32_t title_length;
    bool same_doc;
    if (length < OPEN_HEADER_BYTES || length > OPEN_HEADER_BYTES + TITLE_BYTES)
        return false;
    session = read_u32(data + 2);
    doc_id = read_u32(data + 6);
    total = read_u32(data + 10);
    target = read_u32(data + 14);
    if (session == 0U || total == 0U) return false;
    same_doc = doc_id != 0U && doc_id == self->doc_id && total == self->total_bytes;
    if (!same_doc) reset_document(self);
    self->pending = PENDING_NONE;
    self->waiting = 0U;
    self->session = session;
    self->doc_id = doc_id;
    self->total_bytes = total;
    self->history_depth = data[20];
    title_length = length - OPEN_HEADER_BYTES;
    self->libc->memcpy(self->title, data + OPEN_HEADER_BYTES, title_length);
    self->title[title_length] = '\0';
    self->ui->label_set_text(self->title_label, self->title);
    if (!same_doc && self->auto_scroll != 0U) set_autoscroll(self, false, 0);
    /* "Opening..." or "Back" has done its job once the note arrives. */
    self->notice_elapsed = 0U;
    set_exit(self, false);
    seek(self, target < total ? target : 0U, read_u16(data + 18));
    return true;
}

static bool handle_window(md_reader_t *self, const uint8_t *data, uint32_t length)
{
    uint32_t offset;
    uint16_t link_count;
    uint32_t links_bytes;
    uint32_t text_length;
    const uint8_t *cursor;
    if (length < WINDOW_HEADER_BYTES || read_u32(data + 2) != self->session)
        return false;
    offset = read_u32(data + 6);
    link_count = read_u16(data + 11);
    links_bytes = (uint32_t)link_count * WINDOW_LINK_BYTES;
    if (self->waiting == 0U || offset != self->requested_offset ||
        link_count > MAX_WINDOW_LINKS ||
        length < WINDOW_HEADER_BYTES + links_bytes)
        return false;
    text_length = length - WINDOW_HEADER_BYTES - links_bytes;
    if (text_length == 0U || text_length > WINDOW_BYTES ||
        offset + text_length > self->total_bytes)
        return false;
    cursor = data + WINDOW_HEADER_BYTES;
    for (uint16_t index = 0U; index < link_count; ++index) {
        self->window_links[index].start = read_u32(cursor);
        self->window_links[index].end = read_u32(cursor + 4);
        self->window_links[index].id = read_u16(cursor + 8);
        cursor += WINDOW_LINK_BYTES;
    }
    self->window_link_count = link_count;
    self->libc->memcpy(self->window, cursor, text_length);
    self->window[text_length] = '\0';
    self->window_offset = offset;
    self->window_length = text_length;
    self->window_final = (data[10] != 0U ||
                          offset + text_length >= self->total_bytes) ? 1U : 0U;
    self->window_valid = 1U;
    self->waiting = 0U;
    if (self->pending == PENDING_SEEK) continue_seek(self);
    else if (self->pending == PENDING_SHOW)
        show_page(self, self->pending_page, self->seek_link);
    return true;
}

static bool handle_settings(md_reader_t *self, const uint8_t *data,
                            uint32_t length)
{
    uint8_t previous_font = self->font_mode;
    uint8_t previous_head = self->head_enabled;
    uint8_t previous_flags = self->flags;
    uint8_t received[SETTINGS_BYTES] = {0};
    if (length != SETTINGS_BYTES && length != SETTINGS_V1_BYTES) return false;
    self->libc->memcpy(received, data, length);
    /* Confirm on the glasses that a phone slider change arrived. */
    if (self->settings_received != 0U && self->displayed != 0U &&
        self->libc->memcmp(self->last_settings, received, SETTINGS_BYTES) != 0)
        show_notice(self, "Settings updated");
    self->libc->memcpy(self->last_settings, received, SETTINGS_BYTES);
    self->settings_received = 1U;
    self->brightness_lock = received[12] != 0U ? 1U : 0U;
    self->brightness_level = received[13] > 10U ? 10U : received[13];
    diag(self, "settings: head=%u tilt=%u turn=%u flags=0x%x bright=%u/%u",
         (unsigned int)data[2], (unsigned int)data[3], (unsigned int)read_u16(data + 8),
         (unsigned int)data[10], (unsigned int)received[12], (unsigned int)received[13]);
    apply_brightness(self);
    self->head_enabled = data[2] != 0U ? 1U : 0U;
    self->pitch_threshold = data[3] < 3U ? 3U : (data[3] > 45U ? 45U : data[3]);
    layout_guide_ticks(self);
    self->repeat_ms = read_u16(data + 4);
    if (self->repeat_ms != 0U && self->repeat_ms < MIN_REPEAT_MS)
        self->repeat_ms = MIN_REPEAT_MS;
    self->dwell_ms = read_u16(data + 6);
    if (self->dwell_ms > 2000U) self->dwell_ms = 2000U;
    self->turn_threshold = read_u16(data + 8);
    if (self->turn_threshold < 20U) self->turn_threshold = 20U;
    self->flags = data[10];
    self->font_mode = data[11] == FONT_LARGE ? FONT_LARGE : FONT_DEFAULT;
    if (self->font_mode != previous_font) {
        uint16_t select_id = selected_link_id(self);
        uint32_t offset = self->displayed != 0U
            ? self->page_starts[self->current_page] : 0U;
        calculate_layout(self);
        if (self->session != 0U) {
            reset_pagination(self);
            seek(self, offset, select_id);
        }
    }
    if (self->head_enabled != previous_head ||
        ((self->flags ^ previous_flags) & (FLAG_INVERT_PITCH | FLAG_GESTURE_SLEEP)) != 0U)
        set_imu(self, true);
    if (((self->flags ^ previous_flags) & FLAG_INVERTED_LINK) != 0U && self->displayed != 0U)
        apply_selection(self);
    update_guide_visibility(self);
    update_hint(self);
    return true;
}

static bool handle_notice(md_reader_t *self, const uint8_t *data, uint32_t length)
{
    char text[NOTICE_BYTES + 1U];
    uint32_t text_length = length - 2U;
    if (text_length > NOTICE_BYTES) text_length = NOTICE_BYTES;
    self->libc->memcpy(text, data + 2, text_length);
    text[text_length] = '\0';
    show_notice(self, text);
    return true;
}

static bool receive_message(md_reader_t *self, const uint8_t *data,
                            uint32_t length)
{
    if (data == 0 || length < 2U || data[0] != PROTOCOL_VERSION) return false;
    switch (data[1]) {
    case COMMAND_OPEN: return handle_open(self, data, length);
    case COMMAND_WINDOW: return handle_window(self, data, length);
    case COMMAND_SETTINGS: return handle_settings(self, data, length);
    case COMMAND_NOTICE: return handle_notice(self, data, length);
    case COMMAND_AUTOSCROLL: return handle_autoscroll(self, data, length);
    case COMMAND_CLOSE:
        if (length != 6U || read_u32(data + 2) != self->session) return false;
        wipe_private_data(self);
        self->ui->label_set_text(self->title_label, "MD Reader");
        show_message(self, "Open a note on your phone.");
        return true;
    default: return false;
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void toggle_head(md_reader_t *self)
{
    if (self->imu_available == 0U) {
        show_notice(self, "No head tracking here");
        return;
    }
    self->head_enabled = self->head_enabled == 0U ? 1U : 0U;
    set_imu(self, true);
    /* Tapping to resume is a deliberate "this is neutral". */
    self->confirm_on_calibration = self->head_enabled;
    show_notice(self, self->head_enabled != 0U ? "Head scroll on, recentred"
                                               : "Head scroll paused");
}

static void set_screen(md_reader_t *self, bool on)
{
    gm_plugin_result_t result;
    if (self->display_power_available == 0U) {
        diag(self, "display power control not available on this host");
        show_notice(self, "Can't turn the display off");
        return;
    }
    if (!on) {
        result = self->host->display_control.screen_turn_on(false);
        diag(self, "screen_turn_on(0) -> %d", (int)result);
        if (result != GM_PLUGIN_OK) {
            show_notice(self, "Display off failed");
            return;
        }
        self->screen_off = 1U;
        self->nod_count = 0U;
        self->nod_swings = 0U;
        self->nod_sign = 0;
    } else {
        result = self->host->display_control.screen_turn_on(true);
        diag(self, "screen_turn_on(1) -> %d", (int)result);
        self->screen_off = 0U;
        self->shake_count = 0U;
        self->shake_swings = 0U;
        self->shake_sign = 0;
        show_notice(self, "Display on");
    }
    /* Raw head tracking pauses while off and recalibrates on waking. */
    set_imu(self, true);
}

/* Counts one gesture; true on the GESTURE_COUNT-th within the window. */
static bool count_gesture(uint8_t *count, uint32_t *first_ms, uint32_t now_ms)
{
    if (*count == 0U || now_ms - *first_ms > GESTURE_WINDOW_MS) {
        *count = 1U;
        *first_ms = now_ms;
    } else {
        ++*count;
    }
    if (*count < GESTURE_COUNT) return false;
    *count = 0U;
    return true;
}

static bool handle_gesture(md_reader_t *self, const gm_plugin_event_t *event)
{
    gm_plugin_imu_gesture_t gesture = event->data.imu_gesture.gesture;
    diag(self, "firmware gesture %u active=%u", (unsigned int)gesture,
         (unsigned int)event->data.imu_gesture.active);
    if (!event->data.imu_gesture.active ||
        (self->flags & FLAG_GESTURE_SLEEP) == 0U)
        return true;
    if (gesture == GM_PLUGIN_IMU_GESTURE_SHAKE && self->screen_off == 0U) {
        if (count_gesture(&self->shake_count, &self->shake_first_ms,
                          event->timestamp_ms))
            set_screen(self, false);
    } else if (gesture == GM_PLUGIN_IMU_GESTURE_NOD && self->screen_off != 0U) {
        if (count_gesture(&self->nod_count, &self->nod_first_ms,
                          event->timestamp_ms))
            set_screen(self, true);
    }
    return true;
}

static bool handle_button(md_reader_t *self, const gm_plugin_event_t *event)
{
    gm_plugin_button_t button = event->data.button.button;
    gm_plugin_button_action_t action = event->data.button.action;
    /* While the display is off, any button only wakes it. */
    if (self->screen_off != 0U) {
        if (action != GM_PLUGIN_BUTTON_ACTION_RELEASE) set_screen(self, true);
        return true;
    }
    if (button == GM_PLUGIN_BUTTON_PRIMARY) {
        switch (action) {
        case GM_PLUGIN_BUTTON_ACTION_SINGLE:
            if (self->auto_scroll != 0U) set_autoscroll(self, false, "Auto-scroll off");
            else if (self->exit_active == 0U) toggle_head(self);
            break;
        case GM_PLUGIN_BUTTON_ACTION_DOUBLE:
            follow_selected(self);
            break;
        case GM_PLUGIN_BUTTON_ACTION_LONG:
            if (self->history_depth != 0U) {
                self->back_sent = 1U;
                go_back(self);
            } else {
                set_exit(self, true);
            }
            break;
        case GM_PLUGIN_BUTTON_ACTION_VERY_LONG:
            if (self->back_sent == 0U && self->exit_active == 0U)
                set_exit(self, true);
            break;
        case GM_PLUGIN_BUTTON_ACTION_RELEASE:
            self->back_sent = 0U;
            if (self->exit_active != 0U) set_exit(self, false);
            break;
        default:
            break;
        }
        return true;
    }
    if (action != GM_PLUGIN_BUTTON_ACTION_TRIGGER) return false;
    switch (button) {
    case GM_PLUGIN_BUTTON_UP:
    case GM_PLUGIN_BUTTON_PAGE_UP:
    case GM_PLUGIN_BUTTON_SCROLL_UP:
        previous_page(self);
        break;
    case GM_PLUGIN_BUTTON_DOWN:
    case GM_PLUGIN_BUTTON_PAGE_DOWN:
    case GM_PLUGIN_BUTTON_SCROLL_DOWN:
        next_page(self);
        break;
    case GM_PLUGIN_BUTTON_LEFT:
        step_link(self, -1);
        break;
    case GM_PLUGIN_BUTTON_RIGHT:
        step_link(self, 1);
        break;
    case GM_PLUGIN_BUTTON_BACK:
        if (self->history_depth != 0U) go_back(self);
        else exit_app(self);
        break;
    case GM_PLUGIN_BUTTON_HOME:
        exit_app(self);
        break;
    default:
        return false;
    }
    return true;
}

static bool on_event(void *opaque, const gm_plugin_event_t *event)
{
    md_reader_t *self = opaque;
    if (event == 0 || self->root == 0) return false;
    switch (event->type) {
    case GM_PLUGIN_EVENT_BT_MESSAGE:
        if (event->data.bt.channel != MD_CHANNEL) return false;
        return receive_message(self, event->data.bt.data, event->data.bt.length);
    case GM_PLUGIN_EVENT_BUTTON:
        return handle_button(self, event);
    case GM_PLUGIN_EVENT_IMU_GESTURE:
        return handle_gesture(self, event);
    case GM_PLUGIN_EVENT_CONNECTION:
        self->connected = event->data.connection.connected ? 1U : 0U;
        if (self->connected != 0U) {
            send_hello(self);
        } else {
            wipe_private_data(self);
            self->ui->label_set_text(self->title_label, "MD Reader");
            show_message(self, "Waiting for the phone...");
        }
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static gm_plugin_lvgl_obj_t *create_label(md_reader_t *self, uint8_t text_color,
                                          gm_plugin_lvgl_label_mode_t mode)
{
    gm_plugin_lvgl_obj_t *label = self->ui->label_create(self->root);
    if (label == 0) return 0;
    self->ui->label_set_long_mode(label, mode);
    self->ui->style_set(label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(text_color),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->label_set_text(label, "");
    return label;
}

static gm_plugin_lvgl_obj_t *create_line(md_reader_t *self,
                                         gm_plugin_lvgl_obj_t *parent,
                                         uint8_t line_color, int32_t width)
{
    gm_plugin_lvgl_obj_t *line = self->ui->line_create(parent);
    if (line == 0) return 0;
    self->ui->obj_set_pos(line, 0, 0);
    self->ui->style_set(line, GM_PLUGIN_LVGL_STYLE_LINE_WIDTH, number(width),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->style_set(line, GM_PLUGIN_LVGL_STYLE_LINE_COLOR, color(line_color),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->obj_add_flag(line, GM_PLUGIN_LVGL_FLAG_HIDDEN);
    return line;
}

static gm_plugin_result_t create_ui(md_reader_t *self)
{
    gm_plugin_display_info_t display;
    gm_plugin_result_t result = self->host->display_get_info(&display);
    if (result != GM_PLUGIN_OK) return result;
    if (display.width < 320U || display.height < 200U ||
        display.pixel_format != GM_PLUGIN_PIXEL_GRAY_4)
        return GM_PLUGIN_ENOTSUP;
    self->display_width = display.width;
    self->display_height = display.height;
    self->root = self->ui->root_get();
    if (self->root == 0) return GM_PLUGIN_ESTATE;
    self->ui->obj_clean(self->root);

    self->title_label = create_label(self, COLOR_DIM, GM_PLUGIN_LVGL_LABEL_DOT);
    self->viewport = self->ui->obj_create(self->root);
    if (self->title_label == 0 || self->viewport == 0) return GM_PLUGIN_ENOMEM;
    clear_style_box(self, self->viewport);
    self->ui->obj_clear_flag(self->viewport, GM_PLUGIN_LVGL_FLAG_SCROLLABLE);
    self->ui->style_set(self->viewport, GM_PLUGIN_LVGL_STYLE_BG_COLOR,
                        color(COLOR_BLACK), GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->style_set(self->viewport, GM_PLUGIN_LVGL_STYLE_BG_OPA,
                        number(GM_PLUGIN_LVGL_OPA_COVER),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);

    self->page_label = self->ui->label_create(self->viewport);
    if (self->page_label == 0) return GM_PLUGIN_ENOMEM;
    self->ui->label_set_long_mode(self->page_label, GM_PLUGIN_LVGL_LABEL_WRAP);
    self->ui->style_set(self->page_label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR,
                        color(COLOR_TEXT), GM_PLUGIN_LVGL_SELECTOR_MAIN);
    for (uint16_t index = 0U; index < MAX_UNDERLINES; ++index) {
        self->underline[index] = create_line(self, self->viewport, COLOR_DIM, 1);
        if (self->underline[index] == 0) return GM_PLUGIN_ENOMEM;
    }
    /* Drawn by the plugin rather than with LVGL's label text selection, which
     * showed only the dark text (no background) on the glasses. */
    for (uint16_t index = 0U; index < MAX_SELECT_BOXES; ++index) {
        gm_plugin_lvgl_obj_t *box = self->ui->obj_create(self->viewport);
        gm_plugin_lvgl_obj_t *text = box != 0 ? self->ui->label_create(box) : 0;
        gm_plugin_lvgl_style_value_t font = {0};
        if (box == 0 || text == 0) return GM_PLUGIN_ENOMEM;
        clear_style_box(self, box);
        self->ui->obj_clear_flag(box, GM_PLUGIN_LVGL_FLAG_SCROLLABLE);
        self->ui->style_set(box, GM_PLUGIN_LVGL_STYLE_RADIUS, number(3),
                            GM_PLUGIN_LVGL_SELECTOR_MAIN);
        self->ui->style_set(box, GM_PLUGIN_LVGL_STYLE_BORDER_COLOR, color(COLOR_TEXT),
                            GM_PLUGIN_LVGL_SELECTOR_MAIN);
        self->ui->style_set(box, GM_PLUGIN_LVGL_STYLE_BORDER_OPA,
                            number(GM_PLUGIN_LVGL_OPA_COVER), GM_PLUGIN_LVGL_SELECTOR_MAIN);
        self->ui->style_set(box, GM_PLUGIN_LVGL_STYLE_BG_COLOR, color(COLOR_TEXT),
                            GM_PLUGIN_LVGL_SELECTOR_MAIN);
        self->ui->label_set_long_mode(text, GM_PLUGIN_LVGL_LABEL_CLIP);
        self->ui->obj_set_pos(text, BOX_PAD_X, BOX_PAD_Y);
        self->ui->style_set(text, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(COLOR_BLACK),
                            GM_PLUGIN_LVGL_SELECTOR_MAIN);
        font.ptr = self->ui->font_default;
        self->ui->style_set(text, GM_PLUGIN_LVGL_STYLE_TEXT_FONT, font,
                            GM_PLUGIN_LVGL_SELECTOR_MAIN);
        self->ui->label_set_text(text, "");
        self->ui->obj_add_flag(box, GM_PLUGIN_LVGL_FLAG_HIDDEN);
        self->select_box[index] = box;
        self->select_text[index] = text;
    }

    self->hint_label = create_label(self, COLOR_DIM, GM_PLUGIN_LVGL_LABEL_CLIP);
    self->footer_label = create_label(self, COLOR_DIM, GM_PLUGIN_LVGL_LABEL_CLIP);
    self->guide_track = create_line(self, self->root, COLOR_FAINT, 1);
    self->guide_tick[0] = create_line(self, self->root, COLOR_FAINT, 1);
    self->guide_tick[1] = create_line(self, self->root, COLOR_FAINT, 1);
    self->guide_marker = self->ui->obj_create(self->root);
    self->scroll_arrow = create_line(self, self->root, COLOR_TEXT, 2);
    self->exit_label = create_label(self, COLOR_TEXT, GM_PLUGIN_LVGL_LABEL_CLIP);
    self->exit_arc = self->ui->arc_create(self->root);
    if (self->hint_label == 0 || self->footer_label == 0 ||
        self->guide_track == 0 || self->guide_tick[0] == 0 ||
        self->guide_tick[1] == 0 || self->guide_marker == 0 ||
        self->scroll_arrow == 0 ||
        self->exit_label == 0 || self->exit_arc == 0)
        return GM_PLUGIN_ENOMEM;
    self->ui->style_set(self->footer_label, GM_PLUGIN_LVGL_STYLE_TEXT_ALIGN,
                        number(GM_PLUGIN_LVGL_TEXT_ALIGN_RIGHT),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
    clear_style_box(self, self->guide_marker);
    self->ui->obj_clear_flag(self->guide_marker, GM_PLUGIN_LVGL_FLAG_SCROLLABLE);
    self->ui->style_set(self->guide_marker, GM_PLUGIN_LVGL_STYLE_BG_OPA,
                        number(GM_PLUGIN_LVGL_OPA_COVER),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->style_set(self->guide_marker, GM_PLUGIN_LVGL_STYLE_BG_COLOR,
                        color(COLOR_FAINT), GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->obj_add_flag(self->guide_marker, GM_PLUGIN_LVGL_FLAG_HIDDEN);

    self->ui->obj_set_size(self->exit_label, 180, 30);
    self->ui->obj_align(self->exit_label, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, 34);
    self->ui->style_set(self->exit_label, GM_PLUGIN_LVGL_STYLE_TEXT_ALIGN,
                        number(GM_PLUGIN_LVGL_TEXT_ALIGN_CENTER),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->style_set(self->exit_label, GM_PLUGIN_LVGL_STYLE_BG_COLOR,
                        color(COLOR_BLACK), GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->style_set(self->exit_label, GM_PLUGIN_LVGL_STYLE_BG_OPA,
                        number(GM_PLUGIN_LVGL_OPA_COVER),
                        GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->obj_add_flag(self->exit_label, GM_PLUGIN_LVGL_FLAG_HIDDEN);
    self->ui->obj_set_size(self->exit_arc, 52, 52);
    self->ui->obj_align(self->exit_arc, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, -18);
    self->ui->arc_set_range(self->exit_arc, 0, EXIT_HOLD_MS);
    self->ui->arc_set_value(self->exit_arc, 0);
    self->ui->style_set(self->exit_arc, GM_PLUGIN_LVGL_STYLE_ARC_WIDTH,
                        number(4), GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->style_set(self->exit_arc, GM_PLUGIN_LVGL_STYLE_ARC_COLOR,
                        color(0x49U), GM_PLUGIN_LVGL_SELECTOR_MAIN);
    self->ui->style_set(self->exit_arc, GM_PLUGIN_LVGL_STYLE_ARC_WIDTH,
                        number(4), GM_PLUGIN_LVGL_SELECTOR_INDICATOR);
    self->ui->style_set(self->exit_arc, GM_PLUGIN_LVGL_STYLE_ARC_COLOR,
                        color(COLOR_TEXT), GM_PLUGIN_LVGL_SELECTOR_INDICATOR);
    self->ui->style_set(self->exit_arc, GM_PLUGIN_LVGL_STYLE_OPA,
                        number(0), GM_PLUGIN_LVGL_SELECTOR_KNOB);
    self->ui->obj_add_flag(self->exit_arc, GM_PLUGIN_LVGL_FLAG_HIDDEN);

    calculate_layout(self);
    self->ui->label_set_text(self->title_label, "MD Reader");
    show_message(self, "Open a note on your phone.");
    return GM_PLUGIN_OK;
}

static gm_plugin_result_t on_start(void *opaque)
{
    md_reader_t *self = opaque;
    gm_plugin_result_t result;
    self->head_enabled = 1U;
    self->pitch_threshold = DEFAULT_PITCH_THRESHOLD;
    self->repeat_ms = DEFAULT_REPEAT_MS;
    self->settings_received = 0U;
    self->dwell_ms = DEFAULT_DWELL_MS;
    self->turn_threshold = DEFAULT_TURN_THRESHOLD;
    self->flags = FLAG_TILT_GUIDE | FLAG_GESTURE_SLEEP | FLAG_NOD_OPEN;
    self->font_mode = FONT_DEFAULT;
    self->connected = 1U;
    self->exit_active = 0U;
    self->back_sent = 0U;
    self->notice_elapsed = 0U;
    self->imu_running = 0U;
    self->arrow_pending = 0;
    self->arrow_shown = 0;
    self->arrow_elapsed = 0U;
    self->confirm_on_calibration = 0U;
    self->brightness_lock = 0U;
    self->brightness_level = 0U;
    self->brightness_blocked = 0U;
    self->auto_scroll = 0U;
    self->auto_interval_ms = 4000U;
    self->screen_off = 0U;
    self->shake_count = 0U;
    self->nod_count = 0U;
    self->imu_modes = GM_PLUGIN_IMU_ENABLE_NONE;
    wipe_private_data(self);
    result = create_ui(self);
    if (result != GM_PLUGIN_OK) {
        if (self->root != 0) self->ui->obj_clean(self->root);
        self->root = 0;
        return result;
    }
    diag(self, "glasses plugin v%u started: caps=0x%lx raw=%u gestures=%u display=%u",
         (unsigned int)GLASSES_VERSION, (unsigned long)self->host->capabilities,
         (unsigned int)self->imu_available, (unsigned int)self->gestures_available,
         (unsigned int)self->display_power_available);
    set_imu(self, true);
    send_hello(self);
    return GM_PLUGIN_OK;
}

static void on_loop(void *opaque, uint32_t elapsed_ms)
{
    md_reader_t *self = opaque;
    if (self->root == 0) return;
    if (self->notice_elapsed != 0U) {
        if (elapsed_ms >= self->notice_elapsed) {
            self->notice_elapsed = 0U;
            update_hint(self);
        } else {
            self->notice_elapsed -= elapsed_ms;
        }
    }
    if (self->arrow_elapsed != 0U) {
        if (elapsed_ms >= self->arrow_elapsed) {
            /* Keep a dim arrow so a glance back still shows the last turn. */
            self->arrow_elapsed = 0U;
            if (self->arrow_shown != 0)
                self->ui->style_set(self->scroll_arrow,
                                    GM_PLUGIN_LVGL_STYLE_LINE_COLOR,
                                    color(COLOR_FAINT),
                                    GM_PLUGIN_LVGL_SELECTOR_MAIN);
        } else {
            self->arrow_elapsed -= elapsed_ms;
        }
    }
    if (self->exit_active != 0U) {
        self->exit_elapsed += elapsed_ms;
        if (self->exit_elapsed >= EXIT_HOLD_MS) {
            exit_app(self);
            return;
        }
        show_exit(self);
        return;
    }
    if (self->waiting != 0U) {
        self->wait_elapsed += elapsed_ms;
        if (self->wait_elapsed >= WINDOW_RETRY_MS) {
            uint32_t offset = self->requested_offset;
            self->waiting = 0U;
            request_window(self, offset);
        }
    } else if (self->pending == PENDING_SEEK) {
        continue_seek(self);
    }
    update_autoscroll(self, elapsed_ms);
    update_head(self, elapsed_ms);
}

static void on_suspend(void *opaque)
{
    md_reader_t *self = opaque;
    set_imu(self, false);
    release_brightness(self);
}

static void on_resume(void *opaque)
{
    md_reader_t *self = opaque;
    set_imu(self, self->head_enabled != 0U);
    apply_brightness(self);
    send_hello(self);
}

static void on_stop(void *opaque)
{
    md_reader_t *self = opaque;
    /* Never leave the display off behind us. */
    if (self->screen_off != 0U) {
        (void)self->host->display_control.screen_turn_on(true);
        self->screen_off = 0U;
    }
    set_imu(self, false);
    release_brightness(self);
    wipe_private_data(self);
    if (self->root != 0) self->ui->obj_clean(self->root);
    self->root = 0;
    self->title_label = 0;
    self->viewport = 0;
    self->page_label = 0;
    self->hint_label = 0;
    self->footer_label = 0;
    self->exit_label = 0;
    self->exit_arc = 0;
    self->guide_track = 0;
    self->guide_tick[0] = self->guide_tick[1] = 0;
    self->guide_marker = 0;
    self->scroll_arrow = 0;
    for (uint16_t index = 0U; index < MAX_UNDERLINES; ++index)
        self->underline[index] = 0;
    for (uint16_t index = 0U; index < MAX_SELECT_BOXES; ++index)
        self->select_box[index] = self->select_text[index] = 0;
}

gm_plugin_result_t gm_plugin_entry(const gm_plugin_host_api_t *host,
                                   gm_plugin_descriptor_t *plugin)
{
    const gm_plugin_capabilities_t required = GM_PLUGIN_CAP_DISPLAY_BITMAP |
        GM_PLUGIN_CAP_BUTTON | GM_PLUGIN_CAP_BLUETOOTH;
    if (host == 0 || plugin == 0 ||
        host->struct_size < GM_PLUGIN_HOST_API_MIN_SIZE ||
        !GM_PLUGIN_VERSION_COMPATIBLE(host->abi_version,
                                      GM_PLUGIN_ABI_MIN_VERSION) ||
        host->display_get_info == 0 || host->graphics.lvgl == 0 ||
        host->bt_send == 0 || host->app_exit == 0 ||
        (host->capabilities & required) != required ||
        plugin->struct_size < GM_PLUGIN_DESCRIPTOR_MIN_SIZE)
        return GM_PLUGIN_ENOTSUP;
    reader.host = host;
    reader.ui = host->graphics.lvgl;
    if (gm_plugin_libc_get(host, &reader.libc) != GM_PLUGIN_OK)
        return GM_PLUGIN_ENOTSUP;
    if (reader.ui->struct_size < GM_PLUGIN_LVGL_API_MIN_SIZE ||
        !GM_PLUGIN_VERSION_COMPATIBLE(reader.ui->api_version,
                                      GM_PLUGIN_LVGL_API_MIN_VERSION))
        return GM_PLUGIN_ENOTSUP;
    /* Head tracking is optional; buttons and a ring still work without it. */
    reader.imu_available =
        (host->capabilities & GM_PLUGIN_CAP_IMU_RAW) != 0U &&
        host->imu_enable != 0 && host->imu_read != 0 ? 1U : 0U;
    reader.gestures_available =
        (host->capabilities & GM_PLUGIN_CAP_IMU_EVENTS) != 0U &&
        host->imu_enable != 0 ? 1U : 0U;
    reader.display_power_available =
        (host->capabilities & GM_PLUGIN_CAP_DISPLAY_CONTROL) != 0U &&
        host->display_control.screen_turn_on != 0 ? 1U : 0U;
    /* Brightness holding is optional too. */
    reader.display_control_available =
        (host->capabilities & GM_PLUGIN_CAP_DISPLAY_CONTROL) != 0U &&
        host->display_control.auto_brightness_block != 0 &&
        host->display_control.brightness_set != 0 ? 1U : 0U;
    plugin->abi_version = GM_PLUGIN_ABI_MIN_VERSION;
    plugin->context = &reader;
    plugin->on_start = on_start;
    plugin->on_resume = on_resume;
    plugin->on_loop = on_loop;
    plugin->on_event = on_event;
    plugin->on_suspend = on_suspend;
    plugin->on_stop = on_stop;
    return GM_PLUGIN_OK;
}
