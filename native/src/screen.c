/*
 * Copyright 2026 Secragon Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "privacy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>
#include <wctype.h>

static uint32_t mask_token(Screen *s) {
    /* Decorative randomness is independent of the concealed text. A token
     * follows each cell through redraws/scrolls so the mask does not flicker. */
    uint64_t x = s->mask_random;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    s->mask_random = x;
    uint32_t token = (uint32_t)(x >> 32);
    return token ? token : 1;
}
static Cell blank(const Screen *s) {
    Cell c = {.v = s->pen};
    memset(c.v.chars, 0, sizeof(c.v.chars));
    c.v.width = 1;
    return c;
}
static Cell *at(Screen *s, int row, int col) {
    if (row < 0 || row >= s->rows || col < 0 || col >= s->cols)
        return NULL;
    return &s->grids[s->alt][row * s->cols + col];
}
static void history_clear(Screen *s) {
    for (int i = 0; i < s->nhistory; i++) {
        explicit_bzero(s->history[i].cells, (size_t)s->history[i].cols * sizeof(Cell));
        free(s->history[i].cells);
    }
    s->nhistory = 0;
}
static void history_push(Screen *s, int row) {
    if (s->nhistory == CONTEXT_ROWS) {
        explicit_bzero(s->history[0].cells, (size_t)s->history[0].cols * sizeof(Cell));
        free(s->history[0].cells);
        memmove(s->history, s->history + 1, (CONTEXT_ROWS - 1) * sizeof(Line));
        s->nhistory--;
    }
    Line *l = &s->history[s->nhistory++];
    *l = (Line){.cols = s->cols, .continuation = s->continuations[0][row]};
    l->cells = xcalloc((size_t)s->cols, sizeof(Cell));
    memcpy(l->cells, s->grids[0] + row * s->cols, (size_t)s->cols * sizeof(Cell));
}
static int glyph(VTermGlyphInfo *info, VTermPos pos, void *user) {
    Screen *s = user;
    Cell *c = at(s, pos.row, pos.col);
    if (!c)
        return 0;
    bool same = c->v.chars[0] == info->chars[0];
    bool retain = c->hidden && same;
    bool manual = c->manual_hidden && same;
    uint32_t token = same && c->mask_token ? c->mask_token : mask_token(s);
    *c = blank(s);
    c->mask_token = token;
    c->hidden = retain;
    c->manual_hidden = manual;
    c->input_id = s->input_area ? s->input_id : 0;
    c->command_cell = s->input_area;
    c->v.width = (char)info->width;
    c->protected_cell = info->protected_cell;
    for (int k = 0; k < VTERM_MAX_CHARS_PER_CELL && info->chars[k]; k++)
        c->v.chars[k] = info->chars[k];
    for (int k = 1; k < info->width; k++) {
        Cell *tail = at(s, pos.row, pos.col + k);
        if (tail) {
            *tail = blank(s);
            tail->v.width = 0;
            tail->command_cell = s->input_area;
            tail->input_id = s->input_area ? s->input_id : 0;
            tail->mask_token = token ^ ((uint32_t)k * 0x9e3779b9u);
            if (!tail->mask_token)
                tail->mask_token = 1;
        }
    }
    const VTermLineInfo *li = vterm_state_get_lineinfo(s->state, pos.row);
    s->continuations[s->alt][pos.row] = li->continuation;
    return 1;
}
static int cursor(VTermPos pos, VTermPos old, int visible, void *user) {
    (void)old;
    Screen *s = user;
    s->cursor = pos;
    s->cursor_visible = visible;
    return 1;
}
static int erase(VTermRect rect, int selective, void *user) {
    Screen *s = user;
    for (int y = MAX(0, rect.start_row); y < MIN(s->rows, rect.end_row); y++) {
        for (int x = MAX(0, rect.start_col); x < MIN(s->cols, rect.end_col); x++) {
            Cell *c = at(s, y, x);
            if (!selective || !c->protected_cell)
                *c = blank(s);
        }
        if (rect.start_col == 0 && rect.end_col == s->cols)
            s->continuations[s->alt][y] = false;
        if (rect.end_col == s->cols && y + 1 < s->rows)
            s->continuations[s->alt][y + 1] = false;
    }
    return 1;
}
static int scrollrect(VTermRect r, int down, int right, void *user) {
    Screen *s = user;
    if (!s->alt && down > 0 && !right && !r.start_row && !r.start_col && r.end_row == s->rows &&
        r.end_col == s->cols)
        for (int i = 0; i < MIN(down, s->rows); i++)
            history_push(s, i);
    int height = r.end_row - r.start_row, width = r.end_col - r.start_col;
    Cell *copy = xcalloc((size_t)height * (size_t)width, sizeof(Cell));
    bool *lines = xcalloc((size_t)height, sizeof(bool));
    for (int y = 0; y < height; y++) {
        lines[y] = s->continuations[s->alt][y + r.start_row];
        for (int x = 0; x < width; x++)
            copy[y * width + x] = *at(s, y + r.start_row, x + r.start_col);
    }
    for (int y = 0; y < height; y++) {
        int sy = y + down;
        if (!r.start_col && r.end_col == s->cols && !right)
            s->continuations[s->alt][y + r.start_row] = sy >= 0 && sy < height ? lines[sy] : false;
        for (int x = 0; x < width; x++) {
            int sx = x + right;
            *at(s, y + r.start_row, x + r.start_col) =
                sy >= 0 && sy < height && sx >= 0 && sx < width ? copy[sy * width + sx] : blank(s);
        }
    }
    explicit_bzero(copy, (size_t)height * (size_t)width * sizeof(Cell));
    free(copy);
    free(lines);
    return 1;
}
static int initpen(void *user) {
    Screen *s = user;
    s->pen = (VTermScreenCell){.width = 1};
    vterm_state_get_default_colors(s->state, &s->pen.fg, &s->pen.bg);
    return 1;
}
static int penattr(VTermAttr attr, VTermValue *val, void *user) {
    Screen *s = user;
    switch (attr) {
    case VTERM_ATTR_BOLD:
        s->pen.attrs.bold = !!val->boolean;
        break;
    case VTERM_ATTR_UNDERLINE:
        s->pen.attrs.underline = (unsigned)val->number;
        break;
    case VTERM_ATTR_ITALIC:
        s->pen.attrs.italic = !!val->boolean;
        break;
    case VTERM_ATTR_BLINK:
        s->pen.attrs.blink = !!val->boolean;
        break;
    case VTERM_ATTR_REVERSE:
        s->pen.attrs.reverse = !!val->boolean;
        break;
    case VTERM_ATTR_CONCEAL:
        s->pen.attrs.conceal = !!val->boolean;
        break;
    case VTERM_ATTR_STRIKE:
        s->pen.attrs.strike = !!val->boolean;
        break;
    case VTERM_ATTR_FOREGROUND:
        s->pen.fg = val->color;
        break;
    case VTERM_ATTR_BACKGROUND:
        s->pen.bg = val->color;
        break;
    default:
        break;
    }
    return 1;
}
static int property(VTermProp prop, VTermValue *val, void *user) {
    Screen *s = user;
    if (prop == VTERM_PROP_ALTSCREEN) {
        s->alt = !!val->boolean;
        s->invalid = true;
    }
    if (prop == VTERM_PROP_CURSORVISIBLE)
        s->cursor_visible = !!val->boolean;
    /* Titles, clipboard data and other child metadata never reach the host. */
    return 1;
}
static int resize(int rows, int cols, VTermStateFields *fields, void *user) {
    Screen *s = user;
    int shift = MAX(0, fields->pos.row - rows + 1);
    if (!s->alt)
        for (int i = 0; i < shift; i++)
            history_push(s, i);
    for (int b = 0; b < 2; b++) {
        Cell *grid = xcalloc((size_t)rows * (size_t)cols, sizeof(Cell));
        bool *continuations = xcalloc((size_t)rows, sizeof(bool));
        VTermLineInfo *li = xcalloc((size_t)rows, sizeof(VTermLineInfo));
        for (int y = 0; y < rows; y++) {
            int old = y + shift;
            if (old < s->rows) {
                continuations[y] = s->continuations[b][old];
                if (fields->lineinfos[b])
                    li[y] = fields->lineinfos[b][old];
            }
            for (int x = 0; x < cols; x++)
                grid[y * cols + x] =
                    old < s->rows && x < s->cols ? s->grids[b][old * s->cols + x] : blank(s);
        }
        explicit_bzero(s->grids[b], (size_t)s->rows * (size_t)s->cols * sizeof(Cell));
        free(s->grids[b]);
        free(s->continuations[b]);
        free(fields->lineinfos[b]);
        s->grids[b] = grid;
        s->continuations[b] = continuations;
        fields->lineinfos[b] = li;
    }
    fields->pos.row -= shift;
    s->rows = rows;
    s->cols = cols;
    free(s->last_frame);
    s->last_frame = NULL;
    s->invalid = true;
    return 1;
}
static int sb_clear(void *user) {
    history_clear(user);
    return 1;
}
static void response(const char *bytes, size_t len, void *user) {
    Screen *s = user;
    if (s->responses.len + len <= 65536)
        buf_add(&s->responses, bytes, len);
}
static const VTermStateCallbacks callbacks = {.putglyph = glyph,
                                              .movecursor = cursor,
                                              .scrollrect = scrollrect,
                                              .erase = erase,
                                              .initpen = initpen,
                                              .setpenattr = penattr,
                                              .settermprop = property,
                                              .resize = resize,
                                              .sb_clear = sb_clear};
void screen_init(Screen *s, int rows, int cols) {
    *s = (Screen){.rows = rows, .cols = cols, .cursor_visible = true, .invalid = true};
    if (getrandom(&s->mask_random, sizeof(s->mask_random), 0) != sizeof(s->mask_random) ||
        !s->mask_random)
        s->mask_random =
            ((uint64_t)(monotime() * 1e9) ^ ((uint64_t)getpid() << 32) ^ (uintptr_t)s) | 1;

    s->vt = vterm_new(rows, cols);
    if (!s->vt)
        exit(1);
    vterm_set_utf8(s->vt, 1);
    s->state = vterm_obtain_state(s->vt);
    if (!s->state)
        exit(1);
    for (int b = 0; b < 2; b++) {
        s->grids[b] = xcalloc((size_t)rows * (size_t)cols, sizeof(Cell));
        s->continuations[b] = xcalloc((size_t)rows, sizeof(bool));
    }
    vterm_state_set_callbacks(s->state, &callbacks, s);
    vterm_output_set_callback(s->vt, response, s);
    initpen(s);
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < rows * cols; i++)
            s->grids[b][i] = blank(s);
    vterm_state_reset(s->state, 1);
}
void screen_free(Screen *s) {
    history_clear(s);
    manual_free(&s->manual);
    vterm_free(s->vt);
    for (int b = 0; b < 2; b++) {
        explicit_bzero(s->grids[b], (size_t)s->rows * (size_t)s->cols * sizeof(Cell));
        free(s->grids[b]);
        free(s->continuations[b]);
    }
    free(s->last_frame);
    buf_free(&s->responses);
}
static void finish_string(Screen *s, Buffer *out) {
    static const char prefix[] = "777;incognito;";
    size_t n = sizeof(prefix) - 1;
    bool phase = s->filter_len == n + 1 && strchr("ABCDN", s->filter_string[n]);
    bool arm =
        s->filter_len > n + 2 && s->filter_string[n] == 'M' && s->filter_string[n + 1] == ';';
    if (s->filter_osc && s->filter_len >= n + 1 && !memcmp(s->filter_string, prefix, n) &&
        (phase || arm)) {
        if (out->len) {
            vterm_input_write(s->vt, out->data, out->len);
            buf_consume(out, out->len);
        }
        if (arm) {
            size_t offset = 0;
            bool valid = true;
            for (size_t i = n + 2; i < s->filter_len; i++) {
                char c = s->filter_string[i];
                if (c < '0' || c > '9' || offset > 10000000) {
                    valid = false;
                    break;
                }
                offset = offset * 10 + (size_t)(c - '0');
            }
            if (valid && s->input_area && !s->alt)
                manual_arm_at(s, offset);
        } else if (s->filter_string[n] == 'N') {
            if (s->input_area && !s->alt)
                manual_update(s, true);
        } else {
            if (s->filter_string[n] == 'C' || s->filter_string[n] == 'D')
                manual_update(s, false);
            if (s->filter_string[n] == 'D') {
                manual_finish(s);
                s->input_id++;
            }
            s->input_area = s->filter_string[n] == 'B';
            if (s->input_area)
                s->input_base = manual_cursor_offset(s);
        }
    }
    s->filter_state = 0;
    s->filter_len = 0;
}
static void filter_cp(Screen *s, Buffer *out, uint32_t cp) {
    switch (s->filter_state) {
    case 0:
        if (cp == 27)
            s->filter_state = 1;
        else if (cp == 0x90 || cp == 0x98 || cp == 0x9d || cp == 0x9e || cp == 0x9f) {
            s->filter_state = 2;
            s->filter_osc = cp == 0x9d;
            s->filter_len = 0;
        } else
            buf_cp(out, cp);
        break;
    case 1:
        if (cp < 128 && cp && strchr("]P_^X", (int)cp)) {
            s->filter_state = 2;
            s->filter_osc = cp == ']';
            s->filter_len = 0;
        } else {
            buf_cp(out, 27);
            buf_cp(out, cp);
            s->filter_state = 0;
        }
        break;
    case 2:
        if (cp == 7 || cp == 0x9c)
            finish_string(s, out);
        else if (cp == 27)
            s->filter_state = 3;
        else if (s->filter_len < sizeof(s->filter_string))
            s->filter_string[s->filter_len++] = cp < 128 && cp ? (char)cp : '?';
        break;
    case 3:
        if (cp == '\\' || cp == 7 || cp == 0x9c)
            finish_string(s, out);
        else if (cp != 27)
            s->filter_state = 2;
        break;
    }
}
void screen_feed(Screen *s, const char *bytes, size_t len) {
    Buffer filtered = {0};
    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)bytes[i];
        if (s->utf8_left) {
            if ((b & 0xc0) != 0x80) {
                s->utf8_left = 0;
                filter_cp(s, &filtered, 0xfffd);
                i--;
                continue;
            }
            s->utf8_cp = (s->utf8_cp << 6) | (b & 63);
            if (--s->utf8_left == 0) {
                uint32_t c = s->utf8_cp;
                if (c < s->utf8_min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
                    c = 0xfffd;
                filter_cp(s, &filtered, c);
            }
        } else if (b < 0x80)
            filter_cp(s, &filtered, b);
        else if (b >= 0xc2 && b <= 0xdf)
            s->utf8_left = 1, s->utf8_cp = b & 31, s->utf8_min = 0x80;
        else if (b >= 0xe0 && b <= 0xef)
            s->utf8_left = 2, s->utf8_cp = b & 15, s->utf8_min = 0x800;
        else if (b >= 0xf0 && b <= 0xf4)
            s->utf8_left = 3, s->utf8_cp = b & 7, s->utf8_min = 0x10000;
        else
            filter_cp(s, &filtered, 0xfffd);
    }
    if (filtered.len)
        vterm_input_write(s->vt, filtered.data, filtered.len);
    buf_free(&filtered);
}
static size_t collect_text(Screen *s, uint32_t *text, Cell **refs, bool command_only) {
    int nh = s->alt ? 0 : s->nhistory, lines = nh + s->rows;
    size_t n = 0;
    for (int y = 0; y < lines; y++) {
        Cell *cells = y < nh ? s->history[y].cells : s->grids[s->alt] + (y - nh) * s->cols;
        int cols = y < nh ? s->history[y].cols : s->cols, end = 0;
        bool next_cont = false;
        if (y + 1 < lines)
            next_cont =
                y + 1 < nh ? s->history[y + 1].continuation : s->continuations[s->alt][y + 1 - nh];
        for (int x = 0; x < cols; x++)
            if (cells[x].v.chars[0] && !iswspace(cells[x].v.chars[0]))
                end = x + 1;
        if (next_cont)
            end = cols;
        else if (y - nh == s->cursor.row)
            end = MAX(end, MIN(cols, s->cursor.col));
        for (int x = 0; x < end; x++) {
            Cell *c = &cells[x];
            if (!c->v.width || (command_only && !c->command_cell))
                continue;
            if (!c->v.chars[0]) {
                text[n] = ' ';
                refs[n++] = c;
            } else
                for (int k = 0; k < VTERM_MAX_CHARS_PER_CELL && c->v.chars[k]; k++) {
                    text[n] = c->v.chars[k];
                    refs[n++] = c;
                }
        }
        if (!next_cont) {
            text[n++] = '\n';
            refs[n - 1] = NULL;
        }
    }
    return n;
}
void screen_mask(Screen *s, const Settings *settings) {
    int nh = s->alt ? 0 : s->nhistory;
    size_t cap = 0;
    for (int y = 0; y < nh; y++)
        cap += (size_t)s->history[y].cols * VTERM_MAX_CHARS_PER_CELL + 1;
    cap += (size_t)s->rows * ((size_t)s->cols * VTERM_MAX_CHARS_PER_CELL + 1);
    uint32_t *text = xcalloc(cap, sizeof(*text));
    Cell **refs = xcalloc(cap, sizeof(*refs));
    bool *hidden = xcalloc(cap, sizeof(bool));
    size_t n = collect_text(s, text, refs, false);
    if (settings->exclusions_enabled && settings->exception_count) {
        bool *excluded = xcalloc(n ? n : 1, sizeof(bool));
        command_exceptions(text, n, settings, excluded);
        /* Choosing an exception also updates already-visible command cells. */
        for (size_t i = 0; i < n; i++)
            if (excluded[i] && refs[i])
                refs[i]->hidden = false;
        free(excluded);
    }
    if (!settings->triggers_enabled)
        for (size_t i = 0; i < n; i++)
            if (refs[i])
                refs[i]->hidden = false;
    mask_positions(text, n, settings, hidden);
    for (size_t i = 0; i < n; i++)
        if (hidden[i] && refs[i])
            refs[i]->hidden = true;
    /* Prompt hooks identify actual editable command cells. Parse that clean
     * input once more without PS1/PS2 text so a separated value after a shell
     * backslash-newline continuation remains attached to its trigger. */
    memset(text, 0, cap * sizeof(*text));
    memset(refs, 0, cap * sizeof(*refs));
    memset(hidden, 0, cap * sizeof(*hidden));
    n = collect_text(s, text, refs, true);
    mask_positions(text, n, settings, hidden);
    for (size_t i = 0; i < n; i++)
        if (hidden[i] && refs[i])
            refs[i]->hidden = true;
    explicit_bzero(text, cap * sizeof(*text));
    free(text);
    free(refs);
    free(hidden);
}
void screen_resize(Screen *s, int rows, int cols, const Settings *settings) {
    manual_update(s, false);
    manual_cancel_unstarted(s);
    screen_mask(s, settings);
    vterm_set_size(s->vt, rows, cols);
}
static void color(Buffer *out, const VTermColor *c, bool bg) {
    if ((bg && VTERM_COLOR_IS_DEFAULT_BG(c)) || (!bg && VTERM_COLOR_IS_DEFAULT_FG(c)))
        return;
    if (VTERM_COLOR_IS_INDEXED(c)) {
        unsigned index = c->indexed.idx;
        if (index < 8)
            buf_printf(out, "\033[%um", (bg ? 40U : 30U) + index);
        else if (index < 16)
            buf_printf(out, "\033[%um", (bg ? 100U : 90U) + index - 8U);
        else
            buf_printf(out, "\033[%d;5;%um", bg ? 48 : 38, index);
    } else
        buf_printf(out, "\033[%d;2;%u;%u;%um", bg ? 48 : 38, c->rgb.red, c->rgb.green, c->rgb.blue);
}
static void style(Buffer *out, const VTermScreenCell *v) {
    buf_str(out, "\033[0m");
    color(out, &v->fg, false);
    color(out, &v->bg, true);
    if (v->attrs.bold)
        buf_str(out, "\033[1m");
    if (v->attrs.italic)
        buf_str(out, "\033[3m");
    if (v->attrs.underline)
        buf_str(out, "\033[4m");
    if (v->attrs.blink)
        buf_str(out, "\033[5m");
    if (v->attrs.reverse)
        buf_str(out, "\033[7m");
    if (v->attrs.conceal)
        buf_str(out, "\033[8m");
    if (v->attrs.strike)
        buf_str(out, "\033[9m");
}
typedef struct {
    char bytes[16];
    size_t len;
} Probe;
static void probe_output(const char *bytes, size_t len, void *user) {
    Probe *p = user;
    size_t take = MIN(len, sizeof(p->bytes) - p->len);
    memcpy(p->bytes + p->len, bytes, take);
    p->len += take;
}
static void keyboard_modes(Screen *s, const Menu *m, Buffer *out) {
    /* Public keyboard encoders let us mirror the child's input modes without
     * depending on libvterm internals. Capture these probes; never send them. */
    Probe p = {0};
    vterm_output_set_callback(s->vt, probe_output, &p);
    vterm_keyboard_start_paste(s->vt);
    bool paste = p.len > 0;
    p.len = 0;
    vterm_keyboard_key(s->vt, VTERM_KEY_UP, VTERM_MOD_NONE);
    bool application = p.len >= 2 && p.bytes[0] == 27 && p.bytes[1] == 'O';
    vterm_output_set_callback(s->vt, response, s);
    buf_str(out, paste || m->open ? "\033[?2004h" : "\033[?2004l");
    buf_str(out, application && !m->open ? "\033[?1h" : "\033[?1l");
}
static void conceal_frame(Screen *s, const Menu *m, Cell *frame) {
    size_t count = (size_t)s->rows * (size_t)s->cols;
    bool *concealed = xcalloc(count, sizeof(bool));
    for (size_t i = 0; i < count; i++) {
        bool automatic = m->enabled && (m->crazy ? frame[i].command_cell && frame[i].v.chars[0] &&
                                                       !iswspace(frame[i].v.chars[0])
                                                 : frame[i].hidden);
        if (frame[i].manual_hidden || automatic) {
            int width = MAX(1, frame[i].v.width);
            for (int k = 0;
                 k < width && i + (size_t)k < count && i / s->cols == (i + (size_t)k) / s->cols;
                 k++)
                concealed[i + k] = true;
        }
    }
    static const char alphabet[] =
        "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789!@#$%&+=?";
    /* Only yellow, happy faces: no people, animals, symbols or skin-tone modifiers. */
    static const uint32_t faces[] = {0x1f600, 0x1f603, 0x1f604, 0x1f601, 0x1f606, 0x1f605, 0x1f602,
                                     0x1f609, 0x1f60a, 0x1f642, 0x1f60d, 0x1f60e, 0x1f923};
    for (size_t i = 0; i < count; i++) {
        if (!concealed[i])
            continue;
        if (!frame[i].mask_token) {
            s->grids[s->alt][i].mask_token = mask_token(s);
            frame[i].mask_token = s->grids[s->alt][i].mask_token;
        }
        uint32_t token = frame[i].mask_token, original = frame[i].v.chars[0];
        memset(frame[i].v.chars, 0, sizeof(frame[i].v.chars));
        memset(&frame[i].v.attrs, 0, sizeof(frame[i].v.attrs));
        frame[i].v.width = 1;
        vterm_color_rgb(&frame[i].v.fg, 127, 165, 143);
        if (m->settings.privacy_style == OBFUSCATION) {
            size_t at = token % (sizeof(alphabet) - 1);
            if ((uint32_t)alphabet[at] == original)
                at = (at + 1) % (sizeof(alphabet) - 1);
            frame[i].v.chars[0] = (unsigned char)alphabet[at];
        } else if (m->settings.privacy_style == EMOJI) {
            vterm_color_rgb(&frame[i].v.fg, 239, 199, 94);
            if (i + 1 < count && (i + 1) % s->cols && concealed[i + 1]) {
                size_t at = token % (sizeof(faces) / sizeof(*faces));
                if (faces[at] == original)
                    at = (at + 1) % (sizeof(faces) / sizeof(*faces));
                frame[i].v.chars[0] = faces[at];
                frame[i].v.width = 2;
                frame[i + 1] = frame[i];
                memset(frame[i + 1].v.chars, 0, sizeof(frame[i + 1].v.chars));
                frame[i + 1].v.width = 0;
                i++;
            } else {
                /* Text presentation uses one terminal column for an odd final
                 * cell, a quote boundary, or the right edge. Never overwrite
                 * adjacent syntax or move the shell's cursor. */
                frame[i].v.chars[0] = 0x263a;
                frame[i].v.chars[1] = 0xfe0e;
            }
        } else {
            frame[i].v.chars[0] = '*';
        }
    }
    free(concealed);
}
void screen_render(Screen *s, const Menu *m, Buffer *out) {
    manual_update(s, false);
    if (m->enabled)
        screen_mask(s, &m->settings);
    size_t count = (size_t)s->rows * (size_t)s->cols;
    Cell *frame = xcalloc(count, sizeof(Cell));
    VTermPos pos = s->cursor;
    bool visible = s->cursor_visible;
    if (m->open) {
        menu_draw(m, frame, s->rows, s->cols, &pos);
        visible = pos.row >= 0;
    } else {
        memcpy(frame, s->grids[s->alt], count * sizeof(Cell));
        if (!m->revealing)
            conceal_frame(s, m, frame);
    }
    buf_str(out, "\033[?25l\033[?7l");
    keyboard_modes(s, m, out);
    char header[48];
    snprintf(header, sizeof(header), "Privacy: %s  Menu (F2)", privacy_state_label(m));
    int len = (int)strlen(header), start = MAX(1, s->cols - len + 1);
    buf_str(out, "\033[1;1H\033[0m\033[2K");
    buf_printf(out, "\033[1;%dH\033[38;2;125;135;146m", start);
    if (len > s->cols)
        buf_add(out, header, (size_t)s->cols);
    else {
        buf_str(out, "Privacy: ");
        buf_str(out, m->crazy     ? "\033[38;2;177;143;178mCrazy"
                     : m->enabled ? "\033[38;2;127;165;143mOn"
                                  : "\033[38;2;192;160;118mOff");
        buf_str(out, "\033[38;2;125;135;146m  Menu (F2)");
    }
    if (m->notification_until > monotime() && start > 2) {
        buf_str(out, "\033[1;1H\033[38;2;127;165;143m");
        buf_add(out, m->notification, MIN(strlen(m->notification), (size_t)start - 2));
    }
    for (int y = 0; y < s->rows; y++) {
        Cell *row = frame + y * s->cols;
        if (!s->invalid && s->last_frame &&
            !memcmp(row, s->last_frame + y * s->cols, (size_t)s->cols * sizeof(Cell)))
            continue;
        buf_printf(out, "\033[%d;1H", y + 2);
        VTermScreenCell previous = {0};
        bool have_style = false;
        for (int x = 0; x < s->cols; x++) {
            VTermScreenCell *v = &row[x].v;
            if (!v->width)
                continue;
            if (!have_style || memcmp(&v->attrs, &previous.attrs, sizeof(v->attrs)) ||
                !vterm_color_is_equal(&v->fg, &previous.fg) ||
                !vterm_color_is_equal(&v->bg, &previous.bg)) {
                style(out, v);
                previous = *v;
                have_style = true;
            }
            if (!v->chars[0])
                buf_str(out, " ");
            else
                for (int k = 0; k < VTERM_MAX_CHARS_PER_CELL && v->chars[k]; k++) {
                    uint32_t cp = v->chars[k];
                    if (cp < 32 || (cp >= 0x7f && cp < 0xa0))
                        cp = 0xfffd;
                    buf_cp(out, cp);
                }
        }
    }
    buf_str(out, "\033[0m");
    if (visible) {
        buf_printf(out, "\033[%d;%dH\033[?25h", MAX(0, MIN(pos.row, s->rows - 1)) + 2,
                   MAX(0, MIN(pos.col, s->cols - 1)) + 1);
    }
    if (s->last_frame) {
        explicit_bzero(s->last_frame, count * sizeof(Cell));
        free(s->last_frame);
    }
    s->last_frame = frame;
    s->invalid = false;
}
