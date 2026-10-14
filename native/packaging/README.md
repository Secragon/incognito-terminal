# Release metadata

Edit `release.conf` before a public build:

- `PACKAGE`: lowercase Debian package name.
- `DISPLAY_NAME`: name shown in package descriptions.
- `DEB_REVISION`: packaging revision, normally `1` for the first build of an
  upstream version.
- `MAINTAINER_NAME` and `MAINTAINER_EMAIL`: Debian package contact.
- `COMPANY`: application copyright owner (a person or legal entity).
- `COMPANY_URL`: public website for the copyright owner.
- `HOMEPAGE`: public project or company URL.
- `APP_LICENSE`: SPDX-style license identifier or the exact license name used
  by the application.
- `APP_LICENSE_FILE`: path, relative to the native project, containing the full
  application license text.
- `RELEASE_DATE`: release date in `YYYY-MM-DD` format. It is also used as the
  deterministic timestamp for generated archives.

The committed values describe the Incognito Terminal release identified by
`../VERSION`, owned by Secragon Ltd. Update that version and the release date
here for each future release.

Run `make release` from this directory, or `make release` from the repository
root. The build creates a standalone C source archive, a `.deb` for the build
machine's architecture, and `SHA256SUMS` in `dist/`.

GitHub's release workflow builds independently on native amd64 and arm64
runners and combines their packages with one source archive and one checksum
file. A local build produces only the current machine's architecture.

The release build normalizes compiler paths, timestamps, ownership, and logical
installed size. Rebuilding the same version with the same compiler and toolchain
must produce identical source and Debian package bytes.
