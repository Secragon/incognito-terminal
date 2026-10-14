# Incognito Terminal

Incognito Terminal keeps passwords, tokens, and other sensitive command-line
values out of sight while you type, paste, share your screen, or record a
terminal session.

It can conceal values recognized by your own triggers, mask the next value on
demand, or hide the complete command with Crazy mode. The original command is
still sent to your shell unchanged and executes only once.

## Build

On Kali Linux, Debian, or Ubuntu:

```sh
sudo apt update
sudo apt install build-essential perl
make -j
```

Start a session directly from the build directory:

```sh
./build/incognito-terminal
```

Choose a shell explicitly when needed:

```sh
./build/incognito-terminal -- bash
./build/incognito-terminal -- zsh
```

Install it for all users:

```sh
sudo make install
```

This installs the command, desktop application entries, icons, documentation,
and manual pages under `/usr/local`. Remove that installation with:

```sh
sudo make uninstall
```

## Use

Press **F2** during a session to open the settings menu.

- **Privacy** selects On, Off, or Crazy.
- **Privacy mode** displays concealed text as asterisks, password-like
  characters, or smiley emojis.
- **Triggers** controls which option values are concealed automatically.
- **Exceptions** skips automatic detection for selected commands.
- **History** chooses Normal, Session only, or Off for new sessions.
- **Shortcuts** changes the keys used for manual masking and temporary reveal.

Press **Alt+I** to conceal the next value manually. Hold **Alt+U** to reveal
concealed text temporarily. Both shortcuts can be changed from the menu.

Settings are saved under `~/.config/incognito-terminal/` and custom choices are
preserved across upgrades.

## Create packages

```sh
make source
make deb
make release
```

The generated source archive, Debian package, and checksums are written to
`dist/`. Debian packaging also requires `dpkg-deb`, `dpkg-shlibdeps`, and
`dpkg-architecture`.

## Supported systems

The source supports current glibc-based Linux systems on amd64 and arm64. It is
tested with current Kali Linux, Debian 13+, Ubuntu 24.04+, WSL2, Bash, and Zsh.
Native macOS is not currently supported; Linux virtual machines on macOS use
the build or package matching the guest architecture.

## Privacy scope

Incognito Terminal controls what is shown in its terminal window. The original
value remains part of the command and may still be available to the receiving
program, process inspection tools, remote systems, audit services, or
application logs.

Use disposable values to check your workflow before handling sensitive data.
Complex shell expansions, nested terminal sessions, full-screen applications,
and shell plugins can behave differently.

For the complete user guide, releases, issue tracker, and security reporting,
visit <https://github.com/Secragon/incognito-terminal>.

Incognito Terminal is licensed under Apache License 2.0 and is copyright 2026
Secragon Ltd.
