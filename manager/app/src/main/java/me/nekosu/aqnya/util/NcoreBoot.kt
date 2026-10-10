package me.nekosu.aqnya.util

import android.content.Context
import java.io.File

/**
 * 让随 APK 打包的 ncore 与 busybox 就位到 `/data/adb/nksu/`。
 *
 * 内核注入的 init.rc（见 `kernel/boot/init_rc.h`）在 `post-fs-data` / `services` /
 * `boot-completed` 阶段 exec `/data/adb/nksu/ncore`。与 KernelSU 的 `ksud install`
 * 一致，ncore 不由这里拷贝，而是以 root 运行 `ncore install`，由 ncore 把
 * `/proc/self/exe`（即 APK 的 libncore.so）复制过去——这样它永远是当前随 APK
 * 打包的版本。
 *
 * busybox 由管理器自带（`assets/bin/busybox`），随 ncore 一起释放到
 * `/data/adb/nksu/bin/busybox`；ncore 用它执行模块脚本，NekoSU 因此不再依赖
 * KernelSU 提供的 busybox。
 *
 * 注意：vendor_boot 首次安装时应用通常还没有 root，此时无法写入 `/data/adb`。
 * 刷入并重启后 nksu 已生效，管理器启动时会再执行一次。
 */
object NcoreBoot {
    const val NCORE_DIR = "/data/adb/nksu"
    const val BOOT_PATH = "$NCORE_DIR/ncore"
    const val BIN_DIR = "$NCORE_DIR/bin"
    const val BUSYBOX_PATH = "$BIN_DIR/busybox"

    private const val BUSYBOX_ASSET = "bin/busybox"

    /** 让 ncore 与自带 busybox 就位；全部成功返回 true。 */
    fun install(context: Context): Boolean {
        val ncore = VendorBootInstaller.ncorePath(context)
        if (!ncore.exists()) return false

        val busybox = stageBusybox(context) ?: return false

        val cmd =
            "${quote(ncore.absolutePath)} install" +
                " && mkdir -p ${quote(BIN_DIR)}" +
                " && cp ${quote(busybox.absolutePath)} ${quote(BUSYBOX_PATH)}" +
                " && chmod 0755 ${quote(BUSYBOX_PATH)}"
        return RootShell.exec(cmd).code == 0
    }

    /** 把 assets 里的 busybox 解到应用私有目录，root 才能读到。 */
    private fun stageBusybox(context: Context): File? =
        runCatching {
            val out = File(VendorBootInstaller.installDir(context), "busybox")
            context.assets.open(BUSYBOX_ASSET).use { input ->
                out.outputStream().use { output -> input.copyTo(output) }
            }
            out.setReadable(true, false)
            out.setExecutable(true, false)
            out
        }.getOrNull()

    private fun quote(s: String) = "'" + s.replace("'", "'\\''") + "'"
}
