package com.forkmesh.nativeapp.feature.health

import android.os.Handler
import android.os.Looper
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace

private val mainHandler = Handler(Looper.getMainLooper())

internal class ModelTicker {
    private var value by mutableIntStateOf(0)

    fun bump() {
        mainHandler.post { value++ }
    }

    fun <T> track(model: T): T {
        if (value < 0) return model
        return model
    }
}

@Composable
internal fun onSolid(color: Color): Color = if (Fm.dark) Fm.colors.bgBase else Color.White

@Composable
internal fun SfSolidButton(
    label: String,
    color: Color,
    onClick: (() -> Unit)?,
    modifier: Modifier = Modifier,
    icon: String? = null,
    minHeight: Dp = 44.dp,
) {
    val haptics = FmHaptics.current
    val enabled = onClick != null
    val ink = onSolid(color)
    Row(
        modifier
            .defaultMinSize(minHeight = minHeight)
            .alpha(if (enabled) 1f else 0.45f)
            .clip(RoundedCornerShape(50))
            .background(color)
            .clickable(enabled = enabled, role = Role.Button) {
                haptics.commit()
                onClick?.invoke()
            }
            .padding(horizontal = FmSpace.x5),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.Center,
    ) {
        if (icon != null) {
            FmIcon(icon, size = 18.dp, tint = ink)
            Spacer(Modifier.width(FmSpace.x2))
        }
        Text(label, color = ink, style = Fm.text.titleMedium.copy(color = ink, fontWeight = FontWeight.W600))
    }
}

@Composable
internal fun SfOutlinedButton(
    label: String,
    onClick: (() -> Unit)?,
    modifier: Modifier = Modifier,
    icon: String? = null,
    color: Color? = null,
) {
    val haptics = FmHaptics.current
    val enabled = onClick != null
    val ink = color ?: Fm.colors.textPrimary
    Row(
        modifier
            .defaultMinSize(minHeight = 44.dp)
            .alpha(if (enabled) 1f else 0.45f)
            .clip(RoundedCornerShape(50))
            .border(BorderStroke(1.dp, Fm.colors.border), RoundedCornerShape(50))
            .clickable(enabled = enabled, role = Role.Button) {
                haptics.tap()
                onClick?.invoke()
            }
            .padding(horizontal = FmSpace.x4),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.Center,
    ) {
        if (icon != null) {
            FmIcon(icon, size = 18.dp, tint = ink)
            Spacer(Modifier.width(FmSpace.x2))
        }
        Text(label, style = Fm.text.labelLarge.copy(color = ink))
    }
}

@Composable
internal fun SfHairline(modifier: Modifier = Modifier) {
    Box(modifier.fillMaxWidth().height(1.dp).background(Fm.colors.border.copy(alpha = 0.6f)))
}
