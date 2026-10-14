/*
 * Copyright 2026 Secragon Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PRIVACY_H
#define PRIVACY_H
#define _GNU_SOURCE
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vterm.h>
#define MAX_RULES 32
#define MAX_FLAG 64
#define MAX_EXCEPTIONS 128
#define MAX_COMMAND 64
#define CONTEXT_ROWS 256
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
typedef enum { ATTACHED, SEPARATED, BOTH, EQUALS } ValueMode;
typedef enum { NORMAL, SESSION, OFF } HistoryMode;
typedef enum { ASTERISKS, OBFUSCATION, EMOJI } MaskStyle;
enum { SHORTCUT_MASK, SHORTCUT_REVEAL, SHORTCUT_COUNT, KEY_FUNCTION_BASE = 0x110000 };
typedef struct {
    int code, modifiers;
} Shortcut;
typedef struct {
    char flag[MAX_FLAG + 1];
    ValueMode mode;
} Rule;
typedef struct {
    Rule rules[MAX_RULES];
    int count;
    HistoryMode history;
    MaskStyle privacy_style;
    bool triggers_enabled, exclusions_enabled;
    Shortcut shortcuts[SHORTCUT_COUNT];
    char exceptions[MAX_EXCEPTIONS][MAX_COMMAND + 1];
    int exception_count;
    /* Names of bundled defaults this config has already seen, including any
     * the user removed. Keep these when saving only one preference. */
    char seen_triggers[MAX_RULES][MAX_FLAG + 1];
    int seen_trigger_count;
    char seen_exceptions[MAX_EXCEPTIONS][MAX_COMMAND + 1];
    int seen_exception_count;
} Settings;
typedef struct {
    char *data;
    size_t len, cap;
} Buffer;
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t size);
void buf_add(Buffer *b, const void *s, size_t n);
void buf_str(Buffer *b, const char *s);
void buf_printf(Buffer *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_consume(Buffer *b, size_t n);
void buf_free(Buffer *b);
void buf_cp(Buffer *b, uint32_t cp);
double monotime(void);
int write_all(int fd, const void *s, size_t n);
extern const char *history_names[], *history_labels[], *mode_names[], *mode_labels[];
extern const char *privacy_style_names[], *privacy_style_labels[];
void settings_default(Settings *s);
void settings_merge_defaults(Settings *s, const Settings *defaults);
ValueMode default_mode(const char *s);
bool rule_valid(const Rule *r);
int settings_load(Settings *s);
typedef enum {
    SAVE_TRIGGERS,
    SAVE_HISTORY,
    SAVE_EXCEPTIONS,
    SAVE_STYLE,
    SAVE_SHORTCUTS
} SettingsPart;
bool exception_valid(const char *name);
int settings_save(const Settings *s, SettingsPart part);
void mask_positions(const uint32_t *s, size_t len, const Settings *rules, bool *hidden);
size_t mask_value(const uint32_t *s, size_t len, size_t start, bool *hidden);
void command_exceptions(const uint32_t *s, size_t len, const Settings *rules, bool *excluded);

typedef enum {
    HOME,
    TRIGGERS,
    HISTORY,
    EDITOR,
    EXCEPTIONS,
    EXCEPTION_EDITOR,
    STYLE,
    PRIVACY,
    SHORTCUTS,
    SHORTCUT_EDITOR
} Page;
enum {
    K_F2 = 256,
    K_ESC,
    K_UP,
    K_DOWN,
    K_LEFT,
    K_RIGHT,
    K_HOME,
    K_END,
    K_DELETE,
    K_ENTER,
    K_BACKSPACE,
    K_CLEAR,
    K_TAB
};
typedef struct {
    Settings settings;
    bool open, enabled, crazy;
    Page page;
    int selected, home_selected, history_selected, active_history;
    int exception_selected;
    int style_selected;
    int privacy_selected;
    int shortcut_selected;
    bool revealing, keyboard_events, shell_input;
    double notification_until, reveal_until;
    char notification[96];
    int edit_index, position, focus;
    Rule edit;
    char message[160];
} Menu;
typedef struct {
    Buffer pending;
    int paste;
    double since;
} Router;
void menu_init(Menu *m, const Settings *s, int active);
const char *privacy_state_label(const Menu *m);
void menu_key(Menu *m, int key);
void menu_insert(Menu *m, int ch);
void router_feed(Router *r, Menu *m, Buffer *output, const char *s, size_t len);
void router_flush(Router *r, Menu *m, Buffer *output);
bool shortcut_parse(const char *text, Shortcut *shortcut);
void shortcut_label(Shortcut shortcut, char *text, size_t size);
int keyboard_sequence(Menu *m, Buffer *out, const char *bytes, size_t len);
void menu_notify(Menu *m, const char *text);

typedef struct {
    VTermScreenCell v;
    bool hidden, protected_cell, command_cell;
    bool manual_hidden;
    uint64_t input_id;
    uint32_t mask_token;
} Cell;
typedef struct {
    Cell *cells;
    int cols;
    bool continuation;
} Line;
typedef struct {
    size_t start, end;
    bool active, pending;
} ManualSpan;
typedef struct {
    uint32_t *text;
    size_t len;
    ManualSpan *spans;
    size_t count;
} ManualRecord;
typedef struct {
    ManualSpan *spans;
    size_t count;
    uint32_t *previous;
    size_t len;
    ManualRecord records[CONTEXT_ROWS];
    int record_count;
} ManualMask;
typedef struct {
    VTerm *vt;
    VTermState *state;
    int rows, cols, alt;
    Cell *grids[2];
    bool *continuations[2];
    Line history[CONTEXT_ROWS];
    int nhistory;
    VTermScreenCell pen;
    VTermPos cursor;
    bool cursor_visible;
    Buffer responses;
    Cell *last_frame;
    bool invalid;
    int filter_state, utf8_left;
    bool filter_osc, input_area;
    char filter_string[64];
    size_t filter_len;
    uint32_t utf8_cp, utf8_min;
    uint64_t mask_random;
    uint64_t input_id;
    size_t input_base;
    ManualMask manual;
} Screen;
void screen_init(Screen *s, int rows, int cols);
void screen_free(Screen *s);
void screen_feed(Screen *s, const char *bytes, size_t len);
void screen_mask(Screen *s, const Settings *settings);
void screen_resize(Screen *s, int rows, int cols, const Settings *settings);
void screen_render(Screen *s, const Menu *m, Buffer *out);
void manual_update(Screen *s, bool arm);
void manual_arm_at(Screen *s, size_t offset);
size_t manual_cursor_offset(const Screen *s);
void manual_finish(Screen *s);
void manual_free(ManualMask *manual);
void menu_draw(const Menu *m, Cell *grid, int rows, int cols, VTermPos *cursor);

typedef struct {
    char directory[4096];
    char ready[4096];
    char **argv;
    bool owned;
    HistoryMode mode;
} Startup;
int startup_prepare(Startup *s, char **argv, bool clean, HistoryMode mode);
void startup_cleanup(Startup *s);
bool startup_ready(const Startup *s);
#endif
