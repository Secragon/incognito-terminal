/*
 * Copyright 2026 Secragon Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
/* Shell startup policies used by protected history modes. */
static const char bash_session[] =
    "\n"
    "HISTFILE=/dev/null\n"
    "export HISTFILE\n"
    "builtin history -c\n"
    "\n"
    "if [[ ! $HISTSIZE =~ ^[1-9][0-9]*$ ]]; then HISTSIZE=1000; fi\n"
    "builtin set -o history\n"
    "[[ $HISTFILE == /dev/null && $HISTSIZE -gt 0 && -o history ]] || builtin exit 125\n";
static const char bash_off[] =
    "\n"
    "HISTFILE=/dev/null\n"
    "export HISTFILE\n"
    "builtin history -c\n"
    "\n"
    "HISTSIZE=0\n"
    "builtin set +o history\n"
    "[[ $HISTFILE == /dev/null && $HISTSIZE == 0 && ! -o history ]] || builtin exit 125\n";
static const char zsh_session[] =
    "\n"
    "HISTFILE=/dev/null\n"
    "SAVEHIST=0\n"
    "export HISTFILE\n"
    "builtin unsetopt APPEND_HISTORY INC_APPEND_HISTORY INC_APPEND_HISTORY_TIME SHARE_HISTORY\n"
    "[[ $HISTFILE == /dev/null && $SAVEHIST == 0 ]] || builtin exit 125\n"
    "\n"
    "if (( HISTSIZE < 1 )); then HISTSIZE=1000; fi\n"
    "builtin fc -p /dev/null $HISTSIZE 0 || builtin exit 125\n";
static const char zsh_off[] =
    "\n"
    "HISTFILE=/dev/null\n"
    "SAVEHIST=0\n"
    "export HISTFILE\n"
    "builtin unsetopt APPEND_HISTORY INC_APPEND_HISTORY INC_APPEND_HISTORY_TIME SHARE_HISTORY\n"
    "[[ $HISTFILE == /dev/null && $SAVEHIST == 0 ]] || builtin exit 125\n"
    "\n"
    "HISTSIZE=0\n"
    "builtin fc -p /dev/null 0 0 || builtin exit 125\n"
    "_incognito_terminal_forget_history() {\n"
    "    builtin fc -P\n"
    "    HISTFILE=/dev/null\n"
    "    SAVEHIST=0\n"
    "    HISTSIZE=0\n"
    "    builtin fc -p /dev/null 0 0\n"
    "}\n"
    "# HISTSIZE=0 alone still allows immediate recall of the last Zsh command.\n"
    "# Rotate one private history context before each prompt to discard that line.\n"
    "precmd_functions=(${precmd_functions:#_incognito_terminal_forget_history} "
    "_incognito_terminal_forget_history)\n";
