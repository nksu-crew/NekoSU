// SPDX-License-Identifier: GPL-3.0-or-later
// Top-level build file where you can add configuration options common to all sub-projects/modules.
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.android.library) apply false
    alias(libs.plugins.kotlin.compose) apply false
}

// The Gradle root is manager/, so the repository root is one level up.  Tasks
// that touch the kernel, uapi or Rust sources resolve from here.
val repoRoot = rootDir.parentFile

/*
 * Whole-repository formatting:
 *   cd manager && ./gradlew formatAll
 *
 * Kotlin is handled by the app's ktlint plugin, Rust by rustfmt and the
 * kernel/uapi C sources by clang-format.  Android XML is deliberately left
 * out: there is no lossless, project-independent XML formatter in this setup.
 */
tasks.register("formatRust") {
    group = "formatting"
    description = "Format every Rust crate with rustfmt"
    doLast {
        providers.exec {
            workingDir = repoRoot.resolve("neko")
            commandLine(
                "cargo",
                "fmt",
                "--package",
                "ncore",
                "--package",
                "bootpatcher",
                "--package",
                "jni",
            )
        }.result.get().assertNormalExitValue()
    }
}

tasks.register("formatKernel") {
    group = "formatting"
    description = "Format tracked C/C++ sources and headers with clang-format"
    doLast {
        providers.exec {
            // git ls-files must run from the repository root, not manager/.
            workingDir = repoRoot
            commandLine(
                "bash",
                "-lc",
                """
                set -euo pipefail
                command -v clang-format >/dev/null || {
                    echo 'clang-format is required for formatKernel' >&2
                    exit 1
                }
                git ls-files -z -- '*.c' '*.h' '*.cc' '*.cpp' '*.hpp' |
                    xargs -0 -r clang-format -i
                """.trimIndent(),
            )
        }.result.get().assertNormalExitValue()
    }
}

tasks.register("formatAll") {
    group = "formatting"
    description = "Format Kotlin, Rust, C and C++ sources"
    dependsOn(":app:ktlintFormat", "formatRust", "formatKernel")
}
