# neko

NekoSU's userspace runtime.

- `ncore/` — ported from [KernelSU](https://github.com/tiann/KernelSU) (GPL-3.0).
  The KernelSU kernel interface is replaced by a NekoSU adapter in `ncore/src/ksucalls.rs`.
- `bootpatcher/` — NekoSU's v3/v4 `vendor_boot` patcher. It installs the LKM in
  `lib/modules/nksu.ko` and updates `modules.load` / `modules.dep`, preserving
  the ramdisk's compression and the v4 vendor ramdisk table.
- `jni/` — the JNI bridge the Compose manager loads as `libjni.so`.

All three binaries are cross-compiled by Gradle through `cargo-ndk` as part of
the manager build and shipped as `libncore.so`, `libbootpatcher.so` and
`libjni.so`.

All Rust dependencies come from crates.io or are vendored under `third_party/`
(`android-bootimg`, `prop-rs`/`prop-rs-android`); no Git dependencies are used.
