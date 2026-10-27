// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.data.installer

import android.content.Context
import me.nekosu.aqnya.data.shell.RootShell
import me.nekosu.aqnya.util.getAppVersionCode

object NcoreBoot {
    const val NCORE_DIR = "/data/adb/nksu"
    const val BOOT_PATH = "/data/adb/ncore"

    private const val PREFS_NAME = "ncore_boot"
    private const val KEY_INSTALLED_VERSION = "installed_version"

    fun install(context: Context): Boolean {
        val ncore = VendorBootInstaller.ncorePath(context)
        if (!ncore.exists()) return false

        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val version = getAppVersionCode(context)
        if (version != 0L && prefs.getLong(KEY_INSTALLED_VERSION, -1L) == version) return true

        // ncore installs itself and extracts its embedded busybox/resetprop.
        val cmd = "${quote(ncore.absolutePath)} install"
        val ok = RootShell.exec(cmd).code == 0
        if (ok && version != 0L) {
            prefs.edit().putLong(KEY_INSTALLED_VERSION, version).apply()
        }
        return ok
    }

    private fun quote(s: String) = "'" + s.replace("'", "'\\''") + "'"
}
