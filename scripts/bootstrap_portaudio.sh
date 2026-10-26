#!/usr/bin/env bash
set -euo pipefail

# Source and install artifacts stay within this checkout. No sudo/global install.
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
archive_dir="$project_root/build/portaudio-download"
source_dir="$project_root/build/portaudio-src"
prefix="$project_root/build/portaudio"
archive="$archive_dir/pa_stable_v190700_20210406.tgz"
archive_url='https://files.portaudio.com/archives/pa_stable_v190700_20210406.tgz'
expected_sha='47efbf42c77c19a05d22e627d42873e991ec0c1357219c0d74ce6a2948cb2def'
# Checksum also published in Homebrew/homebrew-core Formula/p/portaudio.rb.

if [[ -f "$prefix/lib/libportaudio.a" && -f "$prefix/include/portaudio.h" ]]; then
    printf 'PortAudio already built: %s\n' "$prefix"
    exit 0
fi

mkdir -p "$archive_dir"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 2 --output "$archive.part" "$archive_url"
    mv -- "$archive.part" "$archive"
fi
if command -v shasum >/dev/null 2>&1; then
    actual_sha=$(shasum -a 256 "$archive")
elif command -v sha256sum >/dev/null 2>&1; then
    actual_sha=$(sha256sum "$archive")
else
    printf 'Install shasum or sha256sum to verify the dependency archive.\n' >&2
    exit 1
fi
actual_sha=${actual_sha%% *}
if [[ "$actual_sha" != "$expected_sha" ]]; then
    printf 'PortAudio checksum mismatch: %s\nExpected: %s\nActual: %s\n' "$archive" "$expected_sha" "$actual_sha" >&2
    exit 1
fi

if [[ ! -f "$source_dir/configure" ]]; then
    mkdir -p "$source_dir"
    tar -xzf "$archive" -C "$source_dir" --strip-components=1
fi
cd -- "$source_dir"
configure_args=("--prefix=$prefix" --disable-shared --enable-static --without-jack)
if [[ "$(uname -s)" == Darwin ]]; then
    configure_args+=(--enable-mac-universal=no)
fi
./configure "${configure_args[@]}"
make -j "${JOBS:-2}"
make install
printf '\nPortAudio installed locally: %s\n' "$prefix"
printf 'pkg-config metadata: %s/lib/pkgconfig/portaudio-2.0.pc\n' "$prefix"
