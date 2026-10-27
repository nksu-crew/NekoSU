package me.nekosu.aqnya.ui.webui

import android.util.Log
import android.webkit.WebResourceResponse
import androidx.webkit.WebViewAssetLoader
import me.nekosu.aqnya.data.shell.RootShell
import java.io.ByteArrayInputStream
import java.io.File
import java.util.concurrent.ConcurrentHashMap

/**
 * 以 root 读取模块 `<module>/webroot` 下的文件，作为 WebUI 静态资源返回。
 *
 * 移植自 KernelSU 的 `SuFilePathHandler`：KernelSU 用 libsu 的 `SuFile` 持久 root
 * shell 读文件，这里走 JNI 原生 `readFile`——管理器是已授权 profile，原生代码
 * 在一次性子进程里通过 setresuid 触发内核提权并 open/read，再把读到的字节经管道
 * 送回父进程。提权只发生在子进程，WebView 的工作线程保持原有凭据不被污染。
 * 路径做了词法归一化，禁止 `..` 逃逸出 webroot。
 *
 * 每次 `readFile` 都要 fork 一个 root 子进程，代价不低；而 WebView 重载 / SPA 路由
 * / 重复请求会再次要同样的文件。这里按路径缓存读到的字节（含 404），把同一份
 * WebUI 会话内的重复读取变成内存命中。
 *
 * 另外实现 KernelSU 的两个内建资源：
 *  - `internal/insets.css`：安全区 CSS 变量，并通知宿主启用边距；
 *  - `internal/colors.css`：当前 Material 3 配色的 CSS 变量。
 */
class ModulePathHandler(
    private val webRoot: File,
    private val insetsProvider: () -> Insets,
    private val onInsetsRequested: () -> Unit,
    private val colorsProvider: () -> String,
) : WebViewAssetLoader.PathHandler {
    /** 命中即直接返回；`bytes == null` 表示“确认不存在”，避免反复 fork 去读 404。 */
    private class Cached(
        val mime: String,
        val bytes: ByteArray?,
    )

    private val cache = ConcurrentHashMap<String, Cached>()

    override fun handle(path: String): WebResourceResponse {
        when (path) {
            "internal/insets.css" -> {
                onInsetsRequested()
                return css(insetsProvider().css)
            }

            "internal/colors.css" -> return css(colorsProvider())
        }

        val root = webRoot.toPath().normalize()
        var target = File(webRoot, path).toPath().normalize()
        if (path.isBlank() || path.endsWith('/')) {
            target = target.resolve("index.html")
        }
        if (!target.startsWith(root)) return notFound(path)

        val file = target.toFile()
        val key = file.absolutePath
        // getOrPut 的默认块在锁外执行（ConcurrentHashMap），并发请求不会互相阻塞，
        // 首屏的多个子资源仍能并行读取。
        val cached =
            cache.getOrPut(key) {
                val bytes = RootShell.readFileBytes(key)
                if (bytes == null) {
                    Log.w("NksuWebUI", "404 webui:$path (root=$webRoot)")
                }
                Cached(MimeUtil.guess(file.name.orEmpty()), bytes)
            }

        val bytes = cached.bytes ?: return notFound(path)
        return WebResourceResponse(cached.mime, null, ByteArrayInputStream(bytes))
    }

    private fun css(text: String): WebResourceResponse =
        WebResourceResponse("text/css", "utf-8", ByteArrayInputStream(text.toByteArray(Charsets.UTF_8)))

    /**
     * 404 时返回一小段可见的 HTML（而不是空白），这样即使不抓 logcat，
     * 页面也会直接显示是哪个文件没读到、webroot 是什么。
     */
    private fun notFound(path: String): WebResourceResponse {
        val html =
            "<!doctype html><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">" +
                "<body style=\"font-family:monospace;padding:16px;line-height:1.6\">" +
                "<h3>NekoSU WebUI</h3><p>404: ${escapeHtml(path.ifBlank { "index.html" })}</p>" +
                "<p>webroot: ${escapeHtml(webRoot.absolutePath)}</p>" +
                "<p>请确认模块存在 webroot/index.html，且管理器已获得 root。</p></body>"
        return WebResourceResponse(
            "text/html",
            "utf-8",
            404,
            "Not Found",
            emptyMap(),
            ByteArrayInputStream(html.toByteArray(Charsets.UTF_8)),
        )
    }

    private fun escapeHtml(s: String): String = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
}
