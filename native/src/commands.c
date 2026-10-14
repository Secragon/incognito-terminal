/*
 * Copyright 2026 Secragon Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "privacy.h"
#include <string.h>
#include <wctype.h>

static bool has(uint32_t c, const char *set) {
    return c && c < 128 && strchr(set, (int)c);
}
static bool space(uint32_t c) {
    return c == ' ' || c == '\t' || c == '\r';
}
/* Recognize common rendered shell prompts, including the PoC's clean prompt.
 * Ordinary argument text is never searched for an exception name. */
static size_t prompt_end(const uint32_t *s, size_t n, size_t start) {
    size_t p = start;
    while (p < n && space(s[p]))
        p++;
    if (p < n && has(s[p], "$#%") && (p + 1 == n || space(s[p + 1])))
        return p + 1;
    bool decorated = false;
    size_t end = p;
    while (end < n && !space(s[end]) && s[end] != '\n') {
        if (has(s[end], "@:[]") || s[end] > 127)
            decorated = true;
        end++;
    }
    const char clean[] = "incognito-terminal>";
    bool clean_prompt = end - p == sizeof(clean) - 1;
    for (size_t i = 0; clean_prompt && i < sizeof(clean) - 1; i++)
        if (s[p + i] != (unsigned char)clean[i])
            clean_prompt = false;
    if (clean_prompt)
        return end;
    if (!decorated)
        return start;
    int quote = 0;
    for (size_t i = p; i < n && s[i] != '\n' && i - p < 256; i++) {
        if (s[i] == '\\' && quote != '\'' && i + 1 < n) {
            i++;
            continue;
        }
        if (quote) {
            if (s[i] == (uint32_t)quote)
                quote = 0;
            continue;
        }
        if (has(s[i], "'\"")) {
            quote = (int)s[i];
            continue;
        }
        if (has(s[i], "$#%>") && (i + 1 == n || space(s[i + 1])))
            return i + 1;
        if (has(s[i], ";|&"))
            break;
    }
    return start;
}
static bool excluded_name(const Settings *settings, const char *name) {
    for (int i = 0; i < settings->exception_count; i++)
        if (!strcmp(name, settings->exceptions[i]))
            return true;
    return false;
}
/* Tokenize enough shell syntax to keep exclusions inside their own command.
 * Names are unquoted/unescaped and compared by basename. Expansions and aliases
 * are not evaluated; uncertain command identities keep normal masking. */
void command_exceptions(const uint32_t *s, size_t n, const Settings *settings, bool *excluded) {
    if (!settings->exclusions_enabled)
        return;
    bool need_command = true, active = false, redirect = false;
    bool sudo_prefix = false, sudo_value = false;
    size_t i = prompt_end(s, n, 0);
    while (i < n) {
        if (space(s[i])) {
            excluded[i++] = active;
            continue;
        }
        if (has(s[i], "\n;|&()")) {
            bool newline = s[i] == '\n';
            i++;
            need_command = true;
            active = redirect = sudo_prefix = sudo_value = false;
            if (newline)
                i = prompt_end(s, n, i);
            continue;
        }
        if (has(s[i], "<>")) {
            excluded[i++] = active;
            redirect = true;
            continue;
        }
        size_t start = i;
        char token[256] = {0}, name[MAX_COMMAND + 1] = {0};
        size_t used = 0, named = 0;
        bool plain = true, overflow = false, name_overflow = false;
        int quote = 0;
        while (i < n) {
            uint32_t c = s[i];
            if (!quote && (space(c) || has(c, "\n;|&()<>")))
                break;
            if (has(c, "'\"") && (!quote || c == (uint32_t)quote)) {
                quote = quote ? 0 : (int)c;
                plain = false;
                i++;
                continue;
            }
            if (c == '\\' && quote != '\'' && i + 1 < n) {
                plain = false;
                c = s[++i];
            }
            if (used + 1 < sizeof(token))
                token[used++] = c < 128 ? (char)c : '?';
            else
                overflow = true;
            if (c == '/') {
                named = 0;
                name_overflow = false;
                name[0] = 0;
            } else if (named < MAX_COMMAND) {
                name[named++] = c < 128 ? (char)c : '?';
                name[named] = 0;
            } else
                name_overflow = true;
            i++;
        }
        if (redirect)
            redirect = false;
        else if (need_command) {
            bool assignment = plain && !overflow;
            size_t equal = 0;
            while (token[equal] && token[equal] != '=') {
                unsigned char c = (unsigned char)token[equal];
                if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (equal && c >= '0' && c <= '9')))
                    assignment = false;
                equal++;
            }
            assignment = assignment && equal > 0 && token[equal] == '=';
            bool fd = plain && used > 0 && i < n && has(s[i], "<>");
            for (size_t k = 0; k < used; k++)
                if (token[k] < '0' || token[k] > '9')
                    fd = false;
            if (sudo_value)
                sudo_value = false;
            else if (assignment ||
                     fd) { /* A leading assignment/redirection is not a command name. */
            } else if (sudo_prefix && plain && token[0] == '-') {
                if (!strcmp(token, "--"))
                    sudo_prefix = false;
                else if (!strcmp(token, "-u") || !strcmp(token, "-g") || !strcmp(token, "--user") ||
                         !strcmp(token, "--group"))
                    sudo_value = true;
                else if (strcmp(token, "-n") && strcmp(token, "-E") && strcmp(token, "-H") &&
                         strcmp(token, "-S") && strcmp(token, "-k"))
                    need_command = false;
            } else if (!name_overflow && excluded_name(settings, name)) {
                active = true;
                need_command = false;
            } else if (!name_overflow && (!strcmp(name, "sudo") || !strcmp(name, "command") ||
                                          !strcmp(name, "exec"))) {
                sudo_prefix = true;
            } else {
                active = false;
                need_command = false;
            }
        }
        for (size_t p = start; p < i; p++)
            excluded[p] = active;
    }
}
