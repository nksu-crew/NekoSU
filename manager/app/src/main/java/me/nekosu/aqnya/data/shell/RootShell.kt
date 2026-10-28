// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.data.shell

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import me.nekosu.aqnya.core.Ncore
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

object RootShell {
    data class Result(
        val code: Int,
        val output: String,
    ) {
        val ok: Boolean get() = code == 0
    }

    private const val SU_PATH = "/system/bin/su"

    private val _availability = MutableStateFlow<Boolean?>(null)
    val availability: StateFlow<Boolean?> = _availability.asStateFlow()
    private val detectionStarted = AtomicBoolean(false)

    /** 在应用启动时后台执行一次 root 检测，供后续页面直接读取结果。 */
    fun startDetection() {
        if (!detectionStarted.compareAndSet(false, true)) return
        Thread({ available() }, "nksu-root-detect").apply { isDaemon = true }.start()
    }

    @Synchronized
    fun available(): Boolean {
        _availability.value?.let { return it }
        val r = runCapture(listOf(SU_PATH, "-c", withPath("id")), timeoutSeconds = 60)
        val ok = r != null && r.code == 0 && r.output.contains("uid=0")
        _availability.value = ok
        return ok
    }

    fun exec(cmd: String): Result {
        val full = withPath(cmd)
        return runCapture(listOf(SU_PATH, "-c", full), timeoutSeconds = 0)
            ?: Result(-1, "root shell unavailable")
    }

    /**
     * 以 root 把 [src] 复制到 [dst]（分区 dump / 写回通用），成功返回 true。
     *
     * 供只有外部 su（Magisk/KernelSU 等）而没有 nksu 内核模块提权的设备使用：
     * 直接安装原本通过 Ncore.writeBlock 在 fork 的子进程里 setresuid(0,0,0)
     * 提权，那只在内核模块已加载时成立；这里改用 `su -c dd`，因此
     * `/system/bin/su` 存在即可。
     */
    fun copyFile(
        src: String,
        dst: String,
    ): Boolean = exec("dd if=${quote(src)} of=${quote(dst)} bs=1048576").ok

    /**
     * 以 root 查找 vendor_boot 分区路径，候选顺序与 Ncore.findVendorBoot 一致：
     * 两个 `by-name` 基目录各自先试带 slot 后缀的、再试无后缀的。找不到返回 null。
     */
    fun findVendorBoot(
        slotSuffix: String,
        allowSlotless: Boolean,
    ): String? = findPartition("vendor_boot", slotSuffix, allowSlotless)

    /** 以 root 查找 boot 分区路径；候选顺序与 [findVendorBoot] 一致。 */
    fun findBoot(
        slotSuffix: String,
        allowSlotless: Boolean,
    ): String? = findPartition("boot", slotSuffix, allowSlotless)

    private fun findPartition(
        partition: String,
        slotSuffix: String,
        allowSlotless: Boolean,
    ): String? {
        val candidates =
            buildList {
                for (base in listOf("/dev/block/by-name", "/dev/block/bootdevice/by-name")) {
                    add("$base/$partition$slotSuffix")
                    if (allowSlotless) add("$base/$partition")
                }
            }
        val script =
            "for f in ${candidates.joinToString(" ")}; do " +
                "[ -e \"\$f\" ] && { echo \"\$f\"; exit 0; }; done; exit 1"
        val r = exec(script)
        return r.output
            .lineSequence()
            .map { it.trim() }
            .firstOrNull { it.isNotEmpty() }
    }

    /**
     * 以 root 重启设备。命令一般不会返回（设备随即重启），
     * 因此调用方应在 IO 线程执行，避免阻塞 UI。
     */
    fun reboot(): Boolean = exec("svc power reboot").ok

    /**
     * 以 root 读取文件。优先走常驻读取器（只 fork 一次，之后管道通信），读取器
     * 失败时回退到每次 fork 的 [Ncore.readFile]。
     */
    fun readFileBytes(path: String): ByteArray? {
        val viaReader = runCatching { Ncore.readFilePersistent(path) }.getOrNull()
        if (viaReader != null) return viaReader.takeIf { it.isNotEmpty() }
        return runCatching { Ncore.readFile(path) }.getOrNull()?.takeIf { it.isNotEmpty() }
    }

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

    data class Output(
        val code: Int,
        val stdout: String,
        val stderr: String,
    )

    fun execSplit(cmd: String): Output {
        val full = withPath(cmd)
        return try {
            val process = ProcessBuilder(SU_PATH, "-c", full).redirectErrorStream(false).start()
            val stdout = StringBuilder()
            val stderr = StringBuilder()
            val outThread =
                Thread {
                    runCatching {
                        process.inputStream.bufferedReader().useLines { it.forEach { l -> stdout.append(l).append('\n') } }
                    }
                }
            val errThread =
                Thread {
                    runCatching {
                        process.errorStream.bufferedReader().useLines { it.forEach { l -> stderr.append(l).append('\n') } }
                    }
                }
            outThread.start()
            errThread.start()
            val code = process.waitFor()
            outThread.join(2000)
            errThread.join(2000)
            Output(code, stdout.toString(), stderr.toString())
        } catch (e: Exception) {
            Output(-1, "", e.message.orEmpty())
        }
    }

    fun execStreamingSplit(
        cmd: String,
        onStdout: (String) -> Unit,
        onStderr: (String) -> Unit,
    ): Int {
        val full = withPath(cmd)
        return try {
            val process = ProcessBuilder(SU_PATH, "-c", full).redirectErrorStream(false).start()
            val outThread =
                Thread {
                    runCatching {
                        process.inputStream.bufferedReader().useLines { it.forEach(onStdout) }
                    }
                }
            val errThread =
                Thread {
                    runCatching {
                        process.errorStream.bufferedReader().useLines { it.forEach(onStderr) }
                    }
                }
            outThread.start()
            errThread.start()
            val code = process.waitFor()
            outThread.join(2000)
            errThread.join(2000)
            code
        } catch (e: Exception) {
            onStderr("ERROR: ${e.message}")
            -1
        }
    }

    private fun withPath(cmd: String): String = "PATH=/sbin:/system/sbin:/system/bin:/system/xbin; export PATH; $cmd"

    private fun quote(s: String): String = "'" + s.replace("'", "'\\''") + "'"

    private fun runCapture(
        command: List<String>,
        timeoutSeconds: Int,
    ): Result? =
        try {
            val process = ProcessBuilder(command).redirectErrorStream(true).start()
            val output = StringBuilder()
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
