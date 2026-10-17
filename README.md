# NekoSU

[![Build](https://github.com/nksu-crew/NekoSU/actions/workflows/build.yml/badge.svg)](https://github.com/nksu-crew/NekoSU/actions/workflows/build.yml)
[![Build LKM](https://github.com/nksu-crew/NekoSU/actions/workflows/ko.yml/badge.svg)](https://github.com/nksu-crew/NekoSU/actions/workflows/ko.yml)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

## Introduction 

Kernel-level root solution for Android: a kernel module plus a Material 3 manager app.
KernelSU-compatible module system, WebUI and SELinux rule wire format support.

## Features

1.Kernel-based su and root access management.

2.A more aggressive technical choice compared to KernelSU.

## Requirements

- Android 12+ (minSdk 31), **arm64-v8a** only
- Unlocked bootloader

## Building

The manager builds its userspace runtime from `neko/` with Gradle: `./gradlew assembleRelease`
runs `cargo ndk` for the bundled `ksud` daemon and `neko-boot` vendor_boot patcher. You need
JDK 25, Android NDK r29 (`ANDROID_NDK_HOME`), Rust stable (`aarch64-linux-android` target) and
`cargo-ndk`.

## Credits

- [KernelSU](https://github.com/tiann/KernelSU) — the SELinux policy/rule engine, LSM hook layout,
  `init.rc` injection technique, SELinux hiding and the WebUI `window.ksu` interface are ported from or
  kept compatible with KernelSU, so KernelSU modules and WebUIs work with NekoSU.
- BusyBox, Material 3 / Jetpack Compose and the open-source libraries listed in the app's *About → Open source* screen.

## License

NekoSU is licensed under the **[GNU General Public License v3 (GPL-3)](http://www.gnu.org/copyleft/gpl.html)**.
