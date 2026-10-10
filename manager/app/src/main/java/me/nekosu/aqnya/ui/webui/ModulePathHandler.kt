package me.nekosu.aqnya.ui.webui

import android.util.Log
import android.webkit.WebResourceResponse
import androidx.webkit.WebViewAssetLoader
import me.nekosu.aqnya.util.RootShell
import java.io.ByteArrayInputStream
import java.io.File

/**
 * 以 root 读取模块 `<module>/webroot` 下的文件，作为 WebUI 静态资源返回。
 *
 * 移植自 KernelSU 的 `SuFilePathHandler`：KernelSU 用 libsu 的 `SuFile` 持久 root
 * shell 读文件，这里直接对每个文件跑 `su -c cat`（NekoSU 没有常驻 root shell）。
 * 路径做了词法归一化，禁止 `..` 逃逸出 webroot。
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

        val bytes = RootShell.readFileBytes(target.toFile().absolutePath)
        if (bytes == null) {
            Log.w("NksuWebUI", "404 webui:$path (root=$webRoot)")
            return notFound(path)
        }

        Log.d("NksuWebUI", "serve webui:$path (${bytes.size} bytes)")
        return WebResourceResponse(
            MimeUtil.guess(target.fileName?.toString().orEmpty()),
            null,
            ByteArrayInputStream(bytes),
        )
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

    private fun escapeHtml(s: String): String =
        s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
}
