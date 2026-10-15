/* Copyright 2026 Secragon Ltd
 * SPDX-License-Identifier: Apache-2.0 */
#include "privacy.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool shortcut_parse(const char *text, Shortcut *shortcut) {
    if (!text || !*text || strlen(text) >= 64)
        return false;
    char copy[64];
    strcpy(copy, text);
    char *p = copy, *token;
    int mods = 0;
    while ((token = strchr(p, '+'))) {
        *token++ = 0;
        int bit = !strcasecmp(p, "Ctrl")    ? 4
                  : !strcasecmp(p, "Shift") ? 1
                  : !strcasecmp(p, "Alt")   ? 2
                                            : 0;
        if (!bit || (mods & bit))
            return false;
        mods |= bit;
        p = token;
    }
    int code = 0;
    if (strlen(p) == 1 && isalpha((unsigned char)*p) && (mods & 6))
        code = tolower((unsigned char)*p);
    else if ((*p == 'F' || *p == 'f') && isdigit((unsigned char)p[1])) {
        char *end;
        long fn = strtol(p + 1, &end, 10);
        if (*end || fn < 1 || fn > 12 || fn == 2)
            return false; /* F2 remains the menu key. */
        code = KEY_FUNCTION_BASE + (int)fn;
    } else
        return false;
    /* In the legacy terminal protocol Ctrl+M and Enter are the same CR byte.
     * Accepting it would consume Enter in both the shell and this menu. */
    if (code == 'm' && mods == 4)
        return false;
    *shortcut = (Shortcut){code, mods};
    return true;
}
void shortcut_label(Shortcut s, char *text, size_t size) {
    char key[16];
    if (s.code > KEY_FUNCTION_BASE)
        snprintf(key, sizeof(key), "F%d", s.code - KEY_FUNCTION_BASE);
    else
        snprintf(key, sizeof(key), "%c", toupper(s.code));
    snprintf(text, size, "%s%s%s%s", s.modifiers & 4 ? "Ctrl+" : "",
             s.modifiers & 1 ? "Shift+" : "", s.modifiers & 2 ? "Alt+" : "", key);
}
void menu_notify(Menu *m, const char *text) {
    snprintf(m->notification, sizeof(m->notification), "%s", text);
    m->notification_until = monotime() + 2.0;
}
static bool action(Menu *m, Buffer *out, int code, int modifiers, int event) {
    if (event == 3) {
        Shortcut reveal = m->settings.shortcuts[SHORTCUT_REVEAL];
        if (code == reveal.code || (modifiers & reveal.modifiers) != reveal.modifiers) {
            m->revealing = false;
            m->reveal_until = 0;
        }
        return true; /* Release events must never become shell input. */
    }
    for (int i = 0; i < SHORTCUT_COUNT; i++) {
        Shortcut s = m->settings.shortcuts[i];
        if (s.code != code || s.modifiers != (modifiers & ~192))
            continue;
        if (m->open)
            return true;
        if (i == SHORTCUT_MASK) {
            if (event == 1) {
                if (m->shell_input) {
                    buf_str(out, "\033[99;1~");
                    menu_notify(m, "Masked typing: On");
                } else
                    menu_notify(m, "Masked typing needs a shell prompt.");
            }
        } else if (m->keyboard_events) {
            m->revealing = true;
            m->reveal_until = 0;
        } else if (event == 1) {
            /* Legacy terminals do not report releases. Their key-repeat
             * presses extend this short deadline, which closely follows a
             * held key and always conceals the screen again after release. */
            double now = monotime();
            bool repeating = m->revealing && m->reveal_until > now;
            m->revealing = true;
            m->reveal_until = now + (repeating ? 0.20 : 0.70);
            if (!repeating)
                menu_notify(m, "Reveal: hold the key; release auto-hides.");
        }
        return true;
    }
    if (code == KEY_FUNCTION_BASE + 2 && !modifiers) {
        if (event == 1)
            menu_key(m, K_F2);
        return true;
    }
    return false;
}
static int function_number(int code) {
    static const int legacy[] = {11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24};
    for (int i = 0; i < 12; i++)
        if (legacy[i] == code)
            return i + 1;
    return 0;
}
static void key_output(Menu *m, Buffer *out, int code, int mods, int event, int shifted) {
    if (action(m, out, code, mods, event))
        return;
    if (code > KEY_FUNCTION_BASE) {
        int fn = code - KEY_FUNCTION_BASE;
        if (m->open)
            return;
        static const int legacy[] = {11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24};
        if (fn <= 4) {
            if (!mods)
                buf_printf(out, "\033O%c", 'P' + fn - 1);
            else
                buf_printf(out, "\033[1;%d%c", mods + 1, 'P' + fn - 1);
        } else if (fn <= 12) {
            if (mods)
                buf_printf(out, "\033[%d;%d~", legacy[fn - 1], mods + 1);
            else
                buf_printf(out, "\033[%d~", legacy[fn - 1]);
        }
        return;
    }
    if (mods & 4) {
        if (code >= 'a' && code <= 'z')
            code = code - 'a' + 1;
        else if (code >= '@' && code <= '_')
            code &= 31;
        else if (code == ' ' || code == '`')
            code = 0;
        else if (code == '?')
            code = 127;
    } else if (mods & 1) {
        if (shifted)
            code = shifted;
        else if (code >= 'a' && code <= 'z')
            code = toupper(code);
    }
    if (m->open) {
        int key = code == 13                 ? K_ENTER
                  : code == 27               ? K_ESC
                  : code == 9                ? K_TAB
                  : code == 127 || code == 8 ? K_BACKSPACE
                  : code == 21               ? K_CLEAR
                  : code == 1                ? K_HOME
                  : code == 5                ? K_END
                                             : code;
        if (!(mods & 2))
            menu_key(m, key);
    } else {
        if (code == 9 && mods == 1) {
            buf_str(out, "\033[Z");
            return;
        }
        if (mods & 2)
            buf_str(out, "\033");
        buf_cp(out, (uint32_t)code);
    }
}
/* 0: legacy/not ours; -1: fragmented escape sequence; positive: bytes consumed. */
int keyboard_sequence(Menu *m, Buffer *out, const char *bytes, size_t len) {
    if (!len)
        return 0;
    if ((unsigned char)bytes[0] != 27) {
        int c = (unsigned char)bytes[0];
        if (c >= 1 && c <= 26 && action(m, out, 'a' + c - 1, 4, 1))
            return 1;
        return 0;
    }
    if (len == 1)
        return -1;
    if (bytes[1] == 'O') {
        if (len < 3)
            return -1;
        if (bytes[2] >= 'P' && bytes[2] <= 'S') {
            key_output(m, out, KEY_FUNCTION_BASE + bytes[2] - 'P' + 1, 0, 1, 0);
            return 3;
        }
        return 0;
    }
    if (bytes[1] != '[') {
        if (isalpha((unsigned char)bytes[1]) &&
            action(m, out, tolower((unsigned char)bytes[1]),
                   2 | (isupper((unsigned char)bytes[1]) ? 1 : 0), 1))
            return 2;
        return 0;
    }
    size_t end = 2;
    while (end < len && !(bytes[end] >= 64 && bytes[end] <= 126))
        end++;
    if (end == len)
        return len < 128 ? -1 : 0;
    if (end >= 128)
        return 0;
    char final = bytes[end], params[128];
    memcpy(params, bytes + 2, end - 2);
    params[end - 2] = 0;
    if ((final == 'I' || final == 'O') && !params[0]) {
        if (final == 'O') {
            m->revealing = false;
            m->reveal_until = 0;
        }
        return (int)end + 1;
    }
    if (final == 'u' && params[0] == '?') {
        char *tail;
        long flags = strtol(params + 1, &tail, 10);
        if (!*tail)
            m->keyboard_events = (flags & 3) == 3;
        return (int)end + 1;
    }
    int parts[3][3] = {{0}}, part = 0, sub = 0;
    for (size_t i = 0; params[i]; i++) {
        char c = params[i];
        if (isdigit((unsigned char)c)) {
            if (parts[part][sub] > 200000)
                return 0;
            parts[part][sub] = parts[part][sub] * 10 + c - '0';
        } else if (c == ';' && part < 2) {
            part++;
            sub = 0;
        } else if (c == ':' && sub < 2)
            sub++;
        else
            return 0;
    }
    int code = parts[0][0], mods = MAX(1, parts[1][0]) - 1;
    int event = parts[1][1] ? parts[1][1] : 1;
    if (event < 1 || event > 3)
        return 0;
    if (final == 'u') {
        if (code >= 57364 && code <= 57375)
            code = KEY_FUNCTION_BASE + code - 57364 + 1;
        else if (code >= 57344 && code <= 57454) {
            /* Do not turn modifier/media key events into private-use text. */
            action(m, out, code, mods, event);
            return (int)end + 1;
        }
        if (code < 0 || (code > 0x10ffff && code <= KEY_FUNCTION_BASE))
            return 0;
        if (code == 9 && mods == 1 && action(m, out, 'i', 4 | 1, event))
            return (int)end + 1;
        if ((mods & 1) && code >= 'A' && code <= 'Z') {
            if (!parts[0][1])
                parts[0][1] = code;
            code = tolower(code);
        }
        key_output(m, out, code, mods, event, parts[0][1]);
        return (int)end + 1;
    }
    if (final == '~' && code == 27 && part == 2) {
        int key = parts[2][0], shifted = key;
        if ((mods & 1) && key >= 'A' && key <= 'Z')
            key = tolower(key);
        key_output(m, out, key, mods, event, shifted);
        return (int)end + 1;
    }
    int fn = final >= 'P' && final <= 'S' && code == 1 ? final - 'P' + 1
             : final == '~'                            ? function_number(code)
                                                       : 0;
    if (fn) {
        key_output(m, out, KEY_FUNCTION_BASE + fn, mods, event, 0);
        return (int)end + 1;
    }
    if (final == 'Z' && !params[0]) {
        /* tmux before 3.5 cannot represent Ctrl+Shift+I distinctly and
         * collapses it to BackTab. Recover it only when the user actually
         * configured that shortcut; otherwise preserve ordinary BackTab. */
        if (action(m, out, 'i', 4 | 1, 1))
            return (int)end + 1;
        key_output(m, out, 9, 1, 1, 0);
        return (int)end + 1;
    }
    if (strchr("ABCDHF", final) && (code == 1 || !params[0])) {
        if (event == 3) {
            action(m, out, 0, mods, event);
            return (int)end + 1;
        }
        if (m->open) {
            int key = final == 'A'   ? K_UP
                      : final == 'B' ? K_DOWN
                      : final == 'C' ? K_RIGHT
                      : final == 'D' ? K_LEFT
                      : final == 'H' ? K_HOME
                                     : K_END;
            menu_key(m, key);
        } else if (!mods)
            buf_printf(out, "\033[%c", final);
        else
            buf_printf(out, "\033[1;%d%c", mods + 1, final);
        return (int)end + 1;
    }
    /* Kitty adds event fields to Home/End/Delete tilde encodings too. */
    if (final == '~' && (code == 1 || code == 3 || code == 4 || code == 7 || code == 8)) {
        if (event == 3) {
            action(m, out, 0, mods, event);
            return (int)end + 1;
        }
        if (m->open)
            menu_key(m, code == 3 ? K_DELETE : code == 1 || code == 7 ? K_HOME : K_END);
        else if (!mods)
            buf_printf(out, "\033[%d~", code);
        else
            buf_printf(out, "\033[%d;%d~", code, mods + 1);
        return (int)end + 1;
    }
    return 0;
}
