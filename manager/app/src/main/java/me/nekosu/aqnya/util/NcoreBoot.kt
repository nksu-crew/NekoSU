package me.nekosu.aqnya.util

import android.content.Context

/**
 * 把随 APK 打包的 ncore 安装到固定路径 `/data/adb/nksu/ncore`。
 *
 * 内核注入的 init.rc（见 `src/init_rc.c`）在 `post-fs-data` / `services` /
 * `boot-completed` 阶段 exec 这个路径，所以它必须在重启前就位。ncore 是内核
 * 无关的用户态模块运行时：枚举 `/data/adb/modules`、执行各阶段脚本、挂载
 * metamodule，并把每个模块的 `sepolicy.rule` 交给内核的 `/proc/nksu/sepolicy`。
 *
 * 注意：vendor_boot 首次安装时应用通常还没有 root，此时无法写入 `/data/adb`。
 * 刷入并重启后 nksu 已生效，管理器启动时会通过 [RootShell] 再复制一次，之后
 * 的每次启动模块都能正常加载。
 */
object NcoreBoot {
    const val BOOT_PATH = "/data/adb/nksu/ncore"

    /** 复制 ncore 到 [BOOT_PATH] 并赋可执行权限；成功返回 true。 */
    fun install(context: Context): Boolean {
        val src = VendorBootInstaller.ncorePath(context)
        if (!src.exists()) return false

        val cmd =
            buildString {
                append("mkdir -p /data/adb/nksu && ")
                append("cp -f ").append(quote(src.absolutePath)).append(' ')
                append(quote(BOOT_PATH)).append(" && ")
                append("chmod 0755 ").append(quote(BOOT_PATH)).append(" && ")
                append("chown 0:0 ").append(quote(BOOT_PATH))
            }
        return RootShell.exec(cmd).code == 0
    }

    private fun quote(s: String) = "'" + s.replace("'", "'\\''") + "'"
}
