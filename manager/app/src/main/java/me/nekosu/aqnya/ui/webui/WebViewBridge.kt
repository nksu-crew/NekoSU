package me.nekosu.aqnya.ui.webui

import android.content.Context
import android.content.pm.ApplicationInfo
import android.os.Handler
import android.os.Looper
import android.webkit.JavascriptInterface
import android.webkit.WebView
import android.widget.Toast
import androidx.core.content.pm.PackageInfoCompat
import me.nekosu.aqnya.util.ModuleRepository
import me.nekosu.aqnya.util.RootShell
import org.json.JSONArray
import org.json.JSONObject

/**
 * 注入到模块 WebUI 的 `window.ksu`，接口与 KernelSU 保持一致，
 * 这样为 KernelSU 写的模块 WebUI 可以原样运行。
 *
 * 所有方法都在 WebView 的 JavaBridge 线程上调用（非主线程），因此可以直接
 * 做阻塞的 root 命令；需要回到页面时用 [eval] 投递到主线程。
 */
class WebViewBridge(
    private val context: Context,
    private val moduleId: String,
    private val webViewProvider: () -> WebView?,
    private val onExit: () -> Unit,
) {
    private val mainHandler = Handler(Looper.getMainLooper())

    private fun eval(js: String) {
        val webView = webViewProvider() ?: return
        // evaluateJavascript() 期望纯 JS，去掉 `javascript:` 前缀（保留它只是个 label，能跑但不规范）。
        val code = if (js.startsWith("javascript:")) js.substring("javascript:".length) else js
        mainHandler.post { webView.evaluateJavascript(code, null) }
    }

    /** 同步执行一条 root 命令，返回合并输出。 */
    @JavascriptInterface
    fun exec(cmd: String): String = RootShell.exec(cmd).output

    /** 执行 root 命令，结束后回调 `callbackFunc(code, stdout, stderr)`。 */
    @JavascriptInterface
    fun exec(
        cmd: String,
        callbackFunc: String,
    ) {
        val result = RootShell.exec(cmd)
        eval(
            "javascript:(function(){try{${callbackFunc}(${result.code},${
                JSONObject.quote(result.output)
            },'');}catch(e){console.error(e);}})();",
        )
    }

    @JavascriptInterface
    fun exec(
        cmd: String,
        options: String?,
        callbackFunc: String,
    ) {
        val sb = StringBuilder()
        applyOptions(sb, options)
        sb.append(cmd)
        val result = RootShell.exec(sb.toString())
        eval(
            "javascript:(function(){try{${callbackFunc}(${result.code},${
                JSONObject.quote(result.output)
            },'');}catch(e){console.error(e);}})();",
        )
    }

    /** 后台执行并流式回填 stdout/stderr，最后发出 exit。 */
    @JavascriptInterface
    fun spawn(
        command: String,
        args: String,
        options: String?,
        callbackFunc: String,
    ) {
        val sb = StringBuilder()
        applyOptions(sb, options)
        sb.append(command)
        runCatching {
            val array = JSONArray(args)
            for (i in 0 until array.length()) {
                sb.append(' ').append(array.getString(i))
            }
        }
        val full = sb.toString()

        Thread {
            val code =
                RootShell.execStreaming(full) { line ->
                    eval(
                        "javascript:(function(){try{${callbackFunc}.stdout.emit('data',${
                            JSONObject.quote(line)
                        });}catch(e){console.error(e);}})();",
                    )
                }
            eval("javascript:(function(){try{${callbackFunc}.emit('exit',${code});}catch(e){console.error(e);}})();")
        }.start()
    }

    private fun applyOptions(
        sb: StringBuilder,
        options: String?,
    ) {
        if (options.isNullOrBlank()) return
        val obj = runCatching { JSONObject(options) }.getOrNull() ?: return
        obj.optString("cwd").takeIf { it.isNotBlank() }?.let { sb.append("cd ").append(it).append(";") }
        obj.optJSONObject("env")?.let { env ->
            env.keys().forEach { key -> sb.append("export ").append(key).append('=').append(env.getString(key)).append(';') }
        }
    }

    @JavascriptInterface
    fun toast(msg: String) {
        mainHandler.post { Toast.makeText(context, msg, Toast.LENGTH_SHORT).show() }
    }

    /** 返回当前模块的信息（含 moduleDir），字段与 KernelSU 的 moduleInfo 对齐。 */
    @JavascriptInterface
    fun moduleInfo(): String {
        val obj = JSONObject()
        obj.put("moduleDir", "/data/adb/modules/$moduleId")
        val info = ModuleRepository.list(context).find { it.id == moduleId }
        if (info != null) {
            obj.put("id", info.id)
            obj.put("name", info.name)
            obj.put("version", info.version)
            obj.put("versionCode", info.versionCode)
            obj.put("author", info.author)
            obj.put("description", info.description)
            obj.put("enabled", info.enabled)
        }
        return obj.toString()
    }

    @JavascriptInterface
    fun listPackages(type: String): String {
        val pm = context.packageManager
        val names =
            pm.getInstalledPackages(0)
                .filter { pkg ->
                    val flags = pkg.applicationInfo?.flags ?: 0
                    when (type.lowercase()) {
                        "system" -> flags and ApplicationInfo.FLAG_SYSTEM != 0
                        "user" -> flags and ApplicationInfo.FLAG_SYSTEM == 0
                        else -> true
                    }
                }.map { it.packageName }
                .sorted()
        return JSONArray(names).toString()
    }

    @JavascriptInterface
    fun getPackagesInfo(packageNamesJson: String): String {
        val pm = context.packageManager
        val out = JSONArray()
        runCatching {
            val names = JSONArray(packageNamesJson)
            for (i in 0 until names.length()) {
                val name = names.getString(i)
                val obj = JSONObject()
                obj.put("packageName", name)
                try {
                    val pkg = pm.getPackageInfo(name, 0)
                    val app = pkg.applicationInfo
                    obj.put("versionName", pkg.versionName ?: "")
                    obj.put("versionCode", PackageInfoCompat.getLongVersionCode(pkg))
                    obj.put("appLabel", app?.loadLabel(pm)?.toString() ?: name)
                    obj.put("isSystem", app != null && (app.flags and ApplicationInfo.FLAG_SYSTEM) != 0)
                    obj.put("uid", app?.uid ?: JSONObject.NULL)
                } catch (_: Exception) {
                    obj.put("error", "Package not found or inaccessible")
                }
                out.put(obj)
            }
        }
        return out.toString()
    }

    @JavascriptInterface
    fun exit() {
        mainHandler.post { onExit() }
    }

    /** KernelSU 的边到边开关；这里保持 no-op，仅保证接口存在。 */
    @JavascriptInterface
    fun enableEdgeToEdge(enable: Boolean) {
        // no-op
    }

    @JavascriptInterface
    fun fullScreen(enable: Boolean) {
        // no-op
    }
}
