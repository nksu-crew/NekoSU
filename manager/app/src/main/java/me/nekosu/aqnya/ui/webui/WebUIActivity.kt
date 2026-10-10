package me.nekosu.aqnya.ui.webui

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.material3.MaterialTheme
import me.nekosu.aqnya.util.DisplayRefreshRate

/**
 * 承载模块 WebUI 的独立 Activity —— 参照 KernelSU 的 `WebUIActivity`。
 *
 * 模块在 `<module>/webroot` 下放网页；[WebUIScreen] 用 [androidx.webkit.WebViewAssetLoader]
 * + [ModulePathHandler] 把它映射到 `https://<domain>/` 下（文件由 root 读取），
 * 注入 `window.ksu`（见 [WebViewBridge]），并处理 `ksu://icon`、`internal/insets.css`、
 * `internal/colors.css`。
 */
class WebUIActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        DisplayRefreshRate.requestHighest(this)
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
