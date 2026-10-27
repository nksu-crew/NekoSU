plugins {
    alias(libs.plugins.android.library)
}

android {
    namespace = "me.nekosu.neko"
    ndkVersion = "29.0.14206865"
    compileSdk {
        version =
            release(37) {
                minorApiLevel = 1
            }
    }

    defaultConfig {
        minSdk = 31
    }
}

// Same layout as LSPosed-cirno's Rust modules: build with cargo-ndk and let
// the app merge the generated jniLibs directory.
val ndkDirProvider = androidComponents.sdkComponents.ndkDirectory

val buildRust =
    tasks.register("buildRust") {
        group = "rust"
        description = "Build ncore, bootpatcher and the JNI library for Android arm64"

        doLast {
            val ndkFile = ndkDirProvider.get().asFile
            val prebuilt =
                File(ndkFile, "toolchains/llvm/prebuilt").listFiles()?.firstOrNull()
                    ?: error("NDK LLVM toolchain not found under $ndkFile")
            val binDir = File(prebuilt, "bin")
            val strip = File(binDir, "llvm-strip")

            val targetDir = layout.buildDirectory.dir("rust/target").get().asFile
            val outJniDir = layout.buildDirectory.dir("generated/rust/jniLibs").get().asFile
            val abiDir = File(outJniDir, "arm64-v8a").apply { mkdirs() }

            val cargoArgs = mutableListOf("cargo", "ndk", "--platform", "28", "-t", "arm64-v8a")
            cargoArgs += listOf("build", "--release", "--target-dir", targetDir.absolutePath)

            providers.exec {
                workingDir = projectDir
                environment("ANDROID_NDK_HOME", ndkFile.absolutePath)
                environment("KSU_PACKAGE_NAME", "me.aqnya.nekosu")
                commandLine(cargoArgs)
            }.result.get().assertNormalExitValue()

            listOf("ncore" to "libncore.so", "bootpatcher" to "libbootpatcher.so", "libjni.so" to "libjni.so").forEach { (binary, name) ->
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

tasks.matching { it.name.startsWith("merge") && it.name.endsWith("JniLibFolders") }.configureEach {
    dependsOn(buildRust)
}
