<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# NekoSU

Kernel-level root for Android.

## Repository layout

```
kernel/     Kernel module (nksu)
uapi/       Kernel/userspace shared UAPI headers
manager/    Gradle root: the Jetpack Compose manager and its build
neko/       Rust tool: ncore, bootpatcher and the JNI bridge
signing/    Signing material (release.keystore is gitignored)
```

## Build

```sh
make -C kernel                 # kernel module, needs KDIR
cd manager && ./gradlew assembleRelease
```

`manager/` is the Gradle root, so the wrapper must be run from that directory.

## License

NekoSU is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

See [LICENSE](LICENSE) for the full license text. Third-party components under
`neko/third_party/` and the generated Gradle wrapper retain their original
licenses.
