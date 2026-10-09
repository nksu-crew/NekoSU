# Building `nksu.ko` with Bazel

The kernel module is compiled by [kbuild], the Linux kernel build system.
Bazel wraps that build so the whole project can be driven by a single
`bazel build` command, without reimplementing `modpost` / `Module.symvers` /
vermagic handling (which native Bazel rules cannot provide for out-of-tree
modules).

```
bazel build //:nksu            # debug build  -> bazel-bin/nksu.ko
bazel build //:nksu_release    # release build -> bazel-bin/release/nksu.ko
```

## Requirements

The build must run where the kernel headers and cross toolchain are available,
i.e. inside a `ghcr.io/ylarod/ddk` container (or with the same environment):

| Variable | Purpose |
| --- | --- |
| `KDIR` | kernel build tree (`make -C $KDIR M=...`) |
| `ARCH`, `CROSS_COMPILE`, `LLVM`, `LLVM_IAS` | kernel cross build settings |
| `PATH`, `CLANG_PATH` | clang toolchain selected by the DDK image |

Inside the DDK container these are already exported, and `.bazelrc` forwards
them to the build action via `--action_env`.

## Embedding the revision

`Kbuild` embeds the short commit into debug builds. Bazel builds in a staging
copy, so pass it explicitly:

```
bazel build //:nksu --define=nksu_git_commit="$(git rev-parse --short=12 HEAD)"
```

If omitted the value is `unknown`.

## Why not `ddk_module` / Kleaf?

Google's Kleaf DDK (`ddk_module`) cannot be used here: the `ylarod` DDK is a
kbuild-only toolkit and ships neither a Kleaf workspace nor `kernel/build`, and
the canonical Debian/GKI kernel source is required for Kleaf. kbuild remains the
only supported way to produce a loadable out-of-tree module for these KMIs.

[kbuild]: https://www.kernel.org/doc/html/latest/kbuild/modules.html
