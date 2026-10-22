<h1 align="center">
  <img src="assets/nekosu-banner.png" alt="NekoSU" width="720">
</h1>

<p align="center">
  <strong>Kernel-level root for Android.</strong><br/>
  A more aggressive kernel-based root solution compared to KernelSU.
</p>

<p align="center">
  <a href="https://github.com/nksu-crew/NekoSU/actions/workflows/build.yml"><img alt="Build" src="https://github.com/nksu-crew/NekoSU/actions/workflows/build.yml/badge.svg"></a>
  <a href="https://github.com/nksu-crew/NekoSU/actions/workflows/ko.yml"><img alt="Build LKM" src="https://github.com/nksu-crew/NekoSU/actions/workflows/ko.yml/badge.svg"></a>
  <a href="LICENSE"><img alt="License: GPL-3.0" src="https://img.shields.io/badge/license-GPL--3.0-blue.svg"></a>
  <img alt="Android 12+" src="https://img.shields.io/badge/Android-12%2B-3DDC84?logo=android&logoColor=white">
  <img alt="arm64-v8a" src="https://img.shields.io/badge/ABI-arm64--v8a-6C63FF">
</p>

## Features

- **Kernel-powered root** — `su`, UID profiles and capability grants enforced in kernel space.
- **Self-contained LKM** — dynamic symbol resolution lets `nksu.ko` load directly with `insmod`.

## Architecture

| Path | Description |
| :--- | :--- |
| `kernel/` | `nksu.ko`: root enforcement, SELinux, module mounting and manager authentication. |
| `manager/` | Android manager app built with Kotlin and Jetpack Compose. |
| `neko/` | Rust runtime: `ncore` daemon plus the `bootpatcher` `vendor_boot` patcher. |
| `uapi/` | Shared kernel/userspace ABI and version header. |

## Requirements

| Requirement | Details |
| :--- | :--- |
| **Device** | Android 12+ (API 31), `arm64-v8a`, unlocked bootloader |
| **Build** | JDK 25, Android NDK r29, Rust stable with `aarch64-linux-android`, `cargo-ndk` |

Set `ANDROID_NDK_HOME` to the NDK r29 directory before building.

## Build

```sh
./gradlew assembleRelease
```

### Version channels

| Channel | Source | Version |
| :--- | :--- | :--- |
| `canary` | `main` and pull requests | `<uapi>-canary.<commit-count>+<short-hash>` |
| `beta` | Tag such as `v1.0.6-beta.1` | Tag version |
| `release` | Tag matching `uapi/version.h`, such as `v1.0.6` | Tag version |

Local builds default to `canary`; select another channel with:

```sh
./gradlew assembleRelease -PversionChannel=release
```

## Credits

- [KernelSU](https://github.com/tiann/KernelSU) — SELinux engine, LSM hooks, `init.rc` injection,
  SELinux hiding and the WebUI `window.ksu` interface.
- BusyBox, Material 3, Jetpack Compose and the open-source libraries listed in
  **About → Open source** inside the app.

## License

NekoSU is licensed under the [GNU General Public License v3.0](LICENSE).
