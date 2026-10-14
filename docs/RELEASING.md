# Release process

Releases are built from the standalone native source tree on GitHub-hosted
amd64 and arm64 Linux runners. A tagged release contains both Debian packages,
the portable C source archive, and combined SHA-256 checksums.

## Prepare

1. Update `native/VERSION` and the release section in `CHANGELOG.md`.
2. Update `RELEASE_DATE` and, when needed, `DEB_REVISION` in
   `native/packaging/release.conf`.
3. Update the release entry in
   `native/data/com.secragon.incognito-terminal.metainfo.xml`.
4. Run the complete local checks:

   ```sh
   make ci
   make sanitize
   ```

5. Confirm that `native/dist/SHA256SUMS` validates and that no generated files,
   credentials, client data, or disposable test output are staged.

## Publish

Create an annotated tag whose value exactly matches `v` followed by
`native/VERSION`, then push the commit and tag:

```sh
version=$(cat native/VERSION)
git tag -a "v$version" -m "Incognito Terminal $version"
git push origin main
git push origin "v$version"
```

The Release workflow tests and packages both architectures independently. It
publishes only after both builds pass. The workflow uses GitHub's temporary
`GITHUB_TOKEN`; no deploy key or personal token is stored in the repository.

A manual **Run workflow** invocation builds downloadable workflow artifacts but
does not publish a GitHub Release.

## Verify the published release

Download all four assets and verify them:

```sh
sha256sum -c SHA256SUMS
dpkg-deb --info incognito-terminal_*_amd64.deb
dpkg-deb --info incognito-terminal_*_arm64.deb
```

Confirm that the GitHub release contains exactly:

- `incognito-terminal-VERSION.tar.gz`
- `incognito-terminal_VERSION-REVISION_amd64.deb`
- `incognito-terminal_VERSION-REVISION_arm64.deb`
- `SHA256SUMS`
