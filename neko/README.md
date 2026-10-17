# neko

NekoSU's userspace runtime.

- `ksud/`, `ksuinit/` — ported from [KernelSU](https://github.com/tiann/KernelSU) (GPL-3.0).
  The KernelSU kernel interface is replaced by a NekoSU adapter in `ksud/src/ksucalls.rs`.
- `neko-boot/` — NekoSU's v3/v4 `vendor_boot` patcher. It installs the LKM in
  `lib/modules/nksu.ko` and updates `modules.load` / `modules.dep`, preserving
  the ramdisk's compression and the v4 vendor ramdisk table.

Both binaries are cross-compiled by Gradle through `cargo-ndk` as part of the
manager build and shipped as `libksud.so` and `libneko-boot.so`.
