<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

<div align="center">
  <img src="manager/app/src/main/res/mipmap-xxxhdpi/ic_launcher.webp" width="112" alt="NekoSU icon">

  <h1>NekoSU</h1>

  <p><strong>Kernel-level root for Android.</strong><br>
  A Compose manager, a Rust userspace runtime, and a GKI kernel module.</p>

  <p>
    <a href="https://github.com/nksu-crew/NekoSU/actions/workflows/build.yml"><img src="https://github.com/nksu-crew/NekoSU/actions/workflows/build.yml/badge.svg" alt="Build"></a>
    <a href="https://github.com/nksu-crew/NekoSU/releases"><img src="https://img.shields.io/github/v/release/nksu-crew/NekoSU?style=flat-square&include_prereleases&label=release" alt="Release"></a>
    <img src="https://img.shields.io/badge/Android-12%2B-3DDC84?style=flat-square&logo=android&logoColor=white" alt="Android 12+">
    <img src="https://img.shields.io/badge/Kotlin-2.4-7F52FF?style=flat-square&logo=kotlin&logoColor=white" alt="Kotlin">
    <img src="https://img.shields.io/badge/Rust-stable-000000?style=flat-square&logo=rust&logoColor=white" alt="Rust">
    <a href="LICENSE"><img src="https://img.shields.io/github/license/nksu-crew/NekoSU?style=flat-square" alt="License"></a>
  </p>

  <p>
    <a href="#overview">Overview</a> ·
    <a href="#features">Features</a> ·
    <a href="#architecture">Architecture</a> ·
    <a href="#build">Build</a> ·
    <a href="#install">Install</a> ·
    <a href="#license">License</a>
  </p>
</div>

## Overview

NekoSU is a kernel-assisted root implementation for modern Android. It keeps
privileged work in a small GKI kernel module and moves policy, module
management, boot patching and the manager-facing API into a Rust userspace
runtime.

The project is split into three cooperating pieces:

- **Manager** — a Jetpack Compose app for app policies, modules, FMAC rules,
  installation and diagnostics.
- **Runtime** — `ncore`, the Rust userspace daemon/tool, reached through the
  JNI bridge and backed by a unified profile store.
- **Kernel** — `nksu.ko`, loaded at first stage through a patched `vendor_boot`
  (GKI) or `boot`/`init_boot` image, exposing the privileged operations the
  runtime needs.

> [!NOTE]
> NekoSU is under active development. Builds from `main` are canary builds;
> tagged builds are beta or release builds. Interfaces and on-disk formats may
> change between canaries.

## Build

### Manager

```sh
cd manager
./gradlew assembleRelease
```

The wrapper must run from `manager/`; it is the Gradle root.

The Rust binaries are built for `aarch64-linux-android`. The Android linker,
`CC`/`AR` and bindgen sysroot must already be configured in the environment;
see [neko/README.md](neko/README.md) for the expected variables and the
reusable AAR flow.

To reuse a previously built native AAR and skip the Rust build:

```sh
cd manager
./gradlew :neko:assembleRelease
./gradlew :app:assembleRelease \
  -PnekoAar=neko/build/outputs/aar/neko-release.aar
```

The `nekoAar` path is resolved from the repository root.

### Kernel module

```sh
make -C kernel
```

`KDIR` must point at a prepared kernel tree. CI builds the full KMI matrix with
prebuilt DDK images; see [.github/workflows/ko.yml](.github/workflows/ko.yml).

## Install

1. Download the latest manager APK from
   [Releases](https://github.com/nksu-crew/NekoSU/releases).
2. Unlock the bootloader and back up the current `boot` and `vendor_boot`
   partitions.
3. Open the manager and use the in-app installer to patch the `vendor_boot`
   image on GKI devices, or the `boot`/`init_boot` image on non-GKI devices.
4. Reboot and verify that `nksu.ko` loaded before configuring modules or app
   policies.

The manager targets Android 12+ (`minSdk 31`) and ships `arm64-v8a` native
libraries.

> [!WARNING]
> Rooting modifies boot-critical partitions. A mismatched kernel, boot image or
> module can leave the device unbootable. Keep a known-good boot image and
> recovery path available before installing.

## Credits

NekoSU's userspace runtime is ported from [KernelSU](https://github.com/tiann/KernelSU)
(GPL-3.0-or-later). Boot image parsing uses the pinned `bootparse` dependency.
Third-party components under `neko/third_party/` and the generated Gradle
wrapper keep their original licenses.

## License

NekoSU is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

See [LICENSE](LICENSE) for the full license text.
