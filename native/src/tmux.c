/*
 * Copyright 2026 Secragon Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "privacy.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define TMUX_SOCKET "incognito-terminal"
#define TMUX_SESSION "incognito-terminal"

static const char *base_name(const char *path) {
    const char *slash = strrchr(path ? path : "", '/');
    return slash ? slash + 1 : path;
}

static bool resolve_executable(const char *name, char *resolved, size_t size) {
    if (!name || !*name)
        return false;
    if (strchr(name, '/')) {
        char *path = realpath(name, resolved);
        return path && !access(resolved, X_OK);
    }
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/bin:/bin";
    char *copy = strdup(path);
    if (!copy)
        return false;
    bool found = false;
    char *cursor = copy, *directory;
    while ((directory = strsep(&cursor, ":"))) {
        char candidate[PATH_MAX];
        int n = snprintf(candidate, sizeof(candidate), "%s/%s", *directory ? directory : ".", name);
        if (n > 0 && n < (int)sizeof(candidate) && !access(candidate, X_OK) &&
            realpath(candidate, resolved)) {
            found = true;
            break;
        }
    }
    free(copy);
    return found && strlen(resolved) < size;
}

static void shell_quote(Buffer *out, const char *text) {
    buf_str(out, "'");
    for (; *text; text++)
        if (*text == '\'')
            buf_str(out, "'\\''");
        else
            buf_add(out, text, 1);
    buf_str(out, "'");
}

int tmux_prepare(const Startup *s, const char *shell, bool clean, int history_override) {
    if (!s->directory[0] || !shell || !*shell || (getenv("TMUX") && *getenv("TMUX")))
        return 0;

    char real_tmux[PATH_MAX], real_shell[PATH_MAX], binary[PATH_MAX];
    if (!resolve_executable("tmux", real_tmux, sizeof(real_tmux)))
        return 0; /* tmux is optional. */
    if (!resolve_executable(shell, real_shell, sizeof(real_shell)))
        return -1;
    ssize_t length = readlink("/proc/self/exe", binary, sizeof(binary) - 1);
    if (length <= 0 || length >= (ssize_t)sizeof(binary) - 1)
        return -1;
    binary[length] = 0;

    char bindir[PATH_MAX], shim[PATH_MAX];
    if (snprintf(bindir, sizeof(bindir), "%s/bin", s->directory) >= (int)sizeof(bindir) ||
        snprintf(shim, sizeof(shim), "%s/tmux", bindir) >= (int)sizeof(shim))
        return -1;
    if (mkdir(bindir, 0700) && errno != EEXIST)
        return -1;
    if (symlink(binary, shim) && errno != EEXIST)
        return -1;

    Buffer pane = {0};
    buf_str(&pane, "INCOGNITO_TMUX_PANE=1 exec ");
    shell_quote(&pane, binary);
    if (clean)
        buf_str(&pane, " --clean");
    if (history_override >= 0)
        buf_printf(&pane, " --history %s", history_names[history_override]);
    buf_str(&pane, " -- ");
    shell_quote(&pane, real_shell);
    buf_str(&pane, "\0");

    const char *old_path = getenv("PATH");
    Buffer path = {0};
    buf_str(&path, bindir);
    buf_str(&path, ":");
    buf_str(&path, old_path ? old_path : "/usr/bin:/bin");
    buf_str(&path, "\0");
    int result = setenv("INCOGNITO_REAL_TMUX", real_tmux, 1) ||
                 setenv("INCOGNITO_TMUX_COMMAND", pane.data, 1) || setenv("PATH", path.data, 1);
    buf_free(&pane);
    buf_free(&path);
    return result ? -1 : 0;
}

bool tmux_shim_invocation(const char *argv0) {
    return argv0 && !strcmp(base_name(argv0), "tmux") && getenv("INCOGNITO_REAL_TMUX") &&
           getenv("INCOGNITO_TMUX_COMMAND");
}

static int wait_child(pid_t pid) {
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return 1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

static int run_tmux(const char *real, const char *const arguments[], bool quiet) {
    size_t count = 0;
    while (arguments[count])
        count++;
    char **argv = xcalloc(count + 4, sizeof(char *));
    argv[0] = (char *)real;
    argv[1] = "-L";
    argv[2] = TMUX_SOCKET;
    for (size_t i = 0; i < count; i++)
        argv[i + 3] = (char *)arguments[i];
    pid_t pid = fork();
    if (pid < 0) {
        free(argv);
        return 1;
    }
    if (!pid) {
        if (quiet) {
            int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        execv(real, argv);
        _exit(127);
    }
    free(argv);
    return wait_child(pid);
}

static int capture_tmux(const char *real, const char *const arguments[], char *output,
                        size_t size) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC))
        return 1;
    size_t count = 0;
    while (arguments[count])
        count++;
    char **argv = xcalloc(count + 4, sizeof(char *));
    argv[0] = (char *)real;
    argv[1] = "-L";
    argv[2] = TMUX_SOCKET;
    for (size_t i = 0; i < count; i++)
        argv[i + 3] = (char *)arguments[i];
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(argv);
        return 1;
    }
    if (!pid) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execv(real, argv);
        _exit(127);
    }
    close(pipefd[1]);
    free(argv);
    size_t used = 0;
    while (used + 1 < size) {
        ssize_t n = read(pipefd[0], output + used, size - used - 1);
        if (n > 0)
            used += (size_t)n;
        else if (!n)
            break;
        else if (errno != EINTR)
            break;
    }
    output[used] = 0;
    close(pipefd[0]);
    return wait_child(pid);
}

static bool transport_signal(char mode) {
    const char *value = getenv("INCOGNITO_CONTROL_FD");
    if (!value)
        return false;
    char *end;
    long fd = strtol(value, &end, 10);
    if (!*value || *end || fd < 3 || fd > INT_MAX)
        return false;
    ssize_t written;
    do {
        written = write((int)fd, &mode, 1);
    } while (written < 0 && errno == EINTR);
    return written == 1;
}

static void clear_wrapper_identity(void) {
    unsetenv("INCOGNITO_TERMINAL");
    unsetenv("INCOGNITO_CONTROL_FD");
    unsetenv("INCOGNITO_TMUX_PANE");
}

static int configure_server(const char *real, const char *command, bool *legacy_keys) {
    const char *set_default[] = {"set-option", "-g", "default-command", command, NULL};
    const char *set_extended[] = {"set-option", "-s", "extended-keys", "always", NULL};
    const char *set_format[] = {"set-option", "-s", "extended-keys-format", "csi-u", NULL};
    const char *set_features[] = {"set-option", "-sa", "terminal-features",
                                  ",xterm-256color:extkeys", NULL};
    const char *show_features[] = {"show-options", "-sv", "terminal-features", NULL};
    if (run_tmux(real, set_default, false))
        return 1;
    if (run_tmux(real, set_extended, true)) {
        const char *fallback[] = {"set-option", "-s", "extended-keys", "on", NULL};
        if (run_tmux(real, fallback, true))
            return 1;
    }
    *legacy_keys = run_tmux(real, set_format, true) != 0;
    char features[4096];
    if (capture_tmux(real, show_features, features, sizeof(features)) ||
        !strstr(features, "xterm-256color:extkeys"))
        (void)run_tmux(real, set_features, true);
    return 0;
}

static int protected_client(const char *real, const char *command) {
    const char *has[] = {"has-session", "-t", "=" TMUX_SESSION, NULL};
    if (run_tmux(real, has, true)) {
        const char *create[] = {"new-session", "-d", "-s", TMUX_SESSION, command, NULL};
        if (run_tmux(real, create, false) && run_tmux(real, has, true))
            return 1;
        const char *mark[] = {"set-option", "-t", TMUX_SESSION, "@incognito_protected", "1", NULL};
        if (run_tmux(real, mark, false))
            return 1;
    } else {
        const char *show[] = {"show-options",         "-t", TMUX_SESSION, "-qv",
                              "@incognito_protected", NULL};
        char marker[32];
        if (capture_tmux(real, show, marker, sizeof(marker)) ||
            (strcmp(marker, "1") && strcmp(marker, "1\n"))) {
            fprintf(stderr, "incognito-terminal: the private tmux session is not protected\n");
            return 1;
        }
    }
    bool legacy_keys;
    if (configure_server(real, command, &legacy_keys))
        return 1;
    const char *attach[] = {"attach-session", "-t", "=" TMUX_SESSION, NULL};
    if (!transport_signal(legacy_keys ? 'L' : 'T')) {
        fputs("incognito-terminal: could not activate tmux transport\n", stderr);
        return 1;
    }
    int result = run_tmux(real, attach, false);
    (void)transport_signal('N');
    return result;
}

int tmux_shim_main(int argc, char **argv) {
    const char *real = getenv("INCOGNITO_REAL_TMUX");
    const char *command = getenv("INCOGNITO_TMUX_COMMAND");
    if (!real || !command)
        return 127;

    const char *descriptor = getenv("INCOGNITO_CONTROL_FD");
    if (descriptor) {
        char *end;
        long fd = strtol(descriptor, &end, 10);
        if (*descriptor && !*end && fd >= 3 && fd <= INT_MAX) {
            int flags = fcntl((int)fd, F_GETFD);
            if (flags >= 0)
                fcntl((int)fd, F_SETFD, flags | FD_CLOEXEC);
        }
    }

    /* tmux commands issued from a protected pane already inherit TMUX and
     * should target that server without another wrapper layer. */
    if (getenv("TMUX") && *getenv("TMUX")) {
        clear_wrapper_identity();
        argv[0] = (char *)real;
        execv(real, argv);
        return 127;
    }

    /* Bare `tmux` is the interactive entry point used both by shell startup
     * files and by a user launching tmux later in the session. */
    if (argc == 1)
        return protected_client(real, command);

    /* Keep administrative tmux commands predictable. They still target the
     * user's normal server and do not switch the terminal into transport. */
    clear_wrapper_identity();
    argv[0] = (char *)real;
    execv(real, argv);
    return 127;
}
