#!/bin/sh
# Copyright 2026 Secragon Ltd
# SPDX-License-Identifier: Apache-2.0
set -eu
umask 022

mode=${1:-all}
case "$mode" in
    source|deb|all) ;;
    *) printf '%s\n' "Usage: $0 source|deb|all" >&2; exit 2 ;;
esac

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"

# The script changes to the native source root before loading this fixed path.
# shellcheck disable=SC1091
. ./packaging/release.conf
version=$(sed -n '1p' VERSION)
deb_version=$version-$DEB_REVISION
dist_dir=$project_dir/dist
release_build=$project_dir/build/release
package_root=$project_dir/build/deb-root
source_file=$dist_dir/$PACKAGE-$version.tar.gz

fail() {
    printf 'release: %s\n' "$1" >&2
    exit 1
}

for required_value in PACKAGE DISPLAY_NAME DEB_REVISION MAINTAINER_NAME \
    MAINTAINER_EMAIL COMPANY COMPANY_URL HOMEPAGE APP_LICENSE APP_LICENSE_FILE RELEASE_DATE
do
    eval "value=\${$required_value-}"
    [ -n "$value" ] || fail "$required_value is missing from packaging/release.conf"
done

printf '%s' "$PACKAGE" | grep -Eq '^[a-z0-9][a-z0-9+.-]+$' || \
    fail "PACKAGE is not a valid Debian package name"
printf '%s' "$RELEASE_DATE" | grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2}$' || \
    fail "RELEASE_DATE must use YYYY-MM-DD"
case "$APP_LICENSE_FILE" in
    /*|../*|*/../*) fail "APP_LICENSE_FILE must stay inside the native source tree" ;;
esac

if [ "${ALLOW_PLACEHOLDERS:-0}" != 1 ]; then
    case "$MAINTAINER_NAME|$MAINTAINER_EMAIL|$COMPANY|$COMPANY_URL|$HOMEPAGE|$APP_LICENSE" in
        *REPLACE*|*.invalid*|*UNLICENSED*)
            fail "replace the identity, homepage, and license placeholders first"
            ;;
    esac
    [ -f "$APP_LICENSE_FILE" ] || \
        fail "application license file not found: $APP_LICENSE_FILE"
fi

source_date_epoch=${SOURCE_DATE_EPOCH:-$(date -u -d "$RELEASE_DATE 00:00:00" +%s)}
export SOURCE_DATE_EPOCH="$source_date_epoch"
mkdir -p "$dist_dir"

build_source() {
    rm -f "$source_file"
    # Include only source, documentation, metadata, and installable assets.
    set -- .clang-format Makefile VERSION README.md NOTICE src tests vendor \
        data/incognito-terminal.desktop data/incognito-terminal-root.desktop \
        data/com.secragon.incognito-terminal.metainfo.xml \
        data/incognito-terminal.lintian-overrides \
        data/icons/README.md data/icons/hicolor docs packaging scripts
    if [ -f "$APP_LICENSE_FILE" ]; then
        set -- "$@" "$APP_LICENSE_FILE"
    fi
    tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 \
        --numeric-owner --transform="s,^,$PACKAGE-$version/," -cf - "$@" | \
        gzip -n -9 > "$source_file"
    printf 'Created %s\n' "$source_file"
}

write_copyright() {
    copyright_file=$1
    {
        printf 'Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n'
        printf 'Upstream-Name: %s\n' "$DISPLAY_NAME"
        printf 'Upstream-Contact: %s <%s>\n' "$MAINTAINER_NAME" "$MAINTAINER_EMAIL"
        printf 'Source: %s\n\n' "$HOMEPAGE"
        printf 'Files: *\n'
        printf 'Copyright: %s %s\n' "${RELEASE_DATE%%-*}" "$COMPANY"
        printf 'License: %s\n' "$APP_LICENSE"
        printf ' On Debian systems, the complete Apache License 2.0 text is available at\n'
        printf ' /usr/share/common-licenses/Apache-2.0.\n'
        printf '\nFiles: data/com.secragon.incognito-terminal.metainfo.xml\n'
        printf 'Copyright: %s %s\n' "${RELEASE_DATE%%-*}" "$COMPANY"
        printf 'License: FSFAP\n'
        printf ' Copying and distribution of this file, with or without modification, are\n'
        printf ' permitted in any medium without royalty provided the copyright notice and\n'
        printf ' this notice are preserved. This file is offered as-is, without any warranty.\n'
        printf '\nFiles: vendor/libvterm/*\n'
        printf 'Copyright: 2008 Paul Evans <leonerd@leonerd.org.uk>\n'
        printf 'License: MIT\n\n'
        printf 'Files: vendor/cjson/*\n'
        printf 'Copyright: 2009-2017 Dave Gamble and cJSON contributors\n'
        printf 'License: MIT\n\n'
        printf 'License: MIT\n'
        sed -n '/^Permission is hereby granted/,$ { s/^$/./; s/^/ /; p; }' \
            vendor/libvterm/LICENSE
    } > "$copyright_file"
}

runtime_dependencies() {
    dependency_tmp=$(mktemp -d)
    mkdir -p "$dependency_tmp/debian"
    {
        printf 'Source: %s\nSection: utils\nPriority: optional\n' "$PACKAGE"
        printf 'Maintainer: %s <%s>\nStandards-Version: 4.6.2\n\n' \
            "$MAINTAINER_NAME" "$MAINTAINER_EMAIL"
        printf 'Package: %s\nArchitecture: any\nDescription: temporary metadata\n' "$PACKAGE"
    } > "$dependency_tmp/debian/control"
    ln -s "$package_root" "$dependency_tmp/debian/$PACKAGE"
    dependency_line=$(cd "$dependency_tmp" && \
        dpkg-shlibdeps -O -e"debian/$PACKAGE/usr/bin/incognito-terminal")
    rm -rf "$dependency_tmp"
    printf '%s' "${dependency_line#shlibs:Depends=}"
}

build_deb() {
    for required_command in dpkg dpkg-architecture dpkg-deb dpkg-shlibdeps gzip strip; do
        command -v "$required_command" >/dev/null 2>&1 || \
            fail "required command not found: $required_command"
    done
    architecture=$(dpkg-architecture -qDEB_HOST_ARCH)
    deb_file=$dist_dir/${PACKAGE}_${deb_version}_${architecture}.deb

    rm -rf "$release_build" "$package_root"
    "$(command -v make)" BUILD="$release_build" all
    "$(command -v make)" BUILD="$release_build" PACKAGE="$PACKAGE" \
        PREFIX=/usr DESTDIR="$package_root" install
    strip --strip-unneeded "$package_root/usr/bin/incognito-terminal"
    gzip -n -9 "$package_root/usr/share/man/man1/incognito-terminal.1" \
        "$package_root/usr/share/man/man1/incognito-terminal-root.1"
    desktop_file=$package_root/usr/share/applications/$PACKAGE.desktop
    awk -v display_name="$DISPLAY_NAME" \
        '/^Name=/{print "Name=" display_name; next} {print}' "$desktop_file" \
        > "$desktop_file.tmp"
    mv "$desktop_file.tmp" "$desktop_file"
    rm -rf "$package_root/usr/share/licenses"

    doc_dir=$package_root/usr/share/doc/$PACKAGE
    mkdir -p "$doc_dir" "$package_root/DEBIAN"
    write_copyright "$doc_dir/copyright"
    {
        printf '%s (%s) stable; urgency=medium\n\n' "$PACKAGE" "$deb_version"
        printf '  * Native application release.\n\n'
        printf ' -- %s <%s>  %s\n' "$MAINTAINER_NAME" "$MAINTAINER_EMAIL" \
            "$(LC_ALL=C date -u -d "@$SOURCE_DATE_EPOCH" '+%a, %d %b %Y %H:%M:%S +0000')"
    } | gzip -n -9 > "$doc_dir/changelog.Debian.gz"

    depends=$(runtime_dependencies)
    # Use logical file sizes instead of filesystem block allocation so the
    # control metadata is stable across ext4, overlayfs, and CI workspaces.
    installed_size=$(find "$package_root/usr" -type f -printf '%s\n' | \
        awk '{bytes += $1} END {print int((bytes + 1023) / 1024)}')
    {
        printf 'Package: %s\n' "$PACKAGE"
        printf 'Version: %s\n' "$deb_version"
        printf 'Architecture: %s\n' "$architecture"
        printf 'Maintainer: %s <%s>\n' "$MAINTAINER_NAME" "$MAINTAINER_EMAIL"
        printf 'Installed-Size: %s\n' "$installed_size"
        printf 'Depends: %s, pkexec\n' "$depends"
        printf 'Section: utils\nPriority: optional\n'
        printf 'Homepage: %s\n' "$HOMEPAGE"
        printf 'Description: privacy controls for interactive terminal sessions\n'
        printf ' %s is a user-space PTY wrapper that masks configured sensitive\n' "$DISPLAY_NAME"
        printf ' command arguments before rendering and provides selectable Bash and Zsh\n'
        printf ' history modes. It does not alter the commands sent to the child shell.\n'
    } > "$package_root/DEBIAN/control"

    (
        cd "$package_root"
        find usr -type f -print0 | sort -z | xargs -0 md5sum > DEBIAN/md5sums
    )
    chmod 0644 "$package_root/DEBIAN/control" "$package_root/DEBIAN/md5sums" \
        "$doc_dir/copyright" "$doc_dir/changelog.Debian.gz" "$desktop_file"

    find "$package_root" -print0 | xargs -0 touch -h -d "@$SOURCE_DATE_EPOCH"
    rm -f "$deb_file"
    dpkg-deb --root-owner-group -Zxz -z9 --build "$package_root" "$deb_file"
    printf 'Created %s\n' "$deb_file"
}

case "$mode" in
    source) build_source ;;
    deb) build_deb ;;
    all)
        build_source
        build_deb
        (
            cd "$dist_dir"
            rm -f SHA256SUMS
            sha256sum "$(basename "$source_file")" \
                "${PACKAGE}_${deb_version}_$(dpkg-architecture -qDEB_HOST_ARCH).deb" \
                > SHA256SUMS
        )
        printf 'Created %s\n' "$dist_dir/SHA256SUMS"
        ;;
esac
