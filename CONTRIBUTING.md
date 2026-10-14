# Contributing

Incognito Terminal is a native C11 project. The application has no Python
dependency. Python is used only by black-box integration tests that run the
compiled executable inside disposable terminal sessions.

## Development setup

On Kali, Debian, or Ubuntu:

```sh
sudo apt update
sudo apt install build-essential perl zsh python3-venv \
  lintian shellcheck mandoc desktop-file-utils appstream
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-test.txt
```

Build and run the full validation suite:

```sh
make -j
make test
make lint
make sanitize
make verify-release
```

Tests must use disposable values. Never commit real credentials, client data,
terminal history, generated configuration, build output, or release artifacts.

Keep changes focused and add regression coverage for behavior changes. Run
`clang-format` using `native/.clang-format` on modified native C files. Pull
requests should explain the user-visible result and list the checks that ran.

By submitting a contribution, you agree that it is provided under the
repository's Apache-2.0 license unless the modified file states another
license.
