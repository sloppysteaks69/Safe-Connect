#!/usr/bin/env bash
# Builds the Safe Connect payloads with the PS5 payload SDK, in WSL Ubuntu.
#   bash payload/build-safe-connect-elf.sh
# Uses the same pinned SDK as the receiver builds (~/ps5-sdk-build), fetching it if missing.
# Result: safe_connect.elf, safe_connect_unblock.elf, safe_connect_check.elf beside this script.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=${PW_SDK_WORK:-$HOME/ps5-sdk-build}
sdk_rev=a7685ce8e97ce1f8a4f38253a41d54241c9124f1
sdk_prefix=$work/ps5-payload-sdk
exec > >(tee "$here/build.log") 2>&1
mkdir -p "$work"
if [[ ! -f $sdk_prefix/toolchain/prospero.mk ]]; then
    [[ -d $work/sdk/.git ]] || git clone https://github.com/ps5-payload-dev/sdk "$work/sdk"
    git -C "$work/sdk" fetch -q origin "$sdk_rev" || true
    git -C "$work/sdk" checkout -q "$sdk_rev"
    make -C "$work/sdk" DESTDIR="$sdk_prefix" install
fi
rm -rf "$work/safe_connect"; mkdir -p "$work/safe_connect"
cp "$here/safe_connect.c" "$work/safe_connect/"
cp "$here/Makefile.txt" "$work/safe_connect/Makefile"
make -C "$work/safe_connect" PS5_PAYLOAD_SDK="$sdk_prefix" all
cp "$work/safe_connect/"*.elf "$here/"
ls -l "$here/"*.elf
md5sum "$here/"*.elf
echo "done"
