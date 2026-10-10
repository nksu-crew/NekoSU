package me.nekosu.aqnya.util

import android.content.Context
import android.content.pm.PackageInfo
import android.content.pm.PackageManager

fun getAppVersion(context: Context): String {
    val info = appPackageInfo(context)
    return "${info.versionName ?: "unknown"} (${info.longVersionCode})"
}

fun getAppVersionCode(context: Context): Long = appPackageInfo(context).longVersionCode

private fun appPackageInfo(context: Context): PackageInfo =
    context.packageManager.getPackageInfo(context.packageName, PackageManager.PackageInfoFlags.of(0))
