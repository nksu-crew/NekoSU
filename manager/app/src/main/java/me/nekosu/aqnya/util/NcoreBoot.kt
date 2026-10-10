package me.nekosu.aqnya.util

import android.content.Context

/**
 * 让随 APK 打包的 ncore 就位到固定路径 `/data/adb/nksu/ncore`。
 *
 * 内核注入的 init.rc（见 `src/boot/init_rc.c`）在 `post-fs-data` / `services` /
 * `boot-completed` 阶段 exec 这个路径。与 KernelSU 的 `ksud install` 一致，
 * 这里不自己拷贝，而是以 root 运行 `ncore install`，由 ncore 把
 * `/proc/self/exe`（即 APK 的 libncore.so）复制过去——这样它永远是当前
 * 随 APK 打包的版本。
 *
 * 注意：vendor_boot 首次安装时应用通常还没有 root，此时无法写入 `/data/adb`。
 * 刷入并重启后 nksu 已生效，管理器启动时会再执行一次，之后的每次启动模块
 * 都能正常加载。
 */
object NcoreBoot {
    const val BOOT_PATH = "/data/adb/nksu/ncore"

    /** 运行 `ncore install` 把它自己复制到 [BOOT_PATH]；成功返回 true。 */
    fun install(context: Context): Boolean {
        val ncore = VendorBootInstaller.ncorePath(context)
        if (!ncore.exists()) return false

        return RootShell.exec("${quote(ncore.absolutePath)} install").code == 0
    }

    private fun quote(s: String) = "'" + s.replace("'", "'\\''") + "'"
}
