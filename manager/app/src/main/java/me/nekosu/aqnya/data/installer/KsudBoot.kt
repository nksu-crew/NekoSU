package me.nekosu.aqnya.data.installer

import android.content.Context
import me.nekosu.aqnya.data.shell.RootShell
import me.nekosu.aqnya.util.getAppVersionCode

object KsudBoot {
    const val KSUD_DIR = "/data/adb/nksu"
    const val BOOT_PATH = "$KSUD_DIR/ksud"
    const val BIN_DIR = "$KSUD_DIR/bin"
    const val BUSYBOX_PATH = "$BIN_DIR/busybox"

    private const val PREFS_NAME = "ksud_boot"
    private const val KEY_INSTALLED_VERSION = "installed_version"

    fun install(context: Context): Boolean {
        val ksud = VendorBootInstaller.ksudPath(context)
        if (!ksud.exists()) return false

        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val version = getAppVersionCode(context)
        if (version != 0L && prefs.getLong(KEY_INSTALLED_VERSION, -1L) == version) return true

        // ksud installs itself and extracts its embedded busybox/resetprop.
        val cmd = "${quote(ksud.absolutePath)} install"
        val ok = RootShell.exec(cmd).code == 0
        if (ok && version != 0L) {
            prefs.edit().putLong(KEY_INSTALLED_VERSION, version).apply()
        }
        return ok
    }

    private fun quote(s: String) = "'" + s.replace("'", "'\\''") + "'"
}
