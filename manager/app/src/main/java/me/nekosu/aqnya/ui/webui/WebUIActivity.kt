package me.nekosu.aqnya.ui.webui

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import androidx.webkit.WebViewAssetLoader
import java.io.File

/** WebViewAssetLoader 使用的虚拟域名；模块网页从这里加载。 */
private const val WEBUI_DOMAIN = "mui.kernelsu.org"

/**
 * 承载模块 WebUI 的独立 Activity —— 参照 KernelSU 的 `WebUIActivity`。
 *
 * 模块在 `<module>/webroot` 下放网页；这里用 [WebViewAssetLoader] +
 * [ModulePathHandler] 把它映射到 `https://<domain>/` 下（文件由 root 读取），
 * 并注入 `window.ksu`（见 [WebViewBridge]），加载 `index.html`。
 */
class WebUIActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val moduleId = intent.getStringExtra(EXTRA_ID) ?: intent.data?.getQueryParameter("id")
        if (moduleId.isNullOrBlank()) {
            finish()
            return
        }
        setContent {
            MaterialTheme {
                WebUIScreen(moduleId = moduleId, onExit = { finish() })
            }
        }
    }

    companion object {
        const val EXTRA_ID = "id"

        fun intent(
            context: Context,
            moduleId: String,
        ): Intent =
            Intent(context, WebUIActivity::class.java)
                .putExtra(EXTRA_ID, moduleId)
                .setData(Uri.parse("nksu://webui?id=$moduleId"))
    }
}

@SuppressLint("SetJavaScriptEnabled")
@Composable
private fun WebUIScreen(
    moduleId: String,
    onExit: () -> Unit,
) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val webRoot = remember(moduleId) { File("/data/adb/modules/$moduleId/webroot") }
    var loading by remember { mutableStateOf(true) }
    var currentWebView by remember { mutableStateOf<WebView?>(null) }

    BackHandler(enabled = true) {
        val webView = currentWebView
        if (webView != null && webView.canGoBack()) webView.goBack() else onExit()
    }

    Box(modifier = Modifier.fillMaxSize()) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { ctx ->
                WebView(ctx).apply {
                    setBackgroundColor(Color.TRANSPARENT)
                    settings.javaScriptEnabled = true
                    settings.domStorageEnabled = true
                    settings.allowFileAccess = false

                    val assetLoader =
                        WebViewAssetLoader.Builder()
                            .setDomain(WEBUI_DOMAIN)
                            .addPathHandler("/", ModulePathHandler(webRoot))
                            .build()

                    webViewClient =
                        object : WebViewClient() {
                            override fun shouldInterceptRequest(
                                view: WebView,
                                request: WebResourceRequest,
                            ): WebResourceResponse? = assetLoader.shouldInterceptRequest(request.url)

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
