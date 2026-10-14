/*
 * Copyright 2026 Secragon Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cJSON.h"
#include "privacy.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
const char *history_names[] = {"normal", "session", "off"};
const char *privacy_style_names[] = {"asterisks", "obfuscation", "emoji"};
const char *privacy_style_labels[] = {"Asterisks", "Obfuscation", "Emoji"};
const char *history_labels[] = {"Normal", "Session only", "Off"};
const char *mode_names[] = {"attached", "separated", "attached_or_separated",
                            "equals_or_separated"};
const char *mode_labels[] = {"Attached only (no space)", "After a space",
                             "Attached or after a space", "After = or a space"};
ValueMode default_mode(const char *s) {
    size_t n = strlen(s);
    if (n && (s[n - 1] == ':' || s[n - 1] == '='))
        return ATTACHED;
    return n == 2 && s[0] == '-' ? BOTH : EQUALS;
}
static const Rule default_rules[] = {
    {"-p", BOTH}, {"--password", EQUALS}, {"/p:", ATTACHED}, {"--pass", EQUALS}, {"-P", BOTH},
};
_Static_assert(sizeof(default_rules) / sizeof(*default_rules) <= MAX_RULES,
               "Default triggers exceed settings capacity");
static const char *const default_exceptions[] = {
    /* Historical prefix used to infer defaults seen by legacy configs. */
    "ssh",
    "nmap",
    "nc",
    "docker",
    "psql",
    "redis-cli",
    "mkdir",
    "scp",
    "dirb",
    "ffuf",
    "feroxbuster",
    "masscan",
    "rustscan",
    "naabu",
    "dirbuster",
    /* Files, permissions, paths, and archives. */
    "cp",
    "rmdir",
    "install",
    "ln",
    "ls",
    "mktemp",
    "realpath",
    "tar",
    "du",
    "df",
    "chown",
    "chgrp",
    "tree",
    "getfacl",
    "setfacl",
    "getfattr",
    "lsattr",
    "chattr",
    "pwd",
    /* Text search, display, and patching. */
    "diff",
    "patch",
    "grep",
    "rg",
    "less",
    "more",
    "tee",
    "nl",
    "bat",
    "batcat",
    /* Processes and system statistics. */
    "lsof",
    "ps",
    "pgrep",
    "pkill",
    "pidwait",
    "pstree",
    "top",
    "htop",
    "renice",
    "vmstat",
    "mpstat",
    "pidstat",
    "uptime",
    "uname",
    "who",
    /* Networking: ports, packet patterns, and capture settings. */
    "ping",
    "ping6",
    "ss",
    "netstat",
    "tcpdump",
    "dumpcap",
    "traceroute",
    "traceroute6",
    "tracepath",
    "mtr",
    "arping",
    "hping3",
    "sftp",
    "nc.openbsd",
    "nc.traditional",
    "netcat",
    /* Packages, kernel settings, and hardware inspection. */
    "dpkg",
    "dpkg-query",
    "sysctl",
    "ldconfig",
    "ipcs",
    "findmnt",
    "lsblk",
    "lscpu",
    "lsns",
    "lslocks",
    "lsipc",
    "lspci",
    /* Compiler preprocessing/profiling and shell introspection. */
    "cc",
    "gcc",
    "g++",
    "c++",
    "clang",
    "clang++",
    "cpp",
    "type",
    "hash",
    "ulimit",
};
_Static_assert(sizeof(default_exceptions) / sizeof(*default_exceptions) <= MAX_EXCEPTIONS,
               "Default exceptions exceed settings capacity");
void settings_default(Settings *s) {
    *s = (Settings){.count = sizeof(default_rules) / sizeof(*default_rules),
                    .history = NORMAL,
                    .triggers_enabled = true,
                    .exclusions_enabled = true,
                    .shortcuts = {{105, 2}, {117, 2}},
                    .exception_count = sizeof(default_exceptions) / sizeof(*default_exceptions)};
    memcpy(s->rules, default_rules, sizeof(default_rules));
    for (int i = 0; i < s->exception_count; i++)
        strcpy(s->exceptions[i], default_exceptions[i]);
    s->seen_trigger_count = s->count;
    for (int i = 0; i < s->count; i++)
        strcpy(s->seen_triggers[i], s->rules[i].flag);
    s->seen_exception_count = s->exception_count;
    memcpy(s->seen_exceptions, s->exceptions, sizeof(s->seen_exceptions));
}
bool exception_valid(const char *name) {
    size_t n = strlen(name);
    if (!n || n > MAX_COMMAND || !(isalnum((unsigned char)name[0]) || name[0] == '_'))
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 128 || !(isalnum(c) || strchr("_.+-", c)))
            return false;
    }
    return true;
}
bool rule_valid(const Rule *r) {
    const unsigned char *s = (const unsigned char *)r->flag;
    size_t n = strlen(r->flag), i = 0;
    if (!n || n > MAX_FLAG || r->mode < ATTACHED || r->mode > EQUALS)
        return false;
    bool slash = s[0] == '/';
    if (s[0] != '-' && !slash)
        return false;
    i = 1;
    if (!slash && s[i] == '-')
        i++;
    if (!isalnum(s[i]) || s[i] >= 128)
        return false;
    for (i++; i < n && (isalnum(s[i]) || s[i] == '_' || s[i] == '-'); i++)
        if (s[i] >= 128)
            return false;
    if (i < n && (s[i] == ':' || (!slash && s[i] == '=')))
        i++;
    if (i != n || (slash && s[n - 1] != ':'))
        return false;
    if ((s[n - 1] == ':' || s[n - 1] == '=') && r->mode != ATTACHED)
        return false;
    return true;
}
static bool has_name(const char *names, size_t width, int count, const char *name) {
    for (int i = 0; i < count; i++)
        if (!strcmp(names + (size_t)i * width, name))
            return true;
    return false;
}
static bool has_rule(const Settings *s, const char *flag) {
    for (int i = 0; i < s->count; i++)
        if (!strcmp(s->rules[i].flag, flag))
            return true;
    return false;
}
void settings_merge_defaults(Settings *s, const Settings *defaults) {
    for (int i = 0; i < defaults->count; i++) {
        const Rule *r = &defaults->rules[i];
        if (has_name((const char *)s->seen_triggers, sizeof(s->seen_triggers[0]),
                     s->seen_trigger_count, r->flag))
            continue;
        if (s->seen_trigger_count == MAX_RULES)
            continue;
        if (!has_rule(s, r->flag)) {
            if (s->count == MAX_RULES)
                continue; /* Leave it unseen so an upgrade can retry after space is freed. */
            s->rules[s->count++] = *r;
        }
        strcpy(s->seen_triggers[s->seen_trigger_count++], r->flag);
    }
    bool empty_exceptions = !s->exception_count;
    for (int i = 0; i < defaults->exception_count; i++) {
        const char *name = defaults->exceptions[i];
        if (has_name((const char *)s->seen_exceptions, sizeof(s->seen_exceptions[0]),
                     s->seen_exception_count, name))
            continue;
        if (s->seen_exception_count == MAX_EXCEPTIONS)
            continue;
        if (!empty_exceptions && !has_name((const char *)s->exceptions, sizeof(s->exceptions[0]),
                                           s->exception_count, name)) {
            if (s->exception_count == MAX_EXCEPTIONS)
                continue;
            strcpy(s->exceptions[s->exception_count++], name);
        }
        strcpy(s->seen_exceptions[s->seen_exception_count++], name);
    }
}
static bool read_seen(const cJSON *list, char *names, size_t width, int *count, int maximum,
                      bool triggers) {
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > maximum)
        return false;
    *count = 0;
    for (int i = 0; i < cJSON_GetArraySize(list); i++) {
        const cJSON *item = cJSON_GetArrayItem(list, i);
        if (!cJSON_IsString(item) || strlen(item->valuestring) >= width)
            return false;
        if (triggers) {
            Rule r = {.mode = default_mode(item->valuestring)};
            strcpy(r.flag, item->valuestring);
            if (!rule_valid(&r))
                return false;
        } else if (!exception_valid(item->valuestring))
            return false;
        if (!has_name(names, width, *count, item->valuestring))
            strcpy(names + (size_t)(*count)++ * width, item->valuestring);
    }
    return true;
}
static void legacy_seen(Settings *s, const Settings *defaults, int version, bool exceptions) {
    /* Earlier schema generations shipped known default sets. Missing names in
     * those sets are treated as removals; only later additions are merged. */
    s->seen_trigger_count = version < 3 ? 3 : version < 4 ? 4 : 5;
    for (int i = 0; i < s->seen_trigger_count; i++)
        strcpy(s->seen_triggers[i], defaults->rules[i].flag);
    s->seen_exception_count = !exceptions    ? defaults->exception_count
                              : version >= 5 ? 97
                              : version < 4  ? 8
                                             : 15;
    for (int i = 0; i < s->seen_exception_count; i++)
        strcpy(s->seen_exceptions[i], defaults->exceptions[i]);
}
static int path_for(char *path, size_t n) {
    const char *base = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw)
            home = pw->pw_dir;
    }
    int k;
    if (base && base[0] == '/')
        k = snprintf(path, n, "%s/incognito-terminal/config.json", base);
    else if (home)
        k = snprintf(path, n, "%s/.config/incognito-terminal/config.json", home);
    else
        return -1;
    return k < 0 || (size_t)k >= n ? -1 : 0;
}
int settings_load(Settings *s) {
    settings_default(s);
    char path[4096];
    if (path_for(path, sizeof(path)))
        return -1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return errno == ENOENT ? 0 : -1;
    char *text = xcalloc(65538, 1);
    size_t n = fread(text, 1, 65537, f);
    int err = ferror(f);
    fclose(f);
    if (err || n > 65536 || memchr(text, 0, n) || strstr(text, "\\u0000")) {
        free(text);
        return -1;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, n + 1, &end, 1);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "version"),
          *hist = cJSON_GetObjectItemCaseSensitive(root, "history_mode"),
          *privacy_style = cJSON_GetObjectItemCaseSensitive(root, "privacy_style"),
          *list = cJSON_GetObjectItemCaseSensitive(root, "triggers"),
          *exceptions = cJSON_GetObjectItemCaseSensitive(root, "exceptions"),
          *seen = cJSON_GetObjectItemCaseSensitive(root, "defaults_seen");
    bool ok = (!v || (cJSON_IsNumber(v) &&
                      (v->valuedouble == 1 || v->valuedouble == 2 || v->valuedouble == 3 ||
                       v->valuedouble == 4 || v->valuedouble == 5 || v->valuedouble == 6))) &&
              cJSON_IsArray(list);
    Settings next = {0};
    next.triggers_enabled = next.exclusions_enabled = true;
    memcpy(next.shortcuts, s->shortcuts, sizeof(next.shortcuts));
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "triggers_enabled");
    if (enabled) {
        ok = ok && cJSON_IsBool(enabled);
        next.triggers_enabled = cJSON_IsTrue(enabled);
    }
    enabled = cJSON_GetObjectItemCaseSensitive(root, "exclusions_enabled");
    if (enabled) {
        ok = ok && cJSON_IsBool(enabled);
        next.exclusions_enabled = cJSON_IsTrue(enabled);
    }
    const cJSON *shortcuts = cJSON_GetObjectItemCaseSensitive(root, "shortcuts");
    if (shortcuts) {
        const char *names[] = {"mask", "reveal"};
        bool reserved_ctrl_m[SHORTCUT_COUNT] = {false};
        ok = ok && cJSON_IsObject(shortcuts);
        for (int i = 0; i < SHORTCUT_COUNT; i++) {
            const cJSON *binding = cJSON_GetObjectItemCaseSensitive(shortcuts, names[i]);
            if (!cJSON_IsString(binding))
                ok = false;
            else if (!strcasecmp(binding->valuestring, "Ctrl+M"))
                reserved_ctrl_m[i] = true;
            else if (!shortcut_parse(binding->valuestring, &next.shortcuts[i]))
                ok = false;
        }
        /* Older builds allowed Ctrl+M even though terminals encode it as
         * Enter. Migrate only that binding and preserve the rest of the file. */
        for (int i = 0; i < SHORTCUT_COUNT; i++) {
            if (!reserved_ctrl_m[i])
                continue;
            next.shortcuts[i] = s->shortcuts[i];
            int other = 1 - i;
            if (next.shortcuts[i].code == next.shortcuts[other].code &&
                next.shortcuts[i].modifiers == next.shortcuts[other].modifiers)
                next.shortcuts[i] = s->shortcuts[other];
        }
        /* Versions through 1.2.1 used Ctrl+Shift+I/U. Legacy terminals such
         * as QTerminal collapse those combinations to Tab/plain text. Move
         * the unchanged default pair to the portable Alt bindings while
         * leaving every customized pair alone. */
        const Shortcut legacy_defaults[SHORTCUT_COUNT] = {{105, 5}, {117, 5}};
        if (!memcmp(next.shortcuts, legacy_defaults, sizeof(legacy_defaults)))
            memcpy(next.shortcuts, s->shortcuts, sizeof(next.shortcuts));
        if (next.shortcuts[0].code == next.shortcuts[1].code &&
            next.shortcuts[0].modifiers == next.shortcuts[1].modifiers)
            ok = false;
    }
    next.exception_count = s->exception_count;
    memcpy(next.exceptions, s->exceptions, sizeof(next.exceptions));
    if (exceptions) {
        int count = cJSON_GetArraySize(exceptions);
        if (!cJSON_IsArray(exceptions) || count > MAX_EXCEPTIONS)
            ok = false;
        next.exception_count = 0;
        memset(next.exceptions, 0, sizeof(next.exceptions));
        for (int i = 0; ok && i < count; i++) {
            cJSON *name = cJSON_GetArrayItem(exceptions, i);
            if (!cJSON_IsString(name) || !exception_valid(name->valuestring)) {
                ok = false;
                break;
            }
            bool duplicate = false;
            for (int j = 0; j < next.exception_count; j++)
                if (!strcmp(next.exceptions[j], name->valuestring))
                    duplicate = true;
            if (!duplicate)
                strcpy(next.exceptions[next.exception_count++], name->valuestring);
        }
    }
    if (hist) {
        ok = ok && cJSON_IsString(hist);
        int h = -1;
        if (cJSON_IsString(hist))
            for (int i = 0; i < 3; i++)
                if (!strcmp(hist->valuestring, history_names[i]))
                    h = i;
        if (h < 0)
            ok = false;
        else
            next.history = (HistoryMode)h;
    }
    if (privacy_style) {
        int selected = -1;
        if (cJSON_IsString(privacy_style))
            for (int i = 0; i < 3; i++)
                if (!strcmp(privacy_style->valuestring, privacy_style_names[i]))
                    selected = i;
        if (selected < 0)
            ok = false;
        else
            next.privacy_style = (MaskStyle)selected;
    }
    int count = cJSON_GetArraySize(list);
    if (count < 1 || count > MAX_RULES)
        ok = false;
    for (int i = 0; ok && i < count; i++) {
        cJSON *obj = cJSON_GetArrayItem(list, i), *flag = obj, *mode = NULL;
        if (cJSON_IsObject(obj)) {
            flag = cJSON_GetObjectItemCaseSensitive(obj, "flag");
            mode = cJSON_GetObjectItemCaseSensitive(obj, "value_mode");
            if (cJSON_GetArraySize(obj) != 2 || !cJSON_IsString(mode)) {
                ok = false;
                break;
            }
        }
        if (!cJSON_IsString(flag) || strlen(flag->valuestring) > MAX_FLAG) {
            ok = false;
            break;
        }
        Rule r = {0};
        strcpy(r.flag, flag->valuestring);
        r.mode = default_mode(r.flag);
        if (mode) {
            int found = -1;
            for (int m = 0; m < 4; m++)
                if (!strcmp(mode->valuestring, mode_names[m]))
                    found = m;
            if (found < 0) {
                ok = false;
                break;
            }
            r.mode = (ValueMode)found;
        }
        if (!rule_valid(&r)) {
            ok = false;
            break;
        }
        bool duplicate = false;
        for (int j = 0; j < next.count; j++)
            if (!strcmp(r.flag, next.rules[j].flag)) {
                duplicate = true;
                if (r.mode != next.rules[j].mode)
                    ok = false;
            }
        if (!duplicate)
            next.rules[next.count++] = r;
    }
    if (ok) {
        if (v && v->valuedouble == 6) {
            ok = cJSON_IsObject(seen) && cJSON_GetArraySize(seen) == 2 &&
                 read_seen(cJSON_GetObjectItemCaseSensitive(seen, "triggers"),
                           (char *)next.seen_triggers, sizeof(next.seen_triggers[0]),
                           &next.seen_trigger_count, MAX_RULES, true) &&
                 read_seen(cJSON_GetObjectItemCaseSensitive(seen, "exceptions"),
                           (char *)next.seen_exceptions, sizeof(next.seen_exceptions[0]),
                           &next.seen_exception_count, MAX_EXCEPTIONS, false);
        } else {
            legacy_seen(&next, s, v ? (int)v->valuedouble : 2, exceptions != NULL);
        }
        if (ok)
            settings_merge_defaults(&next, s);
    }
    cJSON_Delete(root);
    if (!ok)
        return -1;
    *s = next;
    return 0;
}
static int parents(char *path) {
    for (char *p = path + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            int r = mkdir(path, 0700);
            *p = '/';
            if (r && errno != EEXIST)
                return -1;
        }
    return 0;
}
int settings_save(const Settings *s, SettingsPart part) {
    Settings saved;
    if (settings_load(&saved))
        return -1;
    if (part == SAVE_SHORTCUTS)
        memcpy(saved.shortcuts, s->shortcuts, sizeof(saved.shortcuts));
    else if (part == SAVE_STYLE)
        saved.privacy_style = s->privacy_style;
    else if (part == SAVE_HISTORY)
        saved.history = s->history;
    else if (part == SAVE_EXCEPTIONS) {
        saved.exclusions_enabled = s->exclusions_enabled;
        saved.exception_count = s->exception_count;
        memcpy(saved.exceptions, s->exceptions, sizeof(saved.exceptions));
    } else {
        saved.triggers_enabled = s->triggers_enabled;
        saved.count = s->count;
        memcpy(saved.rules, s->rules, sizeof(saved.rules));
    }
    if (saved.privacy_style < ASTERISKS || saved.privacy_style > EMOJI)
        return -1;
    if (saved.count < 1 || saved.count > MAX_RULES)
        return -1;
    for (int i = 0; i < saved.count; i++)
        if (!rule_valid(&saved.rules[i]))
            return -1;
    if (saved.exception_count < 0 || saved.exception_count > MAX_EXCEPTIONS)
        return -1;
    for (int i = 0; i < saved.exception_count; i++)
        if (!exception_valid(saved.exceptions[i]))
            return -1;
    char path[4096], tmp[4120];
    if (path_for(path, sizeof(path)) || parents(path))
        return -1;
    cJSON *root = cJSON_CreateObject(), *list = cJSON_CreateArray();
    if (!root || !list) {
        cJSON_Delete(root);
        cJSON_Delete(list);
        return -1;
    }
    cJSON_AddNumberToObject(root, "version", 6);
    cJSON_AddStringToObject(root, "history_mode", history_names[saved.history]);
    cJSON_AddStringToObject(root, "privacy_style", privacy_style_names[saved.privacy_style]);
    cJSON_AddBoolToObject(root, "triggers_enabled", saved.triggers_enabled);
    cJSON_AddBoolToObject(root, "exclusions_enabled", saved.exclusions_enabled);
    cJSON *bindings = cJSON_AddObjectToObject(root, "shortcuts");
    if (!bindings) {
        cJSON_Delete(root);
        cJSON_Delete(list);
        return -1;
    }
    const char *binding_names[] = {"mask", "reveal"};
    for (int i = 0; i < SHORTCUT_COUNT; i++) {
        char label[64];
        Shortcut checked;
        shortcut_label(saved.shortcuts[i], label, sizeof(label));
        if (!shortcut_parse(label, &checked)) {
            cJSON_Delete(root);
            cJSON_Delete(list);
            return -1;
        }
        cJSON_AddStringToObject(bindings, binding_names[i], label);
    }
    if (saved.shortcuts[0].code == saved.shortcuts[1].code &&
        saved.shortcuts[0].modifiers == saved.shortcuts[1].modifiers) {
        cJSON_Delete(root);
        cJSON_Delete(list);
        return -1;
    }
    cJSON_AddItemToObject(root, "triggers", list);
    cJSON *excluded = cJSON_AddArrayToObject(root, "exceptions");
    if (!excluded) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON *seen = cJSON_AddObjectToObject(root, "defaults_seen");
    cJSON *seen_triggers = seen ? cJSON_AddArrayToObject(seen, "triggers") : NULL;
    cJSON *seen_exceptions = seen ? cJSON_AddArrayToObject(seen, "exceptions") : NULL;
    if (!seen_triggers || !seen_exceptions) {
        cJSON_Delete(root);
        return -1;
    }
    for (int i = 0; i < saved.seen_trigger_count; i++) {
        cJSON *name = cJSON_CreateString(saved.seen_triggers[i]);
        if (!name) {
            cJSON_Delete(root);
            return -1;
        }
        cJSON_AddItemToArray(seen_triggers, name);
    }
    for (int i = 0; i < saved.seen_exception_count; i++) {
        cJSON *name = cJSON_CreateString(saved.seen_exceptions[i]);
        if (!name) {
            cJSON_Delete(root);
            return -1;
        }
        cJSON_AddItemToArray(seen_exceptions, name);
    }
    for (int i = 0; i < saved.exception_count; i++) {
        cJSON *name = cJSON_CreateString(saved.exceptions[i]);
        if (!name) {
            cJSON_Delete(root);
            return -1;
        }
        cJSON_AddItemToArray(excluded, name);
    }
    for (int i = 0; i < saved.count; i++) {
        cJSON *r = cJSON_CreateObject();
        if (!r) {
            cJSON_Delete(root);
            return -1;
        }
        cJSON_AddStringToObject(r, "flag", saved.rules[i].flag);
        cJSON_AddStringToObject(r, "value_mode", mode_names[saved.rules[i].mode]);
        cJSON_AddItemToArray(list, r);
    }
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text)
        return -1;
    snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path);
    int fd = mkstemp(tmp), result = -1;
    if (fd >= 0) {
        int failed = write_all(fd, text, strlen(text)) || write_all(fd, "\n", 1) || fsync(fd);
        if (close(fd))
            failed = 1;
        if (!failed && !rename(tmp, path))
            result = 0;
        unlink(tmp);
    }
    free(text);
    return result;
}
