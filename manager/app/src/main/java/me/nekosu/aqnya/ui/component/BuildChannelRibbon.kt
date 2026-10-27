// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.component

import androidx.compose.foundation.background
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import me.nekosu.aqnya.BuildConfig

/**
 * A corner ribbon used by alpha-style builds.
 */
@Composable
fun NonProductionRibbon(modifier: Modifier = Modifier) {
    val channel = BuildConfig.NKSU_CHANNEL
    if (channel != "canary" && channel != "beta") return

    val isCanary = channel == "canary"
    val dark = isSystemInDarkTheme()
    val background =
        when {
            isCanary && dark -> Color(0xFFFFB74D)
            isCanary -> Color(0xFFD84315)
            dark -> Color(0xFF90CAF9)
            else -> Color(0xFF1565C0)
        }
    val foreground =
        when {
            isCanary && dark -> Color.Black
            isCanary -> Color.White
            dark -> Color.Black
            else -> Color.White
        }
    val layoutDirection = LocalLayoutDirection.current

    Box(
        modifier =
            modifier
                .offset(x = 32.dp, y = 24.dp)
                .rotate(if (layoutDirection == LayoutDirection.Rtl) -45f else 45f)
                .background(background)
                .width(150.dp)
                .heightIn(min = 24.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = if (isCanary) "CANARY" else "BETA",
            color = foreground,
            fontSize = 10.sp,
            fontWeight = FontWeight.Bold,
            letterSpacing = 1.sp,
            textAlign = TextAlign.Center,
        )
    }
}
