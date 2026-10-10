package me.nekosu.aqnya.util

import android.content.Context
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.os.Build

fun getAppVersion(context: Context): String =
    try {
        val pkgInfo = appPackageInfo(context)
        val versionName = pkgInfo.versionName ?: "unknown"
        "$versionName (${appVersionCode(pkgInfo)})"
    } catch (e: Exception) {
        "unknown"
    }

/** 当前 APK 的 versionCode；读取失败时返回 0。 */
fun getAppVersionCode(context: Context): Long =
    try {
        appVersionCode(appPackageInfo(context))
    } catch (e: Exception) {
        0L
    }

private fun appPackageInfo(context: Context): PackageInfo {
    val pm = context.packageManager
    val pkgName = context.packageName
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        pm.getPackageInfo(pkgName, PackageManager.PackageInfoFlags.of(0))
    } else {
        @Suppress("DEPRECATION")
        pm.getPackageInfo(pkgName, 0)
    }
}

private fun appVersionCode(info: PackageInfo): Long =
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
        info.longVersionCode
    } else {
        @Suppress("DEPRECATION")
        info.versionCode.toLong()
    }
