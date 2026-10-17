/*
 * Native test host for md_reader.c.
 *
 * Compiles the glasses plugin for the development machine against a fake Host:
 * a monospace "font", recording LVGL objects, a settable IMU sample and a
 * Bluetooth pipe on stdio. test/e2e.test.mjs drives it line by line:
 *
 *   MSG <hex>                 deliver a phone message on the reader channel
 *   BTN <button> <action>     deliver a button event (numeric GM_PLUGIN_ ids)
 *   IMU <pitch> <gx> <gy> <gz> set the sample returned by imu_read()
 *   TICK <ms>                 run on_loop in 20 ms steps
 *   CONNECT <0|1>             deliver a connection event
 *   DUMP                      print the visible state as one JSON line
 *
 * Every command ends with "OK". bt_send() prints "EVT <hex>" lines.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../glass/md_reader/md_reader.c"

#define MAX_OBJECTS 512

enum { KIND_ROOT, KIND_OBJ, KIND_LABEL, KIND_ARC, KIND_LINE };

struct _lv_obj_t {
    int kind;
    int hidden;
    int x, y, w, h;
    char *text;
    uint32_t sel_start, sel_end;
    const gm_plugin_lvgl_point_t *points;
    uint16_t point_count;
};

struct _lv_font_t {
    int char_width;
    int line_height;
};

static struct _lv_obj_t objects[MAX_OBJECTS];
static int object_count = 1; /* objects[0] is the root */
static struct _lv_font_t font_default = { 11, 24 };
static struct _lv_font_t font_large = { 15, 32 };
static gm_plugin_imu_sample_t imu_sample;
static int imu_mode;
static uint32_t now_ms;
static int exited;

static gm_plugin_lvgl_obj_t *new_object(int kind)
{
    struct _lv_obj_t *object;
    if (object_count >= MAX_OBJECTS) return 0;
    object = &objects[object_count++];
    memset(object, 0, sizeof(*object));
    object->kind = kind;
    object->sel_start = object->sel_end = GM_PLUGIN_LVGL_TEXT_SELECTION_OFF;
    return object;
}

static gm_plugin_lvgl_obj_t *root_get(void) { return &objects[0]; }
static gm_plugin_lvgl_obj_t *obj_create(gm_plugin_lvgl_obj_t *p) { (void)p; return new_object(KIND_OBJ); }
static void obj_delete(gm_plugin_lvgl_obj_t *o) { o->hidden = 1; }
static void obj_clean(gm_plugin_lvgl_obj_t *o)
{
    if (o != &objects[0]) return;
    for (int i = 1; i < object_count; ++i) free(objects[i].text);
    object_count = 1;
}
static void obj_set_pos(gm_plugin_lvgl_obj_t *o, gm_plugin_lvgl_coord_t x, gm_plugin_lvgl_coord_t y) { o->x = x; o->y = y; }
static void obj_set_size(gm_plugin_lvgl_obj_t *o, gm_plugin_lvgl_coord_t w, gm_plugin_lvgl_coord_t h) { o->w = w; o->h = h; }
static void obj_align(gm_plugin_lvgl_obj_t *o, gm_plugin_lvgl_align_t a, gm_plugin_lvgl_coord_t x, gm_plugin_lvgl_coord_t y) { (void)a; o->x = x; o->y = y; }
static void obj_add_flag(gm_plugin_lvgl_obj_t *o, gm_plugin_lvgl_flag_t f) { if (f & GM_PLUGIN_LVGL_FLAG_HIDDEN) o->hidden = 1; }
static void obj_clear_flag(gm_plugin_lvgl_obj_t *o, gm_plugin_lvgl_flag_t f) { if (f & GM_PLUGIN_LVGL_FLAG_HIDDEN) o->hidden = 0; }
static void obj_invalidate(const gm_plugin_lvgl_obj_t *o) { (void)o; }
static gm_plugin_lvgl_coord_t obj_get_width(const gm_plugin_lvgl_obj_t *o) { return o->w; }
static gm_plugin_lvgl_coord_t obj_get_height(const gm_plugin_lvgl_obj_t *o) { return o->h; }
static void style_set(gm_plugin_lvgl_obj_t *o, gm_plugin_lvgl_style_prop_t p, gm_plugin_lvgl_style_value_t v, gm_plugin_lvgl_selector_t s) { (void)o; (void)p; (void)v; (void)s; }
static gm_plugin_lvgl_obj_t *label_create(gm_plugin_lvgl_obj_t *p) { (void)p; return new_object(KIND_LABEL); }
static void label_set_text(gm_plugin_lvgl_obj_t *o, const char *t) { free(o->text); o->text = strdup(t); }
static void label_set_long_mode(gm_plugin_lvgl_obj_t *o, gm_plugin_lvgl_label_mode_t m) { (void)o; (void)m; }
static gm_plugin_lvgl_obj_t *arc_create(gm_plugin_lvgl_obj_t *p) { (void)p; return new_object(KIND_ARC); }
static void arc_set_range(gm_plugin_lvgl_obj_t *o, int16_t a, int16_t b) { (void)o; (void)a; (void)b; }
static void arc_set_value(gm_plugin_lvgl_obj_t *o, int16_t v) { (void)o; (void)v; }
static gm_plugin_lvgl_obj_t *line_create(gm_plugin_lvgl_obj_t *p) { (void)p; return new_object(KIND_LINE); }
static void line_set_points(gm_plugin_lvgl_obj_t *o, const gm_plugin_lvgl_point_t *pts, uint16_t n) { o->points = pts; o->point_count = n; }
static gm_plugin_lvgl_coord_t font_get_line_height(const gm_plugin_lvgl_font_t *f) { return f->line_height; }
static void label_set_selection_start(gm_plugin_lvgl_obj_t *o, uint32_t i) { o->sel_start = i; }
static void label_set_selection_end(gm_plugin_lvgl_obj_t *o, uint32_t i) { o->sel_end = i; }

static int utf8_width(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 4;
}

/* Monospace stand-in for LVGL's word wrapping: breaks after the last space
 * that fits, keeps '\n' and trailing spaces in the line like LVGL does. */
static uint32_t text_get_next_line(const char *text, const gm_plugin_lvgl_font_t *font,
                                   gm_plugin_lvgl_coord_t letter_space,
                                   gm_plugin_lvgl_coord_t max_width,
                                   gm_plugin_lvgl_coord_t *used,
                                   gm_plugin_lvgl_text_flag_t flags)
{
    uint32_t index = 0, after_space = 0;
    int width = 0;
    (void)letter_space; (void)flags;
    while (text[index] != '\0') {
        int length;
        if (text[index] == '\n') { index += 1; break; }
        length = utf8_width((unsigned char)text[index]);
        if (width + font->char_width > max_width && text[index] != ' ') {
            if (after_space != 0U) index = after_space;
            else if (index == 0U) index = (uint32_t)length;
            break;
        }
        width += font->char_width;
        index += (uint32_t)length;
        if (text[index - 1U] == ' ') after_space = index;
    }
    if (used != 0) *used = width;
    return index;
}

static void text_get_size(gm_plugin_lvgl_point_t *size, const char *text,
                          const gm_plugin_lvgl_font_t *font,
                          gm_plugin_lvgl_coord_t letter_space,
                          gm_plugin_lvgl_coord_t line_space,
                          gm_plugin_lvgl_coord_t max_width,
                          gm_plugin_lvgl_text_flag_t flags)
{
    int chars = 0;
    (void)letter_space; (void)line_space; (void)max_width; (void)flags;
    for (const char *p = text; *p; p += utf8_width((unsigned char)*p)) ++chars;
    size->x = chars * font->char_width;
    size->y = font->line_height;
}

static gm_plugin_lvgl_api_t lvgl_api;
static gm_plugin_libc_extension_api_t libc_api;
static gm_plugin_host_api_t host_api;

static void host_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fputs("LOG ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}
static uint32_t host_monotonic(void) { return now_ms; }
static void *host_alloc(size_t size) { return malloc(size); }
static void host_free(void *memory) { free(memory); }
static gm_plugin_result_t host_display(gm_plugin_display_info_t *info)
{
    info->width = 600; info->height = 350; info->refresh_hz = 30;
    info->pixel_format = GM_PLUGIN_PIXEL_GRAY_4; info->logical_display_count = 1;
    return GM_PLUGIN_OK;
}
static gm_plugin_result_t host_bt_send(gm_plugin_bt_channel_t channel, const void *data, uint32_t length)
{
    const uint8_t *bytes = data;
    if (channel != MD_EVENT_CHANNEL) return GM_PLUGIN_EINVAL;
    printf("EVT ");
    for (uint32_t i = 0; i < length; ++i) printf("%02x", bytes[i]);
    printf("\n");
    return GM_PLUGIN_OK;
}
static gm_plugin_result_t host_imu_enable(gm_plugin_imu_modes_t modes) { imu_mode = modes; return GM_PLUGIN_OK; }
static gm_plugin_result_t host_imu_read(gm_plugin_imu_sample_t *sample)
{
    if ((imu_mode & GM_PLUGIN_IMU_ENABLE_RAW) == 0) return GM_PLUGIN_ESTATE;
    *sample = imu_sample;
    return GM_PLUGIN_OK;
}
static void host_exit(void) { exited = 1; }
static gm_plugin_result_t host_extension(gm_plugin_extension_id_t id, const void **api)
{
    if (id != GM_PLUGIN_EXTENSION_LIBC) { *api = 0; return GM_PLUGIN_ENOTSUP; }
    *api = &libc_api;
    return GM_PLUGIN_OK;
}

static void setup_host(void)
{
    lvgl_api = (gm_plugin_lvgl_api_t){
        .struct_size = sizeof(gm_plugin_lvgl_api_t), .api_version = GM_PLUGIN_VERSION(1U, 1U),
        .root_get = root_get, .obj_create = obj_create, .obj_delete = obj_delete,
        .obj_clean = obj_clean, .obj_set_pos = obj_set_pos, .obj_set_size = obj_set_size,
        .obj_align = obj_align, .obj_add_flag = obj_add_flag, .obj_clear_flag = obj_clear_flag,
        .obj_invalidate = obj_invalidate, .obj_get_width = obj_get_width,
        .obj_get_height = obj_get_height, .style_set = style_set, .label_create = label_create,
        .label_set_text = label_set_text, .label_set_long_mode = label_set_long_mode,
        .arc_create = arc_create, .arc_set_range = arc_set_range, .arc_set_value = arc_set_value,
        .line_create = line_create, .line_set_points = line_set_points,
        .font_default = &font_default, .font_large = &font_large,
        .font_get_line_height = font_get_line_height, .text_get_size = text_get_size,
        .text_get_next_line = text_get_next_line,
        .label_set_selection_start = label_set_selection_start,
        .label_set_selection_end = label_set_selection_end,
    };
    libc_api = (gm_plugin_libc_extension_api_t){
        .memcpy = memcpy, .memmove = memmove, .memset = memset, .memcmp = memcmp,
        .memchr = memchr, .strlen = strlen, .strnlen = strnlen, .strcpy = strcpy,
        .snprintf = snprintf, .vsnprintf = vsnprintf,
    };
    host_api = (gm_plugin_host_api_t){
        .struct_size = sizeof(gm_plugin_host_api_t), .abi_version = GM_PLUGIN_ABI_VERSION,
        .capabilities = GM_PLUGIN_CAP_DISPLAY_BITMAP | GM_PLUGIN_CAP_BUTTON |
            GM_PLUGIN_CAP_BLUETOOTH | GM_PLUGIN_CAP_IMU_RAW | GM_PLUGIN_CAP_IMU_EVENTS,
        .log = host_log, .monotonic_ms = host_monotonic, .alloc = host_alloc,
        .free = host_free, .display_get_info = host_display,
        .graphics = { .lvgl = &lvgl_api },
        .bt_send = host_bt_send, .imu_enable = host_imu_enable, .imu_read = host_imu_read,
        .app_exit = host_exit, .extension_get = host_extension,
    };
}

static void json_string(const char *text)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p == '\n') printf("\\n");
        else if (*p < 0x20) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static void dump(void)
{
    md_reader_t *r = &reader;
    int first = 1;
    printf("{\"title\":"); json_string(r->title_label ? r->title_label->text : "");
    printf(",\"page\":"); json_string(r->page_label ? r->page_label->text : "");
    printf(",\"hint\":"); json_string(r->hint_label ? r->hint_label->text : "");
    printf(",\"footer\":"); json_string(r->footer_label ? r->footer_label->text : "");
    printf(",\"selection\":[%d,%d]",
           r->page_label && r->page_label->sel_start != GM_PLUGIN_LVGL_TEXT_SELECTION_OFF ? (int)r->page_label->sel_start : -1,
           r->page_label && r->page_label->sel_end != GM_PLUGIN_LVGL_TEXT_SELECTION_OFF ? (int)r->page_label->sel_end : -1);
    printf(",\"pageIndex\":%u,\"pageCount\":%u,\"complete\":%u,\"links\":%u,\"selected\":%d",
           r->current_page, r->page_count, r->doc_complete, r->page_link_count, r->selected);
    printf(",\"linesPerPage\":%u,\"tiltZone\":%d,\"headEnabled\":%u,\"exitActive\":%u,\"exited\":%d",
           r->lines_per_page, r->tilt_zone, r->head_enabled, r->exit_active, exited);
    printf(",\"guideHidden\":%d,\"underlines\":[",
           r->guide_marker ? r->guide_marker->hidden : 1);
    for (unsigned i = 0; i < MAX_UNDERLINES; ++i) {
        gm_plugin_lvgl_obj_t *line = r->underline[i];
        if (line == 0 || line->hidden || line->point_count < 2) continue;
        printf("%s[%d,%d,%d]", first ? "" : ",", (int)line->points[0].x,
               (int)line->points[1].x, (int)line->points[0].y);
        first = 0;
    }
    printf("]}\n");
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int main(void)
{
    gm_plugin_descriptor_t descriptor;
    char *line = 0;
    size_t capacity = 0;
    setvbuf(stdout, 0, _IOLBF, 0);
    setup_host();
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.struct_size = sizeof(descriptor);
    if (gm_plugin_entry(&host_api, &descriptor) != GM_PLUGIN_OK) {
        fprintf(stderr, "entry failed\n");
        return 1;
    }
    if (descriptor.on_start(descriptor.context) != GM_PLUGIN_OK) {
        fprintf(stderr, "start failed\n");
        return 1;
    }
    printf("OK\n");
    while (getline(&line, &capacity, stdin) > 0) {
        gm_plugin_event_t event;
        memset(&event, 0, sizeof(event));
        event.struct_size = sizeof(event);
        event.timestamp_ms = now_ms;
        if (strncmp(line, "MSG ", 4) == 0) {
            size_t hex_length = strcspn(line + 4, "\r\n");
            uint8_t *bytes = malloc(hex_length / 2 + 1);
            for (size_t i = 0; i < hex_length / 2; ++i)
                bytes[i] = (uint8_t)(hex_value(line[4 + i * 2]) << 4 | hex_value(line[5 + i * 2]));
            event.type = GM_PLUGIN_EVENT_BT_MESSAGE;
            event.data.bt.channel = MD_CHANNEL;
            event.data.bt.data = bytes;
            event.data.bt.length = (uint32_t)(hex_length / 2);
            if (!descriptor.on_event(descriptor.context, &event)) printf("REJECTED\n");
            free(bytes);
        } else if (strncmp(line, "BTN ", 4) == 0) {
            unsigned button = 0, action = 0;
            sscanf(line + 4, "%u %u", &button, &action);
            event.type = GM_PLUGIN_EVENT_BUTTON;
            event.data.button.button = (gm_plugin_button_t)button;
            event.data.button.action = (gm_plugin_button_action_t)action;
            descriptor.on_event(descriptor.context, &event);
        } else if (strncmp(line, "IMU ", 4) == 0) {
            int pitch = 0, gx = 0, gy = 0, gz = 0;
            sscanf(line + 4, "%d %d %d %d", &pitch, &gx, &gy, &gz);
            imu_sample.pitch_degrees = (int16_t)pitch;
            imu_sample.gyro_raw[0] = (int16_t)gx;
            imu_sample.gyro_raw[1] = (int16_t)gy;
            imu_sample.gyro_raw[2] = (int16_t)gz;
        } else if (strncmp(line, "TICK ", 5) == 0) {
            unsigned total = (unsigned)atoi(line + 5);
            for (unsigned elapsed = 0; elapsed < total && !exited; elapsed += 20U) {
                now_ms += 20U;
                descriptor.on_loop(descriptor.context, 20U);
            }
        } else if (strncmp(line, "CONNECT ", 8) == 0) {
            event.type = GM_PLUGIN_EVENT_CONNECTION;
            event.data.connection.connected = atoi(line + 8) != 0;
            descriptor.on_event(descriptor.context, &event);
        } else if (strncmp(line, "DUMP", 4) == 0) {
            dump();
        }
        printf("OK\n");
    }
    descriptor.on_stop(descriptor.context);
    free(line);
    return 0;
}
