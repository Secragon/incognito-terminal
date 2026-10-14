# Security policy

## Reporting a vulnerability

Report suspected vulnerabilities through GitHub's private vulnerability
reporting page:

<https://github.com/Secragon/incognito-terminal/security/advisories/new>

Do not open a public issue for a vulnerability and never include real
passwords, tokens, client names, command history, or unredacted terminal
captures. Reproduce problems with disposable values.

Include the Incognito Terminal version, Linux distribution, architecture,
terminal emulator, shell, and the smallest safe reproduction you can provide.

## Security boundary

Incognito Terminal controls terminal rendering and optional Bash/Zsh history
behavior. It does not encrypt commands, alter arguments sent to the shell, hide
arguments from process inspection, or prevent applications and remote systems
from logging them. The documented limitations in the README are part of the
current security model.
