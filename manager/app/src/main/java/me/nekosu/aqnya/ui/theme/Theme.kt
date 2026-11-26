// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.theme

import android.content.Context
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MotionScheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalContext
import me.nekosu.aqnya.data.prefs.AppPreferences

private const val KEY_THEME_MODE = "theme_mode"
private const val KEY_THEME_COLOR = "theme_color"
private const val KEY_AMOLED = "amoled_mode"

/**
 * The only theme-specific input. Every accent, container, outline and surface
 * tone is derived from this hue/saturation pair by [color] and [neutral].
 */
private data class ThemeSeed(
    val hue: Float,
    val saturation: Float,
) {
    fun color(
        lightness: Float,
        saturationScale: Float = 1f,
        hueShift: Float = 0f,
    ): Color {
        val shiftedHue = ((hue + hueShift) % 360f + 360f) % 360f
        return Color.hsl(
            shiftedHue,
            (saturation * saturationScale).coerceIn(0f, 1f),
            lightness.coerceIn(0f, 1f),
        )
    }

    fun neutral(lightness: Float): Color = Color.hsl(hue, 0.06f, lightness.coerceIn(0f, 1f))
}

/** Values match [me.nekosu.aqnya.ui.screens.settings.enums.ThemeColor]. */
private fun seedFor(value: Int): ThemeSeed =
    when (value) {
        2 -> ThemeSeed(270.5f, 0.24f)
        3 -> ThemeSeed(156.75f, 0.39f)
        else -> ThemeSeed(219.6f, 0.46f)
    }

private fun ColorScheme.withFixedColors(seed: ThemeSeed): ColorScheme =
    copy(
        primaryFixed = seed.color(0.90f),
        primaryFixedDim = seed.color(0.80f),
        onPrimaryFixed = seed.color(0.10f),
        onPrimaryFixedVariant = seed.color(0.30f),
        secondaryFixed = seed.color(0.90f, 0.55f, 10f),
        secondaryFixedDim = seed.color(0.80f, 0.55f, 10f),
        onSecondaryFixed = seed.color(0.10f, 0.55f, 10f),
        onSecondaryFixedVariant = seed.color(0.30f, 0.55f, 10f),
        tertiaryFixed = seed.color(0.90f, 0.80f, -10f),
        tertiaryFixedDim = seed.color(0.80f, 0.80f, -10f),
        onTertiaryFixed = seed.color(0.10f, 0.80f, -10f),
        onTertiaryFixedVariant = seed.color(0.30f, 0.80f, -10f),
    )

private fun lightScheme(seed: ThemeSeed): ColorScheme {
    val primary = seed.color(0.42f)
    val primaryContainer = seed.color(0.90f)
    val onPrimaryContainer = seed.color(0.15f)
    val surface = seed.neutral(0.98f)
    val onSurface = seed.neutral(0.10f)

    return lightColorScheme(
        primary = primary,
        onPrimary = seed.color(1f),
        primaryContainer = primaryContainer,
        onPrimaryContainer = onPrimaryContainer,
        inversePrimary = seed.color(0.80f),
        secondary = seed.color(0.42f, 0.55f, 10f),
        onSecondary = seed.color(1f),
        secondaryContainer = seed.color(0.90f, 0.55f, 10f),
        onSecondaryContainer = onPrimaryContainer,
        tertiary = seed.color(0.42f, 0.80f, -10f),
        onTertiary = seed.color(1f),
        tertiaryContainer = seed.color(0.88f, 0.80f, -10f),
        onTertiaryContainer = onPrimaryContainer,
        background = surface,
        onBackground = onSurface,
        surface = surface,
        onSurface = onSurface,
        surfaceVariant = seed.neutral(0.92f),
        onSurfaceVariant = seed.neutral(0.30f),
        surfaceTint = primary,
        inverseSurface = seed.neutral(0.20f),
        inverseOnSurface = seed.neutral(0.95f),
        outline = seed.neutral(0.50f),
        outlineVariant = seed.neutral(0.80f),
        scrim = Color.Black,
        surfaceBright = seed.neutral(1f),
        surfaceContainerLowest = seed.neutral(1f),
        surfaceContainerLow = seed.neutral(0.96f),
        surfaceContainer = seed.neutral(0.94f),
        surfaceContainerHigh = seed.neutral(0.90f),
        surfaceContainerHighest = seed.neutral(0.87f),
        surfaceDim = seed.neutral(0.88f),
    ).withFixedColors(seed)
}

private fun darkScheme(seed: ThemeSeed): ColorScheme {
    val primary = seed.color(0.80f)
    val primaryContainer = seed.color(0.30f)
    val onPrimaryContainer = seed.color(0.90f)
    val surface = seed.neutral(0.07f)
    val onSurface = seed.neutral(0.92f)

    return darkColorScheme(
        primary = primary,
        onPrimary = seed.color(0.18f),
        primaryContainer = primaryContainer,
        onPrimaryContainer = onPrimaryContainer,
        inversePrimary = seed.color(0.40f),
        secondary = seed.color(0.78f, 0.55f, 10f),
        onSecondary = seed.color(0.18f),
        secondaryContainer = seed.color(0.30f, 0.55f, 10f),
        onSecondaryContainer = onPrimaryContainer,
        tertiary = seed.color(0.82f, 0.80f, -10f),
        onTertiary = seed.color(0.18f),
        tertiaryContainer = seed.color(0.28f, 0.80f, -10f),
        onTertiaryContainer = onPrimaryContainer,
        background = surface,
        onBackground = onSurface,
        surface = surface,
        onSurface = onSurface,
        surfaceVariant = seed.neutral(0.16f),
        onSurfaceVariant = seed.neutral(0.78f),
        surfaceTint = primary,
        inverseSurface = seed.neutral(0.92f),
        inverseOnSurface = seed.neutral(0.10f),
        outline = seed.neutral(0.55f),
        outlineVariant = seed.neutral(0.28f),
        scrim = Color.Black,
        surfaceBright = seed.neutral(0.24f),
        surfaceContainerLowest = seed.neutral(0.03f),
        surfaceContainerLow = seed.neutral(0.07f),
        surfaceContainer = seed.neutral(0.12f),
        surfaceContainerHigh = seed.neutral(0.16f),
        surfaceContainerHighest = seed.neutral(0.20f),
        surfaceDim = seed.neutral(0.03f),
    ).withFixedColors(seed)
}

private fun schemeFor(
    value: Int,
    dark: Boolean,
): ColorScheme {
    val seed = seedFor(value)
    return if (dark) darkScheme(seed) else lightScheme(seed)
}

/** The three-tone palette shown by the large theme swatches in Settings. */
data class ThemeSwatchColors(
    val top: Color,
    val bottom: Color,
    val accent: Color,
    val accentVariant: Color,
)

/** Static palette preview for a fixed theme seed; Material You uses the live scheme instead. */
fun themeSwatchColors(
    value: Int,
    dark: Boolean,
): ThemeSwatchColors {
    val scheme = schemeFor(value, dark)
    return ThemeSwatchColors(
        top = scheme.primaryContainer,
        bottom = scheme.tertiaryContainer,
        accent = scheme.primary,
        accentVariant = scheme.secondary,
    )
}

/** Keeps page and container surfaces black while leaving cards readable. */
private fun ColorScheme.amoledBlack(): ColorScheme =
    copy(
        background = AmoledBlack,
        surface = AmoledBlack,
        surfaceDim = AmoledBlack,
        surfaceContainerLowest = AmoledBlack,
        surfaceContainerLow = AmoledBlack,
        surfaceContainer = AmoledBlack,
        surfaceContainerHigh = AmoledBlack,
        surfaceContainerHighest = AmoledBlack,
    )

@Composable
fun NekosuTheme(content: @Composable () -> Unit) {
    val context = LocalContext.current
    val prefs =
        remember(context) {
            context.getSharedPreferences(AppPreferences.PREF_NAME, Context.MODE_PRIVATE)
        }

    var themeMode by remember { mutableIntStateOf(prefs.getInt(KEY_THEME_MODE, 0)) }
    var themeColor by remember { mutableIntStateOf(prefs.getInt(KEY_THEME_COLOR, 0)) }
    var amoledEnabled by remember { mutableStateOf(prefs.getBoolean(KEY_AMOLED, false)) }

    DisposableEffect(prefs) {
        val listener =
            android.content.SharedPreferences.OnSharedPreferenceChangeListener { _, _ ->
                themeMode = prefs.getInt(KEY_THEME_MODE, 0)
                themeColor = prefs.getInt(KEY_THEME_COLOR, 0)
                amoledEnabled = prefs.getBoolean(KEY_AMOLED, false)
            }
        prefs.registerOnSharedPreferenceChangeListener(listener)
        onDispose { prefs.unregisterOnSharedPreferenceChangeListener(listener) }
    }

    val isDark =
        when (themeMode) {
            1 -> false
            2 -> true
            else -> isSystemInDarkTheme()
        }

    val baseScheme =
        if (themeColor == 0) {
            if (isDark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        } else {
            schemeFor(themeColor, isDark)
        }
    val colorScheme = if (amoledEnabled && isDark) baseScheme.amoledBlack() else baseScheme

    EdgeToEdgeSystemBars(darkMode = isDark)

    MaterialTheme(
        colorScheme = colorScheme,
        motionScheme = MotionScheme.expressive(),
        typography = Typography,
        content = content,
    )
}

@Composable
private fun EdgeToEdgeSystemBars(
    darkMode: Boolean,
    statusBarScrim: Color = Color.Transparent,
    navigationBarScrim: Color = Color.Transparent,
) {
    val context = LocalContext.current
    val activity = context as? ComponentActivity ?: return

    LaunchedEffect(activity, darkMode) {
        activity.enableEdgeToEdge(
            statusBarStyle =
                SystemBarStyle.auto(
                    statusBarScrim.toArgb(),
                    statusBarScrim.toArgb(),
                ) { darkMode },
            navigationBarStyle =
                when {
                    darkMode -> SystemBarStyle.dark(navigationBarScrim.toArgb())
                    else ->
                        SystemBarStyle.light(
                            navigationBarScrim.toArgb(),
                            navigationBarScrim.toArgb(),
                        )
                },
        )
    }
}
