#!/bin/sh
. ./env.sh
source "$(dirname "$0")/env.sh"

build() {
    if [ -n "$TARGET" ] && [ -d out ]; then
        echo "building $TARGET"
    fi
}

build
