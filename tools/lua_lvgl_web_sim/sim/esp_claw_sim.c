#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#include <emscripten.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "lua_module_lvgl.h"

extern esp_err_t lua_lvgl_deinit_runtime(void);
extern void sim_esp_compat_pump_once(void);

#define SIM_DEFAULT_WIDTH 800
#define SIM_DEFAULT_HEIGHT 480

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} sim_color_t;

static lua_State *s_lua;
static bool s_display_initialized;
static bool s_frame_active;
static char s_display_owner[16];
static bool s_lvgl_init_claim;
static const uintptr_t SIM_PANEL_HANDLE = 0xEC1A0001u;
static const uintptr_t SIM_IO_HANDLE = 0xEC1A0002u;
static const uintptr_t SIM_TOUCH_HANDLE = 0xEC1A0003u;
static const uintptr_t SIM_AUDIO_CODEC_HANDLE = 0xEC1A0004u;

static bool sim_stop_requested(void)
{
    return EM_ASM_INT({ return Module.__stopRequested ? 1 : 0; }) != 0;
}

static void sim_set_stop_requested(bool requested)
{
    EM_ASM({
        Module.__stopRequested = !!$0;
    }, requested ? 1 : 0);
}

static char *sim_js_take_last_string(void)
{
    return (char *)EM_ASM_PTR({
        const text = Module.__simLastString || "";
        Module.__simLastString = "";
        const len = lengthBytesUTF8(text) + 1;
        const ptr = _malloc(len);
        stringToUTF8(text, ptr, len);
        return ptr;
    });
}

static int sim_lua_error_last_string(lua_State *L, const char *fallback)
{
    char *message = sim_js_take_last_string();
    lua_pushstring(L, message && message[0] ? message : fallback);
    free(message);
    return lua_error(L);
}

static const char *sim_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void sim_ensure_dir(const char *path)
{
    char tmp[512];
    size_t len;
    char *p;
    if (!path || !path[0]) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static void sim_ensure_runtime_dirs(void)
{
    sim_ensure_dir("/app");
    sim_ensure_dir("/storage");
    sim_ensure_dir("/tmp");
    sim_ensure_dir("/uploads");
    sim_ensure_dir("/ramfs");
    sim_ensure_dir("/ramfs/network_radio");
}

static void sim_extend_package_path(lua_State *L, const char *script_path)
{
    char dir[512];
    const char *slash;
    if (!script_path) {
        return;
    }
    slash = strrchr(script_path, '/');
    if (!slash || slash == script_path) {
        return;
    }
    size_t len = (size_t)(slash - script_path);
    if (len >= sizeof(dir)) {
        len = sizeof(dir) - 1;
    }
    memcpy(dir, script_path, len);
    dir[len] = '\0';
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    const char *old_path = lua_tostring(L, -1);
    lua_pushfstring(L,
                    "%s/?.lua;%s/?/init.lua;"
                    "/scripts/network_radio/?.lua;/scripts/network_radio/?/init.lua;"
                    "%s",
                    dir, dir, old_path ? old_path : "");
    lua_setfield(L, -3, "path");
    lua_pop(L, 2);
}

static void sim_lua_stop_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (sim_stop_requested()) {
        luaL_error(L, "script stopped");
    }
}

static bool sim_take_touch_event(void)
{
    return EM_ASM_INT({
        const event = Module.__takeTouchEvent ? Module.__takeTouchEvent() : null;
        Module.__polledTouchEvent = event;
        return event ? 1 : 0;
    }) != 0;
}

static int sim_polled_touch_x(void)
{
    return EM_ASM_INT({ return Module.__polledTouchEvent ? (Module.__polledTouchEvent.x | 0) : 0; });
}

static int sim_polled_touch_y(void)
{
    return EM_ASM_INT({ return Module.__polledTouchEvent ? (Module.__polledTouchEvent.y | 0) : 0; });
}

static bool sim_polled_touch_pressed(void)
{
    return EM_ASM_INT({ return Module.__polledTouchEvent && Module.__polledTouchEvent.pressed ? 1 : 0; }) != 0;
}

static int sim_polled_touch_id(void)
{
    return EM_ASM_INT({ return Module.__polledTouchEvent ? (Module.__polledTouchEvent.id | 0) : 0; });
}

static int sim_polled_touch_kind(void)
{
    return EM_ASM_INT({
        const type = Module.__polledTouchEvent ? Module.__polledTouchEvent.type : "";
        if (type === "down") return 1;
        if (type === "move") return 2;
        if (type === "up") return 3;
        if (type === "cancel") return 4;
        if (type === "wheel") return 5;
        return 0;
    });
}

static int sim_polled_touch_delta_y(void)
{
    return EM_ASM_INT({
        return Module.__polledTouchEvent ? (Module.__polledTouchEvent.deltaY | 0) : 0;
    });
}

static int sim_canvas_width(void)
{
    int width = EM_ASM_INT({
        return Module.canvas ? (Module.canvas.width | 0) : 0;
    });
    return width > 0 ? width : SIM_DEFAULT_WIDTH;
}

static int sim_canvas_height(void)
{
    int height = EM_ASM_INT({
        return Module.canvas ? (Module.canvas.height | 0) : 0;
    });
    return height > 0 ? height : SIM_DEFAULT_HEIGHT;
}

static void sim_canvas_clear(sim_color_t color)
{
    EM_ASM({
        const canvas = Module.canvas;
        const ctx = canvas.getContext('2d');
        ctx.save();
        ctx.fillStyle = `rgba(${ $0 }, ${ $1 }, ${ $2 }, ${ $3 / 255 })`;
        ctx.fillRect(0, 0, canvas.width, canvas.height);
        ctx.restore();
    }, color.r, color.g, color.b, color.a);
}

static void sim_canvas_push_clip(int x, int y, int w, int h)
{
    EM_ASM({
        const ctx = Module.canvas.getContext('2d');
        ctx.save();
        ctx.beginPath();
        ctx.rect($0, $1, Math.max(0, $2), Math.max(0, $3));
        ctx.clip();
    }, x, y, w, h);
}

static void sim_canvas_pop_clip(void)
{
    EM_ASM({
        Module.canvas.getContext('2d').restore();
    });
}

static void sim_canvas_fill_rect(int x, int y, int w, int h, sim_color_t color)
{
    EM_ASM({
        const ctx = Module.canvas.getContext('2d');
        ctx.save();
        ctx.fillStyle = `rgba(${ $4 }, ${ $5 }, ${ $6 }, ${ $7 / 255 })`;
        ctx.fillRect($0, $1, $2, $3);
        ctx.restore();
    }, x, y, w, h, color.r, color.g, color.b, color.a);
}

static void sim_canvas_stroke_rect(int x, int y, int w, int h, sim_color_t color)
{
    EM_ASM({
        const ctx = Module.canvas.getContext('2d');
        ctx.save();
        ctx.strokeStyle = `rgba(${ $4 }, ${ $5 }, ${ $6 }, ${ $7 / 255 })`;
        ctx.strokeRect($0 + 0.5, $1 + 0.5, $2, $3);
        ctx.restore();
    }, x, y, w, h, color.r, color.g, color.b, color.a);
}

static void sim_canvas_line(int x0, int y0, int x1, int y1, sim_color_t color)
{
    EM_ASM({
        const ctx = Module.canvas.getContext('2d');
        ctx.save();
        ctx.strokeStyle = `rgba(${ $4 }, ${ $5 }, ${ $6 }, ${ $7 / 255 })`;
        ctx.beginPath();
        ctx.moveTo($0 + 0.5, $1 + 0.5);
        ctx.lineTo($2 + 0.5, $3 + 0.5);
        ctx.stroke();
        ctx.restore();
    }, x0, y0, x1, y1, color.r, color.g, color.b, color.a);
}

static void sim_canvas_circle(int cx, int cy, int r, bool fill, sim_color_t color)
{
    EM_ASM({
        const ctx = Module.canvas.getContext('2d');
        ctx.save();
        const style = `rgba(${ $4 }, ${ $5 }, ${ $6 }, ${ $7 / 255 })`;
        ctx.beginPath();
        ctx.arc($0, $1, $2, 0, Math.PI * 2);
        if ($3) {
            ctx.fillStyle = style;
            ctx.fill();
        } else {
            ctx.strokeStyle = style;
            ctx.stroke();
        }
        ctx.restore();
    }, cx, cy, r, fill ? 1 : 0, color.r, color.g, color.b, color.a);
}

static void sim_canvas_ellipse(int cx, int cy, int rx, int ry, bool fill, sim_color_t color)
{
    EM_ASM({
        const ctx = Module.canvas.getContext('2d');
        ctx.save();
        const style = `rgba(${ $5 }, ${ $6 }, ${ $7 }, ${ $8 / 255 })`;
        ctx.beginPath();
        ctx.ellipse($0, $1, $2, $3, 0, 0, Math.PI * 2);
        if ($4) {
            ctx.fillStyle = style;
            ctx.fill();
        } else {
            ctx.strokeStyle = style;
            ctx.stroke();
        }
        ctx.restore();
    }, cx, cy, rx, ry, fill ? 1 : 0, color.r, color.g, color.b, color.a);
}

static void sim_canvas_arc(int cx, int cy, int r, double start_deg, double end_deg, bool fill, int inner_r, sim_color_t color)
{
    EM_ASM({
        const ctx = Module.canvas.getContext('2d');
        const start = $3 * Math.PI / 180;
        const end = $4 * Math.PI / 180;
        const style = `rgba(${ $7 }, ${ $8 }, ${ $9 }, ${ $10 / 255 })`;
        ctx.save();
        ctx.beginPath();
        if ($5) {
            ctx.arc($0, $1, $2, start, end);
            ctx.arc($0, $1, $6, end, start, true);
            ctx.closePath();
            ctx.fillStyle = style;
            ctx.fill();
        } else {
            ctx.arc($0, $1, $2, start, end);
            ctx.strokeStyle = style;
            ctx.stroke();
        }
        ctx.restore();
    }, cx, cy, r, start_deg, end_deg, fill ? 1 : 0, inner_r, color.r, color.g, color.b, color.a);
}

static void sim_canvas_round_rect(int x, int y, int w, int h, int r, bool fill, sim_color_t color)
{
    EM_ASM({
        const ctx = Module.canvas.getContext('2d');
        const style = `rgba(${ $6 }, ${ $7 }, ${ $8 }, ${ $9 / 255 })`;
        ctx.save();
        ctx.beginPath();
        ctx.roundRect($0, $1, $2, $3, $4);
        if ($5) {
            ctx.fillStyle = style;
            ctx.fill();
        } else {
            ctx.strokeStyle = style;
            ctx.stroke();
        }
        ctx.restore();
    }, x, y, w, h, r, fill ? 1 : 0, color.r, color.g, color.b, color.a);
}

static void sim_canvas_triangle(int x1, int y1, int x2, int y2, int x3, int y3, bool fill, sim_color_t color)
{
    EM_ASM({
        const ctx = Module.canvas.getContext('2d');
        const style = `rgba(${ $7 }, ${ $8 }, ${ $9 }, ${ $10 / 255 })`;
        ctx.save();
        ctx.beginPath();
        ctx.moveTo($0, $1);
        ctx.lineTo($2, $3);
        ctx.lineTo($4, $5);
        ctx.closePath();
        if ($6) {
            ctx.fillStyle = style;
            ctx.fill();
        } else {
            ctx.strokeStyle = style;
            ctx.stroke();
        }
        ctx.restore();
    }, x1, y1, x2, y2, x3, y3, fill ? 1 : 0, color.r, color.g, color.b, color.a);
}

static void sim_canvas_text(int x, int y, const char *text, int font_size, sim_color_t color)
{
    EM_ASM({
        const ctx = Module.canvas.getContext('2d');
        const text = UTF8ToString($2);
        ctx.save();
        ctx.font = `${$3}px Arial`;
        ctx.textBaseline = 'top';
        ctx.fillStyle = `rgba(${ $4 }, ${ $5 }, ${ $6 }, ${ $7 / 255 })`;
        ctx.fillText(text, $0, $1);
        ctx.restore();
    }, x, y, text, font_size, color.r, color.g, color.b, color.a);
}

static void sim_canvas_rgb565(int x, int y, const uint8_t *pixels, int full_w, int src_x, int src_y,
                              int src_w, int src_h, int dst_w, int dst_h)
{
    EM_ASM({
        const ctx = Module.canvas.getContext('2d');
        const src = HEAPU8;
        const image = ctx.createImageData($8, $9);
        const out = image.data;
        for (let dy = 0; dy < $9; dy++) {
            const sy = $7 <= 1 ? 0 : Math.floor(dy * $7 / $9);
            for (let dx = 0; dx < $8; dx++) {
                const sx = $6 <= 1 ? 0 : Math.floor(dx * $6 / $8);
                const pixelIndex = $2 + ((($5 + sy) * $3 + ($4 + sx)) * 2);
                const value = src[pixelIndex] | (src[pixelIndex + 1] << 8);
                const outIndex = (dy * $8 + dx) * 4;
                out[outIndex] = ((value >> 11) & 0x1f) * 255 / 31;
                out[outIndex + 1] = ((value >> 5) & 0x3f) * 255 / 63;
                out[outIndex + 2] = (value & 0x1f) * 255 / 31;
                out[outIndex + 3] = 255;
            }
        }
        ctx.putImageData(image, $0, $1);
    }, x, y, pixels, full_w, src_x, src_y, src_w, src_h, dst_w, dst_h);
}

static int sim_text_width(const char *text, int font_size)
{
    if (!text) {
        return 0;
    }
    return (int)(strlen(text) * font_size * 0.58);
}

static bool parse_hex_byte(char hi, char lo, uint8_t *out)
{
    int h = isdigit((unsigned char)hi) ? hi - '0' : 10 + tolower((unsigned char)hi) - 'a';
    int l = isdigit((unsigned char)lo) ? lo - '0' : 10 + tolower((unsigned char)lo) - 'a';
    if (h < 0 || h > 15 || l < 0 || l > 15) {
        return false;
    }
    *out = (uint8_t)((h << 4) | l);
    return true;
}

static int parse_color(lua_State *L, int index, sim_color_t *out)
{
    index = lua_absindex(L, index);
    out->r = 255;
    out->g = 255;
    out->b = 255;
    out->a = 255;
    if (lua_type(L, index) == LUA_TSTRING) {
        const char *s = lua_tostring(L, index);
        if (strcmp(s, "black") == 0) { out->r = out->g = out->b = 0; return 0; }
        if (strcmp(s, "white") == 0) { return 0; }
        if (strcmp(s, "red") == 0) { out->r = 255; out->g = out->b = 0; return 0; }
        if (strcmp(s, "green") == 0) { out->g = 255; out->r = out->b = 0; return 0; }
        if (strcmp(s, "blue") == 0) { out->b = 255; out->r = out->g = 0; return 0; }
        if (strcmp(s, "yellow") == 0) { out->r = out->g = 255; out->b = 0; return 0; }
        if (strcmp(s, "cyan") == 0) { out->g = out->b = 255; out->r = 0; return 0; }
        if (strcmp(s, "magenta") == 0) { out->r = out->b = 255; out->g = 0; return 0; }
        if (strcmp(s, "transparent") == 0) { out->a = 0; return 0; }
        if (s[0] == '#' && strlen(s) == 7 &&
                parse_hex_byte(s[1], s[2], &out->r) &&
                parse_hex_byte(s[3], s[4], &out->g) &&
                parse_hex_byte(s[5], s[6], &out->b)) {
            return 0;
        }
    } else if (lua_istable(L, index)) {
        lua_getfield(L, index, "r");
        lua_getfield(L, index, "g");
        lua_getfield(L, index, "b");
        lua_getfield(L, index, "a");
        out->r = (uint8_t)luaL_optinteger(L, -4, 255);
        out->g = (uint8_t)luaL_optinteger(L, -3, 255);
        out->b = (uint8_t)luaL_optinteger(L, -2, 255);
        out->a = (uint8_t)luaL_optinteger(L, -1, 255);
        lua_pop(L, 4);
        return 0;
    }
    return luaL_error(L, "invalid color");
}

static sim_color_t opt_color_field(lua_State *L, int index, const char *field, sim_color_t fallback)
{
    sim_color_t color = fallback;
    index = lua_absindex(L, index);
    if (lua_istable(L, index)) {
        lua_getfield(L, index, field);
        if (!lua_isnil(L, -1)) {
            parse_color(L, -1, &color);
        }
        lua_pop(L, 1);
    }
    return color;
}

static int opt_int_field(lua_State *L, int index, const char *field, int fallback)
{
    int value = fallback;
    index = lua_absindex(L, index);
    if (lua_istable(L, index)) {
        lua_getfield(L, index, field);
        if (!lua_isnil(L, -1)) {
            value = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);
    }
    return value;
}

static const char *opt_string_field(lua_State *L, int index, const char *field, const char *fallback)
{
    const char *value = fallback;
    index = lua_absindex(L, index);
    if (lua_istable(L, index)) {
        lua_getfield(L, index, field);
        if (!lua_isnil(L, -1)) {
            value = luaL_checkstring(L, -1);
        }
        lua_pop(L, 1);
    }
    return value;
}

static bool get_int_field(lua_State *L, int index, const char *field, int *out)
{
    bool found = false;
    index = lua_absindex(L, index);
    lua_getfield(L, index, field);
    if (!lua_isnil(L, -1)) {
        *out = (int)luaL_checkinteger(L, -1);
        found = true;
    }
    lua_pop(L, 1);
    return found;
}

static void parse_source_rect(lua_State *L, int opts_idx, int *src_x, int *src_y, int *src_w, int *src_h)
{
    opts_idx = lua_absindex(L, opts_idx);
    lua_getfield(L, opts_idx, "source");
    if (lua_istable(L, -1)) {
        int source_idx = lua_absindex(L, -1);
        get_int_field(L, source_idx, "x", src_x);
        get_int_field(L, source_idx, "y", src_y);
        get_int_field(L, source_idx, "width", src_w);
        get_int_field(L, source_idx, "height", src_h);
    }
    lua_pop(L, 1);
}

static void require_display(lua_State *L)
{
    if (!s_display_initialized) {
        luaL_error(L, "display is not initialized");
    }
}

static int claim_display_owner(lua_State *L, const char *owner)
{
    if (s_display_owner[0] != '\0' && strcmp(s_display_owner, owner) != 0) {
        return luaL_error(L, "display already owned by %s", s_display_owner);
    }
    snprintf(s_display_owner, sizeof(s_display_owner), "%s", owner);
    return 0;
}

static int lua_board_get_display_lcd_params(lua_State *L)
{
    (void)luaL_optstring(L, 1, "display_lcd");
    lua_pushlightuserdata(L, (void *)SIM_PANEL_HANDLE);
    lua_pushlightuserdata(L, (void *)SIM_IO_HANDLE);
    lua_pushinteger(L, sim_canvas_width());
    lua_pushinteger(L, sim_canvas_height());
    lua_pushinteger(L, 1);
    return 5;
}

static int lua_board_get_lcd_touch_handle(lua_State *L)
{
    (void)luaL_optstring(L, 1, "lcd_touch");
    lua_pushlightuserdata(L, (void *)SIM_TOUCH_HANDLE);
    return 1;
}

static int lua_board_get_audio_codec_output_params(lua_State *L)
{
    (void)luaL_optstring(L, 1, "audio_dac");
    lua_pushlightuserdata(L, (void *)SIM_AUDIO_CODEC_HANDLE);
    lua_pushinteger(L, 16000);
    lua_pushinteger(L, 1);
    lua_pushinteger(L, 16);
    return 4;
}

static int lua_board_get_camera_paths(lua_State *L)
{
    lua_newtable(L);
    lua_pushstring(L, "/dev/sim_camera0");
    lua_setfield(L, -2, "dev_path");
    lua_pushstring(L, "/dev/sim_camera0");
    lua_setfield(L, -2, "path");
    return 1;
}

static int luaopen_board_manager(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_board_get_display_lcd_params);
    lua_setfield(L, -2, "get_display_lcd_params");
    lua_pushcfunction(L, lua_board_get_lcd_touch_handle);
    lua_setfield(L, -2, "get_lcd_touch_handle");
    lua_pushcfunction(L, lua_board_get_audio_codec_output_params);
    lua_setfield(L, -2, "get_audio_codec_output_params");
    lua_pushcfunction(L, lua_board_get_camera_paths);
    lua_setfield(L, -2, "get_camera_paths");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "PANEL_IF_IO");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "PANEL_IF_RGB");
    lua_pushinteger(L, 2);
    lua_setfield(L, -2, "PANEL_IF_MIPI_DSI");
    return 1;
}

static int lua_audio_output_write(lua_State *L)
{
    int output_id;
    size_t len = 0;
    const char *data;
    int ok;
    luaL_checktype(L, 1, LUA_TTABLE);
    data = luaL_checklstring(L, 2, &len);
    lua_getfield(L, 1, "__sim_output_id");
    output_id = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    ok = EM_ASM_INT({
        const err = Module.__simServices.audio.writePcm($0, $1, $2);
        Module.__simLastString = err || "";
        return err ? 0 : 1;
    }, output_id, data, (int)len);
    if (!ok) {
        char *err = sim_js_take_last_string();
        printf("[audio] pcm write skipped: %s\n", err ? err : "audio write failed");
        free(err);
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, ok);
    return 1;
}

static int lua_audio_output_close(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_audio_output_info(lua_State *L)
{
    int sample_rate;
    int channels;
    int bits;

    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "sample_rate");
    sample_rate = (int)luaL_optinteger(L, -1, 16000);
    lua_pop(L, 1);
    lua_getfield(L, 1, "channels");
    channels = (int)luaL_optinteger(L, -1, 1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "bits");
    bits = (int)luaL_optinteger(L, -1, 16);
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushinteger(L, sample_rate);
    lua_setfield(L, -2, "sample_rate");
    lua_pushinteger(L, channels);
    lua_setfield(L, -2, "channels");
    lua_pushinteger(L, bits);
    lua_setfield(L, -2, "bits");
    return 1;
}

static int lua_audio_output_set_volume(lua_State *L)
{
    int output_id;
    int volume;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__sim_output_id");
    output_id = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    volume = (int)luaL_optinteger(L, 2, 80);
    lua_pushboolean(L, EM_ASM_INT({
        return Module.__simServices.audio.setVolume($0, $1) ? 1 : 0;
    }, output_id, volume));
    return 1;
}

static int lua_audio_new_output(lua_State *L)
{
    int sample_rate = 16000;
    int channels = 1;
    int bits = 16;
    int volume = 70;
    int output_id;

    luaL_checktype(L, 1, LUA_TTABLE);
    lua_rawgeti(L, 1, 2);
    sample_rate = (int)luaL_optinteger(L, -1, sample_rate);
    lua_pop(L, 1);
    lua_rawgeti(L, 1, 3);
    channels = (int)luaL_optinteger(L, -1, channels);
    lua_pop(L, 1);
    lua_rawgeti(L, 1, 4);
    bits = (int)luaL_optinteger(L, -1, bits);
    lua_pop(L, 1);
    lua_getfield(L, 1, "sample_rate");
    sample_rate = (int)luaL_optinteger(L, -1, sample_rate);
    lua_pop(L, 1);
    lua_getfield(L, 1, "rate");
    sample_rate = (int)luaL_optinteger(L, -1, sample_rate);
    lua_pop(L, 1);
    lua_getfield(L, 1, "channels");
    channels = (int)luaL_optinteger(L, -1, channels);
    lua_pop(L, 1);
    lua_getfield(L, 1, "bits");
    bits = (int)luaL_optinteger(L, -1, bits);
    lua_pop(L, 1);
    lua_getfield(L, 1, "volume");
    volume = (int)luaL_optinteger(L, -1, volume);
    lua_pop(L, 1);

    output_id = EM_ASM_INT({
        return Module.__simServices.audio.createOutput({
            sample_rate: $0,
            channels: $1,
            bits: $2,
            volume: $3,
        }) | 0;
    }, sample_rate, channels, bits, volume);

    lua_newtable(L);
    lua_pushinteger(L, output_id);
    lua_setfield(L, -2, "__sim_output_id");
    lua_pushinteger(L, sample_rate);
    lua_setfield(L, -2, "sample_rate");
    lua_pushinteger(L, channels);
    lua_setfield(L, -2, "channels");
    lua_pushinteger(L, bits);
    lua_setfield(L, -2, "bits");
    lua_pushcfunction(L, lua_audio_output_write);
    lua_setfield(L, -2, "write");
    lua_pushcfunction(L, lua_audio_output_close);
    lua_setfield(L, -2, "close");
    lua_pushcfunction(L, lua_audio_output_info);
    lua_setfield(L, -2, "info");
    lua_pushcfunction(L, lua_audio_output_set_volume);
    lua_setfield(L, -2, "set_volume");
    return 1;
}

static int lua_audio_player_play(lua_State *L)
{
    int player_id;
    luaL_checktype(L, 1, LUA_TTABLE);
    const char *url = luaL_optstring(L, 2, "");
    lua_getfield(L, 1, "__sim_player_id");
    player_id = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    int ok = EM_ASM_INT({
        const err = Module.__simServices.audio.play($0, UTF8ToString($1));
        Module.__simLastString = err || "";
        return err ? 0 : 1;
    }, player_id, url);
    lua_pushboolean(L, ok);
    if (!ok) {
        char *err = sim_js_take_last_string();
        lua_pushstring(L, err ? err : "audio play failed");
        free(err);
        return 2;
    }
    return 1;
}

static int lua_audio_player_stop(lua_State *L)
{
    int player_id;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__sim_player_id");
    player_id = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    lua_pushboolean(L, EM_ASM_INT({
        return Module.__simServices.audio.stop($0) ? 1 : 0;
    }, player_id));
    return 1;
}

static int lua_audio_player_close(lua_State *L)
{
    int player_id;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__sim_player_id");
    player_id = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    lua_pushboolean(L, EM_ASM_INT({
        return Module.__simServices.audio.closePlayer($0) ? 1 : 0;
    }, player_id));
    return 1;
}

static int lua_audio_player_poll(lua_State *L)
{
    int player_id;
    char *state;
    char *err;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__sim_player_id");
    player_id = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    EM_ASM({
        const info = Module.__simServices.audio.poll($0);
        Module.__simAudioPollState = info.state || "stopped";
        Module.__simAudioPollError = info.error || "";
    }, player_id);
    state = (char *)EM_ASM_PTR({
        const text = Module.__simAudioPollState || "stopped";
        const len = lengthBytesUTF8(text) + 1;
        const ptr = _malloc(len);
        stringToUTF8(text, ptr, len);
        return ptr;
    });
    err = (char *)EM_ASM_PTR({
        const text = Module.__simAudioPollError || "";
        const len = lengthBytesUTF8(text) + 1;
        const ptr = _malloc(len);
        stringToUTF8(text, ptr, len);
        return ptr;
    });
    lua_newtable(L);
    lua_pushstring(L, state ? state : "stopped");
    lua_setfield(L, -2, "state");
    if (err && err[0]) {
        lua_pushstring(L, err);
        lua_setfield(L, -2, "error");
    }
    free(state);
    free(err);
    return 1;
}

static int lua_audio_player(lua_State *L)
{
    int output_id = 0;
    int player_id;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "output");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "__sim_output_id");
        output_id = (int)luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    player_id = EM_ASM_INT({
        return Module.__simServices.audio.createPlayer($0) | 0;
    }, output_id);
    lua_newtable(L);
    lua_pushinteger(L, player_id);
    lua_setfield(L, -2, "__sim_player_id");
    lua_pushcfunction(L, lua_audio_player_play);
    lua_setfield(L, -2, "play");
    lua_pushcfunction(L, lua_audio_player_stop);
    lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, lua_audio_player_close);
    lua_setfield(L, -2, "close");
    lua_pushcfunction(L, lua_audio_player_poll);
    lua_setfield(L, -2, "poll");
    return 1;
}

static int luaopen_audio(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"new_output", lua_audio_new_output},
        {"player", lua_audio_player},
        {NULL, NULL},
    };
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

static int lua_delay_ms(lua_State *L)
{
    int ms = (int)luaL_checkinteger(L, 1);
    int elapsed = 0;
    while (elapsed < ms) {
        int step = ms - elapsed;
        if (step > 20) {
            step = 20;
        }
        if (sim_stop_requested()) {
            return luaL_error(L, "script stopped");
        }
        sim_esp_compat_pump_once();
        emscripten_sleep((unsigned int)step);
        elapsed += step;
    }
    if (sim_stop_requested()) {
        return luaL_error(L, "script stopped");
    }
    sim_esp_compat_pump_once();
    return 0;
}

static int luaopen_delay(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_delay_ms);
    lua_setfield(L, -2, "delay_ms");
    return 1;
}

static int lua_storage_get_root_dir(lua_State *L)
{
    lua_pushstring(L, "/storage");
    return 1;
}

static int lua_storage_join_path(lua_State *L)
{
    const char *a = luaL_checkstring(L, 1);
    const char *b = luaL_checkstring(L, 2);
    char out[512];
    snprintf(out, sizeof(out), "%s%s%s", a, (a[strlen(a) - 1] == '/') ? "" : "/", b);
    lua_pushstring(L, out);
    return 1;
}

static int lua_storage_exists(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        lua_pushboolean(L, 1);
    } else {
        struct stat st;
        lua_pushboolean(L, stat(path, &st) == 0);
    }
    return 1;
}

static int lua_storage_mkdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    sim_ensure_dir(path);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_storage_read_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;
    if (!f) {
        lua_pushnil(L);
        lua_pushfstring(L, "open failed: %s", path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)len);
    if (len > 0 && !buf) {
        fclose(f);
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        lua_pushnil(L);
        lua_pushstring(L, "read failed");
        return 2;
    }
    fclose(f);
    lua_pushlstring(L, buf ? buf : "", (size_t)len);
    free(buf);
    return 1;
}

static int lua_storage_write_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    char parent[512];
    const char *slash = strrchr(path, '/');
    FILE *f = fopen(path, "wb");
    if (!f && slash && slash != path) {
        size_t parent_len = (size_t)(slash - path);
        if (parent_len >= sizeof(parent)) {
            parent_len = sizeof(parent) - 1;
        }
        memcpy(parent, path, parent_len);
        parent[parent_len] = '\0';
        sim_ensure_dir(parent);
        f = fopen(path, "wb");
    }
    if (!f) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "open failed: %s", path);
        return 2;
    }
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        lua_pushboolean(L, 0);
        lua_pushstring(L, "write failed");
        return 2;
    }
    fclose(f);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_storage_stat(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    struct stat st;
    if (stat(path, &st) != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "stat failed: %s", path);
        return 2;
    }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)st.st_size);
    lua_setfield(L, -2, "size");
    lua_pushstring(L, S_ISDIR(st.st_mode) ? "dir" : "file");
    lua_setfield(L, -2, "type");
    return 1;
}

static int lua_storage_rename(lua_State *L)
{
    const char *from = luaL_checkstring(L, 1);
    const char *to = luaL_checkstring(L, 2);
    lua_pushboolean(L, rename(from, to) == 0);
    return 1;
}

static int lua_storage_remove(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_pushboolean(L, remove(path) == 0);
    return 1;
}

static int lua_storage_listdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    DIR *dir = opendir(path);
    struct dirent *entry;
    int i = 1;
    if (!dir) {
        lua_pushnil(L);
        lua_pushfstring(L, "opendir failed: %s", path);
        return 2;
    }
    lua_newtable(L);
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        lua_newtable(L);
        lua_pushstring(L, entry->d_name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, entry->d_type == DT_DIR ? "dir" : "file");
        lua_setfield(L, -2, "type");
        lua_rawseti(L, -2, i++);
    }
    closedir(dir);
    return 1;
}

static int luaopen_storage(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_storage_get_root_dir);
    lua_setfield(L, -2, "get_root_dir");
    lua_pushcfunction(L, lua_storage_join_path);
    lua_setfield(L, -2, "join_path");
    lua_pushcfunction(L, lua_storage_exists);
    lua_setfield(L, -2, "exists");
    lua_pushcfunction(L, lua_storage_mkdir);
    lua_setfield(L, -2, "mkdir");
    lua_pushcfunction(L, lua_storage_read_file);
    lua_setfield(L, -2, "read_file");
    lua_pushcfunction(L, lua_storage_write_file);
    lua_setfield(L, -2, "write_file");
    lua_pushcfunction(L, lua_storage_stat);
    lua_setfield(L, -2, "stat");
    lua_pushcfunction(L, lua_storage_rename);
    lua_setfield(L, -2, "rename");
    lua_pushcfunction(L, lua_storage_remove);
    lua_setfield(L, -2, "remove");
    lua_pushcfunction(L, lua_storage_listdir);
    lua_setfield(L, -2, "listdir");
    return 1;
}

static int lua_touch_poll(lua_State *L)
{
    static const char *event_types[] = {"unknown", "down", "move", "up", "cancel", "wheel"};
    int kind;

    if (!sim_take_touch_event()) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    lua_pushinteger(L, sim_polled_touch_x());
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, sim_polled_touch_y());
    lua_setfield(L, -2, "y");
    lua_pushboolean(L, sim_polled_touch_pressed());
    lua_setfield(L, -2, "pressed");
    lua_pushinteger(L, sim_polled_touch_id());
    lua_setfield(L, -2, "id");
    kind = sim_polled_touch_kind();
    if (kind < 0 || kind > 5) {
        kind = 0;
    }
    lua_pushstring(L, event_types[kind]);
    lua_setfield(L, -2, "type");
    lua_pushinteger(L, sim_polled_touch_delta_y());
    lua_setfield(L, -2, "delta_y");
    return 1;
}

static int luaopen_touch(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_touch_poll);
    lua_setfield(L, -2, "poll");
    return 1;
}

static int lua_system_millis(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)emscripten_get_now());
    return 1;
}

static int lua_system_date(lua_State *L)
{
    const char *fmt = luaL_optstring(L, 1, "%c");
    char out[128];
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    if (!tm_now || strftime(out, sizeof(out), fmt, tm_now) == 0) {
        lua_pushstring(L, "");
    } else {
        lua_pushstring(L, out);
    }
    return 1;
}

static int luaopen_system(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"millis", lua_system_millis},
        {"date", lua_system_date},
        {NULL, NULL},
    };
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

static int lua_button_new(lua_State *L)
{
    int active = (int)luaL_optinteger(L, 2, 0);
    lua_newtable(L);
    lua_pushinteger(L, active);
    lua_setfield(L, -2, "active_level");
    return 1;
}

static int lua_button_get_key_level(lua_State *L)
{
    int active = 0;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "active_level");
    active = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    lua_pushinteger(L, active == 0 ? 1 : 0);
    return 1;
}

static int lua_true_noop(lua_State *L)
{
    (void)L;
    lua_pushboolean(L, 1);
    return 1;
}

static int luaopen_button(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"new", lua_button_new},
        {"get_key_level", lua_button_get_key_level},
        {"close", lua_true_noop},
        {"off", lua_true_noop},
        {NULL, NULL},
    };
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

static int lua_imu_read(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_newtable(L);
    lua_newtable(L);
    lua_pushnumber(L, 0.0);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, 0.0);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, 1.0);
    lua_setfield(L, -2, "z");
    lua_setfield(L, -2, "accel");
    return 1;
}

static int lua_imu_new(lua_State *L)
{
    (void)L;
    lua_newtable(L);
    lua_pushcfunction(L, lua_imu_read);
    lua_setfield(L, -2, "read");
    lua_pushcfunction(L, lua_true_noop);
    lua_setfield(L, -2, "close");
    return 1;
}

static int luaopen_imu(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_imu_new);
    lua_setfield(L, -2, "new");
    return 1;
}

static int lua_i2c_dev_write(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_i2c_dev_read(lua_State *L)
{
    int len;
    char *buf;
    luaL_checktype(L, 1, LUA_TTABLE);
    len = (int)luaL_checkinteger(L, 2);
    if (len < 0) {
        len = 0;
    }
    buf = (char *)calloc((size_t)len, 1);
    if (!buf && len > 0) {
        return luaL_error(L, "out of memory");
    }
    lua_pushlstring(L, buf ? buf : "", (size_t)len);
    free(buf);
    return 1;
}

static int lua_i2c_dev_read_byte(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushinteger(L, 0);
    return 1;
}

static int lua_i2c_bus_device(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    (void)luaL_checkinteger(L, 2);
    lua_newtable(L);
    lua_pushcfunction(L, lua_i2c_dev_write);
    lua_setfield(L, -2, "write");
    lua_pushcfunction(L, lua_i2c_dev_read);
    lua_setfield(L, -2, "read");
    lua_pushcfunction(L, lua_i2c_dev_read_byte);
    lua_setfield(L, -2, "read_byte");
    lua_pushcfunction(L, lua_true_noop);
    lua_setfield(L, -2, "close");
    return 1;
}

static int lua_i2c_new(lua_State *L)
{
    (void)L;
    lua_newtable(L);
    lua_pushcfunction(L, lua_i2c_bus_device);
    lua_setfield(L, -2, "device");
    lua_pushcfunction(L, lua_true_noop);
    lua_setfield(L, -2, "close");
    return 1;
}

static int luaopen_i2c(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_i2c_new);
    lua_setfield(L, -2, "new");
    return 1;
}

EM_JS(int, sim_camera_open_js, (const char *path, int width, int height), {
    const service = Module.__simServices && Module.__simServices.camera;
    if (!service) {
        Module.__simLastString = "camera service is not available";
        return 0;
    }
    const result = service.openSync(UTF8ToString(path), { width, height });
    if (!result || !result.ok) {
        Module.__simLastString = (result && result.error) || "camera.open failed";
        return 0;
    }
    return 1;
});

EM_JS(void, sim_camera_close_js, (), {
    const service = Module.__simServices && Module.__simServices.camera;
    if (service) {
        service.close();
    }
});

EM_JS(int, sim_camera_info_width_js, (), {
    const service = Module.__simServices && Module.__simServices.camera;
    return service ? (service.info().width | 0) : 320;
});

EM_JS(int, sim_camera_info_height_js, (), {
    const service = Module.__simServices && Module.__simServices.camera;
    return service ? (service.info().height | 0) : 240;
});

EM_JS(int, sim_camera_capture_frame_js, (), {
    const service = Module.__simServices && Module.__simServices.camera;
    if (!service) {
        Module.__simLastString = "camera service is not available";
        return 0;
    }
    const result = service.captureFrame();
    if (!result || !result.ok) {
        Module.__simLastString = (result && result.error) || "camera.get_frame failed";
        return 0;
    }
    return result.ptr | 0;
});

EM_JS(int, sim_camera_frame_width_js, (), {
    return Module.__simCameraFrame ? (Module.__simCameraFrame.width | 0) : 0;
});

EM_JS(int, sim_camera_frame_height_js, (), {
    return Module.__simCameraFrame ? (Module.__simCameraFrame.height | 0) : 0;
});

EM_JS(int, sim_camera_frame_len_js, (), {
    return Module.__simCameraFrame ? (Module.__simCameraFrame.len | 0) : 0;
});

static int lua_camera_open(lua_State *L)
{
    const char *path = luaL_optstring(L, 1, "/dev/sim_camera0");
    int width = 0;
    int height = 0;
    if (lua_istable(L, 2)) {
        get_int_field(L, 2, "width", &width);
        get_int_field(L, 2, "height", &height);
    }
    if (!sim_camera_open_js(path, width, height)) {
        return sim_lua_error_last_string(L, "camera.open failed");
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_camera_close(lua_State *L)
{
    (void)L;
    sim_camera_close_js();
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_camera_info(lua_State *L)
{
    int width = sim_camera_info_width_js();
    int height = sim_camera_info_height_js();
    lua_newtable(L);
    lua_pushinteger(L, width > 0 ? width : 320);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, height > 0 ? height : 240);
    lua_setfield(L, -2, "height");
    lua_pushstring(L, "RGBP");
    lua_setfield(L, -2, "pixel_format");
    return 1;
}

static int lua_camera_frame_release(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_camera_get_frame(lua_State *L)
{
    (void)luaL_optinteger(L, 1, 1000);
    int ptr = sim_camera_capture_frame_js();
    if (!ptr) {
        return sim_lua_error_last_string(L, "camera.get_frame failed");
    }
    int w = sim_camera_frame_width_js();
    int h = sim_camera_frame_height_js();
    int len = sim_camera_frame_len_js();
    if (w <= 0 || h <= 0 || len <= 0) {
        free((void *)ptr);
        return luaL_error(L, "camera.get_frame returned invalid frame");
    }

    lua_newtable(L);
    lua_pushinteger(L, w);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, h);
    lua_setfield(L, -2, "height");
    lua_pushstring(L, "RGBP");
    lua_setfield(L, -2, "pixel_format");
    lua_pushinteger(L, len);
    lua_setfield(L, -2, "bytes");
    lua_pushlstring(L, (const char *)ptr, (size_t)len);
    lua_setfield(L, -2, "data");
    lua_pushcfunction(L, lua_camera_frame_release);
    lua_setfield(L, -2, "release");
    free((void *)ptr);
    return 1;
}

static int luaopen_camera(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"open", lua_camera_open},
        {"close", lua_camera_close},
        {"info", lua_camera_info},
        {"flush", lua_true_noop},
        {"get_frame", lua_camera_get_frame},
        {NULL, NULL},
    };
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

static void json_encode_value(lua_State *L, int index, luaL_Buffer *b)
{
    int type;
    index = lua_absindex(L, index);
    type = lua_type(L, index);
    switch (type) {
    case LUA_TBOOLEAN:
        luaL_addstring(b, lua_toboolean(L, index) ? "true" : "false");
        break;
    case LUA_TNUMBER:
    {
        char number[64];
        snprintf(number, sizeof(number), "%.14g", lua_tonumber(L, index));
        luaL_addstring(b, number);
        break;
    }
    case LUA_TSTRING: {
        size_t len = 0;
        const char *s = lua_tolstring(L, index, &len);
        luaL_addchar(b, '"');
        for (size_t i = 0; i < len; i++) {
            if (s[i] == '"' || s[i] == '\\') {
                luaL_addchar(b, '\\');
            }
            luaL_addchar(b, s[i]);
        }
        luaL_addchar(b, '"');
        break;
    }
    case LUA_TTABLE: {
        bool first = true;
        luaL_addchar(b, '{');
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            if (!first) {
                luaL_addchar(b, ',');
            }
            first = false;
            lua_pushvalue(L, -2);
            json_encode_value(L, -1, b);
            lua_pop(L, 1);
            luaL_addchar(b, ':');
            json_encode_value(L, -1, b);
            lua_pop(L, 1);
        }
        luaL_addchar(b, '}');
        break;
    }
    case LUA_TNIL:
    default:
        luaL_addstring(b, "null");
        break;
    }
}

static int lua_json_encode(lua_State *L)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    if (lua_isnumber(L, 1)) {
        lua_pushvalue(L, 1);
        luaL_addvalue(&b);
    } else {
        json_encode_value(L, 1, &b);
    }
    luaL_pushresult(&b);
    return 1;
}

typedef struct {
    const char *p;
} sim_json_parser_t;

static void json_skip_ws(sim_json_parser_t *parser)
{
    while (*parser->p == ' ' || *parser->p == '\n' || *parser->p == '\r' || *parser->p == '\t') {
        parser->p++;
    }
}

static int json_parse_value(lua_State *L, sim_json_parser_t *parser);

static int json_parse_string(lua_State *L, sim_json_parser_t *parser)
{
    luaL_Buffer b;
    if (*parser->p != '"') {
        return luaL_error(L, "json: expected string");
    }
    parser->p++;
    luaL_buffinit(L, &b);
    while (*parser->p && *parser->p != '"') {
        if (*parser->p == '\\') {
            parser->p++;
            switch (*parser->p) {
            case '"': luaL_addchar(&b, '"'); break;
            case '\\': luaL_addchar(&b, '\\'); break;
            case '/': luaL_addchar(&b, '/'); break;
            case 'b': luaL_addchar(&b, '\b'); break;
            case 'f': luaL_addchar(&b, '\f'); break;
            case 'n': luaL_addchar(&b, '\n'); break;
            case 'r': luaL_addchar(&b, '\r'); break;
            case 't': luaL_addchar(&b, '\t'); break;
            case 'u':
                parser->p += 4;
                luaL_addchar(&b, '?');
                break;
            default:
                luaL_addchar(&b, *parser->p);
                break;
            }
        } else {
            luaL_addchar(&b, *parser->p);
        }
        if (*parser->p) {
            parser->p++;
        }
    }
    if (*parser->p != '"') {
        return luaL_error(L, "json: unterminated string");
    }
    parser->p++;
    luaL_pushresult(&b);
    return 1;
}

static int json_parse_number(lua_State *L, sim_json_parser_t *parser)
{
    char *end = NULL;
    double value = strtod(parser->p, &end);
    if (end == parser->p) {
        return luaL_error(L, "json: invalid number");
    }
    parser->p = end;
    lua_pushnumber(L, value);
    return 1;
}

static int json_parse_array(lua_State *L, sim_json_parser_t *parser)
{
    int index = 1;
    parser->p++;
    lua_newtable(L);
    json_skip_ws(parser);
    if (*parser->p == ']') {
        parser->p++;
        return 1;
    }
    while (*parser->p) {
        json_parse_value(L, parser);
        lua_rawseti(L, -2, index++);
        json_skip_ws(parser);
        if (*parser->p == ']') {
            parser->p++;
            return 1;
        }
        if (*parser->p != ',') {
            return luaL_error(L, "json: expected array separator");
        }
        parser->p++;
        json_skip_ws(parser);
    }
    return luaL_error(L, "json: unterminated array");
}

static int json_parse_object(lua_State *L, sim_json_parser_t *parser)
{
    parser->p++;
    lua_newtable(L);
    json_skip_ws(parser);
    if (*parser->p == '}') {
        parser->p++;
        return 1;
    }
    while (*parser->p) {
        json_parse_string(L, parser);
        json_skip_ws(parser);
        if (*parser->p != ':') {
            return luaL_error(L, "json: expected object separator");
        }
        parser->p++;
        json_skip_ws(parser);
        json_parse_value(L, parser);
        lua_settable(L, -3);
        json_skip_ws(parser);
        if (*parser->p == '}') {
            parser->p++;
            return 1;
        }
        if (*parser->p != ',') {
            return luaL_error(L, "json: expected object comma");
        }
        parser->p++;
        json_skip_ws(parser);
    }
    return luaL_error(L, "json: unterminated object");
}

static int json_parse_value(lua_State *L, sim_json_parser_t *parser)
{
    json_skip_ws(parser);
    if (*parser->p == '"') {
        return json_parse_string(L, parser);
    }
    if (*parser->p == '{') {
        return json_parse_object(L, parser);
    }
    if (*parser->p == '[') {
        return json_parse_array(L, parser);
    }
    if (*parser->p == '-' || isdigit((unsigned char)*parser->p)) {
        return json_parse_number(L, parser);
    }
    if (strncmp(parser->p, "true", 4) == 0) {
        parser->p += 4;
        lua_pushboolean(L, 1);
        return 1;
    }
    if (strncmp(parser->p, "false", 5) == 0) {
        parser->p += 5;
        lua_pushboolean(L, 0);
        return 1;
    }
    if (strncmp(parser->p, "null", 4) == 0) {
        parser->p += 4;
        lua_pushnil(L);
        return 1;
    }
    return luaL_error(L, "json: invalid value");
}

static int lua_json_decode(lua_State *L)
{
    sim_json_parser_t parser;
    parser.p = luaL_checkstring(L, 1);
    json_parse_value(L, &parser);
    json_skip_ws(&parser);
    if (*parser.p != '\0') {
        return luaL_error(L, "json: trailing data");
    }
    return 1;
}

static int luaopen_json(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"encode", lua_json_encode},
        {"decode", lua_json_decode},
        {NULL, NULL},
    };
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

static int lua_capability_http_request(lua_State *L)
{
    const char *method = "GET";
    const char *url = "";
    int ok;
    char *text;

    if (lua_istable(L, 2)) {
        method = opt_string_field(L, 2, "method", method);
        url = opt_string_field(L, 2, "url", url);
    }

    ok = EM_ASM_INT({
        const result = Module.__simServices.capability.call("http_request", {
            method: UTF8ToString($0),
            url: UTF8ToString($1),
        });
        Module.__simLastString = result && result.text ? result.text : "";
        return result && result.ok ? 1 : 0;
    }, method, url);
    text = sim_js_take_last_string();
    if (ok) {
        lua_pushboolean(L, 1);
        lua_pushstring(L, text ? text : "");
        lua_pushnil(L);
    } else {
        lua_pushboolean(L, 0);
        lua_pushnil(L);
        lua_pushstring(L, text && text[0] ? text : "http_request mock not found");
    }
    free(text);
    return 3;
}

static int lua_capability_call(lua_State *L)
{
    const char *name = luaL_optstring(L, 1, "unknown");
    if (strcmp(name, "http_request") == 0) {
        return lua_capability_http_request(L);
    }
    lua_pushboolean(L, 0);
    lua_pushnil(L);
    lua_pushfstring(L, "capability '%s' is not available in Web simulator", name);
    return 3;
}

static int luaopen_capability(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_capability_call);
    lua_setfield(L, -2, "call");
    return 1;
}

static int lua_event_publish_trigger(lua_State *L)
{
    (void)L;
    lua_pushboolean(L, 1);
    return 1;
}

static int luaopen_event_publisher(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_event_publish_trigger);
    lua_setfield(L, -2, "publish_trigger");
    return 1;
}

static int lua_image_convert(lua_State *L)
{
    lua_settop(L, 1);
    return 1;
}

static int lua_image_save_file(lua_State *L)
{
    (void)luaL_checkany(L, 1);
    (void)luaL_checkstring(L, 2);
    lua_pushboolean(L, 1);
    return 1;
}

static int luaopen_image(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_image_convert);
    lua_setfield(L, -2, "convert");
    lua_pushcfunction(L, lua_image_save_file);
    lua_setfield(L, -2, "save_file");
    return 1;
}

static int lua_motion_detect_detect(lua_State *L)
{
    (void)L;
    lua_newtable(L);
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "changed");
    lua_pushnumber(L, 0.0);
    lua_setfield(L, -2, "score");
    return 1;
}

static int luaopen_motion_detect(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_motion_detect_detect);
    lua_setfield(L, -2, "detect");
    lua_pushcfunction(L, lua_true_noop);
    lua_setfield(L, -2, "reset");
    return 1;
}

static int lua_handle_write(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_generic_new_handle(lua_State *L)
{
    (void)L;
    lua_newtable(L);
    lua_pushcfunction(L, lua_handle_write);
    lua_setfield(L, -2, "write");
    lua_pushcfunction(L, lua_true_noop);
    lua_setfield(L, -2, "close");
    lua_pushcfunction(L, lua_true_noop);
    lua_setfield(L, -2, "set_pixel");
    lua_pushcfunction(L, lua_true_noop);
    lua_setfield(L, -2, "refresh");
    return 1;
}

static int luaopen_mcpwm(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_generic_new_handle);
    lua_setfield(L, -2, "new");
    return 1;
}

static int luaopen_led_strip(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_generic_new_handle);
    lua_setfield(L, -2, "new");
    return 1;
}

static int lua_env_read(lua_State *L)
{
    (void)L;
    lua_newtable(L);
    lua_pushnumber(L, 25.0);
    lua_setfield(L, -2, "temperature");
    lua_pushnumber(L, 50.0);
    lua_setfield(L, -2, "humidity");
    return 1;
}

static int luaopen_environmental_sensor(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_env_read);
    lua_setfield(L, -2, "read");
    return 1;
}

static int lua_thread_start(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    const char *name = sim_basename(path);
    int ok;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "name");
        if (lua_isstring(L, -1)) {
            name = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }
    ok = EM_ASM_INT({
        const ok = Module.__simServices.thread.start(UTF8ToString($0), {}, { name: UTF8ToString($1) });
        Module.__simLastString = Module.__simServices.thread.lastError || "";
        return ok ? 1 : 0;
    }, path, name);
    lua_pushboolean(L, ok);
    if (!ok) {
        char *err = sim_js_take_last_string();
        lua_pushstring(L, err ? err : "thread start failed");
        free(err);
        return 2;
    }
    return 1;
}

static int lua_thread_get(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    char *output;
    int ok = EM_ASM_INT({
        const output = Module.__simServices.thread.get(UTF8ToString($0));
        Module.__simLastString = output || Module.__simServices.thread.lastError || "";
        return output ? 1 : 0;
    }, name);
    if (!ok) {
        lua_pushboolean(L, 0);
        output = sim_js_take_last_string();
        lua_pushstring(L, output ? output : "not_found");
        free(output);
        return 2;
    }
    output = sim_js_take_last_string();
    lua_pushboolean(L, 1);
    lua_pushstring(L, output ? output : "");
    free(output);
    return 2;
}

static int lua_thread_queue_create(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int depth = 8;
    int ok;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "depth");
        depth = (int)luaL_optinteger(L, -1, depth);
        lua_pop(L, 1);
    }
    ok = EM_ASM_INT({
        const ok = Module.__simServices.thread.queueCreate(UTF8ToString($0), { depth: $1 });
        Module.__simLastString = Module.__simServices.thread.lastError || "";
        return ok ? 1 : 0;
    }, name, depth);
    lua_pushboolean(L, ok);
    if (!ok) {
        char *err = sim_js_take_last_string();
        lua_pushstring(L, err ? err : "queue_create failed");
        free(err);
        return 2;
    }
    return 1;
}

static int lua_thread_queue_send(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const char *text = luaL_checkstring(L, 2);
    int ok = EM_ASM_INT({
        const ok = Module.__simServices.thread.queueSend(UTF8ToString($0), UTF8ToString($1));
        Module.__simLastString = Module.__simServices.thread.lastError || "";
        return ok ? 1 : 0;
    }, name, text);
    lua_pushboolean(L, ok);
    if (!ok) {
        char *err = sim_js_take_last_string();
        lua_pushstring(L, err ? err : "queue_send failed");
        free(err);
        return 2;
    }
    return 1;
}

static int lua_thread_queue_recv(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int timeout_ms = (int)luaL_optinteger(L, 2, 0);
    double deadline = emscripten_get_now() + (double)timeout_ms;
    char *text = NULL;
    while (true) {
        int ok = EM_ASM_INT({
            const text = Module.__simServices.thread.queueRecv(UTF8ToString($0));
            Module.__simLastString = text || Module.__simServices.thread.lastError || "";
            return text == null ? 0 : 1;
        }, name);
        if (ok) {
            text = sim_js_take_last_string();
            lua_pushstring(L, text ? text : "");
            free(text);
            return 1;
        }
        if (sim_stop_requested()) {
            lua_pushnil(L);
            lua_pushstring(L, "stopped");
            return 2;
        }
        if (timeout_ms <= 0 || emscripten_get_now() >= deadline) {
            lua_pushnil(L);
            text = sim_js_take_last_string();
            lua_pushstring(L, text && text[0] ? text : "timeout");
            free(text);
            return 2;
        }
        emscripten_sleep(10);
    }
}

static int lua_thread_lock_create(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int ok = EM_ASM_INT({
        const ok = Module.__simServices.thread.lockCreate(UTF8ToString($0));
        Module.__simLastString = Module.__simServices.thread.lastError || "";
        return ok ? 1 : 0;
    }, name);
    lua_pushboolean(L, ok);
    if (!ok) {
        char *err = sim_js_take_last_string();
        lua_pushstring(L, err ? err : "lock_create failed");
        free(err);
        return 2;
    }
    return 1;
}

static int lua_thread_lock(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int ok = EM_ASM_INT({
        const ok = Module.__simServices.thread.lock(UTF8ToString($0));
        Module.__simLastString = Module.__simServices.thread.lastError || "";
        return ok ? 1 : 0;
    }, name);
    lua_pushboolean(L, ok);
    if (!ok) {
        char *err = sim_js_take_last_string();
        lua_pushstring(L, err ? err : "lock failed");
        free(err);
        return 2;
    }
    return 1;
}

static int lua_thread_unlock(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int ok = EM_ASM_INT({
        const ok = Module.__simServices.thread.unlock(UTF8ToString($0));
        Module.__simLastString = Module.__simServices.thread.lastError || "";
        return ok ? 1 : 0;
    }, name);
    lua_pushboolean(L, ok);
    if (!ok) {
        char *err = sim_js_take_last_string();
        lua_pushstring(L, err ? err : "unlock failed");
        free(err);
        return 2;
    }
    return 1;
}

static int luaopen_thread(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_thread_start);
    lua_setfield(L, -2, "start");
    lua_pushcfunction(L, lua_thread_get);
    lua_setfield(L, -2, "get");
    lua_newtable(L);
    lua_pushcfunction(L, lua_thread_queue_create);
    lua_setfield(L, -2, "queue_create");
    lua_pushcfunction(L, lua_thread_queue_send);
    lua_setfield(L, -2, "queue_send");
    lua_pushcfunction(L, lua_thread_queue_recv);
    lua_setfield(L, -2, "queue_recv");
    lua_pushcfunction(L, lua_thread_lock_create);
    lua_setfield(L, -2, "lock_create");
    lua_pushcfunction(L, lua_thread_lock);
    lua_setfield(L, -2, "lock");
    lua_pushcfunction(L, lua_thread_unlock);
    lua_setfield(L, -2, "unlock");
    lua_setfield(L, -2, "sync");
    return 1;
}

static int lua_arg_schema_int(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushvalue(L, 1);
    return 1;
}

static int lua_arg_schema_bool(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushvalue(L, 1);
    return 1;
}

static int lua_arg_schema_parse(lua_State *L)
{
    int raw_idx;
    int schema_idx;
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    raw_idx = lua_absindex(L, 1);
    schema_idx = lua_absindex(L, 2);
    lua_newtable(L);
    lua_pushnil(L);
    while (lua_next(L, schema_idx) != 0) {
        lua_pushvalue(L, -2);
        lua_gettable(L, raw_idx);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, -1, "default");
        }
        lua_pushvalue(L, -3);
        lua_insert(L, -2);
        lua_settable(L, -5);
        lua_pop(L, 1);
    }
    lua_pushnil(L);
    while (lua_next(L, raw_idx) != 0) {
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_settable(L, -4);
    }
    return 1;
}

static int luaopen_arg_schema(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_arg_schema_int);
    lua_setfield(L, -2, "int");
    lua_pushcfunction(L, lua_arg_schema_bool);
    lua_setfield(L, -2, "bool");
    lua_pushcfunction(L, lua_arg_schema_parse);
    lua_setfield(L, -2, "parse");
    return 1;
}

static int lua_http_app_method(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushvalue(L, 1);
    return 1;
}

static int lua_http_app(lua_State *L)
{
    (void)luaL_optstring(L, 1, "sim_app");
    lua_newtable(L);
    lua_pushcfunction(L, lua_http_app_method);
    lua_setfield(L, -2, "mount_static");
    lua_pushcfunction(L, lua_http_app_method);
    lua_setfield(L, -2, "get");
    lua_pushcfunction(L, lua_http_app_method);
    lua_setfield(L, -2, "post");
    lua_pushcfunction(L, lua_http_app_method);
    lua_setfield(L, -2, "listen");
    lua_pushcfunction(L, lua_http_app_method);
    lua_setfield(L, -2, "close");
    return 1;
}

static int luaopen_http_server(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_http_app);
    lua_setfield(L, -2, "app");
    return 1;
}

static int lua_lcd_touch_sync(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TLIGHTUSERDATA);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lcd_touch_poll(lua_State *L)
{
    bool had_event;
    bool pressed;
    int x;
    int y;

    luaL_checktype(L, 1, LUA_TLIGHTUSERDATA);
    had_event = sim_take_touch_event();
    if (had_event) {
        x = sim_polled_touch_x();
        y = sim_polled_touch_y();
        pressed = sim_polled_touch_pressed();
    } else {
        x = EM_ASM_INT({ return Module.__touchState ? (Module.__touchState.x | 0) : 0; });
        y = EM_ASM_INT({ return Module.__touchState ? (Module.__touchState.y | 0) : 0; });
        pressed = EM_ASM_INT({ return Module.__touchState && Module.__touchState.pressed ? 1 : 0; }) != 0;
    }

    lua_newtable(L);
    lua_pushboolean(L, pressed);
    lua_setfield(L, -2, "pressed");
    lua_pushboolean(L, had_event && pressed);
    lua_setfield(L, -2, "just_pressed");
    lua_pushboolean(L, had_event && !pressed);
    lua_setfield(L, -2, "just_released");
    lua_pushinteger(L, x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, y);
    lua_setfield(L, -2, "y");
    return 1;
}

static int luaopen_lcd_touch(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"sync", lua_lcd_touch_sync},
        {"poll", lua_lcd_touch_poll},
        {NULL, NULL},
    };
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

static int lua_display_init(lua_State *L)
{
    if (s_display_owner[0] == '\0') {
        snprintf(s_display_owner, sizeof(s_display_owner), "%s", "display");
    } else if (strcmp(s_display_owner, "lvgl") == 0 && s_lvgl_init_claim) {
        s_lvgl_init_claim = false;
    } else if (strcmp(s_display_owner, "display") != 0) {
        return luaL_error(L, "display already owned by %s", s_display_owner);
    }
    luaL_checktype(L, 1, LUA_TLIGHTUSERDATA);
    luaL_checkany(L, 2);
    (void)luaL_checkinteger(L, 3);
    (void)luaL_checkinteger(L, 4);
    s_display_initialized = true;
    s_frame_active = false;
    sim_color_t black = {0, 0, 0, 255};
    sim_canvas_clear(black);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_display_deinit(lua_State *L)
{
    s_display_initialized = false;
    s_frame_active = false;
    if (strcmp(s_display_owner, "display") == 0) {
        s_display_owner[0] = '\0';
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_display_claim_owner(lua_State *L)
{
    const char *owner = luaL_checkstring(L, 1);
    int rc = claim_display_owner(L, owner);
    if (rc != 0) {
        return rc;
    }
    if (strcmp(owner, "lvgl") == 0) {
        s_lvgl_init_claim = true;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_display_release_owner(lua_State *L)
{
    const char *owner = luaL_checkstring(L, 1);
    if (strcmp(s_display_owner, owner) == 0) {
        s_display_owner[0] = '\0';
        s_lvgl_init_claim = false;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_display_clear(lua_State *L)
{
    require_display(L);
    sim_color_t color;
    parse_color(L, 1, &color);
    sim_canvas_clear(color);
    return 0;
}

static int lua_display_begin_frame(lua_State *L)
{
    require_display(L);
    s_frame_active = true;
    bool clear = true;
    sim_color_t color = {0, 0, 0, 255};
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "clear");
        if (!lua_isnil(L, -1)) {
            clear = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
        color = opt_color_field(L, 1, "color", color);
    }
    if (clear) {
        sim_canvas_clear(color);
    }
    return 0;
}

static int lua_display_present(lua_State *L)
{
    require_display(L);
    return 0;
}

static int lua_display_end_frame(lua_State *L)
{
    require_display(L);
    s_frame_active = false;
    return 0;
}

static int lua_display_frame_active(lua_State *L)
{
    lua_pushboolean(L, s_frame_active);
    return 1;
}

static int lua_display_animation_info(lua_State *L)
{
    lua_newtable(L);
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "framebuffer_count");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "double_buffered");
    lua_pushboolean(L, s_frame_active);
    lua_setfield(L, -2, "frame_active");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "flush_in_flight");
    return 1;
}

static int lua_display_fill_rect(lua_State *L)
{
    require_display(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    sim_color_t color;
    parse_color(L, 5, &color);
    sim_canvas_fill_rect(x, y, w, h, color);
    return 0;
}

static int lua_display_draw_rect(lua_State *L)
{
    require_display(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    sim_color_t color;
    parse_color(L, 5, &color);
    sim_canvas_stroke_rect(x, y, w, h, color);
    return 0;
}

static int lua_display_draw_pixel(lua_State *L)
{
    require_display(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    sim_color_t color;
    parse_color(L, 3, &color);
    sim_canvas_fill_rect(x, y, 1, 1, color);
    return 0;
}

static int lua_display_draw_line(lua_State *L)
{
    require_display(L);
    int x0 = (int)luaL_checkinteger(L, 1);
    int y0 = (int)luaL_checkinteger(L, 2);
    int x1 = (int)luaL_checkinteger(L, 3);
    int y1 = (int)luaL_checkinteger(L, 4);
    sim_color_t color;
    parse_color(L, 5, &color);
    sim_canvas_line(x0, y0, x1, y1, color);
    return 0;
}

static int lua_display_circle(lua_State *L, bool fill)
{
    require_display(L);
    int cx = (int)luaL_checkinteger(L, 1);
    int cy = (int)luaL_checkinteger(L, 2);
    int r = (int)luaL_checkinteger(L, 3);
    sim_color_t color;
    parse_color(L, 4, &color);
    sim_canvas_circle(cx, cy, r, fill, color);
    return 0;
}

static int lua_display_fill_circle(lua_State *L) { return lua_display_circle(L, true); }
static int lua_display_draw_circle(lua_State *L) { return lua_display_circle(L, false); }

static int lua_display_ellipse(lua_State *L, bool fill)
{
    require_display(L);
    int cx = (int)luaL_checkinteger(L, 1);
    int cy = (int)luaL_checkinteger(L, 2);
    int rx = (int)luaL_checkinteger(L, 3);
    int ry = (int)luaL_checkinteger(L, 4);
    sim_color_t color;
    parse_color(L, 5, &color);
    sim_canvas_ellipse(cx, cy, rx, ry, fill, color);
    return 0;
}

static int lua_display_fill_ellipse(lua_State *L) { return lua_display_ellipse(L, true); }
static int lua_display_draw_ellipse(lua_State *L) { return lua_display_ellipse(L, false); }

static int lua_display_draw_arc(lua_State *L)
{
    require_display(L);
    int cx = (int)luaL_checkinteger(L, 1);
    int cy = (int)luaL_checkinteger(L, 2);
    int r = (int)luaL_checkinteger(L, 3);
    double start = luaL_checknumber(L, 4);
    double end = luaL_checknumber(L, 5);
    sim_color_t color;
    parse_color(L, 6, &color);
    sim_canvas_arc(cx, cy, r, start, end, false, 0, color);
    return 0;
}

static int lua_display_fill_arc(lua_State *L)
{
    require_display(L);
    int cx = (int)luaL_checkinteger(L, 1);
    int cy = (int)luaL_checkinteger(L, 2);
    int inner = (int)luaL_checkinteger(L, 3);
    int outer = (int)luaL_checkinteger(L, 4);
    double start = luaL_checknumber(L, 5);
    double end = luaL_checknumber(L, 6);
    sim_color_t color;
    parse_color(L, 7, &color);
    sim_canvas_arc(cx, cy, outer, start, end, true, inner, color);
    return 0;
}

static int lua_display_round_rect(lua_State *L, bool fill)
{
    require_display(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    int r = (int)luaL_checkinteger(L, 5);
    sim_color_t color;
    parse_color(L, 6, &color);
    sim_canvas_round_rect(x, y, w, h, r, fill, color);
    return 0;
}

static int lua_display_fill_round_rect(lua_State *L) { return lua_display_round_rect(L, true); }
static int lua_display_draw_round_rect(lua_State *L) { return lua_display_round_rect(L, false); }

static int lua_display_triangle(lua_State *L, bool fill)
{
    require_display(L);
    int x1 = (int)luaL_checkinteger(L, 1);
    int y1 = (int)luaL_checkinteger(L, 2);
    int x2 = (int)luaL_checkinteger(L, 3);
    int y2 = (int)luaL_checkinteger(L, 4);
    int x3 = (int)luaL_checkinteger(L, 5);
    int y3 = (int)luaL_checkinteger(L, 6);
    sim_color_t color;
    parse_color(L, 7, &color);
    sim_canvas_triangle(x1, y1, x2, y2, x3, y3, fill, color);
    return 0;
}

static int lua_display_fill_triangle(lua_State *L) { return lua_display_triangle(L, true); }
static int lua_display_draw_triangle(lua_State *L) { return lua_display_triangle(L, false); }

static int lua_display_measure_text(lua_State *L)
{
    const char *text = luaL_checkstring(L, 1);
    int font_size = opt_int_field(L, 2, "font_size", 24);
    lua_pushinteger(L, sim_text_width(text, font_size));
    lua_pushinteger(L, font_size);
    return 2;
}

static int lua_display_draw_text(lua_State *L)
{
    require_display(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    const char *text = luaL_checkstring(L, 3);
    sim_color_t fallback = {255, 255, 255, 255};
    int font_size = opt_int_field(L, 4, "font_size", 24);
    sim_color_t color = opt_color_field(L, 4, "color", fallback);
    sim_canvas_text(x, y, text, font_size, color);
    return 0;
}

static int lua_display_draw_text_aligned(lua_State *L)
{
    require_display(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    const char *text = luaL_checkstring(L, 5);
    sim_color_t fallback = {255, 255, 255, 255};
    int font_size = opt_int_field(L, 6, "font_size", 24);
    sim_color_t color = opt_color_field(L, 6, "color", fallback);
    const char *align = opt_string_field(L, 6, "align", "left");
    const char *valign = opt_string_field(L, 6, "valign", "top");
    int tw = sim_text_width(text, font_size);
    int tx = x;
    int ty = y;
    if (strcmp(align, "center") == 0 || strcmp(align, "centre") == 0) {
        tx = x + (w - tw) / 2;
    } else if (strcmp(align, "right") == 0) {
        tx = x + w - tw;
    }
    if (strcmp(valign, "middle") == 0 || strcmp(valign, "center") == 0) {
        ty = y + (h - font_size) / 2;
    } else if (strcmp(valign, "bottom") == 0) {
        ty = y + h - font_size;
    }
    sim_canvas_text(tx, ty, text, font_size, color);
    return 0;
}

static int lua_display_draw_pixels(lua_State *L)
{
    require_display(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 3, &len);
    luaL_checktype(L, 4, LUA_TTABLE);
    int opts_idx = lua_absindex(L, 4);
    int full_w = 0;
    int full_h = 0;
    int dst_w;
    int dst_h;
    int src_x = 0;
    int src_y = 0;
    const char *mode;

    get_int_field(L, opts_idx, "width", &full_w);
    get_int_field(L, opts_idx, "height", &full_h);
    if (full_w <= 0 || full_h <= 0) {
        return luaL_error(L, "display draw_pixels invalid image size");
    }
    size_t expected = (size_t)full_w * (size_t)full_h * 2u;
    if (len < expected) {
        return luaL_error(L, "draw_pixels: data too short (%d bytes, need %d)", (int)len, (int)expected);
    }

    int src_w = full_w;
    int src_h = full_h;
    parse_source_rect(L, opts_idx, &src_x, &src_y, &src_w, &src_h);
    if (src_x < 0 || src_y < 0 || src_w <= 0 || src_h <= 0 || src_x + src_w > full_w || src_y + src_h > full_h) {
        return luaL_error(L, "display draw_pixels source rectangle out of bounds");
    }

    dst_w = src_w;
    dst_h = src_h;
    get_int_field(L, opts_idx, "dst_width", &dst_w);
    get_int_field(L, opts_idx, "dst_height", &dst_h);
    get_int_field(L, opts_idx, "max_width", &dst_w);
    get_int_field(L, opts_idx, "max_height", &dst_h);
    mode = opt_string_field(L, opts_idx, "mode", "raw");

    if (strcmp(mode, "fit") == 0) {
        double scale_w = (double)dst_w / (double)src_w;
        double scale_h = (double)dst_h / (double)src_h;
        double scale = scale_w < scale_h ? scale_w : scale_h;
        if (scale <= 0.0) {
            return luaL_error(L, "display draw_pixels invalid image size");
        }
        dst_w = (int)(src_w * scale);
        dst_h = (int)(src_h * scale);
        if (dst_w < 1) { dst_w = 1; }
        if (dst_h < 1) { dst_h = 1; }
    } else if (strcmp(mode, "cover") == 0) {
        int64_t lhs = (int64_t)src_w * dst_h;
        int64_t rhs = (int64_t)dst_w * src_h;
        if (lhs > rhs) {
            int new_w = (int)((int64_t)src_h * dst_w / dst_h);
            src_x += (src_w - new_w) / 2;
            src_w = new_w;
        } else if (lhs < rhs) {
            int new_h = (int)((int64_t)src_w * dst_h / dst_w);
            src_y += (src_h - new_h) / 2;
            src_h = new_h;
        }
    } else if (strcmp(mode, "stretch") != 0 && strcmp(mode, "crop") != 0 && strcmp(mode, "raw") != 0) {
        return luaL_error(L, "display draw_pixels invalid mode");
    }

    if (strcmp(mode, "raw") == 0 || strcmp(mode, "crop") == 0) {
        dst_w = src_w;
        dst_h = src_h;
    }
    sim_canvas_rgb565(x, y, (const uint8_t *)data, full_w, src_x, src_y, src_w, src_h, dst_w, dst_h);
    lua_pushinteger(L, dst_w);
    lua_pushinteger(L, dst_h);
    return 2;
}

static int lua_display_draw_image(lua_State *L)
{
    int x;
    int y;
    int full_w = 0;
    int full_h = 0;
    int dst_w;
    int dst_h;
    int src_x = 0;
    int src_y = 0;
    int src_w;
    int src_h;
    size_t len = 0;
    const char *data = NULL;
    const char *pixel_format = NULL;
    const char *mode = "raw";

    require_display(L);
    x = (int)luaL_checkinteger(L, 1);
    y = (int)luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);

    lua_getfield(L, 3, "width");
    full_w = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    lua_getfield(L, 3, "height");
    full_h = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    if (full_w <= 0 || full_h <= 0) {
        return luaL_error(L, "display draw_image invalid image size");
    }

    lua_getfield(L, 3, "pixel_format");
    pixel_format = luaL_optstring(L, -1, "RGBP");
    if (strcmp(pixel_format, "RGBP") != 0 && strcmp(pixel_format, "RGB565") != 0) {
        lua_pop(L, 1);
        return luaL_error(L, "display draw_image unsupported pixel format: %s", pixel_format);
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "data");
    data = luaL_checklstring(L, -1, &len);
    size_t expected = (size_t)full_w * (size_t)full_h * 2u;
    if (len < expected) {
        lua_pop(L, 1);
        return luaL_error(L, "display draw_image data too short (%d bytes, need %d)", (int)len, (int)expected);
    }

    src_w = full_w;
    src_h = full_h;
    dst_w = src_w;
    dst_h = src_h;
    if (lua_istable(L, 4)) {
        int opts_idx = lua_absindex(L, 4);
        parse_source_rect(L, opts_idx, &src_x, &src_y, &src_w, &src_h);
        lua_getfield(L, 4, "width");
        dst_w = (int)luaL_optinteger(L, -1, dst_w);
        lua_pop(L, 1);
        lua_getfield(L, 4, "height");
        dst_h = (int)luaL_optinteger(L, -1, dst_h);
        lua_pop(L, 1);
        get_int_field(L, opts_idx, "dst_width", &dst_w);
        get_int_field(L, opts_idx, "dst_height", &dst_h);
        get_int_field(L, opts_idx, "max_width", &dst_w);
        get_int_field(L, opts_idx, "max_height", &dst_h);
        mode = opt_string_field(L, opts_idx, "mode", "raw");
    }

    if (src_x < 0 || src_y < 0 || src_w <= 0 || src_h <= 0 || src_x + src_w > full_w || src_y + src_h > full_h) {
        lua_pop(L, 1);
        return luaL_error(L, "display draw_image source rectangle out of bounds");
    }
    if ((strcmp(mode, "fit") == 0 || strcmp(mode, "cover") == 0 || strcmp(mode, "stretch") == 0) &&
        (dst_w <= 0 || dst_h <= 0)) {
        lua_pop(L, 1);
        return luaL_error(L, "display draw_image invalid destination size");
    }

    if (strcmp(mode, "fit") == 0) {
        double scale_w = (double)dst_w / (double)src_w;
        double scale_h = (double)dst_h / (double)src_h;
        double scale = scale_w < scale_h ? scale_w : scale_h;
        dst_w = (int)(src_w * scale);
        dst_h = (int)(src_h * scale);
        if (dst_w < 1) { dst_w = 1; }
        if (dst_h < 1) { dst_h = 1; }
    } else if (strcmp(mode, "cover") == 0) {
        int64_t lhs = (int64_t)src_w * dst_h;
        int64_t rhs = (int64_t)dst_w * src_h;
        if (lhs > rhs) {
            int new_w = (int)((int64_t)src_h * dst_w / dst_h);
            src_x += (src_w - new_w) / 2;
            src_w = new_w;
        } else if (lhs < rhs) {
            int new_h = (int)((int64_t)src_w * dst_h / dst_w);
            src_y += (src_h - new_h) / 2;
            src_h = new_h;
        }
    } else if (strcmp(mode, "stretch") != 0 && strcmp(mode, "crop") != 0 && strcmp(mode, "raw") != 0) {
        lua_pop(L, 1);
        return luaL_error(L, "display draw_image invalid mode");
    }

    if (strcmp(mode, "raw") == 0 || strcmp(mode, "crop") == 0) {
        dst_w = src_w;
        dst_h = src_h;
    }

    sim_canvas_rgb565(x, y, (const uint8_t *)data, full_w, src_x, src_y, src_w, src_h, dst_w, dst_h);
    lua_pop(L, 1);
    lua_pushinteger(L, dst_w);
    lua_pushinteger(L, dst_h);
    return 2;
}

static int lua_display_backlight(lua_State *L)
{
    (void)lua_toboolean(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_display_set_clip_rect(lua_State *L)
{
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    require_display(L);
    sim_canvas_push_clip(x, y, w, h);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_display_clear_clip_rect(lua_State *L)
{
    require_display(L);
    sim_canvas_pop_clip();
    lua_pushboolean(L, 1);
    return 1;
}

static int luaopen_display(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"init", lua_display_init},
        {"deinit", lua_display_deinit},
        {"clear", lua_display_clear},
        {"begin_frame", lua_display_begin_frame},
        {"present", lua_display_present},
        {"present_full", lua_display_present},
        {"end_frame", lua_display_end_frame},
        {"frame_active", lua_display_frame_active},
        {"animation_info", lua_display_animation_info},
        {"fill_rect", lua_display_fill_rect},
        {"draw_rect", lua_display_draw_rect},
        {"draw_pixel", lua_display_draw_pixel},
        {"draw_line", lua_display_draw_line},
        {"fill_circle", lua_display_fill_circle},
        {"draw_circle", lua_display_draw_circle},
        {"draw_arc", lua_display_draw_arc},
        {"fill_arc", lua_display_fill_arc},
        {"fill_ellipse", lua_display_fill_ellipse},
        {"draw_ellipse", lua_display_draw_ellipse},
        {"fill_round_rect", lua_display_fill_round_rect},
        {"draw_round_rect", lua_display_draw_round_rect},
        {"fill_triangle", lua_display_fill_triangle},
        {"draw_triangle", lua_display_draw_triangle},
        {"measure_text", lua_display_measure_text},
        {"draw_text", lua_display_draw_text},
        {"draw_text_aligned", lua_display_draw_text_aligned},
        {"draw_pixels", lua_display_draw_pixels},
        {"draw_image", lua_display_draw_image},
        {"backlight", lua_display_backlight},
        {"set_clip_rect", lua_display_set_clip_rect},
        {"clear_clip_rect", lua_display_clear_clip_rect},
        {"_claim_owner", lua_display_claim_owner},
        {"_release_owner", lua_display_release_owner},
        {NULL, NULL},
    };
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    lua_pushinteger(L, sim_canvas_width());
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, sim_canvas_height());
    lua_setfield(L, -2, "height");
    return 1;
}

static int sim_lvgl_init(lua_State *L)
{
    int nargs = lua_gettop(L);
    int height = (int)luaL_checkinteger(L, 4);
    int min_buffer_lines = height;

    if (nargs < 6 || lua_isnil(L, 6)) {
        lua_settop(L, 5);
        lua_newtable(L);
        lua_pushinteger(L, min_buffer_lines);
        lua_setfield(L, 6, "buffer_lines");
        nargs = 6;
    } else if (lua_istable(L, 6)) {
        int buffer_lines;
        lua_getfield(L, 6, "buffer_lines");
        buffer_lines = lua_isnil(L, -1) ? 0 : (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
        if (buffer_lines < min_buffer_lines) {
            lua_pushinteger(L, min_buffer_lines);
            lua_setfield(L, 6, "buffer_lines");
        }
    }

    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, nargs, LUA_MULTRET);
    return lua_gettop(L);
}

static void sim_patch_lvgl_module(lua_State *L)
{
    lua_getfield(L, -1, "init");
    if (lua_isfunction(L, -1)) {
        lua_pushcclosure(L, sim_lvgl_init, 1);
        lua_setfield(L, -2, "init");
    } else {
        lua_pop(L, 1);
    }
}

static void sim_register_modules(lua_State *L)
{
    luaL_requiref(L, "board_manager", luaopen_board_manager, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "delay", luaopen_delay, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "arg_schema", luaopen_arg_schema, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "audio", luaopen_audio, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "button", luaopen_button, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "camera", luaopen_camera, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "capability", luaopen_capability, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "environmental_sensor", luaopen_environmental_sensor, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "event_publisher", luaopen_event_publisher, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "i2c", luaopen_i2c, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "http_server", luaopen_http_server, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "image", luaopen_image, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "imu", luaopen_imu, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "json", luaopen_json, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "led_strip", luaopen_led_strip, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "mcpwm", luaopen_mcpwm, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "motion_detect", luaopen_motion_detect, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "storage", luaopen_storage, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "thread", luaopen_thread, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "touch", luaopen_touch, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "system", luaopen_system, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "lcd_touch", luaopen_lcd_touch, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "display", luaopen_display, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "lvgl", luaopen_lvgl, 1);
    sim_patch_lvgl_module(L);
    lua_pop(L, 1);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

EMSCRIPTEN_KEEPALIVE
int esp_claw_sim_run_file(const char *path)
{
    char *script = read_file(path);
    int rc;
    sim_set_stop_requested(false);
    sim_ensure_runtime_dirs();
    if (!script) {
        printf("failed to read Lua script: %s\n", path);
        return 1;
    }
    if (s_lua) {
        (void)lua_lvgl_deinit_runtime();
        lua_close(s_lua);
    }
    s_display_initialized = false;
    s_frame_active = false;
    s_display_owner[0] = '\0';
    s_lvgl_init_claim = false;
    s_lua = luaL_newstate();
    luaL_openlibs(s_lua);
    lua_sethook(s_lua, sim_lua_stop_hook, LUA_MASKCOUNT, 1000);
    sim_register_modules(s_lua);
    sim_extend_package_path(s_lua, path);
    rc = luaL_loadbuffer(s_lua, script, strlen(script), path);
    free(script);
    if (rc == LUA_OK) {
        rc = lua_pcall(s_lua, 0, 0, 0);
    }
    if (rc != LUA_OK) {
        printf("Lua error: %s\n", lua_tostring(s_lua, -1));
        lua_pop(s_lua, 1);
        return 1;
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE
void esp_claw_sim_stop(void)
{
    sim_set_stop_requested(true);
}

int main(void)
{
    sim_color_t black = {0, 0, 0, 255};
    sim_canvas_clear(black);
    return 0;
}
