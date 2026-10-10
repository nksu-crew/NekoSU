package me.nekosu.aqnya.ui.webui

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import android.util.Log
import android.webkit.ConsoleMessage
import android.webkit.JsPromptResult
import android.webkit.JsResult
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebChromeClient.FileChooserParams
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBars
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.graphics.drawable.toBitmap
import androidx.webkit.WebViewAssetLoader
import me.nekosu.aqnya.R
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
    var pendingDialog by remember { mutableStateOf<JsDialog?>(null) }
    var fileCallback by remember { mutableStateOf<ValueCallback<Array<Uri>>?>(null) }

    val fileLauncher =
        rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            val uris: Array<Uri>? =
                if (result.resultCode == Activity.RESULT_OK) {
                    val data = result.data
                    data?.clipData?.let { clip -> Array(clip.itemCount) { i -> clip.getItemAt(i).uri } }
                        ?: data?.data?.let { arrayOf(it) }
                } else {
                    null
                }
            fileCallback?.onReceiveValue(uris)
            fileCallback = null
        }

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
                                val response = assetLoader.shouldInterceptRequest(url)
                                if (response == null) Log.w("NksuWebUI", "asset loader miss: $url")
                                return response
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
                                if (request.isForMainFrame) {
                                    loading = false
                                    Log.w("NksuWebUI", "load error: ${error.description} for ${request.url}")
                                }
                            }
                        }

                    webChromeClient =
                        object : WebChromeClient() {
                            override fun onJsAlert(
                                view: WebView,
                                url: String?,
                                message: String?,
                                result: JsResult?,
                            ): Boolean {
                                if (message == null || result == null) return false
                                pendingDialog = JsDialog.Alert(message, result)
                                return true
                            }

                            override fun onJsConfirm(
                                view: WebView,
                                url: String?,
                                message: String?,
                                result: JsResult?,
                            ): Boolean {
                                if (message == null || result == null) return false
                                pendingDialog = JsDialog.Confirm(message, result)
                                return true
                            }

                            override fun onJsPrompt(
                                view: WebView,
                                url: String?,
                                message: String?,
                                defaultValue: String?,
                                result: JsPromptResult?,
                            ): Boolean {
                                if (message == null || defaultValue == null || result == null) return false
                                pendingDialog = JsDialog.Prompt(message, defaultValue, result)
                                return true
                            }

                            override fun onShowFileChooser(
                                view: WebView,
                                callback: ValueCallback<Array<Uri>>?,
                                params: FileChooserParams?,
                            ): Boolean {
                                fileCallback?.onReceiveValue(null)
                                fileCallback = callback
                                val intent =
                                    params?.createIntent()
                                        ?: Intent(Intent.ACTION_GET_CONTENT).apply { type = "*/*" }
                                if (params?.mode == FileChooserParams.MODE_OPEN_MULTIPLE) {
                                    intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
                                }
                                fileLauncher.launch(intent)
                                return true
                            }

                            override fun onConsoleMessage(consoleMessage: ConsoleMessage?): Boolean {
                                Log.d(
                                    "NksuWebUI",
                                    "${consoleMessage?.message()} @ ${consoleMessage?.sourceId()}:${consoleMessage?.lineNumber()}",
                                )
                                return true
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

                    loadUrl("https://$WEBUI_DOMAIN/index.html")
                    currentWebView = this
                }
            },
        )

        if (loading) {
            Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator()
            }
        }

        pendingDialog?.let { dialog ->
            JsDialogHost(dialog) { pendingDialog = null }
        }
    }
}

/** WebView 里 `alert` / `confirm` / `prompt` 的待处理状态。 */
private sealed interface JsDialog {
    val message: String

    data class Alert(
        override val message: String,
        val result: JsResult,
    ) : JsDialog

    data class Confirm(
        override val message: String,
        val result: JsResult,
    ) : JsDialog

    data class Prompt(
        override val message: String,
        val defaultValue: String,
        val result: JsPromptResult,
    ) : JsDialog
}

/** 用 Material 弹窗承接 WebView 的 JS 对话框，代替系统默认样式。 */
@Composable
private fun JsDialogHost(
    dialog: JsDialog,
    onDismiss: () -> Unit,
) {
    when (dialog) {
        is JsDialog.Alert -> {
            AlertDialog(
                onDismissRequest = {
                    dialog.result.confirm()
                    onDismiss()
                },
                confirmButton = {
                    TextButton(onClick = {
                        dialog.result.confirm()
                        onDismiss()
                    }) {
                        Text(stringResource(R.string.dialog_confirm))
                    }
                },
                text = { Text(dialog.message) },
            )
        }

        is JsDialog.Confirm -> {
            AlertDialog(
                onDismissRequest = {
                    dialog.result.cancel()
                    onDismiss()
                },
                confirmButton = {
                    TextButton(onClick = {
                        dialog.result.confirm()
                        onDismiss()
                    }) {
                        Text(stringResource(R.string.dialog_confirm))
                    }
                },
                dismissButton = {
                    TextButton(onClick = {
                        dialog.result.cancel()
                        onDismiss()
                    }) {
                        Text(stringResource(R.string.dialog_cancel))
                    }
                },
                text = { Text(dialog.message) },
            )
        }

        is JsDialog.Prompt -> {
            var value by remember(dialog) { mutableStateOf(dialog.defaultValue) }
            AlertDialog(
                onDismissRequest = {
                    dialog.result.cancel()
                    onDismiss()
                },
                confirmButton = {
                    TextButton(onClick = {
                        dialog.result.confirm(value)
                        onDismiss()
                    }) {
                        Text(stringResource(R.string.dialog_confirm))
                    }
                },
                dismissButton = {
                    TextButton(onClick = {
                        dialog.result.cancel()
                        onDismiss()
                    }) {
                        Text(stringResource(R.string.dialog_cancel))
                    }
                },
                text = {
                    OutlinedTextField(
                        value = value,
                        onValueChange = { value = it },
                        label = { Text(dialog.message) },
                        modifier = Modifier.fillMaxWidth(),
                    )
                },
            )
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
