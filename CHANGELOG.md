# Changelog

All notable user-visible changes are recorded here. Versions follow Semantic
Versioning.

## 1.3.3 - 2026-10-15

- Replace the terse command-line help with a user-focused guide to options,
  history modes, session controls, and common launch examples.
- Preserve configurable modified-key shortcuts through tmux 3.4 as shipped by
  Ubuntu 24.04, while retaining CSI-u support on tmux 3.5 and newer.

## 1.3.2 - 2026-10-15

- Allow Incognito Terminal to start inside ordinary tmux panes whose server
  retained a stale protected-session environment marker.
- Remove the implementation label from version output and show the version at
  the beginning of command-line help.

## 1.3.1 - 2026-10-15

- Cancel an unused manual-mask shortcut when a tmux pane layout changes, so a
  later value is not concealed unexpectedly.
- Keep manual masking active across pane layout changes once sensitive input
  has begun.
- Improve modified-key handling in tmux terminals that advertise extended-key
  support.

## 1.3.0 - 2026-10-15

- Keep Incognito privacy controls active in tmux by protecting default panes
  and windows in a dedicated session.
- Support both shell-startup tmux and manual tmux launches, including detach,
  reattach, and returning to the original Incognito shell.
- Preserve existing tmux configuration while keeping ordinary tmux sessions
  separate and unchanged.

## 1.2.4 - 2026-10-15

- Preserve terminal profile colors for the standard ANSI foreground and
  background palette.

## 1.2.3 - 2026-10-15

- Replace the project README with a user-focused installation and usage guide.
- Remove the retired Python implementation and its packaging from the source tree.
- Keep the remaining integration tests focused exclusively on the native executable.
- Add an uninstall target for source installations.

## 1.2.2 - 2026-10-14

- Use portable Alt+I and Alt+U defaults for manual masking and temporary reveal.
- Keep Ctrl+M reserved for Enter.
- Preserve masking across Bash and Zsh backslash-newline continuations.
- Add configurable shortcut bindings and migration from earlier defaults.
- Add native amd64/arm64 CI and release packaging.

## 1.2.1 - 2026-10-14

- Correct prompt handling in Crazy mode.
- Keep prompts and command output visible while masking typed command input.

## 1.2.0 - 2026-10-14

- Add Asterisks, Obfuscation, and Emoji display styles.
- Add Crazy privacy mode.
- Add manual masking, hold-to-reveal, and persistent trigger/exception switches.
- Add explained submenus and improved menu spacing.

## 1.1.2 - 2026-10-14

- Add `-P` and expand command exceptions for common non-secret uses of `-p`.
- Preserve custom rules while merging new defaults during upgrades.

## 1.1.1 - 2026-10-14

- Add `--pass` and editable command exceptions.

## 1.1.0 - 2026-10-14

- Add Normal, Session only, and Off history modes.
- Make Normal the default.

## 1.0.0 - 2026-10-14

- First native C release with configurable masking for Bash and Zsh sessions.
