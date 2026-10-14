/*
 * Copyright 2026 Secragon Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "privacy.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

static bool in(uint32_t c, const char *set) {
    return c && c < 128 && strchr(set, (int)c);
}
static size_t separated_value_start(const uint32_t *s, size_t len, size_t i) {
    bool separated = false;
    while (i < len && in(s[i], " \t")) {
        separated = true;
        i++;
    }
    if (!separated)
        return i;
    /* A shell removes backslash-newline pairs before parsing arguments. Keep
     * that syntax visible and continue looking for the separated value. */
    while (i < len && s[i] == '\\') {
        size_t newline = i + 1;
        if (newline < len && s[newline] == '\r')
            newline++;
        if (newline >= len || s[newline] != '\n')
            break;
        i = newline + 1;
        while (i < len && in(s[i], " \t"))
            i++;
    }
    return i;
}
size_t mask_value(const uint32_t *s, size_t len, size_t i, bool *hidden) {
    int quote = 0;
    for (; i < len; i++) {
        uint32_t c = s[i];
        if (quote == 1) {
            if (c == '\'')
                quote = 0;
            else {
                hidden[i] = true;
                if (c == '\\' && i + 1 < len)
                    hidden[++i] = true;
            }
        } else if (quote == '\'') {
            if (c == '\'')
                quote = 0;
            else
                hidden[i] = true;
        } else if (quote == '"') {
            if (c == '"')
                quote = 0;
            else {
                hidden[i] = true;
                if (c == '\\' && i + 1 < len && in(s[i + 1], "\\\"$`\n"))
                    hidden[++i] = true;
            }
        } else if (in(c, " \t\r\n;|&()<>"))
            break;
        else if (c == '$' && i + 1 < len && s[i + 1] == '\'') {
            hidden[i] = true;
            quote = 1;
            i++;
        } else if (c == '\'' || c == '"')
            quote = (int)c;
        else {
            hidden[i] = true;
            if (c == '\\' && i + 1 < len)
                hidden[++i] = true;
        }
    }
    return i;
}
/* Operate on Unicode codepoints mapped back to cells, never raw PTY bytes. */
void mask_positions(const uint32_t *s, size_t len, const Settings *rules, bool *hidden) {
    if (!rules->triggers_enabled)
        return;
    bool *excluded = NULL;
    if (rules->exclusions_enabled && rules->exception_count) {
        excluded = xcalloc(len ? len : 1, sizeof(bool));
        command_exceptions(s, len, rules, excluded);
    }
    size_t start = 0;
    while (start < len) {
        size_t found = len, end = 0;
        int best = -1;
        for (size_t p = start; p < len; p++) {
            if (excluded && excluded[p])
                continue;
            if (p && !iswspace(s[p - 1]) && !in(s[p - 1], ";|&()"))
                continue;
            size_t longest = 0;
            for (int r = 0; r < rules->count; r++) {
                const Rule *rule = &rules->rules[r];
                size_t n = strlen(rule->flag);
                if (n <= longest || n > len - p)
                    continue;
                size_t k = 0;
                while (k < n && s[p + k] == (unsigned char)rule->flag[k])
                    k++;
                if (k != n)
                    continue;
                size_t e = p + n;
                if (rule->mode == EQUALS && e < len && s[e] == '=')
                    e++;
                else if ((rule->mode == EQUALS || rule->mode == SEPARATED) && e < len &&
                         !in(s[e], " \t\r\n"))
                    continue;
                best = r;
                longest = n;
                end = e;
            }
            if (best >= 0) {
                found = p;
                break;
            }
        }
        if (found == len)
            break;
        size_t i = end;
        if (rules->rules[best].mode != ATTACHED && !in(s[end - 1], ":="))
            i = separated_value_start(s, len, i);
        i = mask_value(s, len, i, hidden);
        start = MAX(i, end);
    }
    free(excluded);
}
