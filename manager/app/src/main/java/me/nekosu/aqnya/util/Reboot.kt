package me.nekosu.aqnya.util

/**
 * Reboot helpers for the module page's "reboot to apply" snackbar.
 *
 * NekoSU does not persist a soft-reboot preference yet, so [isSoftRebootPreferred] is a stub that
 * always returns false.
 */
fun isSoftRebootPreferred(): Boolean = false

/** Reboot the device, optionally performing a soft reboot (restart the framework only). */
fun reboot(mode: String = "") {
    val cmd = if (mode == "soft_reboot") "setprop ctl.restart zygote" else "reboot"
    Thread {
        runCatching { RootShell.exec(cmd) }
    }.apply { isDaemon = true }.start()
}
