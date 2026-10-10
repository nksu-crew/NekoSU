package me.nekosu.aqnya.ui.webui

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
        if (!target.startsWith(root)) return notFound()

        // 目录（或根路径）→ index.html
        if (path.isBlank() || path.endsWith('/')) {
            target = target.resolve("index.html")
        }

        var bytes = RootShell.readFileBytes(target.toFile().absolutePath)
        if (bytes == null) {
            // 请求可能指向一个目录：退回它的 index.html 再试一次。
            target = target.resolve("index.html")
            bytes = RootShell.readFileBytes(target.toFile().absolutePath)
        }
        if (bytes == null) return notFound()

        return WebResourceResponse(
            MimeUtil.guess(target.fileName?.toString().orEmpty()),
            null,
            ByteArrayInputStream(bytes),
        )
    }

    private fun css(text: String): WebResourceResponse =
        WebResourceResponse("text/css", "utf-8", ByteArrayInputStream(text.toByteArray(Charsets.UTF_8)))

    private fun notFound(): WebResourceResponse =
        WebResourceResponse(
            "text/plain",
            "utf-8",
            404,
            "Not Found",
            emptyMap(),
            ByteArrayInputStream(ByteArray(0)),
        )
}
