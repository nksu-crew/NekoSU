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
