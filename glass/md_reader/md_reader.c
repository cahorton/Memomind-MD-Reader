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

#define EVENT_NEED_WINDOW 1U
#define EVENT_PAGE 2U
#define EVENT_FOLLOW 3U
#define EVENT_BACK 4U
#define EVENT_HELLO 5U

#define OPEN_HEADER_BYTES 21U
#define WINDOW_HEADER_BYTES 13U
#define WINDOW_LINK_BYTES 10U
#define SETTINGS_BYTES 12U

#define WINDOW_BYTES 8192U
#define WINDOW_GUARD_BYTES 512U
#define PAGE_BYTES 4096U
#define MAX_PAGE_LINES 16U
#define MAX_PAGES 2048U
#define MAX_WINDOW_LINKS 128U
#define MAX_PAGE_LINKS 32U
#define MAX_UNDERLINES 40U
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

#define DEFAULT_PITCH_THRESHOLD 12U
#define DEFAULT_REPEAT_MS 1500U
#define DEFAULT_DWELL_MS 300U
#define DEFAULT_TURN_THRESHOLD 120U
#define PITCH_HYSTERESIS 3
#define IMU_SAMPLE_MS 40U
#define CALIBRATION_SAMPLES 8U
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
#define HEADER_GAP 4
#define GUIDE_WIDTH 14

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
    gm_plugin_lvgl_obj_t *guide_track;
    gm_plugin_lvgl_obj_t *guide_tick[2];
    gm_plugin_lvgl_obj_t *guide_marker;
    gm_plugin_lvgl_point_t underline_points[MAX_UNDERLINES][2];
    gm_plugin_lvgl_point_t track_points[2];
    gm_plugin_lvgl_point_t tick_points[2][2];

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

    /* Head tracking. */
    uint8_t imu_available;
    uint8_t imu_running;
    uint32_t imu_elapsed;
    uint8_t calibration_count;
    int32_t calibration_sum;
    int16_t neutral_pitch;
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
    uint8_t data[6];
    data[0] = PROTOCOL_VERSION;
    data[1] = EVENT_HELLO;
    write_u32(data + 2, self->session);
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

static void show_message(md_reader_t *self, const char *text)
{
    self->displayed = 0U;
    self->page_link_count = 0U;
    self->selected = -1;
    if (self->page_label != 0) {
        self->ui->label_set_selection_start(self->page_label,
                                            GM_PLUGIN_LVGL_TEXT_SELECTION_OFF);
        self->ui->label_set_selection_end(self->page_label,
                                          GM_PLUGIN_LVGL_TEXT_SELECTION_OFF);
        self->ui->label_set_text(self->page_label, text);
    }
    for (uint16_t index = 0U; index < MAX_UNDERLINES; ++index)
        set_hidden(self, self->underline[index], true);
    update_footer(self);
    update_hint(self);
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

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

    /* Tilt guide: a track in the right margin with ticks at the thresholds. */
    {
        int16_t x = (int16_t)(self->display_width - SIDE_MARGIN - GUIDE_WIDTH / 2);
        self->track_points[0].x = x;
        self->track_points[0].y = self->viewport_y;
        self->track_points[1].x = x;
        self->track_points[1].y = (int16_t)(self->viewport_y + self->viewport_height);
        self->ui->line_set_points(self->guide_track, self->track_points, 2U);
        for (uint8_t tick = 0U; tick < 2U; ++tick) {
            int16_t y = (int16_t)(self->viewport_y +
                (tick == 0U ? self->viewport_height / 4
                            : self->viewport_height * 3 / 4));
            self->tick_points[tick][0].x = (int16_t)(x - 4);
            self->tick_points[tick][0].y = y;
            self->tick_points[tick][1].x = (int16_t)(x + 4);
            self->tick_points[tick][1].y = y;
            self->ui->line_set_points(self->guide_tick[tick],
                                      self->tick_points[tick], 2U);
        }
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

static uint32_t char_index(const md_reader_t *self, uint32_t byte_count)
{
    uint32_t count = 0U;
    for (uint32_t index = 0U; index < byte_count && self->page_text[index] != '\0';
         ++index) {
        if (((uint8_t)self->page_text[index] & 0xC0U) != 0x80U) ++count;
    }
    return count;
}

static void apply_selection(md_reader_t *self)
{
    uint32_t page_start = self->page_starts[self->current_page];
    if (self->selected < 0 || (uint16_t)self->selected >= self->page_link_count) {
        self->ui->label_set_selection_start(self->page_label,
                                            GM_PLUGIN_LVGL_TEXT_SELECTION_OFF);
        self->ui->label_set_selection_end(self->page_label,
                                          GM_PLUGIN_LVGL_TEXT_SELECTION_OFF);
    } else {
        const md_link_t *link = &self->page_links[self->selected];
        self->ui->label_set_selection_start(
            self->page_label, char_index(self, link->start - page_start));
        self->ui->label_set_selection_end(
            self->page_label, char_index(self, link->end - page_start));
    }
    update_hint(self);
}

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

static void layout_underlines(md_reader_t *self)
{
    uint16_t used = 0U;
    uint32_t page_start = self->page_starts[self->current_page];
    uint32_t text_end = page_start + self->libc->strlen(self->page_text);
    for (uint16_t link = 0U; link < self->page_link_count; ++link) {
        for (uint16_t line = 0U; line < self->line_count; ++line) {
            uint32_t line_start = self->line_offset[line];
            uint32_t line_end = line + 1U < self->line_count
                ? self->line_offset[line + 1U] : text_end;
            uint32_t from = self->page_links[link].start;
            uint32_t to = self->page_links[link].end;
            int16_t x0;
            int16_t x1;
            int16_t y;
            if (from < line_start) from = line_start;
            if (to > line_end) to = line_end;
            while (to > from && self->page_text[to - 1U - page_start] == '\n') --to;
            if (to <= from || used >= MAX_UNDERLINES) continue;
            x0 = measure_prefix(self, line_start - page_start, from - page_start);
            x1 = measure_prefix(self, line_start - page_start, to - page_start);
            if (x1 <= x0) continue;
            y = (int16_t)(line * self->line_height + self->font_height);
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
    continue_seek(self);
}

static void next_page(md_reader_t *self)
{
    if (self->displayed == 0U || self->pending != PENDING_NONE) return;
    if (self->current_page + 1U >= self->page_count) {
        show_notice(self, "End of note");
        return;
    }
    show_page(self, (uint16_t)(self->current_page + 1U), NO_LINK);
}

static void previous_page(md_reader_t *self)
{
    if (self->displayed == 0U || self->pending != PENDING_NONE) return;
    if (self->current_page == 0U) {
        show_notice(self, "Top of note");
        return;
    }
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
    self->yaw_locked = 0U;
    self->yaw_hits = 0U;
    self->yaw_candidate = 0;
    self->imu_elapsed = 0U;
}

static void update_guide_visibility(md_reader_t *self)
{
    bool hidden = self->head_enabled == 0U || self->imu_running == 0U ||
        (self->flags & FLAG_TILT_GUIDE) == 0U ||
        self->calibration_count < CALIBRATION_SAMPLES;
    set_hidden(self, self->guide_track, hidden);
    set_hidden(self, self->guide_tick[0], hidden);
    set_hidden(self, self->guide_tick[1], hidden);
    set_hidden(self, self->guide_marker, hidden);
}

static void set_imu(md_reader_t *self, bool on)
{
    if (self->imu_available == 0U) return;
    if (on && self->imu_running == 0U) {
        self->imu_running = self->host->imu_enable(GM_PLUGIN_IMU_ENABLE_RAW) ==
            GM_PLUGIN_OK ? 1U : 0U;
    } else if (!on && self->imu_running != 0U) {
        (void)self->host->imu_enable(GM_PLUGIN_IMU_ENABLE_NONE);
        self->imu_running = 0U;
    }
    reset_head_state(self);
    update_guide_visibility(self);
}

static void update_guide(md_reader_t *self, int16_t delta)
{
    /* Thresholds sit at the ticks, a quarter of the track from the middle. */
    int32_t half = self->viewport_height / 2;
    int32_t threshold = self->pitch_threshold == 0U ? 1 : self->pitch_threshold;
    int32_t offset = -(int32_t)delta * (half / 2) / threshold;
    int16_t y;
    if (offset < -half) offset = -half;
    if (offset > half) offset = half;
    y = (int16_t)(self->viewport_y + half + offset - 4);
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

static void update_pitch(md_reader_t *self, int16_t delta, uint32_t dt)
{
    int16_t enter = (int16_t)self->pitch_threshold;
    int16_t leave = (int16_t)(enter - PITCH_HYSTERESIS);
    int8_t zone = self->tilt_zone;
    if (leave < 1) leave = 1;
    if (zone == 0) {
        if (delta >= enter) zone = 1;
        else if (delta <= -enter) zone = -1;
    } else if (zone > 0 && delta < leave) {
        zone = delta <= -enter ? -1 : 0;
    } else if (zone < 0 && delta > -leave) {
        zone = delta >= enter ? 1 : 0;
    }
    if (zone != self->tilt_zone) {
        self->tilt_zone = zone;
        self->tilt_fired = 0U;
        self->zone_elapsed = 0U;
        self->repeat_elapsed = 0U;
    }
    if (zone == 0) return;
    if (self->tilt_fired == 0U) {
        self->zone_elapsed += dt;
        if (self->zone_elapsed < self->dwell_ms) return;
        self->tilt_fired = 1U;
        self->repeat_elapsed = 0U;
    } else {
        self->repeat_elapsed += dt;
        if (self->repeat_elapsed < self->repeat_ms) return;
        self->repeat_elapsed = 0U;
    }
    /* Looking up reads further down the note; looking down goes back. */
    if (zone > 0) next_page(self);
    else previous_page(self);
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

static void update_head(md_reader_t *self, uint32_t elapsed_ms)
{
    gm_plugin_imu_sample_t sample;
    uint32_t dt;
    int16_t pitch;
    int16_t delta;
    if (self->imu_running == 0U || self->head_enabled == 0U) return;
    self->imu_elapsed += elapsed_ms;
    if (self->imu_elapsed < IMU_SAMPLE_MS) return;
    dt = self->imu_elapsed > 250U ? 250U : self->imu_elapsed;
    self->imu_elapsed = 0U;
    if (self->host->imu_read(&sample) != GM_PLUGIN_OK) return;
    pitch = sample.pitch_degrees;
    if ((self->flags & FLAG_INVERT_PITCH) != 0U) pitch = (int16_t)-pitch;
    if (self->calibration_count < CALIBRATION_SAMPLES) {
        self->calibration_sum += pitch;
        if (++self->calibration_count == CALIBRATION_SAMPLES) {
            self->neutral_pitch =
                (int16_t)(self->calibration_sum / (int32_t)CALIBRATION_SAMPLES);
            self->guide_zone = 2;
            update_guide_visibility(self);
        }
        return;
    }
    delta = (int16_t)(pitch - self->neutral_pitch);
    update_pitch(self, delta, dt);
    if ((self->flags & FLAG_TILT_GUIDE) != 0U) update_guide(self, delta);
    if (self->tilt_zone == 0) update_yaw(self, &sample, dt);
}

/* ------------------------------------------------------------------ */
/* Message handlers                                                    */
/* ------------------------------------------------------------------ */

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
    if (length != SETTINGS_BYTES) return false;
    self->head_enabled = data[2] != 0U ? 1U : 0U;
    self->pitch_threshold = data[3] < 3U ? 3U : (data[3] > 45U ? 45U : data[3]);
    self->repeat_ms = read_u16(data + 4);
    if (self->repeat_ms < 400U) self->repeat_ms = 400U;
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
        ((self->flags ^ previous_flags) & FLAG_INVERT_PITCH) != 0U)
        set_imu(self, self->head_enabled != 0U);
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
    set_imu(self, self->head_enabled != 0U);
    show_notice(self, self->head_enabled != 0U ? "Head scroll on, recentred"
                                               : "Head scroll paused");
}

static bool handle_button(md_reader_t *self, const gm_plugin_event_t *event)
{
    gm_plugin_button_t button = event->data.button.button;
    gm_plugin_button_action_t action = event->data.button.action;
    if (button == GM_PLUGIN_BUTTON_PRIMARY) {
        switch (action) {
        case GM_PLUGIN_BUTTON_ACTION_SINGLE:
            if (self->exit_active == 0U) toggle_head(self);
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
    /* The selected link is drawn inverted through LVGL's label selection. */
    self->ui->style_set(self->page_label, GM_PLUGIN_LVGL_STYLE_BG_COLOR,
                        color(COLOR_TEXT), GM_PLUGIN_LVGL_SELECTOR_SELECTED);
    self->ui->style_set(self->page_label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR,
                        color(COLOR_BLACK), GM_PLUGIN_LVGL_SELECTOR_SELECTED);
    for (uint16_t index = 0U; index < MAX_UNDERLINES; ++index) {
        self->underline[index] = create_line(self, self->viewport, COLOR_DIM, 1);
        if (self->underline[index] == 0) return GM_PLUGIN_ENOMEM;
    }

    self->hint_label = create_label(self, COLOR_DIM, GM_PLUGIN_LVGL_LABEL_CLIP);
    self->footer_label = create_label(self, COLOR_DIM, GM_PLUGIN_LVGL_LABEL_CLIP);
    self->guide_track = create_line(self, self->root, COLOR_FAINT, 1);
    self->guide_tick[0] = create_line(self, self->root, COLOR_FAINT, 1);
    self->guide_tick[1] = create_line(self, self->root, COLOR_FAINT, 1);
    self->guide_marker = self->ui->obj_create(self->root);
    self->exit_label = create_label(self, COLOR_TEXT, GM_PLUGIN_LVGL_LABEL_CLIP);
    self->exit_arc = self->ui->arc_create(self->root);
    if (self->hint_label == 0 || self->footer_label == 0 ||
        self->guide_track == 0 || self->guide_tick[0] == 0 ||
        self->guide_tick[1] == 0 || self->guide_marker == 0 ||
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
    self->dwell_ms = DEFAULT_DWELL_MS;
    self->turn_threshold = DEFAULT_TURN_THRESHOLD;
    self->flags = FLAG_TILT_GUIDE;
    self->font_mode = FONT_DEFAULT;
    self->connected = 1U;
    self->exit_active = 0U;
    self->back_sent = 0U;
    self->notice_elapsed = 0U;
    self->imu_running = 0U;
    wipe_private_data(self);
    result = create_ui(self);
    if (result != GM_PLUGIN_OK) {
        if (self->root != 0) self->ui->obj_clean(self->root);
        self->root = 0;
        return result;
    }
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
    update_head(self, elapsed_ms);
}

static void on_suspend(void *opaque)
{
    md_reader_t *self = opaque;
    set_imu(self, false);
}

static void on_resume(void *opaque)
{
    md_reader_t *self = opaque;
    set_imu(self, self->head_enabled != 0U);
    send_hello(self);
}

static void on_stop(void *opaque)
{
    md_reader_t *self = opaque;
    set_imu(self, false);
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
    for (uint16_t index = 0U; index < MAX_UNDERLINES; ++index)
        self->underline[index] = 0;
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
