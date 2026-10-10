package me.nekosu.aqnya.util

import android.content.Context
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json

/**
 * 模块列表与安装流程的入口。
 *
 * 与 KernelSU 管理器依赖 ksud 守护进程不同，这里没有用户态守护进程：
 *  - 模块枚举由用户态的 ncore 完成，管理器以 root 运行
 *    `ncore module list --json` 并解析其输出的 JSON（见 [list]）；
 *  - action 脚本的存在性由该 JSON 的 `hasActionScript` 给出，执行同样交给
 *    ncore（`ncore module action <id>`，见 [actionCommand]）；
 *  - 安装交给 ncore（`ncore module install <zip>`，见 [install]）：由 ncore
 *    校验 module.prop、解压到 modules_update/、运行 customize.sh、处理
 *    metamodule，并在下次开机生效；管理器只负责把所选 zip 交给 ncore。
 *  - 启用 / 移除通过 root shell 操作标记文件。
 */
object ModuleRepository {
    private val json =
        Json {
            ignoreUnknownKeys = true
            isLenient = true
        }

    data class Result(
        val code: Int,
        val output: String,
    ) {
        val ok: Boolean get() = code == 0
    }

    /** 以 root 运行 ncore 枚举模块，返回解析后的列表；失败时返回空表。 */
    fun list(context: Context): List<ModuleInfo> {
        val ncore = VendorBootInstaller.ncorePath(context)
        if (!ncore.exists()) return emptyList()

        val result = RootShell.exec("${quote(ncore.absolutePath)} module list --json")
        if (!result.ok) return emptyList()

        val payload = extractJsonArray(result.output) ?: return emptyList()
        return runCatching {
            json.decodeFromString(ListSerializer(ModuleInfo.serializer()), payload)
        }.getOrDefault(emptyList())
    }

    /** 取出输出中的 JSON 数组，忽略 su 可能附带的前后噪声。 */
    private fun extractJsonArray(output: String): String? {
        val start = output.indexOf('[')
        val end = output.lastIndexOf(']')
        if (start < 0 || end <= start) return null
        return output.substring(start, end + 1)
    }

    fun setEnabled(
        id: String,
        enabled: Boolean,
    ): Result {
        val dir = quote("/data/adb/modules/$id")
        val cmd =
            if (enabled) {
                "rm -f $dir/disable"
            } else {
                "touch $dir/disable"
            }
        return exec(cmd)
    }

    fun markRemove(id: String): Result = exec("touch ${quote("/data/adb/modules/$id")}/remove")

    /** 撤销「下次启动时移除」的标记。 */
    fun undoRemove(id: String): Result = exec("rm -f ${quote("/data/adb/modules/$id")}/remove")

    fun uninstall(id: String): Result = exec("rm -rf ${quote("/data/adb/modules/$id")}")

    /**
     * 用 ncore 安装模块：`ncore module install <zip>`。
     *
     * ncore 会完成校验、解压、customize.sh、metamodule 处理与 modules.rc 刷新，
     * 模块在下次开机生效。输出即 ncore 的进度/错误信息。
     */
    fun install(
        context: Context,
        zipPath: String,
    ): Result {
        val ncore = VendorBootInstaller.ncorePath(context)
        if (!ncore.exists()) return Result(-1, "ncore not found")
        return exec("${quote(ncore.absolutePath)} module install ${quote(zipPath)}")
    }

    /** 用 ncore 执行模块的 `action.sh`：`ncore module action <id>`。 */
    fun actionCommand(
        context: Context,
        id: String,
    ): String {
        val ncore = VendorBootInstaller.ncorePath(context)
        return "${quote(ncore.absolutePath)} module action ${quote(id)}"
    }

    /** 执行任意 root 命令，返回退出码与合并输出。 */
    fun exec(cmd: String): Result {
        val r = RootShell.ncoreExec(cmd)
        return Result(r.code, r.output)
    }

    private fun quote(s: String) = "'" + s.replace("'", "'\\''") + "'"
}
