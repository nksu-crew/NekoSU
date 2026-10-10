package me.nekosu.aqnya.util

import android.content.Context
import java.io.File

/**
 * 让随 APK 打包的 ncore 与 busybox 就位到 `/data/adb/nksu/`。
 *
 * ncore 与 busybox 都以 jniLibs 形式随 APK 打包（`libncore.so` /
 * `libbusybox.so`），解包后位于 `applicationInfo.nativeLibraryDir`。内核在
 * 每次开机扫描管理器时，会用 `libncore.so` 引导 `/data/adb/nksu/ncore` 并复制
 * `libbusybox.so`（见 `kernel/manager/manager.c`），因此即使管理器从未运行过，
 * nksu 也能起来。这里保留一次主动安装，用于 APK 升级后把二进制刷新到新版本。
 *
 * 内核注入的 init.rc（见 `kernel/boot/init_rc.h`）在 `post-fs-data` /
 * `services` / `boot-completed` 阶段 exec `/data/adb/nksu/ncore`。与 KernelSU
 * 的 `ksud install` 一致，ncore 由 `ncore install` 把 `/proc/self/exe`（即 APK
 * 的 libncore.so）复制过去，这样它永远是当前随 APK 打包的版本。
 *
 * 注意：vendor_boot 首次安装时应用通常还没有 root，此时无法写入 `/data/adb`。
 * 刷入并重启后 nksu 已生效，管理器启动时会再执行一次。
 */
object NcoreBoot {
    const val NCORE_DIR = "/data/adb/nksu"
    const val BOOT_PATH = "$NCORE_DIR/ncore"
    const val BIN_DIR = "$NCORE_DIR/bin"
    const val BUSYBOX_PATH = "$BIN_DIR/busybox"

    private const val BUSYBOX_LIB_NAME = "libbusybox.so"
    private const val PREFS_NAME = "ncore_boot"
    private const val KEY_INSTALLED_VERSION = "installed_version"

    /** 让 ncore 与自带 busybox 就位；全部成功返回 true。 */
    fun install(context: Context): Boolean {
        val ncore = VendorBootInstaller.ncorePath(context)
        if (!ncore.exists()) return false

        val busybox = File(context.applicationInfo.nativeLibraryDir, BUSYBOX_LIB_NAME)
        if (!busybox.exists()) return false

        // APK 没升级就不用每次启动都再跑一遍 root 安装：内核在开机时已用管理器
        // 引导过一次，这里只是升级后刷新二进制，避免每开一次管理器就 fork 一次 su
        // 并复制约 2MB。
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val version = getAppVersionCode(context)
        if (version != 0L && prefs.getLong(KEY_INSTALLED_VERSION, -1L) == version) return true

        val cmd =
            "${quote(ncore.absolutePath)} install" +
                " && mkdir -p ${quote(BIN_DIR)}" +
                " && cp ${quote(busybox.absolutePath)} ${quote(BUSYBOX_PATH)}" +
                " && chmod 0755 ${quote(BUSYBOX_PATH)}"
        val ok = RootShell.exec(cmd).code == 0
        if (ok && version != 0L) {
            prefs.edit().putLong(KEY_INSTALLED_VERSION, version).apply()
        }
        return ok
    }

    private fun quote(s: String) = "'" + s.replace("'", "'\\''") + "'"
}
