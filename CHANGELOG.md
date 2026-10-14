# Changelog

All notable user-visible changes are recorded here. Versions follow Semantic
Versioning.

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
