package me.nekosu.aqnya.util

import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import androidx.core.content.FileProvider
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.nekosu.aqnya.R
import me.nekosu.aqnya.data.installer.VendorBootInstaller
import java.io.File

object LogUtils {
    // 日志保存 / 导出都是纯文件与进程操作，统一放到后台线程，避免卡住 UI。
    private val ioScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    /**
     * 把一段文本日志保存到 /sdcard/Download。
     *
     * 先写进缓存目录，再复用 [VendorBootInstaller.exportToDownload]：它在
     * Android 10+ 走 MediaStore、否则直接写公共目录，无需额外拼接导出逻辑。
     */
    fun saveLog(
        context: Context,
        fileName: String,
        content: String,
    ) {
        val appContext = context.applicationContext
        ioScope.launch {
            val result =
                runCatching {
                    val tmp = File(appContext.cacheDir, fileName)
                    tmp.writeText(content)
                    VendorBootInstaller.exportToDownload(appContext, tmp)
                }
            withContext(Dispatchers.Main) {
                result
                    .onSuccess { dest ->
                        Toast
                            .makeText(
                                appContext,
                                appContext.getString(R.string.modules_log_saved, dest.name),
                                Toast.LENGTH_SHORT,
                            ).show()
                    }.onFailure {
                        Toast
                            .makeText(
                                appContext,
                                appContext.getString(R.string.modules_log_save_failed),
                                Toast.LENGTH_SHORT,
                            ).show()
                    }
            }
        }
    }

    fun exportLogs(context: Context) {
        val appContext = context.applicationContext
        ioScope.launch {
            val logFile = File(appContext.cacheDir, "logcat.log")
            val error =
                runCatching {
                    val process =
                        Runtime.getRuntime().exec(
                            arrayOf("logcat", "-d", "-v", "threadtime", "-f", logFile.absolutePath),
                        )
                    process.inputStream.bufferedReader().use { it.readText() }
                    process.errorStream.bufferedReader().use { it.readText() }
                    process.waitFor()
                }.exceptionOrNull()

            withContext(Dispatchers.Main) {
                if (error != null) {
                    Toast.makeText(appContext, "导出失败: ${error.message}", Toast.LENGTH_SHORT).show()
                    return@withContext
                }

                if (logFile.exists() && logFile.length() > 0) {
                    shareLogFile(context, logFile)
                } else {
                    Toast.makeText(appContext, "日志为空", Toast.LENGTH_SHORT).show()
                }
            }
        }
    }

    private fun shareLogFile(
        context: Context,
        file: File,
    ) {
        val authority = "${context.packageName}.fileprovider"
        val contentUri: Uri = FileProvider.getUriForFile(context, authority, file)

        val intent =
            Intent(Intent.ACTION_SEND).apply {
                type = "text/plain"
                putExtra(Intent.EXTRA_STREAM, contentUri)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                clipData = ClipData.newUri(context.contentResolver, "logcat.log", contentUri)
            }

        context.startActivity(Intent.createChooser(intent, "分享/导出日志"))
    }
}
