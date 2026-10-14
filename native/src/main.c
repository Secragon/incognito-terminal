/*
 * Copyright 2026 Secragon Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "privacy.h"

#ifndef INCOGNITO_TERMINAL_VERSION
#define INCOGNITO_TERMINAL_VERSION "0.0.0"
#endif
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t stopped, resized;
static struct termios saved_tty;
static bool raw, terminal_entered;
static pid_t child = -1;
static int master = -1;
static Startup startup;
static void signal_handler(int sig) {
    if (sig == SIGWINCH)
        resized = 1;
    else
        stopped = sig;
}
static void restore(void) {
    if (terminal_entered) {
        const char *reset =
            "\033[<u\033[>4;0m\033[?1004l\033[0m\033[?7h\033[?25h\033[?2004l\033[?1l\033[?1049l";
        write_all(STDOUT_FILENO, reset, strlen(reset));
        terminal_entered = false;
    }
    if (raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tty);
        raw = false;
    }
    if (child > 0) {
        pid_t fg = master >= 0 ? tcgetpgrp(master) : -1;
        if (fg > 0)
            kill(-fg, SIGHUP);
        kill(-child, SIGHUP);
        double deadline = monotime() + 0.5;
        int status;
        while (monotime() < deadline) {
            pid_t k = waitpid(child, &status, WNOHANG);
            if (k == child || (k < 0 && errno == ECHILD)) {
                child = -1;
                break;
            }
            usleep(10000);
        }
        if (child > 0) {
            if (fg > 0)
                kill(-fg, SIGKILL);
            kill(-child, SIGKILL);
            while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
            }
            child = -1;
        }
    }
    if (master >= 0) {
        close(master);
        master = -1;
    }
    startup_cleanup(&startup);
}
static int dimensions(int *rows, int *cols) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w))
        return -1;
    *rows = MAX(2, (int)w.ws_row - 1);
    *cols = MAX(2, (int)w.ws_col);
    /* Bound allocation from externally supplied terminal dimensions. */
    return *rows > 1000 || *cols > 2000 ? -1 : 0;
}
static bool executable(const char *s) {
    if (strchr(s, '/'))
        return !access(s, X_OK);
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/bin:/bin";
    char *copy = strdup(path);
    if (!copy)
        return false;
    char *p = copy, *dir;
    bool ok = false;
    while ((dir = strsep(&p, ":"))) {
        char file[4096];
        int n = snprintf(file, sizeof(file), "%s/%s", *dir ? dir : ".", s);
        if (n > 0 && n < (int)sizeof(file) && !access(file, X_OK)) {
            ok = true;
            break;
        }
    }
    free(copy);
    return ok;
}
static int error(const char *s) {
    fprintf(stderr, "incognito-terminal: %s\n", s);
    return 2;
}
static int emit(Screen *s, Menu *m) {
    Buffer frame = {0};
    screen_render(s, m, &frame);
    int result = write_all(STDOUT_FILENO, frame.data, frame.len);
    buf_free(&frame);
    return result;
}
static int session(const Settings *settings) {
    int rows, cols;
    if (dimensions(&rows, &cols))
        return error("unsupported terminal size");
    if (tcgetattr(STDIN_FILENO, &saved_tty))
        return error("cannot read terminal settings");
    struct sigaction sa = {.sa_handler = signal_handler};
    sigemptyset(&sa.sa_mask);
    int sigs[] = {SIGWINCH, SIGTERM, SIGHUP, SIGINT, SIGQUIT};
    for (size_t i = 0; i < 5; i++)
        sigaction(sigs[i], &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    struct winsize ws = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols};
    child = forkpty(&master, NULL, NULL, &ws);
    if (child < 0)
        return error("cannot create child terminal");
    if (child == 0) {
        for (size_t i = 0; i < 5; i++)
            signal(sigs[i], SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        setenv("TERM", "xterm-256color", 1);
        setenv("INCOGNITO_TERMINAL", "1", 1);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        execvp(startup.argv[0], startup.argv);
        const char msg[] = "incognito-terminal: could not start child process\r\n";
        write_all(2, msg, sizeof(msg) - 1);
        _exit(127);
    }
    struct termios t = saved_tty;
    cfmakeraw(&t);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t))
        return error("cannot set raw terminal mode");
    raw = true;
    if (fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK) < 0)
        return error("cannot configure child terminal");
    terminal_entered = true;
    const char *enter = "\033[?1049h\033[>3u\033[?u\033[>4;2m\033[?1004h\033[2J\033[H";
    if (write_all(1, enter, strlen(enter)))
        return 1;
    Screen screen;
    screen_init(&screen, rows, cols);
    bool ready = startup_ready(&startup);
    Menu menu;
    menu_init(&menu, settings, ready ? (int)startup.mode : -1);
    Router router = {0};
    Buffer pending = {0};
    double deadline = monotime() + 10;
    int result = 0;
    bool alive = true;
    if (emit(&screen, &menu))
        alive = false, result = 1;
    while (alive && !stopped) {
        bool redraw = false;
        if (menu.notification_until && monotime() >= menu.notification_until) {
            menu.notification_until = 0;
            redraw = true;
        }
        if (menu.reveal_until && monotime() >= menu.reveal_until) {
            menu.reveal_until = 0;
            if (menu.revealing) {
                menu.revealing = false;
                redraw = true;
            }
        }
        if (!ready) {
            if (startup_ready(&startup)) {
                ready = true;
                menu.active_history = startup.mode;
                redraw = true;
            } else if (monotime() > deadline) {
                result = 2;
                break;
            }
        }
        bool was_open = menu.open;
        Page page = menu.page;
        router_flush(&router, &menu, &pending);
        if (was_open != menu.open || page != menu.page)
            redraw = true;
        if (resized) {
            resized = 0;
            if (dimensions(&rows, &cols)) {
                result = 2;
                break;
            }
            screen_resize(&screen, rows, cols, &menu.settings);
            ws.ws_row = (unsigned short)rows;
            ws.ws_col = (unsigned short)cols;
            ioctl(master, TIOCSWINSZ, &ws);
            redraw = true;
        }
        if (redraw && emit(&screen, &menu)) {
            result = 1;
            break;
        }
        struct pollfd fds[2] = {{.fd = pending.len > 65536 ? -1 : 0, .events = POLLIN},
                                {.fd = master, .events = POLLIN}};
        if (screen.responses.len || (ready && pending.len))
            fds[1].events |= POLLOUT;
        int polled = poll(fds, 2, 50);
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            result = 1;
            break;
        }
        if (fds[0].revents & POLLIN) {
            char bytes[4096];
            ssize_t n = read(0, bytes, sizeof(bytes));
            if (n <= 0) {
                if (n < 0 && errno == EINTR)
                    continue;
                break;
            }
            menu.shell_input = screen.input_area && !screen.alt;
            router_feed(&router, &menu, &pending, bytes, (size_t)n);
            explicit_bzero(bytes, sizeof(bytes));
            if (emit(&screen, &menu)) {
                result = 1;
                break;
            }
        }
        if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL))
            break;
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            char bytes[16384];
            ssize_t n = read(master, bytes, sizeof(bytes));
            if (n > 0) {
                screen_feed(&screen, bytes, (size_t)n);
                explicit_bzero(bytes, sizeof(bytes));
                if (emit(&screen, &menu)) {
                    result = 1;
                    break;
                }
            } else if (!n || (n < 0 && errno == EIO))
                alive = false;
            else if (errno != EINTR && errno != EAGAIN) {
                result = 1;
                break;
            }
        }
        if (fds[1].revents & POLLOUT) {
            Buffer *b = screen.responses.len ? &screen.responses : &pending;
            if (b->len && (b == &screen.responses || ready)) {
                ssize_t n = write(master, b->data, b->len);
                if (n > 0)
                    buf_consume(b, (size_t)n);
                else if (n < 0 && errno != EAGAIN && errno != EINTR)
                    alive = false;
            }
        }
    }
    if (stopped)
        result = 128 + stopped;
    else if (!ready)
        result = 2;
    else if (!alive && !result) {
        int status;
        pid_t pid;
        double end = monotime() + 0.5;
        do {
            pid = waitpid(child, &status, WNOHANG);
            if (pid == 0)
                usleep(10000);
        } while ((pid == 0 || (pid < 0 && errno == EINTR)) && !stopped && monotime() < end);
        if (pid == child) {
            child = -1;
            result = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
    }
    screen_free(&screen);
    buf_free(&router.pending);
    buf_free(&pending);
    restore();
    if (!ready)
        fputs(
            "incognito-terminal: History setup failed before the protected session could start.\n",
            stderr);
    return result;
}
int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "");
    bool clean = false;
    int override = -1, i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            puts("Usage: incognito-terminal [--clean] [--history normal|session|off] [-- SHELL "
                 "[ARGS...]]\n\nMask password arguments on screen. F2 opens Privacy "
                 "settings.\n--clean    Skip Bash/Zsh user startup files.\n--history  Override the "
                 "saved history preference for this launch.\nDefault history: Normal (the shell's "
                 "usual behavior).");
            return 0;
        }
        if (!strcmp(argv[i], "--version")) {
            puts("incognito-terminal " INCOGNITO_TERMINAL_VERSION " (native C)");
            return 0;
        }
        if (!strcmp(argv[i], "--clean")) {
            clean = true;
            continue;
        }
        const char *value = NULL;
        if (!strcmp(argv[i], "--history")) {
            if (++i == argc)
                return error("--history requires normal, session, or off");
            value = argv[i];
        } else if (!strncmp(argv[i], "--history=", 10))
            value = argv[i] + 10;
        if (value) {
            override = -1;
            for (int k = 0; k < 3; k++)
                if (!strcmp(value, history_names[k]))
                    override = k;
            if (override < 0)
                return error("invalid history mode");
            continue;
        }
        if (argv[i][0] == '-')
            return error("unknown option; see --help");
        break;
    }
    char *fallback[] = {getenv("SHELL"), NULL};
    if (!fallback[0] || !fallback[0][0])
        fallback[0] = "/bin/bash";
    char **command = i < argc ? argv + i : fallback;
    if (!executable(command[0]))
        return error("shell executable was not found");
    if (!isatty(0) || !isatty(1))
        return error("run this inside an interactive terminal");
    if (getenv("INCOGNITO_TERMINAL") && !strcmp(getenv("INCOGNITO_TERMINAL"), "1"))
        return error("already inside a protected session; use exit to return");
    Settings settings;
    if (settings_load(&settings))
        return error("cannot read incognito-terminal/config.json");
    /* Avoid writing password-bearing process memory to a core dump. */
    struct rlimit limit = {0, 0};
    setrlimit(RLIMIT_CORE, &limit);
    atexit(restore);
    if (startup_prepare(&startup, command, clean,
                        override >= 0 ? (HistoryMode) override : settings.history))
        return error("cannot prepare shell; --clean and Session-only/Off require a plain Bash or "
                     "Zsh launch");
    return session(&settings);
}
