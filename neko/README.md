# neko

NekoSU's userspace runtime.

- `ncore/` — ported from [KernelSU](https://github.com/tiann/KernelSU) (GPL-3.0-or-later).
  The KernelSU kernel interface is replaced by the NekoSU adapter in `kzcall/`.
  Its source is grouped by responsibility:
  - `boot/` — anti-brick bookkeeping and `post-fs-data` / `service` / boot-complete events
  - `manager/` — manager package discovery and ABX/packages.xml scanning
  - `module/` — module lifecycle, scripts, configs, hot install and metamodules
  - `selinux/` — sepolicy parsing/patching and file-context restoration
  - `system/` — `su`, `resetprop` and filesystem/process/property helpers
  - `assets.rs`, `cli.rs`, `defs.rs` — embedded runtime assets, CLI dispatch and constants
- `jni/` — the JNI bridge the Compose manager loads as `libjni.so`. It also
  contains the boot-image patchers (`jni/src/patch/`):
  - the v3/v4 `vendor_boot` patcher installs the LKM in `lib/modules/nksu.ko`,
    updates `modules.load` / `modules.dep` and preserves the ramdisk compression
    and v4 vendor ramdisk table. Before patching it reads the `.nksu_ksymtab`
    symbol table embedded in the LKM and verifies the required kallsyms entries
    against the kernel in the supplied boot image;
  - the `boot`/`init_boot` patcher keeps the original first-stage init as
    `/init.real`, installs the manager's init wrapper as `/init` and places the
    LKM at `/nksu.ko`. The wrapper loads the module and hands over to the
    original init.
- `bootparse` (Git dependency) — the boot/vendor_boot image library the patcher
  is built on: AOSP headers via bindgen, block extraction with compression
  detection, a newc `cpio` reader/writer and the vendor_boot repacker.

Both binaries are cross-compiled by Gradle with
`cargo build --target aarch64-linux-android`, then shipped as `libncore.so` and
`libjni.so`. The Android linker, `CC`/`AR` and bindgen sysroot must be
configured in the build environment first; CI exports them from the NDK.

The `:neko` module also packages both libraries into
`neko/build/outputs/aar/neko-release.aar`. To reuse that AAR and skip the
Rust/native build:

```sh
cd manager
./gradlew :app:assembleRelease \
  -PnekoAar=neko/build/outputs/aar/neko-release.aar
```

The `nekoAar` path is resolved from the repository root.

Most Rust dependencies come from crates.io or are vendored under `third_party/`
(`prop-rs`/`prop-rs-android`). The patcher's `bootparse` dependency is pinned to
a Git revision in `Cargo.lock`.
