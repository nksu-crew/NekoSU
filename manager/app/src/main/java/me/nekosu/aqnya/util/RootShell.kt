package me.nekosu.aqnya.util

import android.util.Base64
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.io.Writer
import java.util.concurrent.BlockingQueue
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * 以 root 身份执行 shell 命令。
 *
 * 统一使用 `/system/bin/su`：ncore 在内核里把对该路径的 execve 重定向到自身的
 * su 实现，因此无论设备运行的是 nksu、Magisk 还是 KernelSU，管理器都只需要这
 * 一条路径，不再单独维护「内核接口」执行流。
 */
object RootShell {
    data class Result(
        val code: Int,
        val output: String,
    ) {
        val ok: Boolean get() = code == 0
    }

    /** 内核重定向到 ncore su 的统一入口。 */
    private const val SU_PATH = "/system/bin/su"

    @Volatile
    private var rootConfirmed = false

    /** root 是否可用（成功授权后缓存，避免反复弹 su 授权框）。 */
    fun available(): Boolean {
        if (rootConfirmed) return true
        val r = runCapture(listOf(SU_PATH, "-c", withPath("id")), timeoutSeconds = 60) ?: return false
        val ok = r.code == 0 && r.output.contains("uid=0")
        if (ok) rootConfirmed = true
        return ok
    }

    /** 执行一条 root 命令并返回退出码与合并输出。 */
    fun exec(cmd: String): Result {
        val full = withPath(cmd)
        return runCapture(listOf(SU_PATH, "-c", full), timeoutSeconds = 0)
            ?: Result(-1, "root shell unavailable")
    }

    /**
     * 以 root 读取一个文件的原始字节（供模块 WebUI 提供静态资源）。
     *
     * 优先走常驻 root shell：一次性 exec `/system/bin/su` 起一个交互 shell，之后
     * 用 `base64` + 标记定界读取文件（二进制安全），避免每个资源都 fork 一次
     * `su -c cat`。常驻 shell 不可用时退回一次性 `su -c cat`。
     */
    fun readFileBytes(path: String): ByteArray? {
        if (!shellDisabled) {
            synchronized(shellLock) {
                persistentRead(path)?.let { return it }
            }
        }
        return oneShotRead(path)
    }

    private const val B64_BEGIN = "__NKSU_B64_BEGIN__"
    private const val B64_END = "__NKSU_B64_END__"
    private const val SHELL_EOF = "__NKSU_SHELL_EOF__"
    private const val SHELL_READ_TIMEOUT_MS = 3000L

    @Volatile
    private var shellDisabled = false

    private val shellLock = Any()
    private var shellProcess: Process? = null
    private var shellWriter: Writer? = null
    private var shellQueue: BlockingQueue<String>? = null

    private fun ensureShell(): Boolean {
        val existing = shellProcess
        if (existing != null && existing.isAlive) return true
        return try {
            val process = ProcessBuilder(SU_PATH).redirectErrorStream(true).start()
            val queue = LinkedBlockingQueue<String>()
            val reader = BufferedReader(InputStreamReader(process.inputStream))
            val thread =
                Thread {
                    try {
                        while (true) {
                            val line = reader.readLine() ?: break
                            queue.put(line)
                        }
                    } catch (_: Exception) {
                        // stream closed
                    } finally {
                        runCatching { queue.put(SHELL_EOF) }
                    }
                }
            thread.isDaemon = true
            thread.start()

            shellProcess = process
            shellWriter = BufferedWriter(OutputStreamWriter(process.outputStream))
            shellQueue = queue
            true
        } catch (e: Exception) {
            killShell()
            false
        }
    }

    private fun killShell() {
        runCatching { shellProcess?.destroy() }
        shellProcess = null
        shellWriter = null
        shellQueue = null
    }

    private fun persistentRead(path: String): ByteArray? {
        if (!ensureShell()) return null
        val writer = shellWriter ?: return null
        val queue = shellQueue ?: return null
        return try {
            writer.write(
                withPath("echo $B64_BEGIN; base64 -w0 ${quote(path)} 2>/dev/null; __rc=\$?; echo; echo ${B64_END}\$__rc") + "\n",
            )
            writer.flush()

            val payload = StringBuilder()
            var started = false
            while (true) {
                val line = queue.poll(SHELL_READ_TIMEOUT_MS, TimeUnit.MILLISECONDS)
                if (line == null || line == SHELL_EOF) {
                    // 会话失效：本次及后续读取都退回一次性 su，避免每个资源都等超时。
                    shellDisabled = true
                    killShell()
                    return null
                }
                if (!started) {
                    if (line == B64_BEGIN) started = true
                    continue
                }
                if (line.startsWith(B64_END)) {
                    val code = line.removePrefix(B64_END).trim().toIntOrNull() ?: return null
                    if (code != 0) return null
                    return if (payload.isEmpty()) ByteArray(0) else Base64.decode(payload.toString(), Base64.DEFAULT)
                }
                payload.append(line)
            }
        } catch (e: Exception) {
            shellDisabled = true
            killShell()
            null
        }
    }

    private fun oneShotRead(path: String): ByteArray? =
        try {
            val process =
                ProcessBuilder(SU_PATH, "-c", withPath("cat ${quote(path)}"))
                    .redirectErrorStream(false)
                    .start()
            val bytes = process.inputStream.readBytes()
            process.waitFor()
            if (process.exitValue() == 0 && bytes.isNotEmpty()) bytes else null
        } catch (e: Exception) {
            null
        }

    /** 执行 root 命令并把输出逐行回调，返回退出码。 */
    fun execStreaming(
        cmd: String,
        onOutput: (String) -> Unit,
    ): Int {
        val full = withPath(cmd)
        return try {
            val process =
                ProcessBuilder(SU_PATH, "-c", full)
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

    private fun quote(s: String): String = "'" + s.replace("'", "'\\''") + "'"

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
