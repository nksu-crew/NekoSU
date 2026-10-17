plugins {
    alias(libs.plugins.android.library)
}

android {
    namespace = "me.nekosu.neko"
    compileSdk {
        version =
            release(37) {
                minorApiLevel = 1
            }
    }

    defaultConfig {
        minSdk = 31
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    sourceSets["main"].jniLibs.srcDir(layout.buildDirectory.dir("rust/jniLibs"))
}

// Build the Rust userspace runtime with cargo-ndk and expose the executables
// as jniLibs, exactly like NeoZygisk's zygiskd module.  Android extracts them
// to nativeLibraryDir, where the app and the kernel can execute them.
val buildRust by tasks.registering {
    group = "rust"
    description = "Build ksud and neko-boot for Android arm64"

    doLast {
        val ndk =
            System.getenv("ANDROID_NDK_HOME")?.let(::File)
                ?: androidComponents.sdkComponents.ndkDirectory.get().asFile
        require(ndk.isDirectory) { "Android NDK not found (set ANDROID_NDK_HOME)" }

        val targetDir = layout.buildDirectory.dir("rust/target").get().asFile
        val jniDir = layout.buildDirectory.dir("rust/jniLibs/arm64-v8a").get().asFile
        jniDir.mkdirs()

        providers.exec {
            workingDir = rootProject.projectDir
            environment("ANDROID_NDK_HOME", ndk.absolutePath)
            environment("KSU_PACKAGE_NAME", "me.aqnya.nekosu")
            commandLine(
                "cargo",
                "ndk",
                "--platform",
                "28",
                "-t",
                "arm64-v8a",
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
        }.result.get().assertNormalExitValue()

        val prebuilt =
            File(ndk, "toolchains/llvm/prebuilt").listFiles()?.firstOrNull()
                ?: error("NDK LLVM toolchain not found under $ndk")
        val strip = File(prebuilt, "bin/llvm-strip")

        listOf("ksud" to "libksud.so", "neko-boot" to "libneko-boot.so").forEach { (binary, name) ->
            val built = File(targetDir, "aarch64-linux-android/release/$binary")
            require(built.isFile) { "Rust binary not found: $built" }
            val output = File(jniDir, name)
            built.copyTo(output, overwrite = true)
            providers.exec {
                commandLine(strip.absolutePath, "--strip-all", output.absolutePath)
            }.result.get().assertNormalExitValue()
        }
    }
}

tasks.named("preBuild").configure { dependsOn(buildRust) }
tasks.matching { it.name.startsWith("merge") && it.name.contains("JniLibFolders") }
    .configureEach { dependsOn(buildRust) }
