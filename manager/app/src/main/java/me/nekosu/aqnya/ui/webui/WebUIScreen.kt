package me.nekosu.aqnya.ui.webui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Color
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBars
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.graphics.drawable.toBitmap
import androidx.webkit.WebViewAssetLoader
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File

/** WebViewAssetLoader 使用的虚拟域名；模块网页从这里加载。 */
private const val WEBUI_DOMAIN = "mui.kernelsu.org"

/** `ksu://icon/<pkg>` 返回的图标边长（像素）。 */
private const val ICON_PX = 192

/**
 * 模块 WebUI 的宿主：加载 `<module>/webroot/index.html`，注入 `window.ksu`，
 * 处理 `ksu://icon/<pkg>` 与 `internal/insets.css` / `internal/colors.css`。
 */
@SuppressLint("SetJavaScriptEnabled")
@Composable
internal fun WebUIScreen(
    moduleId: String,
    onExit: () -> Unit,
) {
    val context = LocalContext.current
    val density = LocalDensity.current
    val layoutDirection = LocalLayoutDirection.current
    val systemBars = WindowInsets.systemBars

    val webRoot = remember(moduleId) { File("/data/adb/modules/$moduleId/webroot") }
    var loading by remember { mutableStateOf(true) }
    var currentWebView by remember { mutableStateOf<WebView?>(null) }
    val currentInsets = remember { mutableStateOf(Insets(0, 0, 0, 0)) }
    val insetsEnabled = remember { mutableStateOf(false) }

    MonetColorsProvider.UpdateCss()

    // 页面通过 internal/insets.css 申请安全区后，跟随系统栏变化持续注入 CSS 变量。
    LaunchedEffect(density, layoutDirection, systemBars, insetsEnabled.value) {
        if (!insetsEnabled.value) return@LaunchedEffect
        snapshotFlow {
            Insets(
                top = (systemBars.getTop(density) / density.density).toInt(),
                bottom = (systemBars.getBottom(density) / density.density).toInt(),
                left = (systemBars.getLeft(density, layoutDirection) / density.density).toInt(),
                right = (systemBars.getRight(density, layoutDirection) / density.density).toInt(),
            )
        }.collect { newInsets ->
            if (currentInsets.value != newInsets) {
                currentInsets.value = newInsets
                currentWebView?.evaluateJavascript(newInsets.js, null)
            }
        }
    }

    BackHandler(enabled = true) {
        val webView = currentWebView
        if (webView != null && webView.canGoBack()) webView.goBack() else onExit()
    }

    val contentModifier =
        if (insetsEnabled.value) {
            Modifier.fillMaxSize().padding(systemBars.asPaddingValues())
        } else {
            Modifier.fillMaxSize()
        }

    Box(modifier = Modifier.fillMaxSize()) {
        AndroidView(
            modifier = contentModifier,
            factory = { ctx ->
                WebView(ctx).apply {
                    setBackgroundColor(Color.TRANSPARENT)
                    settings.javaScriptEnabled = true
                    settings.domStorageEnabled = true
                    settings.allowFileAccess = false

                    val assetLoader =
                        WebViewAssetLoader.Builder()
                            .setDomain(WEBUI_DOMAIN)
                            .addPathHandler(
                                "/",
                                ModulePathHandler(
                                    webRoot = webRoot,
                                    insetsProvider = { currentInsets.value },
                                    onInsetsRequested = { insetsEnabled.value = true },
                                    colorsProvider = { MonetColorsProvider.getColorsCss() },
                                ),
                            )
                            .build()

                    webViewClient =
                        object : WebViewClient() {
                            override fun shouldInterceptRequest(
                                view: WebView,
                                request: WebResourceRequest,
                            ): WebResourceResponse? {
                                val url = request.url
                                if (url.scheme.equals("ksu", ignoreCase = true) &&
                                    url.host.equals("icon", ignoreCase = true)
                                ) {
                                    return iconResponse(context, url.path?.trimStart('/').orEmpty())
                                }
                                return assetLoader.shouldInterceptRequest(url)
                            }

                            override fun onPageFinished(
                                view: WebView,
                                url: String,
                            ) {
                                loading = false
                            }

                            override fun onReceivedError(
                                view: WebView,
                                request: WebResourceRequest,
                                error: WebResourceError,
                            ) {
                                if (request.isForMainFrame) loading = false
                            }
                        }

                    addJavascriptInterface(
                        WebViewBridge(
                            context = context,
                            moduleId = moduleId,
                            webViewProvider = { currentWebView },
                            onExit = onExit,
                        ),
                        "ksu",
                    )

                    loadUrl("https://$WEBUI_DOMAIN/")
                    currentWebView = this
                }
            },
        )

        if (loading) {
            Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator()
            }
        }
    }
}

/** 把某个包的应用图标编码成 PNG 返回给 `ksu://icon/<pkg>`。 */
private fun iconResponse(
    context: Context,
    packageName: String,
): WebResourceResponse {
    val headers = mapOf("Access-Control-Allow-Origin" to "*")
    val bytes =
        if (packageName.isBlank()) {
            null
        } else {
            runCatching {
                val drawable = context.packageManager.getApplicationIcon(packageName)
                val bitmap: Bitmap = drawable.toBitmap(ICON_PX, ICON_PX)
                ByteArrayOutputStream().use { out ->
                    bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)
                    out.toByteArray()
                }
            }.getOrNull()
        }

    return if (bytes != null) {
        WebResourceResponse("image/png", null, 200, "OK", headers, ByteArrayInputStream(bytes))
    } else {
        WebResourceResponse(
            "text/plain",
            "utf-8",
            404,
            "Not Found",
            headers,
            ByteArrayInputStream("No such package".toByteArray()),
        )
    }
}
