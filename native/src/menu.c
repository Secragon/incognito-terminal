/*
 * Copyright 2026 Secragon Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "privacy.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

void menu_init(Menu *m, const Settings *s, int active) {
    *m = (Menu){.settings = *s,
                .enabled = true,
                .shell_input = true,
                .active_history = active,
                .edit_index = -1};
}
const char *privacy_state_label(const Menu *m) {
    return m->crazy ? "Crazy" : m->enabled ? "On" : "Off";
}
static void message(Menu *m, const char *s) {
    snprintf(m->message, sizeof(m->message), "%s", s);
}
static void update_mode(Menu *m, const char *old) {
    size_t n = strlen(m->edit.flag), o = strlen(old);
    if ((n && strchr(":=", m->edit.flag[n - 1])) || m->edit.mode == default_mode(old) ||
        (o && strchr(":=", old[o - 1])))
        m->edit.mode = default_mode(m->edit.flag);
    m->message[0] = 0;
}
void menu_insert(Menu *m, int ch) {
    if ((m->page != EDITOR && m->page != EXCEPTION_EDITOR && m->page != SHORTCUT_EDITOR) ||
        m->focus || ch < 32 || ch > 126)
        return;
    size_t n = strlen(m->edit.flag);
    if (n >= MAX_FLAG)
        return;
    char old[MAX_FLAG + 1];
    strcpy(old, m->edit.flag);
    memmove(m->edit.flag + m->position + 1, m->edit.flag + m->position,
            n - (size_t)m->position + 1);
    m->edit.flag[m->position++] = (char)ch;
    if (m->page == EDITOR)
        update_mode(m, old);
    else
        m->message[0] = 0;
}
static void edit(Menu *m, int i) {
    m->edit_index = i;
    m->edit = i < 0 ? (Rule){.mode = EQUALS} : m->settings.rules[i];
    m->position = (int)strlen(m->edit.flag);
    m->focus = 0;
    m->page = EDITOR;
    m->message[0] = 0;
}
static void edit_exception(Menu *m, int index) {
    m->edit_index = index;
    m->edit = (Rule){0};
    if (index >= 0)
        strcpy(m->edit.flag, m->settings.exceptions[index]);
    m->position = (int)strlen(m->edit.flag);
    m->focus = 0;
    m->page = EXCEPTION_EDITOR;
    m->message[0] = 0;
}
static bool persist_exceptions(Menu *m, const Settings *next) {
    if (settings_save(next, SAVE_EXCEPTIONS)) {
        message(m, "Could not save. Your active exceptions are unchanged.");
        return false;
    }
    m->settings = *next;
    m->exception_selected = MAX(0, MIN(m->exception_selected, next->exception_count - 1));
    m->page = EXCEPTIONS;
    message(m, "Exceptions saved.");
    return true;
}
static bool persist(Menu *m, Settings *s) {
    if (s->count < 1 || s->count > MAX_RULES) {
        message(m, "Keep between 1 and 32 triggers.");
        return false;
    }
    for (int i = 0; i < s->count; i++)
        if (!rule_valid(&s->rules[i])) {
            message(m, "Use flags such as -P, --password, or /p:.");
            return false;
        }
    if (settings_save(s, false)) {
        message(m, "Could not save. Your active triggers are unchanged.");
        return false;
    }
    m->settings = *s;
    m->selected = MIN(m->selected, s->count - 1);
    m->page = TRIGGERS;
    message(m, "Triggers saved.");
    return true;
}
void menu_key(Menu *m, int key) {
    if (key == K_F2) {
        m->revealing = false;
        m->reveal_until = 0;
        m->open = !m->open;
        m->page = HOME;
        m->message[0] = 0;
        return;
    }
    if (!m->open)
        return;
    if (key == K_ESC || key == 3) {
        if (m->page == EDITOR)
            m->page = TRIGGERS;
        else if (m->page == EXCEPTION_EDITOR)
            m->page = EXCEPTIONS;
        else if (m->page == SHORTCUT_EDITOR)
            m->page = SHORTCUTS;
        else if (m->page != HOME)
            m->page = HOME;
        else
            m->open = false;
        m->message[0] = 0;
        return;
    }
    int lower = key < 128 ? tolower(key) : key;
    if (m->page == HOME) {
        if (key == K_UP || key == K_DOWN || key == K_TAB)
            m->home_selected = (m->home_selected + (key == K_UP ? 5 : 1)) % 6;
        if (lower == 'p' || (key == K_ENTER && m->home_selected == 0)) {
            m->page = PRIVACY;
            m->privacy_selected = m->crazy ? 2 : m->enabled ? 0 : 1;
        } else if (lower == 'm' || (key == K_ENTER && m->home_selected == 1)) {
            m->page = STYLE;
            m->style_selected = m->settings.privacy_style;
        } else if (lower == 't' || (key == K_ENTER && m->home_selected == 2))
            m->page = TRIGGERS;
        else if (lower == 'h' || (key == K_ENTER && m->home_selected == 3)) {
            m->page = HISTORY;
            m->history_selected = m->settings.history;
        } else if (lower == 'x' || (key == K_ENTER && m->home_selected == 4))
            m->page = EXCEPTIONS;
        else if (lower == 's' || (key == K_ENTER && m->home_selected == 5))
            m->page = SHORTCUTS;
    } else if (m->page == PRIVACY) {
        if (key == K_UP || key == K_DOWN || key == K_TAB)
            m->privacy_selected = (m->privacy_selected + (key == K_UP ? 2 : 1)) % 3;
        else if (lower == 'o')
            m->privacy_selected = 0;
        else if (lower == 'f')
            m->privacy_selected = 1;
        else if (lower == 'c')
            m->privacy_selected = 2;
        else if (key == K_ENTER) {
            m->enabled = m->privacy_selected != 1;
            m->crazy = m->privacy_selected == 2;
            message(m, "Applied to this session.");
        }
    } else if (m->page == STYLE) {
        if (key == K_UP || key == K_DOWN || key == K_TAB)
            m->style_selected = (m->style_selected + (key == K_UP ? 2 : 1)) % 3;
        else if (lower == 'a')
            m->style_selected = ASTERISKS;
        else if (lower == 'o')
            m->style_selected = OBFUSCATION;
        else if (lower == 'e')
            m->style_selected = EMOJI;
        else if (key == K_ENTER) {
            Settings next = m->settings;
            next.privacy_style = (MaskStyle)m->style_selected;
            if (settings_save(&next, SAVE_STYLE))
                message(m, "Could not save the privacy mode.");
            else {
                m->settings.privacy_style = next.privacy_style;
                message(m, "Privacy mode saved.");
            }
        }
    } else if (m->page == HISTORY) {
        if (key == K_UP || key == K_DOWN || key == K_TAB)
            m->history_selected = (m->history_selected + (key == K_UP ? 2 : 1)) % 3;
        else if (lower == 'n')
            m->history_selected = NORMAL;
        else if (lower == 's')
            m->history_selected = SESSION;
        else if (lower == 'o')
            m->history_selected = OFF;
        else if (key == K_ENTER) {
            Settings next = m->settings;
            next.history = (HistoryMode)m->history_selected;
            if (settings_save(&next, true))
                message(m, "Could not save the history preference.");
            else {
                m->settings.history = next.history;
                message(m, "Saved. Applies to new sessions.");
            }
        }
    } else if (m->page == TRIGGERS) {
        if (key == ' ') {
            Settings next = m->settings;
            next.triggers_enabled = !next.triggers_enabled;
            if (persist(m, &next))
                message(m, next.triggers_enabled ? "Triggers enabled." : "Triggers disabled.");
            return;
        }
        if (key == K_UP)
            m->selected = MAX(0, m->selected - 1);
        else if (key == K_DOWN)
            m->selected = MIN(m->settings.count - 1, m->selected + 1);
        else if (key == K_HOME)
            m->selected = 0;
        else if (key == K_END)
            m->selected = m->settings.count - 1;
        else if (lower == 'a')
            edit(m, -1);
        else if (lower == 'e' || key == K_ENTER)
            edit(m, m->selected);
        else if (lower == 'd' || key == K_DELETE) {
            Settings next = m->settings;
            memmove(next.rules + m->selected, next.rules + m->selected + 1,
                    (size_t)(next.count - m->selected - 1) * sizeof(Rule));
            next.count--;
            persist(m, &next);
        }
    } else if (m->page == EXCEPTIONS) {
        if (key == ' ') {
            Settings next = m->settings;
            next.exclusions_enabled = !next.exclusions_enabled;
            if (persist_exceptions(m, &next))
                message(m,
                        next.exclusions_enabled ? "Exclusions enabled." : "Exclusions disabled.");
            return;
        }
        int count = m->settings.exception_count;
        if (key == K_UP)
            m->exception_selected = MAX(0, m->exception_selected - 1);
        else if (key == K_DOWN)
            m->exception_selected = MAX(0, MIN(count - 1, m->exception_selected + 1));
        else if (key == K_HOME)
            m->exception_selected = 0;
        else if (key == K_END)
            m->exception_selected = MAX(0, count - 1);
        else if (lower == 'a')
            edit_exception(m, -1);
        else if ((lower == 'e' || key == K_ENTER) && count)
            edit_exception(m, m->exception_selected);
        else if ((lower == 'd' || key == K_DELETE) && count) {
            Settings next = m->settings;
            memmove(next.exceptions + m->exception_selected,
                    next.exceptions + m->exception_selected + 1,
                    (size_t)(count - m->exception_selected - 1) * sizeof(next.exceptions[0]));
            memset(next.exceptions[--next.exception_count], 0, sizeof(next.exceptions[0]));
            persist_exceptions(m, &next);
        }
    } else if (m->page == SHORTCUTS) {
        if (key == K_UP || key == K_DOWN || key == K_TAB)
            m->shortcut_selected = (m->shortcut_selected + 1) % SHORTCUT_COUNT;
        else if (lower == 'e' || key == K_ENTER) {
            m->edit = (Rule){0};
            shortcut_label(m->settings.shortcuts[m->shortcut_selected], m->edit.flag,
                           sizeof(m->edit.flag));
            m->position = (int)strlen(m->edit.flag);
            m->focus = 0;
            m->page = SHORTCUT_EDITOR;
            m->message[0] = 0;
        }
    } else if (key == K_ENTER && m->page == SHORTCUT_EDITOR) {
        Shortcut binding;
        if (!shortcut_parse(m->edit.flag, &binding)) {
            if (!strcasecmp(m->edit.flag, "Ctrl+M"))
                message(m, "Ctrl+M is Enter and cannot be assigned.");
            else
                message(m, "Use Alt+I or F1/F3-F12 (F2 is reserved).");
            return;
        }
        Shortcut other = m->settings.shortcuts[1 - m->shortcut_selected];
        if (binding.code == other.code && binding.modifiers == other.modifiers) {
            message(m, "That shortcut is already assigned.");
            return;
        }
        Settings next = m->settings;
        next.shortcuts[m->shortcut_selected] = binding;
        if (settings_save(&next, SAVE_SHORTCUTS))
            message(m, "Could not save the shortcuts.");
        else {
            m->settings = next;
            m->page = SHORTCUTS;
            message(m, "Shortcuts saved.");
        }
    } else if (key == K_TAB && m->page == EDITOR)
        m->focus = !m->focus;
    else if (key == K_ENTER && m->page == EXCEPTION_EDITOR) {
        if (!exception_valid(m->edit.flag)) {
            message(m, "Enter a command name, e.g. nmap or redis-cli.");
            return;
        }
        for (int i = 0; i < m->settings.exception_count; i++)
            if (i != m->edit_index && !strcmp(m->settings.exceptions[i], m->edit.flag)) {
                message(m, "That exception already exists. Edit its existing row.");
                return;
            }
        Settings next = m->settings;
        int index = m->edit_index;
        if (index < 0) {
            if (next.exception_count == MAX_EXCEPTIONS) {
                snprintf(m->message, sizeof(m->message), "Keep at most %d exceptions.",
                         MAX_EXCEPTIONS);
                return;
            }
            index = next.exception_count++;
        }
        memset(next.exceptions[index], 0, sizeof(next.exceptions[index]));
        strcpy(next.exceptions[index], m->edit.flag);
        if (persist_exceptions(m, &next))
            m->exception_selected = index;
    } else if (key == K_ENTER) {
        for (int i = 0; i < m->settings.count; i++)
            if (i != m->edit_index && !strcmp(m->settings.rules[i].flag, m->edit.flag)) {
                message(m, "That trigger already exists. Edit its existing row.");
                return;
            }
        Settings next = m->settings;
        int index = m->edit_index;
        if (index < 0) {
            if (next.count == MAX_RULES) {
                message(m, "Keep between 1 and 32 triggers.");
                return;
            }
            index = next.count++;
        }
        next.rules[index] = m->edit;
        if (persist(m, &next))
            m->selected = index;
    } else if (m->focus) {
        size_t n = strlen(m->edit.flag);
        if (n && strchr(":=", m->edit.flag[n - 1]))
            return;
        if (key == K_LEFT || key == K_UP)
            m->edit.mode = (m->edit.mode + 3) % 4;
        if (key == K_RIGHT || key == K_DOWN || key == ' ')
            m->edit.mode = (m->edit.mode + 1) % 4;
    } else {
        char old[MAX_FLAG + 1];
        strcpy(old, m->edit.flag);
        int n = (int)strlen(old);
        if (key == K_BACKSPACE && m->position) {
            m->position--;
            memmove(m->edit.flag + m->position, m->edit.flag + m->position + 1,
                    (size_t)(n - m->position));
        } else if (key == K_DELETE && m->position < n)
            memmove(m->edit.flag + m->position, m->edit.flag + m->position + 1,
                    (size_t)(n - m->position));
        else if (key == K_LEFT)
            m->position = MAX(0, m->position - 1);
        else if (key == K_RIGHT)
            m->position = MIN(n, m->position + 1);
        else if (key == K_HOME)
            m->position = 0;
        else if (key == K_END)
            m->position = n;
        else if (key == K_CLEAR)
            m->edit.flag[0] = 0, m->position = 0;
        else {
            menu_insert(m, key);
            return;
        }
        if (strcmp(old, m->edit.flag)) {
            if (m->page == EDITOR)
                update_mode(m, old);
            else
                m->message[0] = 0;
        }
    }
}
typedef struct {
    const char *bytes;
    int key;
} Sequence;
static const Sequence sequences[] = {
    {"\033OQ", K_F2},      {"\033[12~", K_F2}, {"\033[A", K_UP},    {"\033OA", K_UP},
    {"\033[B", K_DOWN},    {"\033OB", K_DOWN}, {"\033[C", K_RIGHT}, {"\033OC", K_RIGHT},
    {"\033[D", K_LEFT},    {"\033OD", K_LEFT}, {"\033[H", K_HOME},  {"\033OH", K_HOME},
    {"\033[1~", K_HOME},   {"\033[F", K_END},  {"\033OF", K_END},   {"\033[4~", K_END},
    {"\033[3~", K_DELETE}, {"\033[200~", 300}, {"\033[201~", 301}};
static int control(int c) {
    switch (c) {
    case 13:
    case 10:
        return K_ENTER;
    case 127:
    case 8:
        return K_BACKSPACE;
    case 27:
        return K_ESC;
    case 21:
        return K_CLEAR;
    case 1:
        return K_HOME;
    case 5:
        return K_END;
    case 9:
        return K_TAB;
    default:
        return c;
    }
}
void router_feed(Router *r, Menu *m, Buffer *out, const char *s, size_t len) {
    buf_add(&r->pending, s, len);
    while (r->pending.len) {
        char *p = r->pending.data;
        size_t n = r->pending.len;
        if (!r->paste) {
            int consumed = keyboard_sequence(m, out, p, n);
            if (consumed < 0)
                break;
            if (consumed > 0) {
                buf_consume(&r->pending, (size_t)consumed);
                continue;
            }
        }
        bool prefix = false;
        const Sequence *match = NULL;
        for (size_t i = 0; i < sizeof(sequences) / sizeof(*sequences); i++) {
            const Sequence *seq = &sequences[i];
            if (r->paste && seq->key != 301)
                continue;
            size_t k = strlen(seq->bytes);
            if (n >= k && !memcmp(p, seq->bytes, k)) {
                match = seq;
                break;
            }
            if (n < k && !memcmp(p, seq->bytes, n))
                prefix = true;
        }
        if (match) {
            size_t k = strlen(match->bytes);
            if (match->key == 300) {
                r->paste = m->open ? 2 : 1;
                if (r->paste == 1)
                    buf_add(out, p, k);
            } else if (match->key == 301 && r->paste) {
                if (r->paste == 1)
                    buf_add(out, p, k);
                r->paste = 0;
            } else if (match->key == K_F2)
                menu_key(m, K_F2);
            else if (m->open)
                menu_key(m, match->key);
            else
                buf_add(out, p, k);
            buf_consume(&r->pending, k);
        } else if (prefix)
            break;
        else if (!r->paste && m->open && n >= 2 && p[0] == 27 && (p[1] == '[' || p[1] == 'O')) {
            size_t i = 2;
            while (i < n && !(p[i] >= 64 && p[i] <= 126))
                i++;
            if (i == n && n < 64)
                break;
            buf_consume(&r->pending, MIN(i + 1, n));
        } else if (!r->paste && m->open && n > 1 && p[0] == 27)
            buf_consume(&r->pending, 2);
        else {
            int c = (unsigned char)p[0];
            if (r->paste == 1 || (!r->paste && !m->open))
                buf_add(out, p, 1);
            else if (r->paste)
                menu_insert(m, c);
            else
                menu_key(m, control(c));
            buf_consume(&r->pending, 1);
        }
    }
    r->since = r->pending.len ? monotime() : 0;
}
void router_flush(Router *r, Menu *m, Buffer *out) {
    if (r->paste || !r->since || monotime() - r->since < 0.1)
        return;
    if (!m->open)
        buf_add(out, r->pending.data, r->pending.len);
    else if (r->pending.len == 1 && r->pending.data[0] == 27)
        menu_key(m, K_ESC);
    buf_consume(&r->pending, r->pending.len);
    r->since = 0;
}
/* Window layout uses screen cells so clipping, colors and cursor placement are
 * shared with the shell renderer. No menu text can introduce escape sequences. */
enum {
    BACKDROP,
    SHADOW,
    BODY,
    BORDER,
    TITLE,
    MUTED,
    SELECTED,
    INPUT,
    ACCENT,
    NOTICE,
    KEY,
    OPTION,
    DESCRIPTION
};
static const unsigned colors[][2] = {
    {0x627089, 0x101722}, {0x101722, 0x090e16}, {0xe4eaf2, 0x1b283a}, {0x5b92ad, 0x1b283a},
    {0x8ce0d0, 0x1b283a}, {0xa4b4c8, 0x1b283a}, {0xffffff, 0x315575}, {0xffffff, 0x101c2c},
    {0x8ce0d0, 0x1b283a}, {0xf0cf88, 0x1b283a}, {0x8ce0d0, 0x315575}, {0xe4eaf2, 0x1b283a},
    {0x91b9bc, 0x1b283a}};
static void put(Cell *g, int rows, int cols, int x, int y, uint32_t c, int role) {
    if (x < 0 || y < 0 || x >= cols || y >= rows)
        return;
    Cell *p = &g[y * cols + x];
    *p = (Cell){.v = {.chars = {c}, .width = 1}};
    unsigned fg = colors[role][0], bg = colors[role][1];
    vterm_color_rgb(&p->v.fg, (uint8_t)(fg >> 16), (uint8_t)(fg >> 8), (uint8_t)fg);
    vterm_color_rgb(&p->v.bg, (uint8_t)(bg >> 16), (uint8_t)(bg >> 8), (uint8_t)bg);
    p->v.attrs.bold = (role == TITLE || role == KEY || role == OPTION || role == SELECTED);
    p->v.attrs.italic = (role == DESCRIPTION);
}
static void text(Cell *g, int rows, int cols, int x, int y, const char *s, int role, int limit) {
    int i = 0;
    while (*s && i < limit) {
        unsigned char b = (unsigned char)*s++;
        uint32_t cp = b;
        int extra = 0;
        if (b >= 0xc0 && b < 0xe0)
            cp = b & 31, extra = 1;
        else if (b >= 0xe0 && b < 0xf0)
            cp = b & 15, extra = 2;
        else if (b >= 0xf0)
            cp = b & 7, extra = 3;
        while (extra-- && *s)
            cp = (cp << 6) | ((unsigned char)*s++ & 63);
        put(g, rows, cols, x + i++, y, cp, role);
    }
}
static void option_row(Cell *g, int rows, int cols, int x, int y, int width, const char *label,
                       const char *value, bool selected) {
    for (int i = 0; i < width; i++)
        put(g, rows, cols, x + i, y, ' ', selected ? SELECTED : BODY);
    int value_width = value ? MIN((int)strlen(value), width / 2) : 0;
    text(g, rows, cols, x, y, label, selected ? SELECTED : OPTION,
         width - (value_width ? value_width + 2 : 0));
    if (value_width)
        text(g, rows, cols, x + width - value_width, y, value, selected ? SELECTED : ACCENT,
             value_width);
}
static void example(char *out, size_t n, const Rule *r) {
    const char *f = r->flag[0] ? r->flag : "-P";
    if (r->mode == ATTACHED)
        snprintf(out, n, "%ssecret", f);
    else if (r->mode == SEPARATED)
        snprintf(out, n, "%s secret", f);
    else if (r->mode == BOTH)
        snprintf(out, n, "%ssecret  /  %s secret", f, f);
    else
        snprintf(out, n, "%s=secret  /  %s secret", f, f);
}
void menu_draw(const Menu *m, Cell *g, int rows, int cols, VTermPos *cursor) {
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++)
            put(g, rows, cols, x, y, ' ', BACKDROP);
    *cursor = (VTermPos){-1, -1};
    if (cols < 40 || rows < 12) {
        text(g, rows, cols, 0, 0, "Privacy settings", TITLE, cols);
        text(g, rows, cols, 0, MIN(2, rows - 1), "Resize to at least 40 x 13.", BODY, cols);
        text(g, rows, cols, 0, MIN(4, rows - 1), "F2: Close", MUTED, cols);
        return;
    }
    int w = MIN(m->page == HOME ? 56 : 72, cols - 4), h = MIN(m->page == HOME ? 25 : 20, rows),
        left = (cols - w) / 2, top = (rows - h) / 2;
    for (int y = top + 1; y < MIN(rows, top + h + 1); y++)
        for (int x = left + 1; x < left + w + 1; x++)
            put(g, rows, cols, x, y, ' ', SHADOW);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            put(g, rows, cols, left + x, top + y, ' ', BODY);
    for (int y = 1; y < h - 1; y++) {
        put(g, rows, cols, left, top + y, 0x2502, BORDER);
        put(g, rows, cols, left + w - 1, top + y, 0x2502, BORDER);
    }
    for (int x = 1; x < w - 1; x++) {
        put(g, rows, cols, left + x, top, 0x2500, BORDER);
        put(g, rows, cols, left + x, top + h - 1, 0x2500, BORDER);
    }
    put(g, rows, cols, left, top, 0x256d, BORDER);
    put(g, rows, cols, left + w - 1, top, 0x256e, BORDER);
    put(g, rows, cols, left, top + h - 1, 0x2570, BORDER);
    put(g, rows, cols, left + w - 1, top + h - 1, 0x256f, BORDER);
#define CONTENT(y, s, r) text(g, rows, cols, left + 2, top + (y), s, r, w - 4)
    const char *title = m->page == HOME              ? " Privacy "
                        : m->page == TRIGGERS        ? " Triggers "
                        : m->page == HISTORY         ? " History "
                        : m->page == STYLE           ? " Privacy mode "
                        : m->page == PRIVACY         ? " Privacy state "
                        : m->page == EXCEPTIONS      ? " Exceptions "
                        : m->page == SHORTCUTS       ? " Shortcuts "
                        : m->page == SHORTCUT_EDITOR ? " Edit shortcut "
                        : m->page == EXCEPTION_EDITOR
                            ? (m->edit_index < 0 ? " Add exception " : " Edit exception ")
                        : m->edit_index < 0 ? " Add trigger "
                                            : " Edit trigger ";
    CONTENT(0, title, TITLE);
    const char *active =
        m->active_history < 0 ? "Waiting for shell setup..." : history_labels[m->active_history];
    char line[256], ex[160];
    const char *legend;
    if (m->page == HOME) {
        CONTENT(1, "Screen privacy settings", MUTED);
        const char *desc[] = {
            "Control which text stays concealed.", "Choose how concealed text looks.",
            "Recognize passwords by their flags.", "Choose command recall and storage.",
            "Keep selected commands visible.",     "Set masking and reveal key bindings."};
        const char *short_desc[] = {
            "Choose what stays concealed.",    "Choose how concealed text looks.",
            "Flags used to detect passwords.", "Control command recall/storage.",
            "Keep selected commands visible.", "Change your privacy shortcuts."};
        const char *labels[] = {"  [P] Privacy", "  [M] Privacy mode", "  [T] Triggers",
                                "  [H] History", "  [X] Exceptions",   "  [S] Shortcuts"};
        char values[6][64];
        snprintf(values[0], sizeof(values[0]), "%s", privacy_state_label(m));
        snprintf(values[1], sizeof(values[1]), "%s",
                 privacy_style_labels[m->settings.privacy_style]);
        snprintf(values[2], sizeof(values[2]), "%s",
                 m->settings.triggers_enabled ? "Enabled" : "Disabled");
        bool pending = m->active_history >= 0 && m->active_history != (int)m->settings.history;
        snprintf(values[3], sizeof(values[3]), "%s%s", active, pending ? "*" : "");
        snprintf(values[4], sizeof(values[4]), "%s",
                 m->settings.exclusions_enabled ? "Enabled" : "Disabled");
        snprintf(values[5], sizeof(values[5]), "2 bindings");
        int gap = h >= 23 ? 3 : h >= 18 ? 2 : 1;
        for (int i = 0; i < 6; i++)
            option_row(g, rows, cols, left + 2, top + 3 + i * gap, w - 4, labels[i], values[i],
                       m->home_selected == i);
        if (gap == 3) {
            for (int i = 0; i < 6; i++) {
                snprintf(line, sizeof(line), "%s%s", w >= 50 ? "      " : "",
                         i == 3 && pending ? "History change applies next session."
                         : w >= 50         ? desc[i]
                                           : short_desc[i]);
                CONTENT(4 + i * gap, line, DESCRIPTION);
            }
        } else
            CONTENT(h - 4, short_desc[m->home_selected], DESCRIPTION);
        legend =
            w >= 46 ? "↑↓ Select  Enter Open  Esc/F2 Close" : "↑↓ Select  Enter Open  Esc Close";
    } else if (m->page == PRIVACY) {
        CONTENT(2, "Choose what stays concealed.", DESCRIPTION);
        const char *names[] = {"On", "Off", "Crazy"};
        const char *desc[] = {"Mask passwords; respect command exceptions.",
                              "Show original text, including passwords.",
                              "Mask command text; keep the prompt visible."};
        const char *short_desc[] = {"Mask matching password values.",
                                    "Reveal all text and passwords.",
                                    "Hide input; show the prompt."};
        for (int i = 0; i < 3; i++) {
            int row = h >= 16 ? 4 + i * (h >= 18 ? 3 : 2) : 3 + i;
            snprintf(line, sizeof(line), "%s [%c] %s", i == m->privacy_selected ? "›" : " ",
                     "OFC"[i], names[i]);
            option_row(g, rows, cols, left + 2, top + row, w - 4, line, NULL,
                       i == m->privacy_selected);
            if (h >= 16) {
                snprintf(line, sizeof(line), "%s%s", w >= 56 ? "      " : "",
                         w >= 56 ? desc[i] : short_desc[i]);
                CONTENT(row + 1, line, DESCRIPTION);
            }
        }
        if (h < 16)
            CONTENT(6, short_desc[m->privacy_selected], DESCRIPTION);
        snprintf(line, sizeof(line), "Current: %s", privacy_state_label(m));
        CONTENT(h - 5, line, ACCENT);
        CONTENT(h - 4, m->message[0] ? m->message : "Changes apply to this session.",
                m->message[0] ? NOTICE : DESCRIPTION);
        legend = w >= 50 ? "↑↓ Select  Enter Apply  Esc Back  F2 Close"
                         : "↑↓ Select  Enter Apply  Esc Back";
    } else if (m->page == STYLE) {
        CONTENT(2, "Choose how concealed text looks.", DESCRIPTION);
        const char *desc[] = {"Green stars, matching the On indicator.",
                              "Random password-like characters.", "Random yellow smiling faces."};
        const char *short_desc[] = {"Green stars match the On label.",
                                    "Random password-like characters.",
                                    "Random yellow smiling faces."};
        for (int i = 0; i < 3; i++) {
            int row = h >= 16 ? 4 + i * (h >= 18 ? 3 : 2) : 3 + i;
            snprintf(line, sizeof(line), "%s [%c] %s", i == m->style_selected ? "›" : " ", "AOE"[i],
                     privacy_style_labels[i]);
            option_row(g, rows, cols, left + 2, top + row, w - 4, line, NULL,
                       i == m->style_selected);
            if (h >= 16) {
                snprintf(line, sizeof(line), "%s%s", w >= 56 ? "      " : "",
                         w >= 56 ? desc[i] : short_desc[i]);
                CONTENT(row + 1, line, DESCRIPTION);
            }
        }
        if (h < 16)
            CONTENT(6, short_desc[m->style_selected], DESCRIPTION);
        snprintf(line, sizeof(line), "Current: %s",
                 privacy_style_labels[m->settings.privacy_style]);
        CONTENT(h - 5, line, ACCENT);
        CONTENT(h - 4, m->message[0] ? m->message : "Applies to On and Crazy.",
                m->message[0] ? NOTICE : DESCRIPTION);
        legend = w >= 50 ? "↑↓ Select  Enter Save  Esc Back  F2 Close"
                         : "↑↓ Select  Enter Save  Esc Back";
    } else if (m->page == HISTORY) {
        snprintf(line, sizeof(line), "Active: %s", active);
        CONTENT(2, line, ACCENT);
        const char *desc[] = {"Use the shell's usual history settings.",
                              "Keep Up-arrow recall; do not save to disk.",
                              "Do not retain commands for recall or saving."};
        const char *short_desc[] = {"Use the shell's history settings.",
                                    "Recall commands; no disk storage.",
                                    "No command recall or storage."};
        for (int i = 0; i < 3; i++) {
            int row = h >= 16 ? 4 + i * (h >= 18 ? 3 : 2) : 3 + i;
            snprintf(line, sizeof(line), "%s [%c] %s", i == m->history_selected ? "›" : " ",
                     "NSO"[i], history_labels[i]);
            option_row(g, rows, cols, left + 2, top + row, w - 4, line, NULL,
                       i == m->history_selected);
            if (h >= 16) {
                snprintf(line, sizeof(line), "%s%s", w >= 56 ? "      " : "",
                         w >= 56 ? desc[i] : short_desc[i]);
                CONTENT(row + 1, line, DESCRIPTION);
            }
        }
        if (h < 16)
            CONTENT(6, short_desc[m->history_selected], DESCRIPTION);
        snprintf(line, sizeof(line), "Next session: %s", history_labels[m->settings.history]);
        CONTENT(h - 5, line, ACCENT);
        CONTENT(h - 4, m->message[0] ? m->message : "Save applies to new sessions.",
                m->message[0] ? NOTICE : DESCRIPTION);
        legend = w >= 50 ? "↑↓ Select  Enter Save  Esc Back  F2 Close"
                         : "↑↓ Select  Enter Save  Esc Back";
    } else if (m->page == TRIGGERS) {
        CONTENT(1,
                w >= 50 ? "Flags that identify password arguments."
                        : "Flags used to detect passwords.",
                DESCRIPTION);
        snprintf(line, sizeof(line), "[Space] Automatic triggers: %s",
                 m->settings.triggers_enabled ? "On" : "Off");
        CONTENT(2, line, OPTION);
        CONTENT(h >= 16 ? 4 : 3, "  TRIGGER          VALUE FORMAT", MUTED);
        int visible = MAX(1, h - 12), start = MAX(0, m->selected - visible + 1);
        for (int i = start; i < MIN(m->settings.count, start + visible); i++) {
            snprintf(line, sizeof(line), "%s %-17.16s%s", i == m->selected ? "›" : " ",
                     m->settings.rules[i].flag, mode_labels[m->settings.rules[i].mode]);
            CONTENT((h >= 16 ? 5 : 4) + i - start, line, i == m->selected ? SELECTED : BODY);
        }
        example(ex, sizeof(ex), &m->settings.rules[m->selected]);
        snprintf(line, sizeof(line), "Example: %s", ex);
        CONTENT(h - 6, line, ACCENT);
        snprintf(line, sizeof(line), "%d / %d   •   Case-sensitive", m->selected + 1,
                 m->settings.count);
        CONTENT(h - 5, line, MUTED);
        CONTENT(h - 4, m->message, NOTICE);
        legend = w >= 70   ? "Space Toggle  A Add  E Edit  D Remove  Esc Back  F2 Close"
                 : w >= 51 ? "Space Toggle  A Add  E Edit  D Remove  Esc Back"
                           : "Space On/Off  A Add  E Edit";
    } else if (m->page == EXCEPTIONS) {
        CONTENT(1,
                w >= 50 ? "Commands left visible in Privacy On." : "Commands left visible in On.",
                DESCRIPTION);
        snprintf(line, sizeof(line), "[Space] Command exclusions: %s",
                 m->settings.exclusions_enabled ? "On" : "Off");
        CONTENT(2, line, OPTION);
        CONTENT(h >= 16 ? 4 : 3, "  COMMAND          MASKING", MUTED);
        int visible = MAX(1, h - 12), start = MAX(0, m->exception_selected - visible + 1);
        for (int i = start; i < MIN(m->settings.exception_count, start + visible); i++) {
            snprintf(line, sizeof(line), "%s %-17.16sSkipped",
                     i == m->exception_selected ? "›" : " ", m->settings.exceptions[i]);
            CONTENT((h >= 16 ? 5 : 4) + i - start, line,
                    i == m->exception_selected ? SELECTED : BODY);
        }
        if (!m->settings.exception_count)
            CONTENT(h >= 16 ? 5 : 4, "No exceptions. All commands use triggers.", MUTED);
        CONTENT(h - 6,
                w >= 50 ? "Crazy mode also conceals these commands."
                        : "Crazy hides these commands too.",
                DESCRIPTION);
        snprintf(line, sizeof(line), "%d commands   •   Exact names, case-sensitive",
                 m->settings.exception_count);
        CONTENT(h - 5, line, MUTED);
        CONTENT(h - 4, m->message, NOTICE);
        legend = w >= 70   ? "Space Toggle  A Add  E Edit  D Remove  Esc Back  F2 Close"
                 : w >= 51 ? "Space Toggle  A Add  E Edit  D Remove  Esc Back"
                           : "Space On/Off  A Add  E Edit";
    } else if (m->page == SHORTCUTS) {
        CONTENT(2, "Edit your privacy key bindings.", DESCRIPTION);
        const char *names[] = {"Mask next value", "Hold to reveal"};
        const char *descs[] = {"One value; spaces and quote delimiters stay visible.",
                               "Show concealed text until the key is released."};
        for (int i = 0; i < SHORTCUT_COUNT; i++) {
            int row = h >= 16 ? 4 + i * 3 : 3 + i;
            char binding[64];
            shortcut_label(m->settings.shortcuts[i], binding, sizeof(binding));
            option_row(g, rows, cols, left + 2, top + row, w - 4, names[i], binding,
                       m->shortcut_selected == i);
            if (h >= 16)
                CONTENT(row + 1, descs[i], DESCRIPTION);
        }
        CONTENT(h - 5, m->keyboard_events ? "Exact key-release reveal is available here."
                                          : "Reveal safely auto-hides after key repeat stops.",
                DESCRIPTION);
        CONTENT(h - 4, m->message, NOTICE);
        legend = w >= 50 ? "↑↓ Select  Enter Edit  Esc Back  F2 Close"
                         : "↑↓ Select  Enter Edit  Esc Back";
    } else if (m->page == SHORTCUT_EDITOR) {
        CONTENT(2, m->shortcut_selected ? "Hold to reveal" : "Mask next value", OPTION);
        int start = MAX(0, m->position - (w - 6) + 1);
        snprintf(line, sizeof(line), " %s", m->edit.flag + start);
        CONTENT(4, line, INPUT);
        CONTENT(6, "Use Alt+letter or a function key for broad support.", DESCRIPTION);
        CONTENT(7, "Function keys F1, F3-F12 are also accepted.", DESCRIPTION);
        if (h >= 16)
            CONTENT(9, "F2 always opens the privacy menu.", DESCRIPTION);
        CONTENT(h - 4, m->message, NOTICE);
        legend =
            w >= 60 ? "Enter Save  Esc Cancel  F2 Close  Ctrl+U Clear" : "Enter Save  Esc Cancel";
        *cursor = (VTermPos){top + 4, left + 3 + m->position - start};
    } else if (m->page == EXCEPTION_EDITOR) {
        CONTENT(2, "Command name", OPTION);
        int start = MAX(0, m->position - (w - 6) + 1);
        snprintf(line, sizeof(line), " %s", m->edit.flag + start);
        CONTENT(3, line, INPUT);
        CONTENT(5, "Example: nmap  /  redis-cli", MUTED);
        CONTENT(7, "Use the name only, without a path or flags.", DESCRIPTION);
        if (h >= 15)
            CONTENT(8, "In On, this command skips all triggers.", DESCRIPTION);
        CONTENT(h - 4, m->message, NOTICE);
        legend =
            w >= 60 ? "Enter Save  Esc Cancel  F2 Close  Ctrl+U Clear" : "Enter Save  Esc Cancel";
        *cursor = (VTermPos){top + 3, left + 3 + m->position - start};
    } else {
        CONTENT(2, "Trigger", OPTION);
        int start = MAX(0, m->position - (w - 6) + 1);
        snprintf(line, sizeof(line), " %s", m->edit.flag + start);
        CONTENT(3, line, m->focus == 0 ? INPUT : BODY);
        CONTENT(5, "Value format", OPTION);
        snprintf(line, sizeof(line), " %s", mode_labels[m->edit.mode]);
        CONTENT(6, line, m->focus == 1 ? SELECTED : BODY);
        size_t n = strlen(m->edit.flag);
        bool attached = n && strchr(":=", m->edit.flag[n - 1]);
        const char *format_desc[] = {"Value directly follows the flag, without a space.",
                                     "A space separates the flag and its value.",
                                     "Accept values with or without a separating space.",
                                     "Accept =value or a space before the value."};
        CONTENT(8, attached ? "Ends in : or = → no space allowed." : format_desc[m->edit.mode],
                DESCRIPTION);
        if (h >= 16)
            CONTENT(9, "Tab Field   ←→ Format", MUTED);
        if (h >= 15) {
            example(ex, sizeof(ex), &m->edit);
            snprintf(line, sizeof(line), "Example: %s", ex);
            CONTENT(10, line, ACCENT);
        }
        CONTENT(h - 4, m->message, NOTICE);
        legend =
            w >= 60 ? "Enter Save  Esc Cancel  F2 Close  Ctrl+U Clear" : "Enter Save  Esc Cancel";
        if (!m->focus)
            *cursor = (VTermPos){top + 3, left + 3 + m->position - start};
    }
    /* Each pair stays on the single footer row, with its key highlighted. */
    int x = left + 2, remaining = w - 4;
    const char *p = legend;
    while (*p && remaining > 0) {
        const char *space = strchr(p, ' ');
        if (!space)
            break;
        size_t bytes = (size_t)(space - p);
        char key[32];
        if (bytes >= sizeof(key))
            break;
        memcpy(key, p, bytes);
        key[bytes] = 0;
        int width = !strcmp(key, "↑↓") ? 2 : (int)bytes;
        text(g, rows, cols, x, top + h - 2, key, KEY, remaining);
        x += width;
        remaining -= width;
        p = space;
        const char *next = strstr(p, "  ");
        size_t count = next ? (size_t)(next - p) + 2 : strlen(p);
        char label[40];
        count = MIN(count, sizeof(label) - 1);
        memcpy(label, p, count);
        label[count] = 0;
        text(g, rows, cols, x, top + h - 2, label, MUTED, remaining);
        x += (int)count;
        remaining -= (int)count;
        p += count;
    }
#undef CONTENT
}
