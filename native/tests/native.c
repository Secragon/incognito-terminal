/*
 * Copyright 2026 Secragon Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
/* Native unit/regression tests. All secrets are disposable fixtures. */
#include "privacy.h"
#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

static size_t assertions;
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        assertions++;                                                                              \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static void mask_case(const char *source, const char *expected, const Settings *rules) {
    uint32_t *cp = xcalloc(strlen(source) + 1, sizeof(uint32_t));
    size_t n = 0;
    mbstate_t state = {0};
    for (const char *p = source; *p;) {
        wchar_t c;
        size_t k = mbrtowc(&c, p, strlen(p), &state);
        CHECK(k != (size_t)-1 && k != (size_t)-2);
        cp[n++] = (uint32_t)c;
        p += k;
    }
    bool *hidden = xcalloc(n + 1, sizeof(bool));
    mask_positions(cp, n, rules, hidden);
    Buffer out = {0};
    for (size_t i = 0; i < n; i++)
        buf_cp(&out, hidden[i] && cp[i] != '\r' && cp[i] != '\n' ? '*' : cp[i]);
    if (strcmp(out.data ? out.data : "", expected))
        fprintf(stderr, "mask mismatch for %s\nexpected %s\nactual %s\n", source, expected,
                out.data);
    CHECK(!strcmp(out.data ? out.data : "", expected));
    free(cp);
    free(hidden);
    buf_free(&out);
}
static void detector(void) {
    Settings rules;
    settings_default(&rules);
    const char *prefixes[] = {"-p",           "-p ",         "-p   ", "-p\t",    "--password ",
                              "--password\t", "--password=", "/p:",   "--pass ", "--pass=",
                              "-P",           "-P ",         "-P   ", "-P\t"};
    const char *values[][2] = {{"Q9xy", "****"},
                               {"'Q9xy'", "'****'"},
                               {"\"Q9xy\"", "\"****\""},
                               {"''", "''"},
                               {"\"\"", "\"\""},
                               {"'two words'", "'*********'"},
                               {"\"two words\"", "\"*********\""},
                               {"abc'dcb'", "***'***'"},
                               {"abc\"dcb\"", "***\"***\""},
                               {"\"abc'def\"", "\"*******\""},
                               {"'abc\"def'", "'*******'"},
                               {"abc\\'def", "********"},
                               {"abc\\\"def", "********"},
                               {"ab\\ cd", "******"},
                               {"ab\\\\cd", "******"},
                               {"\"ab\\\" cd\"", "\"*******\""},
                               {"$'ab\\' cd'", "*'*******'"},
                               {"'!;|&()<>'", "'********'"},
                               {"'abc'\"def\"ghi", "'***'\"***\"***"},
                               {"'密🔐码'", "'***'"}};
    for (size_t p = 0; p < sizeof(prefixes) / sizeof(*prefixes); p++)
        for (size_t v = 0; v < 20; v++) {
            char source[256], expected[256];
            snprintf(source, sizeof(source), "record %s%s tail", prefixes[p], values[v][0]);
            snprintf(expected, sizeof(expected), "record %s%s tail", prefixes[p], values[v][1]);
            mask_case(source, expected, &rules);
            /* Each completed Unicode prefix must already be concealed. */
            const char *val = values[v][0], *exp = values[v][1];
            mbstate_t state = {0};
            size_t bytes = 0, chars = 0;
            while (bytes <= strlen(val)) {
                snprintf(source, sizeof(source), "record %s%.*s", prefixes[p], (int)bytes, val);
                snprintf(expected, sizeof(expected), "record %s%.*s", prefixes[p], (int)chars, exp);
                mask_case(source, expected, &rules);
                if (!val[bytes])
                    break;
                wchar_t c;
                size_t k = mbrtowc(&c, val + bytes, strlen(val) - bytes, &state);
                CHECK(k != (size_t)-1 && k != (size_t)-2);
                bytes += k;
                chars++;
            }
        }
    const char *edges[][2] = {
        {"cmd /p: next", "cmd /p: next"},
        {"cmd --password= next", "cmd --password= next"},
        {"cmd --password-file file --passwordless --Password value",
         "cmd --password-file file --passwordless --Password value"},
        {"cmd word-p word/p:secret", "cmd word-p word/p:secret"},
        {"cmd -p one&&cmd /p:two|cmd --password=three",
         "cmd -p ***&&cmd /p:***|cmd --password=*****"},
        {"cmd -p ab\\;cd>file", "cmd -p ******>file"},
        {"cmd -p\nnext", "cmd -p\nnext"},
        {"cmd -p \\\nsecret", "cmd -p \\\n******"},
        {"cmd --password \\\n  secret tail", "cmd --password \\\n  ****** tail"},
        {"cmd --pass \\\n\\\nsecret", "cmd --pass \\\n\\\n******"},
        {"cmd -p 'a\nb' next", "cmd -p '*\n*' next"}};
    for (size_t i = 0; i < sizeof(edges) / sizeof(*edges); i++)
        mask_case(edges[i][0], edges[i][1], &rules);
    const char *attached[] = {"cmd -P******", "cmd -Psecret", "cmd -P******", "cmd -Psecret"};
    const char *spaced[] = {"cmd -P secret", "cmd -P ******", "cmd -P ******", "cmd -P ******"};
    const char *equals[] = {"cmd -P*******", "cmd -P=secret", "cmd -P*******", "cmd -P=******"};
    rules.count = 1;
    strcpy(rules.rules[0].flag, "-P");
    for (int mode = 0; mode < 4; mode++) {
        rules.rules[0].mode = (ValueMode)mode;
        mask_case("cmd -Psecret", attached[mode], &rules);
        mask_case("cmd -P secret", spaced[mode], &rules);
        mask_case("cmd -P=secret", equals[mode], &rules);
    }
    rules.count = 2;
    rules.rules[0] = (Rule){"-p", BOTH};
    rules.rules[1] = (Rule){"-pass", EQUALS};
    mask_case("cmd -pass secret", "cmd -pass ******", &rules);
}
static void rendered_text(const Screen *s, Buffer *out) {
    for (int y = 0; y < s->rows; y++) {
        for (int x = 0; x < s->cols; x++) {
            const VTermScreenCell *v = &s->last_frame[y * s->cols + x].v;
            if (!v->width)
                continue;
            if (!v->chars[0])
                buf_str(out, " ");
            else
                for (int k = 0; k < VTERM_MAX_CHARS_PER_CELL && v->chars[k]; k++)
                    buf_cp(out, v->chars[k]);
        }
        buf_str(out, "\n");
    }
}
static void screen_cases(void) {
    Settings settings;
    settings_default(&settings);
    Menu menu;
    menu_init(&menu, &settings, NORMAL);
    Screen s;
    screen_init(&s, 5, 40);
    Buffer frame = {0};
    const char *palette = "\033[33mY\033[43mB\033[93mH\033[103mI\033[38;5;200mP\033[38;2;1;2;3mT";
    screen_feed(&s, palette, strlen(palette));
    screen_render(&s, &menu, &frame);
    CHECK(strstr(frame.data, "\033[33m"));
    CHECK(strstr(frame.data, "\033[43m"));
    CHECK(strstr(frame.data, "\033[93m"));
    CHECK(strstr(frame.data, "\033[103m"));
    CHECK(strstr(frame.data, "\033[38;5;200m"));
    CHECK(strstr(frame.data, "\033[38;2;1;2;3m"));
    CHECK(!strstr(frame.data, "\033[38;5;3m"));
    CHECK(!strstr(frame.data, "\033[48;5;3m"));
    screen_free(&s);
    buf_free(&frame);
    screen_init(&s, 5, 40);
    const char *source = "cmd -p UNIQUE_SECRET next";
    for (const char *p = source; *p; p++) {
        screen_feed(&s, p, 1);
        screen_render(&s, &menu, &frame);
        CHECK(!strstr(frame.data, "UNIQUE"));
        buf_consume(&frame, frame.len);
    }
    screen_feed(&s, "\033[1;5Hxx", 9);
    screen_render(&s, &menu, &frame);
    CHECK(!strstr(frame.data, "UNIQUE"));
    buf_consume(&frame, frame.len);
    screen_resize(&s, 3, 20, &settings);
    screen_render(&s, &menu, &frame);
    CHECK(!strstr(frame.data, "UNIQUE"));
    screen_free(&s);
    buf_free(&frame);
    for (int prefix = 0; prefix < 3; prefix++) {
        const char *starts[] = {"cmd -p ", "cmd --password=", "command /p:"};
        screen_init(&s, 3, 10);
        for (const char *p = starts[prefix]; *p; p++)
            screen_feed(&s, p, 1);
        for (int i = 0; i < 100; i++) {
            screen_feed(&s, "Q", 1);
            screen_render(&s, &menu, &frame);
            CHECK(!strchr(frame.data, 'Q'));
            buf_consume(&frame, frame.len);
        }
        screen_free(&s);
        buf_free(&frame);
    }
    const char *unicode[] = {"cmd -p '密🔐码' next", "cmd -p 'éXYZ' next", "cmd -p 'a\tXYZ' next"};
    const char *masked[] = {"cmd -p '******' next", "cmd -p '****' next",
                            "cmd -p '***********' next"};
    for (int i = 0; i < 3; i++) {
        screen_init(&s, 4, 40);
        for (const char *p = unicode[i]; *p; p++) {
            screen_feed(&s, p, 1);
            screen_render(&s, &menu, &frame);
        }
        Buffer visible = {0};
        rendered_text(&s, &visible);
        CHECK(strstr(visible.data, masked[i]));
        buf_free(&visible);
        screen_free(&s);
        buf_free(&frame);
    }
    const char *openings[] = {"\033]0;", "\033]52;c;", "\033P", "\033_", "\033^", "\xc2\x9d"};
    for (int i = 0; i < 6; i++) {
        screen_init(&s, 4, 40);
        char payload[128];
        snprintf(payload, sizeof(payload), "%sHIDDEN_METADATA\033\\safe", openings[i]);
        for (const char *p = payload; *p; p++) {
            screen_feed(&s, p, 1);
            screen_render(&s, &menu, &frame);
            CHECK(!strstr(frame.data, "HIDDEN_METADATA"));
        }
        CHECK(strstr(frame.data, "safe"));
        screen_free(&s);
        buf_free(&frame);
    }
    screen_init(&s, 4, 40);
    screen_feed(&s, "abc\033[6n", 7);
    CHECK(s.responses.len == 6 && !memcmp(s.responses.data, "\033[1;4R", 6));
    screen_free(&s);
    /* Preserve the main buffer across child alternate-screen use. */
    screen_init(&s, 4, 40);
    screen_feed(&s, "cmd -p SECRET", 13);
    screen_render(&s, &menu, &frame);
    buf_consume(&frame, frame.len);
    screen_feed(&s, "\033[?1049hother\033[?1049l", 21);
    screen_render(&s, &menu, &frame);
    CHECK(!strstr(frame.data, "SECRET"));
    CHECK(strstr(frame.data, "******"));
    screen_free(&s);
    buf_free(&frame);
}
static void config_and_menu(void) {
    char directory[] = "/tmp/privacy-native-tests-XXXXXX";
    CHECK(mkdtemp(directory));
    setenv("XDG_CONFIG_HOME", directory, 1);
    Settings s;
    CHECK(!settings_load(&s));
    CHECK(s.history == NORMAL && s.count == 5);
    s.history = OFF;
    CHECK(!settings_save(&s, true));
    s.rules[s.count++] = (Rule){"-Q", BOTH};
    CHECK(!settings_save(&s, false));
    CHECK(!settings_load(&s));
    CHECK(s.history == OFF && s.count == 6);
    char path[4096];
    snprintf(path, sizeof(path), "%s/incognito-terminal/config.json", directory);
    struct stat st;
    CHECK(!stat(path, &st) && (st.st_mode & 0777) == 0600);
    Menu menu;
    menu_init(&menu, &s, NORMAL);
    Router r = {0};
    Buffer out = {0};
    const char *keys = "\033OQte\025-X\r";
    for (const char *p = keys; *p; p++)
        router_feed(&r, &menu, &out, p, 1);
    CHECK(menu.open && menu.page == TRIGGERS && !strcmp(menu.settings.rules[0].flag, "-X"));
    CHECK(!out.len);
    keys = "a/key:\t\033[C \r";
    router_feed(&r, &menu, &out, keys, strlen(keys));
    CHECK(menu.settings.rules[6].mode == ATTACHED);
    keys = "d\033OQ";
    router_feed(&r, &menu, &out, keys, strlen(keys));
    CHECK(!menu.open && menu.settings.count == 6);
    const char paste[] = "\033[200~literal \033OQ \003\r\033[201~";
    for (size_t i = 0; i < sizeof(paste) - 1; i++)
        router_feed(&r, &menu, &out, paste + i, 1);
    CHECK(out.len == sizeof(paste) - 1 && !memcmp(out.data, paste, out.len));
    CHECK(!menu.open);
    buf_consume(&out, out.len);
    keys = "\033OQho\r";
    router_feed(&r, &menu, &out, keys, strlen(keys));
    CHECK(menu.active_history == NORMAL && menu.settings.history == OFF);
    CHECK(strstr(menu.message, "Saved."));
    keys = "\033";
    router_feed(&r, &menu, &out, keys, 1);
    r.since = monotime() - 1;
    router_flush(&r, &menu, &out);
    CHECK(menu.page == HOME);
    keys = "ta\033[200~-Z\r\003\033[201~";
    router_feed(&r, &menu, &out, keys, strlen(keys));
    CHECK(menu.page == EDITOR && !strcmp(menu.edit.flag, "-Z"));
    menu_key(&menu, K_ENTER);
    CHECK(menu.page == TRIGGERS);
    menu_key(&menu, 'e');
    menu_key(&menu, K_CLEAR);
    menu_key(&menu, 'x');
    menu_key(&menu, K_ENTER);
    CHECK(menu.page == EDITOR);
    CHECK(strstr(menu.message, "Use flags"));
    for (int page = HOME; page <= SHORTCUT_EDITOR; page++) {
        menu.page = (Page)page;
        for (int width = 2; width < 120; width += 7)
            for (int height = 2; height < 30; height += 3) {
                Cell *g = xcalloc((size_t)width * (size_t)height, sizeof(Cell));
                VTermPos cursor;
                menu_draw(&menu, g, height, width, &cursor);
                CHECK(cursor.row < height && cursor.col < width);
                free(g);
            }
    }
    FILE *f = fopen(path, "w");
    CHECK(f);
    fputs("{\"triggers\":[\"-p\",\"/p:\"]}", f);
    fclose(f);
    CHECK(!settings_load(&s) && s.history == NORMAL && s.rules[1].mode == ATTACHED);
    f = fopen(path, "w");
    CHECK(f);
    fputs("{broken", f);
    fclose(f);
    CHECK(settings_load(&s) < 0);
    buf_free(&r.pending);
    buf_free(&out);
    unlink(path);
    snprintf(path, sizeof(path), "%s/incognito-terminal", directory);
    rmdir(path);
    rmdir(directory);
}
static void keys(Router *r, Menu *m, Buffer *out, const char *s) {
    router_feed(r, m, out, s, strlen(s));
    CHECK(!out->len);
}
static bool same_settings(const Settings *a, const Settings *b) {
    if (a->count != b->count || a->history != b->history ||
        a->triggers_enabled != b->triggers_enabled ||
        a->exclusions_enabled != b->exclusions_enabled ||
        memcmp(a->shortcuts, b->shortcuts, sizeof(a->shortcuts)) ||
        a->exception_count != b->exception_count || a->privacy_style != b->privacy_style ||
        a->seen_trigger_count != b->seen_trigger_count ||
        a->seen_exception_count != b->seen_exception_count)
        return false;
    for (int i = 0; i < a->count; i++)
        if (strcmp(a->rules[i].flag, b->rules[i].flag) || a->rules[i].mode != b->rules[i].mode)
            return false;
    for (int i = 0; i < a->exception_count; i++)
        if (strcmp(a->exceptions[i], b->exceptions[i]))
            return false;
    for (int i = 0; i < a->seen_trigger_count; i++)
        if (strcmp(a->seen_triggers[i], b->seen_triggers[i]))
            return false;
    for (int i = 0; i < a->seen_exception_count; i++)
        if (strcmp(a->seen_exceptions[i], b->seen_exceptions[i]))
            return false;
    return true;
}
static void default_updates(void) {
    Settings defaults, future, s;
    settings_default(&defaults);
    future = defaults;
    future.rules[future.count++] = (Rule){"--token", EQUALS};
    strcpy(future.exceptions[future.exception_count++], "future-tool");
    s = defaults;
    /* Keep customized formats/order and custom entries; preserve old removals. */
    s.rules[0].mode = SEPARATED;
    s.rules[s.count++] = (Rule){"-Q", ATTACHED};
    strcpy(s.exceptions[s.exception_count++], "custom-net");
    memmove(s.rules + 1, s.rules + 2, (size_t)(s.count - 2) * sizeof(s.rules[0]));
    s.count--; /* --password was deliberately removed. */
    memmove(s.exceptions + 1, s.exceptions + 2,
            (size_t)(s.exception_count - 2) * sizeof(s.exceptions[0]));
    s.exception_count--; /* nmap was deliberately removed. */
    Settings before = s;
    settings_merge_defaults(&s, &future);
    CHECK(s.count == before.count + 1 && s.exception_count == before.exception_count + 1);
    CHECK(s.rules[0].mode == SEPARATED && !strcmp(s.rules[s.count - 2].flag, "-Q"));
    CHECK(!strcmp(s.rules[s.count - 1].flag, "--token"));
    CHECK(!strcmp(s.exceptions[s.exception_count - 2], "custom-net"));
    CHECK(!strcmp(s.exceptions[s.exception_count - 1], "future-tool"));
    for (int i = 0; i < before.count; i++)
        CHECK(!strcmp(s.rules[i].flag, before.rules[i].flag) &&
              s.rules[i].mode == before.rules[i].mode);
    for (int i = 0; i < before.exception_count; i++)
        CHECK(!strcmp(s.exceptions[i], before.exceptions[i]));
    before = s;
    settings_merge_defaults(&s, &future);
    CHECK(same_settings(&s, &before)); /* Repeated upgrades are idempotent. */
    s.count--;                         /* Removing a newly added default is remembered too. */
    s.exception_count--;
    before = s;
    settings_merge_defaults(&s, &future);
    CHECK(same_settings(&s, &before));
    /* A default that disappears upstream and returns remains deliberately removed. */
    settings_merge_defaults(&s, &defaults);
    settings_merge_defaults(&s, &future);
    CHECK(same_settings(&s, &before));
    /* A custom rule/name matching a new default wins without a duplicate. */
    s = defaults;
    s.rules[s.count++] = (Rule){"--token", ATTACHED};
    strcpy(s.exceptions[s.exception_count++], "future-tool");
    settings_merge_defaults(&s, &future);
    CHECK(s.count == future.count && s.rules[s.count - 1].mode == ATTACHED);
    CHECK(s.exception_count == future.exception_count);
    /* An explicitly empty exclusions list remains empty across updates. */
    s = defaults;
    s.exception_count = 0;
    settings_merge_defaults(&s, &future);
    CHECK(!s.exception_count && s.seen_exception_count == future.exception_count);
    /* Full lists retain every custom entry and retry when space becomes available. */
    s = defaults;
    while (s.count < MAX_RULES) {
        snprintf(s.rules[s.count].flag, sizeof(s.rules[0].flag), "--custom%d", s.count);
        s.rules[s.count++].mode = EQUALS;
    }
    while (s.exception_count < MAX_EXCEPTIONS) {
        snprintf(s.exceptions[s.exception_count], sizeof(s.exceptions[0]), "custom%d",
                 s.exception_count);
        s.exception_count++;
    }
    before = s;
    settings_merge_defaults(&s, &future);
    CHECK(same_settings(&s, &before));
    s.count--;
    s.exception_count--;
    settings_merge_defaults(&s, &future);
    CHECK(s.count == MAX_RULES && !strcmp(s.rules[MAX_RULES - 1].flag, "--token"));
    CHECK(s.exception_count == MAX_EXCEPTIONS &&
          !strcmp(s.exceptions[MAX_EXCEPTIONS - 1], "future-tool"));
    /* Persisted tracking merges a simulated previous release with custom lists. */
    char directory[] = "/tmp/privacy-default-update-tests-XXXXXX";
    CHECK(mkdtemp(directory) && !setenv("XDG_CONFIG_HOME", directory, 1));
    char path[4096];
    snprintf(path, sizeof(path), "%s/incognito-terminal", directory);
    CHECK(!mkdir(path, 0700));
    snprintf(path, sizeof(path), "%s/incognito-terminal/config.json", directory);
    FILE *f = fopen(path, "w");
    CHECK(f);
    fputs("{\"version\":6,\"history_mode\":\"session\","
          "\"triggers\":[{\"flag\":\"-p\",\"value_mode\":\"separated\"},\"/p:\",\"-Q\"],"
          "\"exceptions\":[\"ssh\",\"custom-net\",\"cp\"],"
          "\"defaults_seen\":{\"triggers\":[\"-p\",\"--password\",\"/p:\"],"
          "\"exceptions\":[\"ssh\",\"nmap\",\"nc\",\"docker\",\"psql\",\"redis-cli\",\"mkdir\","
          "\"scp\"]}}",
          f);
    fclose(f);
    CHECK(!settings_load(&s) && s.count == 5 && s.exception_count == 91 && s.history == SESSION);
    CHECK(s.rules[0].mode == SEPARATED && !strcmp(s.rules[2].flag, "-Q"));
    CHECK(!strcmp(s.rules[3].flag, "--pass") && !strcmp(s.rules[4].flag, "-P"));
    CHECK(s.seen_trigger_count == 5 && s.seen_exception_count == 97);
    CHECK(!strcmp(s.exceptions[1], "custom-net") && !strcmp(s.exceptions[2], "cp"));
    mask_case("nmap -p SECRET", "nmap -p ******", &s);
    mask_case("lsof -p 1234", "lsof -p 1234", &s);
    CHECK(!settings_save(&s, SAVE_HISTORY));
    Settings loaded;
    CHECK(!settings_load(&loaded) && same_settings(&s, &loaded));
    /* Renaming/removing a bundled item in the real menus does not resurrect it. */
    Menu m;
    menu_init(&m, &s, SESSION);
    Router r = {0};
    Buffer out = {0};
    keys(&r, &m, &out, "\033OQte\025--custom-password\r");
    CHECK(!settings_load(&loaded) && !strcmp(loaded.rules[0].flag, "--custom-password"));
    mask_case("record -p SECRET", "record -p SECRET", &loaded);
    keys(&r, &m, &out, "\033OQ\033OQxd"); /* Remove ssh. */
    CHECK(!settings_load(&loaded) && strcmp(loaded.exceptions[0], "ssh"));
    mask_case("ssh --pass SECRET", "ssh --pass ******", &loaded);
    loaded.history = OFF;
    CHECK(!settings_save(&loaded, SAVE_HISTORY));
    CHECK(!settings_load(&s) && same_settings(&s, &loaded));
    /* Missing or malformed tracking in schema 6 is rejected. */
    const char *invalid[] = {
        "{\"version\":6,\"triggers\":[\"-p\"]}",
        "{\"version\":6,\"triggers\":[\"-p\"],\"defaults_seen\":{\"triggers\":[\"bad\"],"
        "\"exceptions\":[]}}",
        "{\"version\":6,\"triggers\":[\"-p\"],\"defaults_seen\":{\"triggers\":[],\"exceptions\":["
        "\"bad/name\"]}}",
        "{\"version\":6,\"triggers\":[\"-p\"],\"defaults_seen\":{\"triggers\":[],\"exceptions\":"
        "null}}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        f = fopen(path, "w");
        CHECK(f);
        fputs(invalid[i], f);
        fclose(f);
        CHECK(settings_load(&s) < 0);
    }
    buf_free(&r.pending);
    buf_free(&out);
    unlink(path);
    snprintf(path, sizeof(path), "%s/incognito-terminal", directory);
    rmdir(path);
    rmdir(directory);
}
static void menu_functions(void) {
    char directory[] = "/tmp/privacy-menu-tests-XXXXXX";
    CHECK(mkdtemp(directory));
    CHECK(!setenv("XDG_CONFIG_HOME", directory, 1));
    Settings settings;
    settings_default(&settings);
    CHECK(!settings_save(&settings, false));
    Menu m;
    menu_init(&m, &settings, NORMAL);
    Router r = {0};
    Buffer out = {0};
    /* Both F2 encodings, fragmented at every byte boundary. */
    const char *f2[] = {"\033OQ", "\033[12~"};
    for (int i = 0; i < 2; i++) {
        for (const char *p = f2[i]; *p; p++)
            router_feed(&r, &m, &out, p, 1);
        CHECK(m.open && m.page == HOME);
        menu_key(&m, K_F2);
        CHECK(!m.open && !out.len);
    }
    keys(&r, &m, &out, "\033OQ\r");
    CHECK(m.enabled && m.page == PRIVACY); /* Opening a dialog does not change privacy. */
    keys(&r, &m, &out, "F\r");
    CHECK(!m.enabled);
    menu_key(&m, K_ESC);
    keys(&r, &m, &out, "PO\r");
    menu_key(&m, K_ESC);
    keys(&r, &m, &out, "\033[B\033[B\r");
    CHECK(m.enabled && m.page == TRIGGERS);
    keys(&r, &m, &out, "\033[F\033[A");
    CHECK(m.selected == m.settings.count - 2);
    keys(&r, &m, &out, "\033[H\033[B");
    CHECK(m.selected == 1);
    keys(&r, &m, &out, "\033[H\r\033[H\033[C\177P");
    CHECK(m.page == EDITOR && !strcmp(m.edit.flag, "Pp"));
    keys(&r, &m, &out, "\025-QX\033[D\033[3~");
    CHECK(!strcmp(m.edit.flag, "-Q") && m.position == 2);
    keys(&r, &m, &out, "\033[H\033[C\033[C\033[F\t");
    CHECK(m.focus == 1 && m.edit.mode == BOTH);
    keys(&r, &m, &out, "\033[D");
    CHECK(m.edit.mode == SEPARATED);
    keys(&r, &m, &out, "\033[A");
    CHECK(m.edit.mode == ATTACHED);
    keys(&r, &m, &out, "\033[A");
    CHECK(m.edit.mode == EQUALS);
    keys(&r, &m, &out, " \033[B");
    CHECK(m.edit.mode == SEPARATED);
    keys(&r, &m, &out, "\t\r");
    CHECK(m.page == TRIGGERS && m.settings.rules[0].mode == SEPARATED);
    CHECK(!settings_load(&settings) && settings.rules[0].mode == SEPARATED);
    /* Canceling edits never changes the active or saved rule. */
    Settings before = m.settings;
    keys(&r, &m, &out, "e\025-X\003");
    CHECK(m.page == TRIGGERS && !memcmp(&m.settings, &before, sizeof(before)));
    keys(&r, &m, &out, "e\025-X");
    menu_key(&m, K_ESC);
    CHECK(m.page == TRIGGERS);
    keys(&r, &m, &out, "e\025-X\033OQ");
    CHECK(!m.open && !memcmp(&m.settings, &before, sizeof(before)));
    keys(&r, &m, &out, "\033OQTa/key:\t\033[C \033[A\r");
    CHECK(m.page == TRIGGERS && m.settings.rules[5].mode == ATTACHED);
    keys(&r, &m, &out, "a--token=\t\033[D\033[B\r");
    CHECK(m.settings.rules[6].mode == ATTACHED);
    keys(&r, &m, &out, "a-Q\r");
    CHECK(m.page == EDITOR && strstr(m.message, "already exists"));
    keys(&r, &m, &out, "\025invalid\r");
    CHECK(m.page == EDITOR && strstr(m.message, "Use flags"));
    keys(&r, &m, &out, "\003\033[F\033[3~");
    CHECK(m.page == TRIGGERS && m.settings.count == 6);
    /* History shortcuts, arrows, Tab, pending selection, and saved mode. */
    menu_key(&m, K_ESC);
    keys(&r, &m, &out, "H");
    CHECK(m.page == HISTORY && m.history_selected == NORMAL);
    keys(&r, &m, &out, "\033[A");
    CHECK(m.history_selected == OFF);
    keys(&r, &m, &out, "\t");
    CHECK(m.history_selected == NORMAL);
    keys(&r, &m, &out, "\033[B");
    CHECK(m.history_selected == SESSION);
    for (int i = 0; i < 3; i++) {
        char sequence[] = {'N', '\r', 0};
        sequence[0] = "NSO"[i];
        keys(&r, &m, &out, sequence);
        CHECK((int)m.settings.history == i && m.active_history == NORMAL);
        CHECK(strstr(m.message, "Saved."));
        CHECK(!settings_load(&settings) && (int)settings.history == i);
    }
    keys(&r, &m, &out, "N");
    menu_key(&m, K_ESC);
    CHECK(m.settings.history == OFF); /* Escape doesn't save selection. */
    m.home_selected = 0;
    keys(&r, &m, &out, "\033[A\033[A\033[A\r");
    CHECK(m.page == HISTORY);
    menu_key(&m, K_ESC);
    keys(&r, &m, &out, "\t\t\t\r");
    CHECK(m.enabled && m.page == PRIVACY); /* Wrap Home selection from History to Privacy. */
    keys(&r, &m, &out, "f\r");
    CHECK(!m.enabled);
    menu_key(&m, K_ESC);
    keys(&r, &m, &out, "po\r");
    menu_key(&m, K_ESC);
    /* Unknown terminal/Alt keys must not activate menu shortcuts. */
    keys(&r, &m, &out, "\033t\033p\033[13~");
    CHECK(m.open && m.enabled && m.page == HOME);
    /* Deterministic save failure, including when running tests as root. */
    char path[4096], backup[4096];
    snprintf(path, sizeof(path), "%s/incognito-terminal/config.json", directory);
    snprintf(backup, sizeof(backup), "%s/incognito-terminal/config.backup", directory);
    CHECK(!rename(path, backup) && !mkdir(path, 0700));
    before = m.settings;
    keys(&r, &m, &out, "ta-X\r");
    CHECK(m.page == EDITOR && strstr(m.message, "Could not save"));
    CHECK(!memcmp(&m.settings, &before, sizeof(before)));
    keys(&r, &m, &out, "\003");
    menu_key(&m, K_ESC);
    keys(&r, &m, &out, "hn\r");
    CHECK(strstr(m.message, "Could not save") && m.settings.history == OFF);
    CHECK(!rmdir(path) && !rename(backup, path));
    CHECK(!settings_load(&settings) && same_settings(&settings, &before));
    /* One-rule minimum and 32-rule maximum. */
    settings_default(&settings);
    settings.count = 1;
    CHECK(!settings_save(&settings, false));
    menu_init(&m, &settings, NORMAL);
    keys(&r, &m, &out, "\033OQtd");
    CHECK(m.settings.count == 1 && strstr(m.message, "between 1 and 32"));
    settings.count = MAX_RULES;
    for (int i = 0; i < MAX_RULES; i++) {
        snprintf(settings.rules[i].flag, sizeof(settings.rules[i].flag), "--flag%d", i);
        settings.rules[i].mode = EQUALS;
    }
    CHECK(!settings_save(&settings, false));
    menu_init(&m, &settings, NORMAL);
    keys(&r, &m, &out, "\033OQta-new\r");
    CHECK(m.page == EDITOR && m.settings.count == MAX_RULES);
    CHECK(strstr(m.message, "between 1 and 32"));
    /* Privacy Off/On applies to the C renderer, and menu covers child text. */
    Screen screen;
    settings_default(&settings);
    menu_init(&m, &settings, NORMAL);
    screen_init(&screen, 23, 100);
    const char *command = "cmd -p MENU_SECRET";
    screen_feed(&screen, command, strlen(command));
    Buffer frame = {0};
    screen_render(&screen, &m, &frame);
    CHECK(!strstr(frame.data, "MENU_SECRET"));
    buf_consume(&frame, frame.len);
    keys(&r, &m, &out, "\033OQpf\r");
    screen_render(&screen, &m, &frame);
    CHECK(!strstr(frame.data, "MENU_SECRET"));
    buf_consume(&frame, frame.len);
    keys(&r, &m, &out, "\033OQ");
    screen_render(&screen, &m, &frame);
    CHECK(strstr(frame.data, "MENU_SECRET"));
    buf_consume(&frame, frame.len);
    keys(&r, &m, &out, "\033OQpo\r\033OQ");
    screen_render(&screen, &m, &frame);
    CHECK(!strstr(frame.data, "MENU_SECRET"));
    buf_free(&frame);
    screen_free(&screen);
    buf_free(&r.pending);
    buf_free(&out);
    unlink(path);
    snprintf(path, sizeof(path), "%s/incognito-terminal", directory);
    rmdir(path);
    rmdir(directory);
}

static void exception_cases(void) {
    Settings s;
    settings_default(&s);
    const char *cases[][2] = {
        {"nmap -p 80,443 --pass VISIBLE", "nmap -p 80,443 --pass VISIBLE"},
        {"ssh -p 2222", "ssh -p 2222"},
        {"lsof -p 1234", "lsof -p 1234"},
        {"ping -p deadbeef example.test", "ping -p deadbeef example.test"},
        {"cp -p source destination", "cp -p source destination"},
        {"cp -P source destination", "cp -P source destination"},
        {"grep -P pattern file", "grep -P pattern file"},
        {"htop -p 1234", "htop -p 1234"},
        {"gcc -P -E source.c", "gcc -P -E source.c"},
        {"sudo /bin/cp -p source destination; nxc -p SECRET",
         "sudo /bin/cp -p source destination; nxc -p ******"},
        {"lsof -p 1234 | nxc --pass SECRET", "lsof -p 1234 | nxc --pass ******"},
        {"cmd cp --pass SECRET", "cmd cp --pass ******"},
        {"cp-wrapper -p SECRET", "cp-wrapper -p ******"},
        {"curl --pass SECRET", "curl --pass ******"},
        {"wget --password SECRET", "wget --password ******"},
        {"unzip -P SECRET archive.zip", "unzip -P ****** archive.zip"},
        {"nc -p 4444", "nc -p 4444"},
        {"docker run -p 8080:80", "docker run -p 8080:80"},
        {"psql -p 5432", "psql -p 5432"},
        {"redis-cli -p 6379 --pass VISIBLE", "redis-cli -p 6379 --pass VISIBLE"},
        {"mkdir -p /tmp/example", "mkdir -p /tmp/example"},
        {"scp -p file host:/tmp", "scp -p file host:/tmp"},
        {"scp -P 2222 file host:/tmp", "scp -P 2222 file host:/tmp"},
        {"dirb http://example.test -p 127.0.0.1:8080",
         "dirb http://example.test -p 127.0.0.1:8080"},
        {"dirb http://example.test -P proxyuser:PROXY_SECRET",
         "dirb http://example.test -P proxyuser:PROXY_SECRET"},
        {"ffuf -p 0.1 -x http://127.0.0.1:8080", "ffuf -p 0.1 -x http://127.0.0.1:8080"},
        {"feroxbuster -P http://127.0.0.1:8080", "feroxbuster -P http://127.0.0.1:8080"},
        {"masscan -p80,443", "masscan -p80,443"},
        {"rustscan -p 80,443", "rustscan -p 80,443"},
        {"naabu -p 80,443", "naabu -p 80,443"},
        {"dirbuster -P -u http://example.test", "dirbuster -P -u http://example.test"},
        {"ffuf -p 0.1; cmd -P SECRET", "ffuf -p 0.1; cmd -P ******"},
        {"nmap -p 80; nxc --pass SECRET", "nmap -p 80; nxc --pass ******"},
        {"nmap -p 80&&nxc -p SECRET", "nmap -p 80&&nxc -p ******"},
        {"nmap -p 80||nxc --password=SECRET", "nmap -p 80||nxc --password=******"},
        {"nmap -p 80|nxc /p:SECRET", "nmap -p 80|nxc /p:******"},
        {"nmap -p 80\nnxc --pass SECRET", "nmap -p 80\nnxc --pass ******"},
        {"nxc --pass SECRET; nmap -p 80", "nxc --pass ******; nmap -p 80"},
        {"cmd nmap --pass SECRET", "cmd nmap --pass ******"},
        {"cmd -p 'a;nmap -p b'", "cmd -p '***********'"},
        {"nmap --pass 'a;nxc -p b'; cmd -p SECRET", "nmap --pass 'a;nxc -p b'; cmd -p ******"},
        {"Nmap --pass SECRET", "Nmap --pass ******"},
        {"nmap-wrapper --pass SECRET", "nmap-wrapper --pass ******"},
        {"/usr/bin/nmap -p 80", "/usr/bin/nmap -p 80"},
        {"'nmap' -p 80", "'nmap' -p 80"},
        {"nma\\p -p 80", "nma\\p -p 80"},
        {"$ nmap -p 80", "$ nmap -p 80"},
        {"incognito-terminal> nmap -p 80", "incognito-terminal> nmap -p 80"},
        {"user@host:~$ nmap -p 80", "user@host:~$ nmap -p 80"},
        {"└─$ nmap -p 80", "└─$ nmap -p 80"},
        {"sudo nmap -p 80", "sudo nmap -p 80"},
        {"sudo -u root -- nmap -p 80", "sudo -u root -- nmap -p 80"},
        {"sudo --unknown nmap -p SECRET", "sudo --unknown nmap -p ******"},
        {"command nmap -p 80", "command nmap -p 80"},
        {"exec nmap -p 80", "exec nmap -p 80"},
        {"FOO=bar nmap -p 80", "FOO=bar nmap -p 80"},
        {"> nmap cmd -p SECRET", "> nmap cmd -p ******"},
        {"nmap > file -p 80; cmd --pass SECRET", "nmap > file -p 80; cmd --pass ******"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++)
        mask_case(cases[i][0], cases[i][1], &s);
    /* Every default works by executable basename; none exempt a following command. */
    for (int i = 0; i < s.exception_count; i++) {
        CHECK(exception_valid(s.exceptions[i]));
        for (int j = 0; j < i; j++)
            CHECK(strcmp(s.exceptions[i], s.exceptions[j]));
        char source[256], expected[256];
        snprintf(source, sizeof(source), "/usr/bin/%s -p 1234 -P value; record --pass SECRET",
                 s.exceptions[i]);
        snprintf(expected, sizeof(expected), "/usr/bin/%s -p 1234 -P value; record --pass ******",
                 s.exceptions[i]);
        mask_case(source, expected, &s);
    }
    s.exception_count = 0;
    mask_case("nmap -p 80", "nmap -p **", &s);
    s.exception_count = 1;
    strcpy(s.exceptions[0], "sudo");
    mask_case("sudo record --pass VISIBLE", "sudo record --pass VISIBLE", &s);
    s.exception_count = 0;
    /* The full new value syntax matrix also applies to --pass. */
    mask_case("cmd --pass-file file --passwordless", "cmd --pass-file file --passwordless", &s);
    mask_case("cmd --pass \"abc'def\"", "cmd --pass \"*******\"", &s);
    mask_case("cmd --pass='two words'", "cmd --pass='*********'", &s);
    mask_case("cmd --pass= next", "cmd --pass= next", &s);
    mask_case("cmd --pass \"unfinished", "cmd --pass \"**********", &s);
}
static void exception_menu(void) {
    char directory[] = "/tmp/privacy-exception-tests-XXXXXX";
    CHECK(mkdtemp(directory));
    CHECK(!setenv("XDG_CONFIG_HOME", directory, 1));
    Settings s, loaded;
    settings_default(&s);
    const int default_count = s.exception_count;
    CHECK(default_count == 97);
    Menu m;
    menu_init(&m, &s, NORMAL);
    Router r = {0};
    Buffer out = {0};
    keys(&r, &m, &out, "\033OQ\033[A\033[A\r");
    CHECK(m.page == EXCEPTIONS);
    keys(&r, &m, &out, "acustom\r");
    CHECK(m.page == EXCEPTIONS && m.settings.exception_count == default_count + 1);
    CHECK(!strcmp(m.settings.exceptions[default_count], "custom"));
    CHECK(!settings_load(&loaded) && loaded.exception_count == default_count + 1);
    keys(&r, &m, &out, "e\025other\r");
    CHECK(!strcmp(m.settings.exceptions[default_count], "other"));
    Settings before = m.settings;
    keys(&r, &m, &out, "e\025bad/name\r");
    CHECK(m.page == EXCEPTION_EDITOR && strstr(m.message, "command name"));
    CHECK(same_settings(&m.settings, &before));
    keys(&r, &m, &out, "\025nmap\r");
    CHECK(strstr(m.message, "already exists"));
    keys(&r, &m, &out, "\003e\025changed");
    menu_key(&m, K_ESC);
    CHECK(m.page == EXCEPTIONS && same_settings(&m.settings, &before));
    keys(&r, &m, &out, "e\025\033[200~new\r\003\033OQ\033[201~");
    CHECK(m.page == EXCEPTION_EDITOR && !strcmp(m.edit.flag, "newOQ"));
    menu_key(&m, K_ESC);
    char path[4096], backup[4096];
    snprintf(path, sizeof(path), "%s/incognito-terminal/config.json", directory);
    snprintf(backup, sizeof(backup), "%s/incognito-terminal/config.backup", directory);
    CHECK(!rename(path, backup) && !mkdir(path, 0700));
    keys(&r, &m, &out, "e\025changed\r");
    CHECK(strstr(m.message, "Could not save") && same_settings(&m.settings, &before));
    CHECK(!rmdir(path) && !rename(backup, path));
    menu_key(&m, K_ESC);
    while (m.settings.exception_count)
        menu_key(&m, 'd');
    CHECK(m.page == EXCEPTIONS && !settings_load(&loaded) && !loaded.exception_count);
    menu_key(&m, K_ENTER);
    menu_key(&m, 'e');
    menu_key(&m, 'd');
    CHECK(m.page == EXCEPTIONS && !m.settings.exception_count);
    keys(&r, &m, &out, "anmap\r");
    CHECK(m.settings.exception_count == 1);
    /* History and trigger saves preserve exceptions. */
    s = m.settings;
    s.history = OFF;
    CHECK(!settings_save(&s, SAVE_HISTORY));
    s.rules[s.count++] = (Rule){"-Q", BOTH};
    CHECK(!settings_save(&s, SAVE_TRIGGERS));
    CHECK(!settings_load(&loaded) && loaded.history == OFF && loaded.exception_count == 1 &&
          loaded.count == 6);
    /* Empty exceptions remain empty on restart, not replaced by defaults. */
    s.exception_count = 0;
    CHECK(!settings_save(&s, SAVE_EXCEPTIONS));
    CHECK(!settings_load(&loaded) && !loaded.exception_count && loaded.count == 6 &&
          loaded.history == OFF);
    /* A full list saves, scrolls, edits its last row, and refuses a 129th entry. */
    settings_default(&s);
    s.exception_count = MAX_EXCEPTIONS;
    for (int i = 0; i < MAX_EXCEPTIONS; i++)
        snprintf(s.exceptions[i], sizeof(s.exceptions[i]), "tool%d", i);
    CHECK(!settings_save(&s, SAVE_EXCEPTIONS));
    CHECK(!settings_load(&loaded) && loaded.exception_count == MAX_EXCEPTIONS);
    menu_init(&m, &loaded, NORMAL);
    keys(&r, &m, &out, "\033OQx");
    menu_key(&m, K_END);
    CHECK(m.exception_selected == MAX_EXCEPTIONS - 1);
    keys(&r, &m, &out, "e\025finalcmd\r");
    CHECK(m.page == EXCEPTIONS && !strcmp(m.settings.exceptions[MAX_EXCEPTIONS - 1], "finalcmd"));
    CHECK(!settings_load(&loaded) && same_settings(&loaded, &m.settings));
    mask_case("finalcmd -p 1234; record -p SECRET", "finalcmd -p 1234; record -p ******", &loaded);
    before = m.settings;
    keys(&r, &m, &out, "aoverflow\r");
    CHECK(m.page == EXCEPTION_EDITOR && strstr(m.message, "128 exceptions"));
    CHECK(same_settings(&m.settings, &before));
    menu_key(&m, K_ESC);
    /* Oversized config input is rejected rather than truncated. */
    FILE *oversized = fopen(path, "w");
    CHECK(oversized);
    fputs("{\"version\":5,\"triggers\":[\"-p\"],\"exceptions\":[", oversized);
    for (int i = 0; i <= MAX_EXCEPTIONS; i++)
        fprintf(oversized, "%s\"tool%d\"", i ? "," : "", i);
    fputs("]}", oversized);
    fclose(oversized);
    CHECK(settings_load(&loaded) < 0 && loaded.exception_count == default_count);
    /* Existing untouched defaults receive --pass; custom legacy rules remain custom. */
    FILE *f = fopen(path, "w");
    CHECK(f);
    fputs("{\"version\":2,\"triggers\":[\"-p\",\"--password\",\"/p:\"],\"history_mode\":\"off\"}",
          f);
    fclose(f);
    CHECK(!settings_load(&loaded) && loaded.count == 5 && loaded.exception_count == default_count &&
          loaded.history == OFF);
    /* Adding -P to v2's defaults used to prevent the missing --pass upgrade. */
    f = fopen(path, "w");
    CHECK(f);
    fputs("{\"version\":2,\"triggers\":[\"-p\",\"--password\",\"/p:\",\"-P\"],"
          "\"history_mode\":\"normal\",\"exceptions\":[]}",
          f);
    fclose(f);
    CHECK(!settings_load(&loaded) && loaded.count == 5 && !loaded.exception_count &&
          loaded.history == NORMAL);
    CHECK(!strcmp(loaded.rules[3].flag, "-P") && loaded.rules[3].mode == BOTH);
    CHECK(!strcmp(loaded.rules[4].flag, "--pass") && loaded.rules[4].mode == EQUALS);
    mask_case("record --pass SECRET", "record --pass ******", &loaded);
    mask_case("record --pass=SECRET", "record --pass=******", &loaded);
    CHECK(!settings_save(&loaded, SAVE_HISTORY));
    CHECK(!settings_load(&s) && same_settings(&s, &loaded));
    /* Reordered legacy defaults upgrade without rearranging saved rows. */
    f = fopen(path, "w");
    CHECK(f);
    fputs("{\"version\":2,\"triggers\":[\"-P\",\"/p:\",\"--password\",\"-p\"]}", f);
    fclose(f);
    CHECK(!settings_load(&loaded) && loaded.count == 5 && !strcmp(loaded.rules[0].flag, "-P") &&
          !strcmp(loaded.rules[4].flag, "--pass"));
    /* Legacy customizations survive while newly introduced defaults are appended. */
    const char *custom_legacy[] = {
        "{\"version\":2,\"triggers\":[\"-p\",\"--password\",\"/p:\",\"-Q\"]}",
        "{\"version\":2,\"triggers\":[{\"flag\":\"-p\",\"value_mode\":\"separated\"},"
        "\"--password\",\"/p:\",\"-P\"]}",
        "{\"version\":2,\"triggers\":[\"-p\",\"/p:\",\"--pass\",\"-P\"]}",
        "{\"version\":5,\"triggers\":[\"-p\",\"--password\",\"/p:\",\"-P\"]}",
    };
    for (size_t i = 0; i < sizeof(custom_legacy) / sizeof(*custom_legacy); i++) {
        f = fopen(path, "w");
        CHECK(f);
        fputs(custom_legacy[i], f);
        fclose(f);
        const int expected_counts[] = {6, 5, 4, 4};
        CHECK(!settings_load(&loaded) && loaded.count == expected_counts[i]);
        if (i == 0)
            CHECK(!strcmp(loaded.rules[3].flag, "-Q"));
        if (i == 1)
            CHECK(loaded.rules[0].mode == SEPARATED);
    }
    /* Both untouched and customized legacy lists gain only newer defaults. */
    f = fopen(path, "w");
    CHECK(f);
    fputs("{\"version\":3,\"triggers\":[\"-p\",\"--password\",\"/p:\",\"--pass\"],"
          "\"exceptions\":[\"ssh\",\"nmap\",\"nc\",\"docker\",\"psql\",\"redis-cli\",\"mkdir\","
          "\"scp\"]}",
          f);
    fclose(f);
    CHECK(!settings_load(&loaded) && loaded.count == 5 && loaded.exception_count == default_count);
    f = fopen(path, "w");
    CHECK(f);
    fputs("{\"version\":4,\"triggers\":[\"-p\",\"--password\",\"/p:\",\"--pass\"],"
          "\"exceptions\":[\"ssh\",\"nmap\",\"nc\",\"docker\",\"psql\",\"redis-cli\",\"mkdir\","
          "\"scp\"]}",
          f);
    fclose(f);
    CHECK(!settings_load(&loaded) && loaded.count == 4 &&
          loaded.exception_count == default_count - 7);
    settings_default(&s);
    /* Version 4's untouched 15-command list gains the expanded defaults. */
    f = fopen(path, "w");
    CHECK(f);
    fputs("{\"version\":4,\"triggers\":[\"-Q\"],\"exceptions\":[", f);
    for (int i = 0; i < 15; i++)
        fprintf(f, "%s\"%s\"", i ? "," : "", s.exceptions[i]);
    fputs("]}", f);
    fclose(f);
    CHECK(!settings_load(&loaded) && loaded.count == 1 && loaded.exception_count == default_count);
    /* Saving version 5 makes deliberate removals permanent. */
    loaded.exception_count = 15;
    CHECK(!settings_save(&loaded, SAVE_EXCEPTIONS));
    CHECK(!settings_load(&loaded) && loaded.count == 1 && loaded.exception_count == 15);
    /* A previous-generation custom addition remains exactly as saved. */
    f = fopen(path, "w");
    CHECK(f);
    fputs("{\"version\":4,\"triggers\":[\"-Q\"],\"exceptions\":[", f);
    for (int i = 0; i < 15; i++)
        fprintf(f, "%s\"%s\"", i ? "," : "", s.exceptions[i]);
    fputs(",\"custom\"]}", f);
    fclose(f);
    CHECK(!settings_load(&loaded) && loaded.count == 1 &&
          loaded.exception_count == default_count + 1 && !strcmp(loaded.exceptions[15], "custom"));
    f = fopen(path, "w");
    CHECK(f);
    fputs("{\"version\":3,\"triggers\":[\"-Q\"],\"exceptions\":[]}", f);
    fclose(f);
    CHECK(!settings_load(&loaded) && loaded.count == 2 && !strcmp(loaded.rules[0].flag, "-Q") &&
          !strcmp(loaded.rules[1].flag, "-P") && !loaded.exception_count);
    f = fopen(path, "w");
    CHECK(f);
    fputs("{\"version\":3,\"triggers\":[\"-p\"],\"exceptions\":[\"bad/name\"]}", f);
    fclose(f);
    CHECK(settings_load(&loaded) < 0);
    /* Adding/removing an exception updates cells already on the screen. */
    settings_default(&s);
    s.exception_count = 0;
    Screen screen;
    screen_init(&screen, 5, 80);
    menu_init(&m, &s, NORMAL);
    Buffer frame = {0};
    const char *line = "incognito-terminal> custom --pass CHANGE_SECRET";
    screen_feed(&screen, line, strlen(line));
    screen_render(&screen, &m, &frame);
    CHECK(!strstr(frame.data, "CHANGE_SECRET"));
    buf_consume(&frame, frame.len);
    m.settings.exception_count = 1;
    strcpy(m.settings.exceptions[0], "custom");
    screen_render(&screen, &m, &frame);
    CHECK(strstr(frame.data, "CHANGE_SECRET"));
    buf_consume(&frame, frame.len);
    m.settings.exception_count = 0;
    screen_render(&screen, &m, &frame);
    CHECK(!strstr(frame.data, "CHANGE_SECRET"));
    buf_free(&frame);
    screen_free(&screen);
    buf_free(&r.pending);
    buf_free(&out);
    unlink(path);
    snprintf(path, sizeof(path), "%s/incognito-terminal", directory);
    rmdir(path);
    rmdir(directory);
}

static void stress_screen(void) {
    Settings settings;
    settings_default(&settings);
    Menu menu;
    menu_init(&menu, &settings, NORMAL);
    Screen s;
    screen_init(&s, 10, 40);
    Buffer frame = {0};
    unsigned rng = 71;
    const char *chunks[] = {"abc",
                            "cmd -p FIXTURE\r\n",
                            "\033[2J",
                            "\033[H",
                            "\033[4;8H",
                            "\033[2K",
                            "\033[3P",
                            "\033[2@",
                            "\033[2L",
                            "\033[2M",
                            "\033[2;8r",
                            "\033[r",
                            "\033[?1049h",
                            "\033[?1049l",
                            "\033[31;1m",
                            "\033[0m",
                            "密🔐码",
                            "é",
                            "\r\n",
                            "\033M",
                            "\033]52;c;BLOCKED\007",
                            "\033P1$qBLOCKED\033\\"};
    for (int i = 0; i < 2000; i++) {
        rng = rng * 1664525u + 1013904223u;
        const char *chunk = chunks[rng % (sizeof(chunks) / sizeof(*chunks))];
        for (const char *p = chunk; *p; p++)
            screen_feed(&s, p, 1);
        if (i % 37 == 0)
            screen_resize(&s, 2 + (int)(rng % 25), 2 + (int)((rng >> 8) % 100), &settings);
        screen_render(&s, &menu, &frame);
        CHECK(!strstr(frame.data, "BLOCKED"));
        buf_consume(&frame, frame.len);
    }
    screen_free(&s);
    buf_free(&frame);
}
static void privacy_style_cases(void) {
    Settings settings;
    settings_default(&settings);
    CHECK(settings.privacy_style == ASTERISKS);
    Menu menu;
    menu_init(&menu, &settings, NORMAL);
    Screen s;
    screen_init(&s, 5, 64);
    s.mask_random = 0x123456789abcdefULL;
    const char *source = "\033]777;incognito;A\auser@host> \033]777;incognito;B\a"
                         "nmap -p 80\r\nrecord -p 'A密🔐Bc' tail\r\n"
                         "\033]777;incognito;C\aPLAIN_OUTPUT";
    screen_feed(&s, source, strlen(source));
    Buffer frame = {0}, visible = {0};
    VTermPos original_cursor = s.cursor;
    for (int mode = ASTERISKS; mode <= EMOJI; mode++) {
        menu.settings.privacy_style = (MaskStyle)mode;
        screen_render(&s, &menu, &frame);
        rendered_text(&s, &visible);
        CHECK(strstr(visible.data, "nmap -p 80") && strstr(visible.data, "PLAIN_OUTPUT"));
        CHECK(!strstr(frame.data, "密") && !strstr(frame.data, "🔐"));
        CHECK(s.cursor.row == original_cursor.row && s.cursor.col == original_cursor.col);
        if (mode == ASTERISKS) {
            CHECK(strstr(frame.data, "\033[38;2;127;165;143m"));
            CHECK(strstr(visible.data, "'*******' tail"));
        } else if (mode == EMOJI) {
            CHECK(strstr(frame.data, "☺")); /* Seven concealed columns leave one narrow smiley. */
        }
        size_t bytes = (size_t)s.rows * (size_t)s.cols * sizeof(Cell);
        Cell *before = xcalloc(1, bytes);
        memcpy(before, s.last_frame, bytes);
        s.invalid = true;
        screen_render(&s, &menu, &frame);
        CHECK(!memcmp(before, s.last_frame, bytes)); /* Forced redraw does not randomize masks. */
        free(before);
        buf_consume(&visible, visible.len);
        menu.crazy = true;
        screen_render(&s, &menu, &frame);
        rendered_text(&s, &visible);
        CHECK(!strstr(visible.data, "nmap") && !strstr(visible.data, "record"));
        CHECK(strstr(visible.data, "user@host> ") && strstr(visible.data, "PLAIN_OUTPUT"));
        CHECK(strstr(frame.data, "Crazy"));
        buf_consume(&visible, visible.len);
        menu.crazy = false;
        screen_render(&s, &menu, &frame);
        rendered_text(&s, &visible);
        CHECK(strstr(visible.data, "nmap -p 80") && strstr(visible.data, "PLAIN_OUTPUT"));
        CHECK(!strstr(visible.data, "密"));
        buf_consume(&visible, visible.len);
    }
    menu.enabled = false;
    screen_render(&s, &menu, &frame);
    rendered_text(&s, &visible);
    CHECK(strstr(visible.data, "A密🔐Bc"));
    buf_free(&visible);
    buf_free(&frame);
    screen_free(&s);
    /* Narrow windows never emit a two-column emoji beyond the right edge. */
    for (int width = 2; width < 40; width++) {
        screen_init(&s, 3, width);
        menu_init(&menu, &settings, NORMAL);
        menu.crazy = true;
        menu.settings.privacy_style = EMOJI;
        const char *input = "\033]777;incognito;B\aabcdef 'quoted' XY";
        screen_feed(&s, input, strlen(input));
        screen_render(&s, &menu, &frame);
        for (int y = 0; y < s.rows; y++) {
            int columns = 0;
            for (int x = 0; x < s.cols; x++) {
                const Cell *c = &s.last_frame[y * s.cols + x];
                columns += c->v.width;
                if (c->v.width == 2)
                    CHECK(x + 1 < s.cols && !s.last_frame[y * s.cols + x + 1].v.width);
            }
            CHECK(columns == s.cols);
        }
        screen_free(&s);
        buf_free(&frame);
    }
    /* Prompt and command markers may be split at any PTY read boundary. */
    const char *marked = "\033]777;incognito;A\aprompt@host> "
                         "\033]777;incognito;B\aTYPED_SECRET"
                         "\033]777;incognito;C\a\r\nVISIBLE_OUTPUT";
    for (size_t split = 0; split <= strlen(marked); split++) {
        screen_init(&s, 3, 64);
        menu_init(&menu, &settings, NORMAL);
        menu.crazy = true;
        screen_feed(&s, marked, split);
        screen_feed(&s, marked + split, strlen(marked) - split);
        screen_render(&s, &menu, &frame);
        rendered_text(&s, &visible);
        CHECK(strstr(visible.data, "prompt@host> "));
        CHECK(strstr(visible.data, "VISIBLE_OUTPUT"));
        CHECK(!strstr(visible.data, "TYPED_SECRET"));
        CHECK(!strstr(frame.data, "777;incognito"));
        screen_free(&s);
        buf_free(&frame);
        buf_free(&visible);
    }
    /* Styles persist independently of history, triggers and exceptions. */
    char directory[] = "/tmp/privacy-style-tests-XXXXXX";
    CHECK(mkdtemp(directory) && !setenv("XDG_CONFIG_HOME", directory, 1));
    settings_default(&settings);
    CHECK(!settings_save(&settings, SAVE_HISTORY));
    Settings loaded;
    for (int mode = ASTERISKS; mode <= EMOJI; mode++) {
        settings.privacy_style = (MaskStyle)mode;
        CHECK(!settings_save(&settings, SAVE_STYLE));
        CHECK(!settings_load(&loaded) && same_settings(&settings, &loaded));
    }
    settings.history = SESSION;
    settings.rules[settings.count++] = (Rule){"-Q", BOTH};
    CHECK(!settings_save(&settings, SAVE_HISTORY) && !settings_save(&settings, SAVE_TRIGGERS));
    CHECK(!settings_load(&loaded) && loaded.privacy_style == EMOJI && loaded.history == SESSION &&
          loaded.count == 6);
    settings.privacy_style = (MaskStyle)99;
    CHECK(settings_save(&settings, SAVE_STYLE) < 0);
    CHECK(!settings_load(&settings) && settings.privacy_style == EMOJI);
    menu_init(&menu, &settings, NORMAL);
    Router router = {0};
    Buffer out = {0};
    keys(&router, &menu, &out, "\033OQmA\r");
    CHECK(menu.page == STYLE && menu.settings.privacy_style == ASTERISKS);
    keys(&router, &menu, &out, "e\003");
    CHECK(menu.page == HOME && menu.settings.privacy_style == ASTERISKS);
    keys(&router, &menu, &out, "P");
    CHECK(menu.page == PRIVACY && menu.privacy_selected == 0 && menu.enabled && !menu.crazy);
    keys(&router, &menu, &out, "\033[A");
    CHECK(menu.privacy_selected == 2 && !menu.crazy);
    keys(&router, &menu, &out, "\t");
    CHECK(menu.privacy_selected == 0);
    keys(&router, &menu, &out, "\033[B");
    CHECK(menu.privacy_selected == 1 && menu.enabled);
    menu_key(&menu, K_ESC);
    CHECK(menu.enabled && menu.page == HOME);
    keys(&router, &menu, &out, "Pf\r");
    CHECK(!menu.enabled && !menu.crazy && !strcmp(privacy_state_label(&menu), "Off"));
    keys(&router, &menu, &out, "c\r");
    CHECK(menu.enabled && menu.crazy && !strcmp(privacy_state_label(&menu), "Crazy"));
    menu_key(&menu, K_ESC);
    keys(&router, &menu, &out, "P");
    CHECK(menu.privacy_selected == 2); /* Reopening selects the actual current state. */
    keys(&router, &menu, &out, "o\r");
    CHECK(menu.enabled && !menu.crazy && !strcmp(privacy_state_label(&menu), "On"));
    char path[4096];
    snprintf(path, sizeof(path), "%s/incognito-terminal/config.json", directory);
    FILE *f = fopen(path, "w");
    CHECK(f);
    fputs("{\"version\":5,\"history_mode\":\"session\",\"privacy_style\":\"emoji\","
          "\"triggers\":[{\"flag\":\"-Q\",\"value_mode\":\"attached_or_separated\"}],"
          "\"exceptions\":[\"custom\"],"
          "\"shortcuts\":{\"mask\":\"Ctrl+M\",\"reveal\":\"Alt+U\"}}",
          f);
    fclose(f);
    CHECK(!settings_load(&loaded) && loaded.history == SESSION && loaded.privacy_style == EMOJI &&
          loaded.count == 1 && !strcmp(loaded.rules[0].flag, "-Q") && loaded.exception_count == 1 &&
          !strcmp(loaded.exceptions[0], "custom"));
    char migrated[64];
    shortcut_label(loaded.shortcuts[SHORTCUT_MASK], migrated, sizeof(migrated));
    CHECK(!strcmp(migrated, "Alt+I"));
    shortcut_label(loaded.shortcuts[SHORTCUT_REVEAL], migrated, sizeof(migrated));
    CHECK(!strcmp(migrated, "Alt+U"));
    CHECK(!settings_save(&loaded, SAVE_SHORTCUTS) && !settings_load(&settings) &&
          same_settings(&loaded, &settings));
    f = fopen(path, "w");
    CHECK(f);
    fputs("{\"version\":5,\"triggers\":[\"-Q\"],"
          "\"shortcuts\":{\"mask\":\"Ctrl+Shift+I\",\"reveal\":\"Ctrl+Shift+U\"}}",
          f);
    fclose(f);
    CHECK(!settings_load(&loaded));
    shortcut_label(loaded.shortcuts[SHORTCUT_MASK], migrated, sizeof(migrated));
    CHECK(!strcmp(migrated, "Alt+I"));
    shortcut_label(loaded.shortcuts[SHORTCUT_REVEAL], migrated, sizeof(migrated));
    CHECK(!strcmp(migrated, "Alt+U"));
    f = fopen(path, "w");
    CHECK(f);
    fputs("{\"version\":5,\"triggers\":[\"-p\"],\"privacy_style\":\"invalid\"}", f);
    fclose(f);
    CHECK(settings_load(&loaded) < 0);
    buf_free(&router.pending);
    buf_free(&out);
    unlink(path);
    snprintf(path, sizeof(path), "%s/incognito-terminal", directory);
    rmdir(path);
    rmdir(directory);
}
static void agile_controls(void) {
#define INPUT(bytes) router_feed(&r, &m, &out, bytes, strlen(bytes))
    Settings settings;
    settings_default(&settings);
    const char *arms[] = {"\033i", "\033[105;3u", "\033[105:73;3:1u", "\033[27;3;105~"};
    for (size_t i = 0; i < sizeof(arms) / sizeof(*arms); i++) {
        size_t n = strlen(arms[i]);
        for (size_t split = 0; split <= n; split++) {
            Menu m;
            menu_init(&m, &settings, NORMAL);
            Router r = {0};
            Buffer out = {0};
            router_feed(&r, &m, &out, arms[i], split);
            router_feed(&r, &m, &out, arms[i] + split, n - split);
            CHECK(out.len == 7 && !memcmp(out.data, "\033[99;1~", 7));
            CHECK(strstr(m.notification, "Masked typing") && !r.pending.len);
            buf_free(&r.pending);
            buf_free(&out);
        }
    }
    Menu m;
    menu_init(&m, &settings, NORMAL);
    Router r = {0};
    Buffer out = {0};
    INPUT("\033u");
    CHECK(m.revealing && m.reveal_until > monotime() && !out.len);
    m.revealing = false;
    m.reveal_until = 0;
    INPUT("\033[?3u\033[117;3u\033[117;3:2u");
    CHECK(m.keyboard_events && m.revealing && !out.len);
    INPUT("\033[117;1:3u");
    CHECK(!m.revealing && !out.len);
    INPUT("\033[117;3u\033[O");
    CHECK(!m.revealing && !out.len);
    INPUT("\033[99;5u\033[117;5u\t\033[97;2u\033[9;2u");
    const char ordinary[] = "\003\025\tA\033[Z";
    CHECK(out.len == sizeof(ordinary) - 1 && !memcmp(out.data, ordinary, out.len));
    buf_consume(&out, out.len);
    INPUT("\033[105;3:2u\033[105;3:3u");
    CHECK(!out.len); /* Arm repeats and releases must not run another widget. */
    CHECK(shortcut_parse("Alt+Shift+I", &m.settings.shortcuts[SHORTCUT_MASK]));
    INPUT("\033I");
    CHECK(out.len == 7 && !memcmp(out.data, "\033[99;1~", 7));
    buf_consume(&out, out.len);
    Shortcut parsed;
    CHECK(!shortcut_parse("F2", &parsed) && !shortcut_parse("I", &parsed));
    CHECK(!shortcut_parse("Ctrl+M", &parsed) && !shortcut_parse("ctrl+m", &parsed));
    CHECK(shortcut_parse("Ctrl+Shift+M", &parsed));
    CHECK(!shortcut_parse("Ctrl+Ctrl+I", &parsed));
    CHECK(shortcut_parse("Ctrl+F8", &parsed));
    char label[64];
    shortcut_label(parsed, label, sizeof(label));
    CHECK(!strcmp(label, "Ctrl+F8"));
    buf_free(&r.pending);
    buf_free(&out);
    {
        const char *prompt = "\033]777;incognito;D\a\033]777;incognito;A\ap> "
                             "\033]777;incognito;B\a";
        Screen screen;
        screen_init(&screen, 4, 70);
        screen_feed(&screen, prompt, strlen(prompt));
        screen_feed(&screen, "\033]777;incognito;N\a", 18);
        screen_render(&screen, &m, &out);
        CHECK(screen.manual.count == 1 && screen.manual.spans[0].active);
        screen_resize(&screen, 5, 60, &settings);
        CHECK(screen.manual.count == 0); /* An unused arm cannot survive a pane resize. */
        buf_free(&out);
        screen_free(&screen);

        screen_init(&screen, 4, 70);
        screen_feed(&screen, prompt, strlen(prompt));
        screen_feed(&screen, "\033]777;incognito;N\aSECRET", 24);
        screen_render(&screen, &m, &out);
        CHECK(screen.manual.count == 1 &&
              screen.manual.spans[0].end > screen.manual.spans[0].start);
        screen_resize(&screen, 5, 60, &settings);
        CHECK(screen.manual.count == 1); /* Started secrets stay protected. */
        buf_free(&out);
        screen_free(&screen);
    }
    const char *marked = "\033]777;incognito;D\a\033]777;incognito;A\auser@host> "
                         "\033]777;incognito;B\arecord \033]777;incognito;M;7\a"
                         "'MANUAL_SECRET' tail\033]777;incognito;C\a\r\nOUTPUT_VISIBLE";
    for (size_t split = 0; split <= strlen(marked); split++) {
        Screen screen;
        screen_init(&screen, 4, 70);
        menu_init(&m, &settings, NORMAL);
        m.enabled = false;
        screen_feed(&screen, marked, split);
        screen_feed(&screen, marked + split, strlen(marked) - split);
        screen_render(&screen, &m, &out);
        Buffer visible = {0};
        rendered_text(&screen, &visible);
        CHECK(strstr(visible.data, "user@host> record '*************' tail"));
        CHECK(strstr(visible.data, "OUTPUT_VISIBLE"));
        CHECK(!strstr(out.data, "MANUAL_SECRET"));
        m.revealing = true;
        screen_render(&screen, &m, &out);
        CHECK(strstr(out.data, "MANUAL_SECRET"));
        buf_free(&visible);
        buf_free(&out);
        screen_free(&screen);
    }
    settings.triggers_enabled = false;
    mask_case("record -p visible", "record -p visible", &settings);
    settings.triggers_enabled = true;
    settings.exclusions_enabled = false;
    mask_case("nmap -p 80", "nmap -p **", &settings);
#undef INPUT
}
static void benchmark(void) {
    Settings settings;
    settings_default(&settings);
    Menu menu;
    menu_init(&menu, &settings, NORMAL);
    Screen s;
    screen_init(&s, 23, 100);
    Buffer frame = {0};
    const char *chunk = "record -p BENCH_FIXTURE next\r\n";
    double start = monotime();
    for (int i = 0; i < 300; i++) {
        screen_feed(&s, chunk, strlen(chunk));
        screen_render(&s, &menu, &frame);
        buf_consume(&frame, frame.len);
    }
    printf("%.6f\n", monotime() - start);
    screen_free(&s);
    buf_free(&frame);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--benchmark")) {
        benchmark();
        return 0;
    }
    setlocale(LC_CTYPE, "C.UTF-8");
    detector();
    screen_cases();
    config_and_menu();
    default_updates();
    menu_functions();
    exception_cases();
    exception_menu();
    stress_screen();
    privacy_style_cases();
    agile_controls();
    printf("Native checks passed (%zu assertions).\n", assertions);
    return 0;
}
