#!/bin/sh
# Applies local 3DS fixes to third-party submodules that we don't own.
# Runs automatically from the Makefile and is safe to run repeatedly:
# patches that are already applied are skipped.
#
#   moonlight-common-c.patch
#     - pollSockets(): don't return an uninitialized value when called with a
#       0 ms timeout on 3DS (backport of upstream e6b1aaf).
#     - stopVideoStream(): close the RTP socket before joining the video
#       receive thread so it can't block stream shutdown forever.
#   enet.patch
#     - enet_socket_wait(): same 0 ms timeout fix (backport of upstream 98dbb89).
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

apply_patch() {
    dir="$ROOT/$1"
    patch_file="$ROOT/3ds/patches/$2"

    if patch -p1 -d "$dir" -R --dry-run -s -f < "$patch_file" > /dev/null 2>&1; then
        echo "patch $2: already applied"
    elif patch -p1 -d "$dir" --dry-run -s -f < "$patch_file" > /dev/null 2>&1; then
        patch -p1 -d "$dir" -s -f < "$patch_file"
        echo "patch $2: applied"
    else
        echo "patch $2: FAILED to apply to $1 (submodule changed?)" >&2
        exit 1
    fi
}

apply_patch third_party/moonlight-common-c moonlight-common-c.patch
apply_patch third_party/moonlight-common-c/enet enet.patch
