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
        }
        ?.groupValues
        ?.getOrNull(1)
        ?.trim()
        ?.ifEmpty { null }
        ?: "unknown"

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
        versionCode = gitCommitCount()
        versionName = gitCommitHash()

        buildConfigField("String", "NKSU_VERSION", "\"${nksuVersion()}\"")

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        
        ndk {
        abiFilters += listOf(
            "arm64-v8a"
        )
     }
    }
    
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
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
            storeFile = file("$rootDir/release.keystore")
            storePassword = System.getenv("KEYSTORE_PASSWORD")
            keyAlias = System.getenv("KEY_ALIAS")
            keyPassword = System.getenv("KEY_PASSWORD")
        }
    }

    buildTypes {
        debug {
            signingConfig = signingConfigs.getByName("debugKey")
        }
        release {
            val withR8 = (project.findProperty("withR8") as? String)?.toBoolean() ?: true
            signingConfig = signingConfigs.getByName("releaseKey")
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
            // busybox is a static executable shipped as a jniLib for the kernel
            // to copy; never let AGP strip it.
            keepDebugSymbols += "**/libbusybox.so"
        }
        resources {
            excludes += "DebugProbesKt.bin"
        }
    }
}

// ksud and neko-boot are produced by :neko:buildRust; merge them with the
// app's native libraries like any other jniLibs directory.
android.sourceSets.getByName("main").jniLibs.srcDir(
    project(":neko").layout.buildDirectory.dir("generated/rust/jniLibs").get().asFile,
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
    implementation(libs.androidx.graphics.shapes)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    debugImplementation(libs.androidx.compose.ui.tooling)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
}