import javax.inject.Inject
import org.gradle.api.DefaultTask
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.provider.Property
import org.gradle.api.tasks.Input
import org.gradle.api.tasks.Internal
import org.gradle.api.tasks.OutputDirectory
import org.gradle.api.tasks.TaskAction
import org.gradle.process.ExecOperations

plugins {
    alias(libs.plugins.android.library)
}

// Build the Rust userspace runtime with cargo-ndk and expose the executables
// through AGP's generated jniLibs source directory.  Android extracts them to
// nativeLibraryDir, where the app and the kernel can execute them.
abstract class BuildRustTask : DefaultTask() {
    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @get:Input
    abstract val repositoryRoot: Property<String>

    @get:Input
    abstract val ndkHome: Property<String>

    @get:Input
    abstract val packageName: Property<String>

    @get:Internal
    abstract val execOperations: ExecOperations

    @TaskAction
    fun build() {
        val root = File(repositoryRoot.get())
        val ndk = File(ndkHome.get())
        require(ndk.isDirectory) { "Android NDK not found (set ANDROID_NDK_HOME)" }

        val jniRoot = outputDir.get().asFile
        val targetDir = File(jniRoot.parentFile, "target")
        val abiDir = File(jniRoot, "arm64-v8a")
        abiDir.mkdirs()

        execOperations.exec {
            workingDir = root
            environment("ANDROID_NDK_HOME", ndk.absolutePath)
            environment("KSU_PACKAGE_NAME", packageName.get())
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
        }

        val prebuilt =
            File(ndk, "toolchains/llvm/prebuilt").listFiles()?.firstOrNull()
                ?: error("NDK LLVM toolchain not found under $ndk")
        val strip = File(prebuilt, "bin/llvm-strip")

        listOf("ksud" to "libksud.so", "neko-boot" to "libneko-boot.so").forEach { (binary, name) ->
            val built = File(targetDir, "aarch64-linux-android/release/$binary")
            require(built.isFile) { "Rust binary not found: $built" }
            val output = File(abiDir, name)
            built.copyTo(output, overwrite = true)
            execOperations.exec {
                commandLine(strip.absolutePath, "--strip-all", output.absolutePath)
            }
        }
    }
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
}

val ndkHome =
    System.getenv("ANDROID_NDK_HOME")
        ?: androidComponents.sdkComponents.ndkDirectory.get().asFile.absolutePath

val buildRust =
    tasks.register<BuildRustTask>("buildRust") {
        group = "rust"
        description = "Build ksud and neko-boot for Android arm64"
        outputDir.set(layout.buildDirectory.dir("rust/jniLibs"))
        repositoryRoot.set(rootProject.projectDir.absolutePath)
        this.ndkHome.set(ndkHome)
        packageName.set("me.aqnya.nekosu")
    }

androidComponents.onVariants { variant ->
    variant.sources.jniLibs?.addGeneratedSourceDirectory(buildRust) { it.outputDir }
}
