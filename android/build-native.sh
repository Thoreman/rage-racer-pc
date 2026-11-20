#!/bin/sh
# Cross-compiles libmain.so for Quest (arm64-v8a) and stages it for Gradle.
# Usage: android/build-native.sh   (from Git Bash; needs the Android SDK + NDK)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${ANDROID_SDK_ROOT:-$HOME/AppData/Local/Android/Sdk}"
NDK="$SDK/ndk/27.2.12479018"
CMAKE="${CMAKE:-cmake}"
"$CMAKE" -S "$ROOT" -B "$ROOT/build/android" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$SDK/cmake/3.22.1/bin/ninja.exe" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-32 \
    -DCMAKE_BUILD_TYPE=Release -DRAGE_EMBED_AUTHORED_CARS=OFF -DRAGE_ENABLE_LINT=OFF
"$SDK/cmake/3.22.1/bin/ninja.exe" -C "$ROOT/build/android" rage-racer
mkdir -p "$ROOT/android/app/jniLibs/arm64-v8a"
cp "$ROOT/build/android/libmain.so" "$ROOT/android/app/jniLibs/arm64-v8a/"
