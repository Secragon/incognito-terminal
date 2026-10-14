# Bundled dependencies

Both libraries are compiled into the native executable. Their license notices
are retained here and installed by `make install`. No network access is needed
during a build.

- **libvterm 0.3.3**, MIT, Paul Evans and contributors.
  Source: https://github.com/neovim/libvterm
  Commit: `9d6d2112335080312ef8c36667fa717ded4f7daf` (tag `v0.3.3`).
  Download: https://github.com/neovim/libvterm/archive/9d6d2112335080312ef8c36667fa717ded4f7daf.tar.gz
  License: `libvterm/LICENSE`.
  Encoding `.inc` files are generated from the upstream `.tbl` files using its
  Perl generator with `-CSD`, as specified in its Makefile.
- **cJSON 1.7.19**, MIT, Dave Gamble and contributors.
  Source: https://github.com/DaveGamble/cJSON/tree/v1.7.19
  Files: `cJSON.c`, `cJSON.h`, `LICENSE` from the tag above.
  License: `cjson/LICENSE`.

The app uses libvterm's public state callbacks with its own screen cells and
privacy markings. It does not depend on libvterm's private data structures.
