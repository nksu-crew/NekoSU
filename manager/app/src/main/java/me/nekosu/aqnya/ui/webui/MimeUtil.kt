package me.nekosu.aqnya.ui.webui

import java.net.URLConnection

/**
 * 模块 WebUI 静态资源的 MIME 猜测 —— 简化的 KernelSU `MimeUtil`。
 *
 * 常见扩展名优先用内置表（`URLConnection` 对 js/wasm/字体等支持不全），
 * 其余交给 [URLConnection.guessContentTypeFromName]，最后退回 text/plain。
 */
object MimeUtil {
    private val overrides =
        mapOf(
            "html" to "text/html",
            "htm" to "text/html",
            "js" to "application/javascript",
            "mjs" to "application/javascript",
            "css" to "text/css",
            "json" to "application/json",
            "map" to "application/json",
            "webmanifest" to "application/manifest+json",
            "svg" to "image/svg+xml",
            "png" to "image/png",
            "jpg" to "image/jpeg",
            "jpeg" to "image/jpeg",
            "gif" to "image/gif",
            "webp" to "image/webp",
            "ico" to "image/x-icon",
            "woff" to "font/woff",
            "woff2" to "font/woff2",
            "ttf" to "font/ttf",
            "otf" to "font/otf",
            "wasm" to "application/wasm",
            "txt" to "text/plain",
            "xml" to "text/xml",
            "mp4" to "video/mp4",
            "webm" to "video/webm",
            "mp3" to "audio/mpeg",
            "ogg" to "audio/ogg",
        )

    fun guess(name: String): String {
        val ext = name.substringAfterLast('.', "").lowercase()
        return overrides[ext] ?: URLConnection.guessContentTypeFromName(name) ?: "text/plain"
    }
}
