package me.nekosu.aqnya.util

import me.nekosu.aqnya.ncore
import java.io.File
import java.util.concurrent.TimeUnit

/**
 * 以 root 身份执行 shell 命令。
 *
 * 优先使用设备上已有的 `su`（Magisk / KernelSU 等）。这一点很重要：安装页
 * 常常正是在 nksu 尚未安装时使用的，此时 [ncore.execRoot]（依赖 nksu 内核接口）
 * 无法提权。若没有可用的 `su`，再退回 nksu 自身的内核接口，用于已装 nksu 的
 * 升级场景。
 */
object RootShell {
    data class Result(
        val code: Int,
        val output: String,
    ) {
        val ok: Boolean get() = code == 0
    }

    /** 常见的 su 绝对路径（PATH 中的 "su" 另行尝试）。 */
    private val SU_ABSOLUTE_PATHS =
        listOf(
            "/system/bin/su",
            "/system/xbin/su",
            "/sbin/su",
            "/debug_ramdisk/su",
        )

    @Volatile
    private var resolved: String? = null

    @Volatile
    private var checked = false

    /** 找到可用的 su 路径；没有则返回 null（结果缓存）。 */
    @Synchronized
    private fun resolveSu(): String? {
        if (checked) return resolved

        // 只对确实存在的绝对路径发起 su；"su" 交给 PATH 解析。第一个能成功
        // 启动的 su 即为最终结果，避免用户拒绝后反复弹窗。
        val candidates =
            buildList {
                add("su")
                for (path in SU_ABSOLUTE_PATHS) {
                    if (File(path).exists()) add(path)
                }
            }

        for (path in candidates) {
            val out = runCapture(listOf(path, "-c", "id"), timeoutSeconds = 60) ?: continue
            // su 成功启动，其答复就是最终结论（哪怕用户拒绝）。
            checked = true
            if (out.output.contains("uid=0")) resolved = path
            return resolved
        }

        // 一个 su 都没找到：不锁定结论，稍后可重试。
        return null
    }

    /** root 是否可用（外部 su 或 nksu 内核接口任一即可）。 */
    fun available(): Boolean {
        if (resolveSu() != null) return true
        val r = ncoreExec("id")
        return r.code == 0 && r.output.contains("uid=0")
    }

    /** 执行一条 root 命令并返回退出码与合并输出。 */
    fun exec(cmd: String): Result {
        val full = withPath(cmd)
        resolveSu()?.let { su ->
            runCapture(listOf(su, "-c", full), timeoutSeconds = 0)?.let { return it }
        }
        return ncoreExec(full)
    }

    /** 执行 root 命令并把输出逐行回调，返回退出码。 */
    fun execStreaming(
        cmd: String,
        onOutput: (String) -> Unit,
    ): Int {
        val su = resolveSu()
        val full = withPath(cmd)
        if (su == null) {
            val r = ncoreExec(full)
            if (r.output.isNotBlank()) r.output.lineSequence().forEach(onOutput)
            return r.code
        }

        return try {
            val process =
                ProcessBuilder(su, "-c", full)
                    .redirectErrorStream(true)
                    .start()
            process.inputStream.bufferedReader().useLines { lines -> lines.forEach(onOutput) }
            process.waitFor()
        } catch (e: Exception) {
            onOutput("ERROR: ${e.message}")
            -1
        }
    }

    /** su 环境可能没有可用的 PATH，显式补齐 toybox 等系统工具。 */
    private fun withPath(cmd: String): String =
        "PATH=/sbin:/system/sbin:/system/bin:/system/xbin; export PATH; $cmd"

    /** nksu 内核接口的 root shell（不回退到 su）。 */
    fun ncoreExec(cmd: String): Result {
        val out = runCatching { ncore.execRoot(cmd) }.getOrNull() ?: return Result(-1, "root shell unavailable")
        val marker = out.lastIndexOf("[exit ")
        if (marker < 0) return Result(-1, out)
        val code =
            out.substring(marker + 6)
                .takeWhile { it.isDigit() || it == '-' }
                .toIntOrNull()
                ?: -1
        return Result(code, out.substring(0, marker).trimEnd())
    }

    private fun runCapture(
        command: List<String>,
        timeoutSeconds: Int,
    ): Result? =
        try {
            val process = ProcessBuilder(command).redirectErrorStream(true).start()
            val output = StringBuilder()
            // 后台读取，避免大输出把管道写满后与 waitFor 互相阻塞。
            val reader =
                Thread {
                    runCatching {
                        process.inputStream.bufferedReader().useLines { lines ->
                            lines.forEach { output.append(it).append('\n') }
                        }
                    }
                }
            reader.isDaemon = true
            reader.start()

            val finished =
                if (timeoutSeconds > 0) {
                    process.waitFor(timeoutSeconds.toLong(), TimeUnit.SECONDS)
                } else {
                    process.waitFor()
                    true
                }
            if (!finished) {
                process.destroyForcibly()
                null
            } else {
                reader.join(2000)
                Result(process.exitValue(), output.toString())
            }
        } catch (e: Exception) {
            null
        }
}
