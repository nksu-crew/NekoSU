package me.nekosu.aqnya.util.module

import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.util.Log
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.core.graphics.drawable.IconCompat
import androidx.core.net.toUri
import me.nekosu.aqnya.MainActivity
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.screens.modules.ShortcutType

/**
 * Module shortcut helpers.
 *
 * This is a trimmed port of KernelSU's `Shortcut`: the root-shell pinned-shortcut permission dance
 * (MIUI/ColorOS specific) is dropped, so shortcuts are pushed as dynamic shortcuts only. The
 * deep-link scheme is `nksu://` instead of `ksu://`.
 */
object Shortcut {
    private const val TAG = "ModuleShortcut"
    const val SCHEME_NKSU = "nksu"
    const val HOST_ACTION = "action"
    const val HOST_WEBUI = "webui"

    fun buildShortcutUri(
        moduleId: String,
        type: ShortcutType,
    ): Uri {
        val host =
            when (type) {
                ShortcutType.Action -> HOST_ACTION
                ShortcutType.WebUI -> HOST_WEBUI
            }
        return Uri.Builder()
            .scheme(SCHEME_NKSU)
            .authority(host)
            .appendQueryParameter("id", moduleId)
            .build()
    }

    fun createModuleActionShortcut(
        context: Context,
        moduleId: String,
        name: String,
        iconUri: String?,
    ) {
        createModuleShortcut(
            context = context,
            moduleId = moduleId,
            name = name,
            iconUri = iconUri,
            shortcutId = "module_action_$moduleId",
            type = ShortcutType.Action,
        )
    }

    fun createModuleWebUiShortcut(
        context: Context,
        moduleId: String,
        name: String,
        iconUri: String?,
    ) {
        createModuleShortcut(
            context = context,
            moduleId = moduleId,
            name = name,
            iconUri = iconUri,
            shortcutId = "module_webui_$moduleId",
            type = ShortcutType.WebUI,
        )
    }

    private fun createModuleShortcut(
        context: Context,
        moduleId: String,
        name: String,
        iconUri: String?,
        shortcutId: String,
        type: ShortcutType,
    ) {
        val intent =
            Intent(context, MainActivity::class.java).apply {
                action = Intent.ACTION_VIEW
                data = buildShortcutUri(moduleId, type)
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP)
            }
        val icon = createShortcutIcon(context, iconUri) ?: IconCompat.createWithResource(context, R.mipmap.ic_launcher)
        val shortcut =
            ShortcutInfoCompat.Builder(context, shortcutId)
                .setShortLabel(name)
                .setIntent(intent)
                .setIcon(icon)
                .build()
        runCatching { ShortcutManagerCompat.pushDynamicShortcut(context, shortcut) }
            .onFailure { Log.w(TAG, "pushDynamicShortcut failed: ${it.message}", it) }
    }

    fun hasModuleActionShortcut(
        context: Context,
        moduleId: String,
    ): Boolean = hasPinnedShortcut(context, "module_action_$moduleId")

    fun hasModuleWebUiShortcut(
        context: Context,
        moduleId: String,
    ): Boolean = hasPinnedShortcut(context, "module_webui_$moduleId")

    fun deleteModuleActionShortcut(
        context: Context,
        moduleId: String,
    ) = deleteShortcut(context, "module_action_$moduleId")

    fun deleteModuleWebUiShortcut(
        context: Context,
        moduleId: String,
    ) = deleteShortcut(context, "module_webui_$moduleId")

    fun loadShortcutBitmap(
        context: Context,
        iconUri: String?,
    ): Bitmap? {
        if (iconUri.isNullOrBlank()) return null
        return runCatching {
            val uri = iconUri.toUri()
            if (uri.scheme.equals("content", ignoreCase = true) || uri.scheme.equals("file", ignoreCase = true)) {
                context.contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it) }
            } else {
                null
            }
        }.getOrNull()
    }

    private fun createShortcutIcon(
        context: Context,
        iconUri: String?,
    ): IconCompat? = loadShortcutBitmap(context, iconUri)?.let { IconCompat.createWithBitmap(it) }

    private fun hasPinnedShortcut(
        context: Context,
        id: String,
    ): Boolean =
        runCatching {
            val shortcuts =
                ShortcutManagerCompat.getShortcuts(
                    context,
                    ShortcutManagerCompat.FLAG_MATCH_PINNED,
                )
            shortcuts.any { it.id == id && it.isEnabled }
        }.getOrDefault(false)

    private fun deleteShortcut(
        context: Context,
        id: String,
    ) {
        runCatching { ShortcutManagerCompat.removeDynamicShortcuts(context, listOf(id)) }
        runCatching { ShortcutManagerCompat.disableShortcuts(context, listOf(id), "") }
    }
}
