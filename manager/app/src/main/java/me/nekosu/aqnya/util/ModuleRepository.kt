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
 *  - 安装 / 启用 / 移除通过 root shell 执行。
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

    fun uninstall(id: String): Result = exec("rm -rf ${quote("/data/adb/modules/$id")}")

    fun install(zipPath: String): Result = exec(installCommand(zipPath))

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

    private fun installCommand(zipPath: String): String {
        val zip = quote(zipPath)
        return """
            set -e
            ZIP=$zip
            TMP=/data/adb/nksu-install
            MODROOT=/data/adb/modules
            rm -rf "${'$'}TMP"
            mkdir -p "${'$'}TMP"
            unzip -o "${'$'}ZIP" -d "${'$'}TMP" >/dev/null 2>&1
            [ -f "${'$'}TMP/module.prop" ] || { echo "invalid module: missing module.prop"; exit 1; }
            ID=$(sed -n 's/^id=//p' "${'$'}TMP/module.prop" | head -n1 | tr -d '\r')
            [ -n "${'$'}ID" ] || { echo "invalid module: missing id"; exit 1; }
            case "${'$'}ID" in *[!A-Za-z0-9._-]*) echo "invalid module id: ${'$'}ID"; exit 1;; esac
            if [ -f "${'$'}TMP/customize.sh" ]; then
              echo "running customize.sh"
              ( cd "${'$'}TMP" && MODPATH="${'$'}TMP" sh "${'$'}TMP/customize.sh" ) || { echo "customize.sh failed"; exit 1; }
            fi
            DEST="${'$'}MODROOT/${'$'}ID"
            rm -rf "${'$'}DEST"
            mkdir -p "${'$'}DEST"
            cp -a "${'$'}TMP/." "${'$'}DEST/"
            chmod -R 0755 "${'$'}DEST" 2>/dev/null || true
            chcon -R u:object_r:system_file:s0 "${'$'}DEST" 2>/dev/null || true
            rm -rf "${'$'}TMP"
            echo "module ${'$'}ID installed"
        """.trimIndent()
    }
}
