// SPDX-License-Identifier: GPL-3.0-or-later
plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
    kotlin("plugin.serialization") version embeddedKotlinVersion
    alias(libs.plugins.ktlint)
}

fun gitCommitCount(): Int =
    ProcessBuilder("git", "rev-list", "--count", "HEAD")
        .directory(rootDir)
        .start()
        .inputStream
        .bufferedReader()
        .readText()
        .trim()
        .toInt()

// The APK's build commit, used as versionName for display/diagnostics.
fun gitCommitHash(): String =
    ProcessBuilder("git", "rev-parse", "--short=12", "HEAD")
        .directory(rootDir)
        .start()
        .inputStream
        .bufferedReader()
        .readText()
        .trim()

// The LKM's manually maintained version, shared with the kernel module through
// uapi/version.h.  Exposed as BuildConfig.NKSU_VERSION so the manager can
// compare it with the running module's version.
fun nksuVersion(): String =
    rootDir
        .resolve("uapi/version.h")
        .takeIf { it.isFile }
        ?.readText()
        ?.let {
            Regex(
                "^#define\\s+NKSU_VERSION\\s+\"([^\"]+)\"",
                RegexOption.MULTILINE,
            ).find(it)
        }?.groupValues
        ?.getOrNull(1)
        ?.trim()
        ?.ifEmpty { null }
        ?: "unknown"

/*
 * Release channels:
 *   canary  - every commit (CI default for main/PR builds)
 *   beta    - pre-release test build (tag containing "-beta")
 *   release - public release (other version tags)
 *
 * Select with -PversionChannel=<name> or VERSION_CHANNEL=<name>.
 */
val versionChannel: String =
    (
        (project.findProperty("versionChannel") as? String)
            ?: System.getenv("VERSION_CHANNEL")
            ?: "canary"
    ).trim().lowercase()

val versionChannelCode: Int =
    when (versionChannel) {
        "canary" -> 0
        "beta" -> 1
        "release" -> 2
        else -> error("unknown versionChannel: $versionChannel (expected canary, beta or release)")
    }

val baseVersion: String = nksuVersion()

val versionNameOverride: String? =
    (project.findProperty("versionNameOverride") as? String)
        ?.trim()
        ?.takeIf { it.isNotEmpty() }

val buildVersionName: String =
    versionNameOverride
        ?: when (versionChannel) {
            "canary" -> "$baseVersion-canary.${gitCommitCount()}+${gitCommitHash()}"
            "beta" -> "$baseVersion-beta.${gitCommitCount()}+${gitCommitHash()}"
            else -> baseVersion
        }

// The manager's own package name (applicationId).  It is deliberately
// rotatable: the kernel authenticates the manager by its signing certificate
// and shared uid (android:sharedUserId="me.nekosu.shared"), never by this name,
// so switching it dodges package-name based detection without touching the
// certificate or the shared uid.  Rotate at build time with, e.g.:
//   ./gradlew -PmanagerId=me.nekosu.manager assembleRelease
// The Kotlin namespace stays fixed (source/R/BuildConfig do not move).
val managerPackage: String =
    (project.findProperty("managerId") as? String)
        ?.trim()
        ?.takeIf { it.isNotEmpty() }
        ?: "me.aqnya.nekosu"

val releaseKeystore = rootDir.resolve("release.keystore")
val hasReleaseSigning: Boolean =
    releaseKeystore.isFile &&
        releaseKeystore.length() > 0 &&
        listOf("KEYSTORE_PASSWORD", "KEY_ALIAS", "KEY_PASSWORD").all {
            !System.getenv(it).isNullOrBlank()
        }
val requireReleaseSigning: Boolean =
    (project.findProperty("requireReleaseSigning") as? String)?.toBoolean() ?: false

if (requireReleaseSigning && !hasReleaseSigning) {
    error("release signing is required but release.keystore or signing environment variables are missing")
}

android {
    namespace = "me.nekosu.aqnya"
    compileSdk {
        version =
            release(37) {
                minorApiLevel = 1
            }
    }

    defaultConfig {
        applicationId = managerPackage
        minSdk = 31
        targetSdk = 37
        versionCode = gitCommitCount() * 10 + versionChannelCode
        versionName = buildVersionName

        buildConfigField("String", "NKSU_VERSION", "\"${nksuVersion()}\"")
        buildConfigField("String", "NKSU_CHANNEL", "\"$versionChannel\"")

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            abiFilters +=
                listOf(
                    "arm64-v8a",
                )
        }
    }

    signingConfigs {
        create("debugKey") {
            storeFile = file("$rootDir/debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
        create("releaseKey") {
            if (hasReleaseSigning) {
                storeFile = releaseKeystore
                storePassword = System.getenv("KEYSTORE_PASSWORD")
                keyAlias = System.getenv("KEY_ALIAS")
                keyPassword = System.getenv("KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        debug {
            signingConfig = signingConfigs.getByName("debugKey")
        }
        release {
            val withR8 = (project.findProperty("withR8") as? String)?.toBoolean() ?: true
            signingConfig = if (hasReleaseSigning) signingConfigs.getByName("releaseKey") else null
            isMinifyEnabled = withR8
            isShrinkResources = withR8
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
        resources {
            excludes += "DebugProbesKt.bin"
        }
    }
}

// ncore and bootpatcher are produced by :neko:buildRust; merge them with the
// app's native libraries like any other jniLibs directory.
android.sourceSets.getByName("main").jniLibs.directories.add(
    project(":neko")
        .layout.buildDirectory
        .dir("generated/rust/jniLibs")
        .get()
        .toString(),
)
tasks.matching { it.name.startsWith("merge") && it.name.endsWith("JniLibFolders") }.configureEach {
    dependsOn(":neko:buildRust")
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.androidx.compose.material.icons.extended)
    implementation(libs.androidx.datastore.preferences)
    implementation(libs.androidx.webkit)
    implementation(libs.androidx.profileinstaller)
    implementation(libs.okhttp)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.kotlinx.serialization.protobuf)
    implementation(libs.haze)
    implementation(libs.haze.blur)
    implementation(libs.kyant.backdrop)
    implementation(libs.kyant.capsule)
    implementation(libs.androidx.graphics.shapes)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    debugImplementation(libs.androidx.compose.ui.tooling)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
}
