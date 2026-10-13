#!/usr/bin/env bash
# name: Launch native app in emulator (Android)
# description: Install the JDK and Android SDK if needed, then build, install and open the Kotlin + Compose ForkMesh app in native/android on its own forkmesh-native-36 emulator, starting it if needed, even when a phone is attached.
set -euo pipefail

FORKMESH_NATIVE_ANDROID_TARGET=emulator exec "$(dirname "$0")/launch-native-android.sh" "$@"
