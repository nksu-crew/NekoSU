package me.nekosu.aqnya.ui.webui

import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.graphics.Color
import java.util.concurrent.atomic.AtomicReference

/**
 * 为模块 WebUI 提供 `internal/colors.css` —— 把当前 Material 3 配色导出成
 * CSS 变量（移植自 KernelSU 的 `MonetColorsProvider`，只保留 Material 分支）。
 *
 * 变量名与 KernelSU 对齐，这样为 KernelSU 写的模块 WebUI 能直接复用。
 */
object MonetColorsProvider {
    private val colorsCss = AtomicReference("")

    fun getColorsCss(): String = colorsCss.get()

    @Composable
    fun UpdateCss() {
        val scheme = MaterialTheme.colorScheme
        LaunchedEffect(scheme) {
            val colors =
                mapOf(
                    "primary" to scheme.primary.toCssValue(),
                    "onPrimary" to scheme.onPrimary.toCssValue(),
                    "primaryContainer" to scheme.primaryContainer.toCssValue(),
                    "onPrimaryContainer" to scheme.onPrimaryContainer.toCssValue(),
                    "inversePrimary" to scheme.inversePrimary.toCssValue(),
                    "secondary" to scheme.secondary.toCssValue(),
                    "onSecondary" to scheme.onSecondary.toCssValue(),
                    "secondaryContainer" to scheme.secondaryContainer.toCssValue(),
                    "onSecondaryContainer" to scheme.onSecondaryContainer.toCssValue(),
                    "tertiary" to scheme.tertiary.toCssValue(),
                    "onTertiary" to scheme.onTertiary.toCssValue(),
                    "tertiaryContainer" to scheme.tertiaryContainer.toCssValue(),
                    "onTertiaryContainer" to scheme.onTertiaryContainer.toCssValue(),
                    "background" to scheme.background.toCssValue(),
                    "onBackground" to scheme.onBackground.toCssValue(),
                    "surface" to scheme.surface.toCssValue(),
                    "tonalSurface" to scheme.surfaceContainer.toCssValue(),
                    "onSurface" to scheme.onSurface.toCssValue(),
                    "surfaceVariant" to scheme.surfaceVariant.toCssValue(),
                    "onSurfaceVariant" to scheme.onSurfaceVariant.toCssValue(),
                    "surfaceTint" to scheme.surfaceTint.toCssValue(),
                    "inverseSurface" to scheme.inverseSurface.toCssValue(),
                    "inverseOnSurface" to scheme.inverseOnSurface.toCssValue(),
                    "error" to scheme.error.toCssValue(),
                    "onError" to scheme.onError.toCssValue(),
                    "errorContainer" to scheme.errorContainer.toCssValue(),
                    "onErrorContainer" to scheme.onErrorContainer.toCssValue(),
                    "outline" to scheme.outline.toCssValue(),
                    "outlineVariant" to scheme.outlineVariant.toCssValue(),
                    "scrim" to scheme.scrim.toCssValue(),
                    "surfaceBright" to scheme.surfaceBright.toCssValue(),
                    "surfaceDim" to scheme.surfaceDim.toCssValue(),
                    "surfaceContainer" to scheme.surfaceContainer.toCssValue(),
                    "surfaceContainerHigh" to scheme.surfaceContainerHigh.toCssValue(),
                    "surfaceContainerHighest" to scheme.surfaceContainerHighest.toCssValue(),
                    "surfaceContainerLow" to scheme.surfaceContainerLow.toCssValue(),
                    "surfaceContainerLowest" to scheme.surfaceContainerLowest.toCssValue(),
                    "filledTonalButtonContentColor" to scheme.onPrimaryContainer.toCssValue(),
                    "filledTonalButtonContainerColor" to scheme.secondaryContainer.toCssValue(),
                    "filledTonalButtonDisabledContentColor" to scheme.onSurfaceVariant.toCssValue(),
                    "filledTonalButtonDisabledContainerColor" to scheme.surfaceVariant.toCssValue(),
                    "filledCardContentColor" to scheme.onPrimaryContainer.toCssValue(),
                    "filledCardContainerColor" to scheme.primaryContainer.toCssValue(),
                    "filledCardDisabledContentColor" to scheme.onSurfaceVariant.toCssValue(),
                    "filledCardDisabledContainerColor" to scheme.surfaceVariant.toCssValue(),
                )
            colorsCss.set(colors.toCssVars())
        }
    }

    private fun Map<String, String>.toCssVars(): String =
        buildString {
            append(":root {\n")
            for ((key, value) in this@toCssVars) {
                append("  --$key: $value;\n")
            }
            append("}\n")
        }

    private fun Color.toCssValue(): String {
        fun Float.toHex(): String = (this * 255).toInt().coerceIn(0, 255).toString(16).padStart(2, '0')

        return if (alpha == 1f) {
            "#${red.toHex()}${green.toHex()}${blue.toHex()}"
        } else {
            "#${red.toHex()}${green.toHex()}${blue.toHex()}${alpha.toHex()}"
        }
    }
}
