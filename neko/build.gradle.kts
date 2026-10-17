// Builds ksud and neko-boot with cargo-ndk.  This module deliberately has no
// Android plugin: the app consumes the generated jniLibs directory, following
// the same pattern as LSPosed-cirno's dex2oat module.
val rustJniDir = layout.buildDirectory.dir("generated/rust/jniLibs")

val buildRust =
    tasks.register("buildRust") {
        group = "rust"
        description = "Build ksud and neko-boot for Android arm64"

        doLast {
            val ndkHome =
                System.getenv("ANDROID_NDK_HOME")
                    ?: System.getenv("ANDROID_NDK_ROOT")
                    ?: error("ANDROID_NDK_HOME is not set")
            val ndkFile = File(ndkHome)
            val prebuilt =
                File(ndkFile, "toolchains/llvm/prebuilt").listFiles()?.firstOrNull()
                    ?: error("NDK LLVM toolchain not found under $ndkFile")
            val strip = File(prebuilt, "bin/llvm-strip")

            val targetDir = layout.buildDirectory.dir("rust/target").get().asFile
            val outJniDir = rustJniDir.get().asFile
            val abiDir = File(outJniDir, "arm64-v8a").apply { mkdirs() }

            val cargoArgs = mutableListOf("cargo", "ndk", "--platform", "28", "-t", "arm64-v8a")
            cargoArgs +=
                listOf(
                    "build",
                    "--release",
                    "--manifest-path",
                    "neko/Cargo.toml",
                    "--target-dir",
                    targetDir.absolutePath,
                    "-p",
                    "ksud",
                    "-p",
                    "neko-boot",
                )

            providers.exec {
                workingDir = rootProject.projectDir
                environment("ANDROID_NDK_HOME", ndkFile.absolutePath)
                environment("KSU_PACKAGE_NAME", "me.aqnya.nekosu")
                commandLine(cargoArgs)
            }.result.get().assertNormalExitValue()

            listOf("ksud" to "libksud.so", "neko-boot" to "libneko-boot.so").forEach { (binary, name) ->
                val source = File(targetDir, "aarch64-linux-android/release/$binary")
                check(source.isFile) { "Rust binary not found: $source" }
                val target = File(abiDir, name)
                source.copyTo(target, overwrite = true)
                providers.exec {
                    commandLine(strip.absolutePath, "--strip-all", target.absolutePath)
                }.result.get().assertNormalExitValue()
            }
        }
    }
