/* Copyright 2026 Secragon Ltd
 * SPDX-License-Identifier: Apache-2.0 */
#include "privacy.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

static void record_free(ManualRecord *r) {
    if (r->text)
        explicit_bzero(r->text, r->len * sizeof(*r->text));
    free(r->text);
    free(r->spans);
    *r = (ManualRecord){0};
}
void manual_free(ManualMask *m) {
    for (int i = 0; i < m->record_count; i++)
        record_free(&m->records[i]);
    if (m->previous)
        explicit_bzero(m->previous, m->len * sizeof(*m->previous));
    free(m->previous);
    free(m->spans);
    *m = (ManualMask){0};
}
/* Use logical input positions, not screen columns: prompts, wrapping, wide
 * characters and a full readline/ZLE redraw must not move a manual boundary. */
void manual_update(Screen *s, bool arm) {
    ManualMask *m = &s->manual;
    if (!arm && !m->count && !m->record_count)
        return;
    size_t cap = ((size_t)s->nhistory + s->rows) * MAX(s->cols, 1) * VTERM_MAX_CHARS_PER_CELL;
    for (int y = 0; y < s->nhistory; y++)
        cap += (size_t)s->history[y].cols * VTERM_MAX_CHARS_PER_CELL;
    uint32_t *text = xcalloc(cap ? cap : 1, sizeof(*text));
    Cell **refs = xcalloc(cap ? cap : 1, sizeof(*refs));
    size_t n = 0, caret = 0;
    int nh = s->alt ? 0 : s->nhistory;
    for (int y = 0; y < nh + s->rows; y++) {
        Cell *row = y < nh ? s->history[y].cells : s->grids[s->alt] + (y - nh) * s->cols;
        int cols = y < nh ? s->history[y].cols : s->cols, end = 0;
        bool continuation = y + 1 < nh             ? s->history[y + 1].continuation
                            : y + 1 < nh + s->rows ? s->continuations[s->alt][y + 1 - nh]
                                                   : false;
        for (int x = 0; x < cols; x++)
            if (row[x].command_cell && row[x].input_id == s->input_id && row[x].v.chars[0] &&
                !iswspace(row[x].v.chars[0]))
                end = x + 1;
        if (continuation)
            end = cols;
        if (y == nh + s->cursor.row && s->input_area)
            end = MAX(end, MIN(cols, s->cursor.col));
        for (int x = 0; x < end; x++) {
            Cell *c = &row[x];
            if (!c->command_cell || c->input_id != s->input_id || !c->v.width)
                continue;
            c->manual_hidden = false;
            for (int k = 0; k < VTERM_MAX_CHARS_PER_CELL && c->v.chars[k]; k++) {
                text[n] = c->v.chars[k];
                refs[n++] = c;
                if (y < nh + s->cursor.row || (y == nh + s->cursor.row && x < s->cursor.col))
                    caret = n;
            }
        }
    }
    bool partial = m->previous && n < m->len && !memcmp(text, m->previous, n * sizeof(*text));
    if (m->count && m->previous && !partial) {
        size_t prefix = 0, suffix = 0;
        while (prefix < n && prefix < m->len && text[prefix] == m->previous[prefix])
            prefix++;
        while (suffix < n - prefix && suffix < m->len - prefix &&
               text[n - suffix - 1] == m->previous[m->len - suffix - 1])
            suffix++;
        size_t removed = m->len - prefix - suffix, inserted = n - prefix - suffix;
        for (size_t i = 0; i < m->count;) {
            ManualSpan *span = &m->spans[i];
            if (span->pending) {
                i++;
                continue;
            }
            if (removed && prefix <= span->start && prefix + removed >= span->end &&
                span->end > span->start && !span->active) {
                memmove(span, span + 1, (m->count - i - 1) * sizeof(*span));
                m->count--;
                continue;
            }
            if (prefix < span->start)
                span->start =
                    span->start >= prefix + removed ? span->start - removed + inserted : prefix;

            i++;
        }
    }
    /* Recalled manual secrets keep their protection. Adopt their boundaries
     * so subsequent edits follow the same rules as the original command. */
    if (!m->count && n)
        for (int i = m->record_count - 1; i >= 0; i--) {
            ManualRecord *r = &m->records[i];
            if (r->len == n && !memcmp(r->text, text, n * sizeof(*text))) {
                m->spans = xrealloc(m->spans, r->count * sizeof(*m->spans));
                memcpy(m->spans, r->spans, r->count * sizeof(*m->spans));
                m->count = r->count;
                break;
            }
        }
    if (arm && s->input_area && !s->alt) {
        if (!m->count || m->spans[m->count - 1].start != caret) {
            m->spans = xrealloc(m->spans, (m->count + 1) * sizeof(*m->spans));
            m->spans[m->count++] = (ManualSpan){.start = caret, .end = caret, .active = true};
        }
    }
    bool *hidden = xcalloc(n ? n : 1, sizeof(bool));
    for (size_t i = 0; i < m->count; i++) {
        ManualSpan *span = &m->spans[i];
        if (span->pending && n >= span->start)
            span->pending = false;
        size_t first = span->start;
        while (first < n && (text[first] == ' ' || text[first] == '\t'))
            first++;
        span->end = mask_value(text, n, first, hidden);
        if (span->end < n)
            span->active = false;
    }
    for (size_t i = 0; i < n; i++)
        if (hidden[i])
            refs[i]->manual_hidden = true;
    if (!partial || arm) {
        if (m->previous)
            explicit_bzero(m->previous, m->len * sizeof(*m->previous));
        free(m->previous);
        m->previous = text;
        m->len = n;
    } else {
        explicit_bzero(text, n * sizeof(*text));
        free(text);
    }
    free(hidden);
    free(refs);
}
/* A one-shot arm should not remain hidden across a terminal or tmux layout
 * change. Keep spans that have started consuming a value so resizing never
 * exposes text that is already concealed. */
void manual_cancel_unstarted(Screen *s) {
    ManualMask *m = &s->manual;
    for (size_t i = 0; i < m->count;) {
        ManualSpan *span = &m->spans[i];
        bool started = false;
        size_t start = MIN(span->start, m->len), end = MIN(MAX(span->end, start), m->len);
        for (size_t p = start; p < end; p++)
            if (m->previous[p] != ' ' && m->previous[p] != '\t') {
                started = true;
                break;
            }
        if ((span->active || span->pending) && !started) {
            memmove(span, span + 1, (m->count - i - 1) * sizeof(*span));
            m->count--;
            continue;
        }
        i++;
    }
}
void manual_arm_at(Screen *s, size_t offset) {
    manual_update(s, false);
    ManualMask *m = &s->manual;
    offset += s->input_base;
    m->spans = xrealloc(m->spans, (m->count + 1) * sizeof(*m->spans));
    m->spans[m->count++] =
        (ManualSpan){.start = offset, .end = offset, .active = true, .pending = true};
}
size_t manual_cursor_offset(const Screen *s) {
    size_t n = 0;
    int nh = s->alt ? 0 : s->nhistory;
    for (int y = 0; y <= nh + s->cursor.row; y++) {
        const Cell *row = y < nh ? s->history[y].cells : s->grids[s->alt] + (y - nh) * s->cols;
        int cols = y < nh ? s->history[y].cols : s->cols, end = 0;
        bool cont = y + 1 < nh             ? s->history[y + 1].continuation
                    : y + 1 < nh + s->rows ? s->continuations[s->alt][y + 1 - nh]
                                           : false;
        for (int x = 0; x < cols; x++)
            if (row[x].command_cell && row[x].input_id == s->input_id && row[x].v.chars[0] &&
                !iswspace(row[x].v.chars[0]))
                end = x + 1;
        if (cont)
            end = cols;
        if (y == nh + s->cursor.row)
            end = MIN(cols, s->cursor.col);
        for (int x = 0; x < end; x++) {
            const Cell *c = &row[x];
            if (!c->command_cell || c->input_id != s->input_id || !c->v.width)
                continue;
            for (int k = 0; k < VTERM_MAX_CHARS_PER_CELL && c->v.chars[k]; k++)
                n++;
        }
    }
    return n;
}
void manual_finish(Screen *s) {
    manual_update(s, false);
    ManualMask *m = &s->manual;
    if (m->count && m->len) {
        if (m->record_count == CONTEXT_ROWS) {
            record_free(&m->records[0]);
            memmove(m->records, m->records + 1, (CONTEXT_ROWS - 1) * sizeof(*m->records));
            m->record_count--;
        }
        ManualRecord *r = &m->records[m->record_count++];
        *r = (ManualRecord){
            .text = m->previous, .len = m->len, .spans = m->spans, .count = m->count};
        for (size_t i = 0; i < r->count; i++)
            r->spans[i].active = false;
        m->previous = NULL;
        m->spans = NULL;
    } else {
        if (m->previous)
            explicit_bzero(m->previous, m->len * sizeof(*m->previous));
        free(m->previous);
        free(m->spans);
        m->previous = NULL;
        m->spans = NULL;
    }
    m->len = m->count = 0;
}
