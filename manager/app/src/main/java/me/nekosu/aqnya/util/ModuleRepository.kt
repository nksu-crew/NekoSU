package me.nekosu.aqnya.util

import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json
import me.nekosu.aqnya.ncore

/**
 * 模块列表与安装流程的入口。
 *
 * 与 KernelSU 管理器依赖 ksud 守护进程不同，这里没有用户态守护进程：
 *  - 模块信息由内核接口返回（[ncore.listModules] -> IOC_LIST_MODULES）
 *  - 安装 / 启用 / 移除通过内核提供的 root sh 执行（[ncore.execRoot]）
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

    /** 内核接口是否可用（模块列表非 null 即视为可用）。 */
    fun available(): Boolean = ncore.listModules() != null

    fun list(): List<ModuleInfo> {
        val raw = ncore.listModules() ?: return emptyList()
        if (raw.isBlank()) return emptyList()
        return runCatching {
            json.decodeFromString(ListSerializer(ModuleInfo.serializer()), raw)
        }.getOrDefault(emptyList())
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
