/*
 * Copyright 2026 Secragon Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "history_policies.h"
#include "privacy.h"
#include "prompt_hooks.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void quote(Buffer *b, const char *s) {
    buf_str(b, "'");
    for (; *s; s++)
        if (*s == '\'')
            buf_str(b, "'\\''");
        else
            buf_add(b, s, 1);
    buf_str(b, "'");
}
static int file_write(const Startup *s, const char *name, const Buffer *b) {
    char path[4200];
    snprintf(path, sizeof(path), "%s/%s", s->directory, name);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    int result = write_all(fd, b->data, b->len);
    if (close(fd))
        result = -1;
    return result;
}
void startup_cleanup(Startup *s) {
    if (s->directory[0]) {
        const char *names[] = {"ready", "bashrc", ".zshenv", ".zshrc"};
        char path[4200];
        for (size_t i = 0; i < 4; i++) {
            snprintf(path, sizeof(path), "%s/%s", s->directory, names[i]);
            unlink(path);
        }
        rmdir(s->directory);
        s->directory[0] = 0;
    }
    if (s->owned) {
        for (int i = 0; s->argv[i]; i++)
            free(s->argv[i]);
        free(s->argv);
        s->owned = false;
    }
}
static void argv_copy(Startup *s, const char **args, int n) {
    s->argv = xcalloc((size_t)n + 1, sizeof(char *));
    s->owned = true;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(args[i]) + 1;
        s->argv[i] = xcalloc(len, 1);
        memcpy(s->argv[i], args[i], len);
    }
}
int startup_prepare(Startup *s, char **argv, bool clean, HistoryMode mode) {
    *s = (Startup){.argv = argv, .mode = mode};
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    bool bash = !strcmp(name, "bash"), zsh = !strcmp(name, "zsh");
    if ((clean || mode != NORMAL) && ((!bash && !zsh) || argv[1]))
        return -1;
    if (clean)
        setenv("PS1", "incognito-terminal> ", 1);
    if (mode == NORMAL && ((!bash && !zsh) || argv[1]))
        return 0;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || *tmp != '/')
        tmp = "/tmp";
    int n =
        snprintf(s->directory, sizeof(s->directory), "%s/incognito-terminal-startup-XXXXXX", tmp);
    if (n < 0 || n > (int)sizeof(s->directory) - 16) {
        s->directory[0] = 0;
        return -1;
    }
    if (!mkdtemp(s->directory)) {
        s->directory[0] = 0;
        return -1;
    }
    memcpy(s->ready, s->directory, (size_t)n);
    memcpy(s->ready + n, "/ready", 7);
    if (mode != NORMAL)
        setenv("HISTFILE", "/dev/null", 1);
    Buffer finish = {0}, rc = {0}, setup = {0};
    int result = -1;
    if (clean)
        buf_str(&finish, "\nPS1='incognito-terminal> '\n");
    buf_printf(&finish, "\nbuiltin printf %%s %s > ", history_names[mode]);
    quote(&finish, s->ready);
    buf_str(&finish, "\n");
    if (bash) {
        if (!clean)
            buf_str(&rc, "if [[ -r ~/.bashrc ]]; then builtin source ~/.bashrc; fi\n");
        if (mode != NORMAL)
            buf_str(&rc, mode == OFF ? bash_off : bash_session);
        buf_add(&rc, finish.data, finish.len);
        buf_str(&rc, bash_prompt_hooks);
        if (file_write(s, "bashrc", &rc))
            goto done;
        char path[4200];
        snprintf(path, sizeof(path), "%s/bashrc", s->directory);
        const char *a[] = {argv[0], "--noprofile", "--rcfile", path, "-i"};
        argv_copy(s, a, 5);
    } else {
        const char *original = getenv("ZDOTDIR");
        if (original) {
            buf_str(&setup, "export ZDOTDIR=");
            quote(&setup, original);
            buf_str(&setup, "\n");
        } else
            buf_str(&setup, "builtin unset ZDOTDIR\n");
        if (!clean)
            buf_str(&setup, "if [[ -r ${ZDOTDIR-$HOME}/.zshenv ]]; then builtin source "
                            "\"${ZDOTDIR-$HOME}/.zshenv\"; fi\n");
        buf_str(
            &setup,
            "typeset -g _INCOGNITO_TERMINAL_USER_DIR=${ZDOTDIR-$HOME}\ntypeset -g "
            "_INCOGNITO_TERMINAL_HAS_ZDOTDIR=${+ZDOTDIR}\ntypeset -g "
            "_INCOGNITO_TERMINAL_SOURCE_RC=$options[rcs]\nif [[ $_INCOGNITO_TERMINAL_SOURCE_RC == "
            "off ]]; then builtin unsetopt GLOBAL_RCS; fi\nbuiltin setopt RCS\n");
        if (clean)
            buf_str(&setup, "builtin unsetopt GLOBAL_RCS\n");
        buf_str(&setup, "export ZDOTDIR=");
        quote(&setup, s->directory);
        buf_str(&setup, "\n");
        if (file_write(s, ".zshenv", &setup))
            goto done;
        buf_str(&rc,
                "if (( _INCOGNITO_TERMINAL_HAS_ZDOTDIR )); then\n    export "
                "ZDOTDIR=$_INCOGNITO_TERMINAL_USER_DIR\nelse\n    builtin unset ZDOTDIR\nfi\n");
        if (!clean)
            buf_str(&rc,
                    "if [[ $_INCOGNITO_TERMINAL_SOURCE_RC == on && -r "
                    "$_INCOGNITO_TERMINAL_USER_DIR/.zshrc "
                    "]]; then\n    builtin source \"$_INCOGNITO_TERMINAL_USER_DIR/.zshrc\"\nfi\n");
        buf_str(&rc, "builtin unset _INCOGNITO_TERMINAL_USER_DIR _INCOGNITO_TERMINAL_HAS_ZDOTDIR "
                     "_INCOGNITO_TERMINAL_SOURCE_RC\n");
        if (mode != NORMAL)
            buf_str(&rc, mode == OFF ? zsh_off : zsh_session);
        buf_add(&rc, finish.data, finish.len);
        buf_str(&rc, zsh_prompt_hooks);
        if (file_write(s, ".zshrc", &rc))
            goto done;
        setenv("ZDOTDIR", s->directory, 1);
        const char *a[] = {argv[0], "-i"};
        argv_copy(s, a, 2);
    }
    result = 0;
done:
    buf_free(&finish);
    buf_free(&rc);
    buf_free(&setup);
    if (result)
        startup_cleanup(s);
    return result;
}
bool startup_ready(const Startup *s) {
    if (s->mode == NORMAL)
        return true;
    int fd = open(s->ready, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    char value[16] = {0};
    ssize_t n = read(fd, value, sizeof(value) - 1);
    close(fd);
    return n > 0 && (size_t)n == strlen(history_names[s->mode]) &&
           !memcmp(value, history_names[s->mode], (size_t)n);
}
