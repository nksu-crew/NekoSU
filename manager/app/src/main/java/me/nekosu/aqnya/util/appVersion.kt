package me.nekosu.aqnya.util

import android.content.Context
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.os.Build

fun getAppVersion(context: Context): String {
    val info = appPackageInfo(context)
    return "${info.versionName ?: "unknown"} (${info.longVersionCode})"
}

fun getAppVersionCode(context: Context): Long = appPackageInfo(context).longVersionCode

/**
 * 本 APK 构建时的 git commit（即 APK 的 versionName）。
 *
 * 内核模块把同一 commit 编译进 [me.nekosu.aqnya.ncore.moduleVersion]，两者绑定，
 * 因此管理器可以据此判断运行中的 LKM 是否已过期。
 */
fun getAppCommit(context: Context): String? =
    appPackageInfo(context).versionName?.takeIf { it.isNotBlank() }

private fun appPackageInfo(context: Context): PackageInfo {
    val pm = context.packageManager
    val name = context.packageName
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        pm.getPackageInfo(name, PackageManager.PackageInfoFlags.of(0))
    } else {
        @Suppress("DEPRECATION")
        pm.getPackageInfo(name, 0)
    }
}
