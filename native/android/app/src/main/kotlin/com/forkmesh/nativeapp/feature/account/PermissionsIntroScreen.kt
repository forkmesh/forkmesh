package com.forkmesh.nativeapp.feature.account

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.account.PermissionPacing
import com.forkmesh.nativeapp.account.PermissionReason
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.ui.kit.FmElevatedButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlin.math.cos
import kotlin.math.sin

object PermissionsIntro {
    private val pendingState = MutableStateFlow(false)

    val pending: StateFlow<Boolean> = pendingState.asStateFlow()

    fun install(freshInstall: Boolean) {
        val prefs = Services.get<PrefStore>() ?: return
        PermissionPacing.settle(prefs, freshInstall)
        pendingState.value = PermissionPacing.needsIntro(prefs)
    }

    fun finish() {
        Services.get<PrefStore>()?.let(PermissionPacing::finishIntro)
        pendingState.value = false
    }
}

@Composable
fun PermissionsIntroScreen(onContinue: () -> Unit = PermissionsIntro::finish) {
    val c = Fm.colors
    Column(
        Modifier.fillMaxSize().background(c.bgBase).safeDrawingPadding(),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Column(
            Modifier.weight(1f).widthIn(max = 520.dp).fillMaxWidth().verticalScroll(rememberScrollState())
                .padding(horizontal = FmSpace.x5).padding(top = FmSpace.x6),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x5),
        ) {
            PermissionsIllustration(Modifier.fillMaxWidth().padding(top = FmSpace.x2))
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                Text(PermissionPacing.TITLE, style = Fm.text.displaySmall, color = c.textPrimary, modifier = Modifier.semantics { heading() })
                Text(PermissionPacing.SUMMARY, style = Fm.text.bodyMedium, color = c.textSecondary)
            }
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x4)) {
                PermissionPacing.reasons.forEach { PermissionRow(it) }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.Top) {
                FmIcon("lock", size = 14.dp, tint = c.textTertiary)
                Text(PermissionPacing.FOOTNOTE, style = Fm.text.bodySmall, color = c.textTertiary)
            }
        }
        FmElevatedButton(
            onClick = onContinue,
            modifier = Modifier.widthIn(max = 520.dp).fillMaxWidth().padding(horizontal = FmSpace.x5, vertical = FmSpace.x4)
                .heightIn(min = 52.dp).testTag("permissions-intro-continue"),
        ) {
            Text("Sounds good", style = Fm.text.labelLarge.copy(color = Color.Unspecified))
        }
    }
}

@Composable
private fun PermissionRow(item: PermissionReason) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().semantics(mergeDescendants = true) {}.testTag("permissions-intro-${item.id}"),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
        verticalAlignment = Alignment.Top,
    ) {
        Box(
            Modifier.size(34.dp).clip(RoundedCornerShape(FmRadius.lg)).background(c.accentSubtle).clearAndSetSemantics {},
            contentAlignment = Alignment.Center,
        ) {
            FmIcon(item.icon, size = 17.dp, tint = c.accent)
        }
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(item.title, style = Fm.text.titleSmall, color = c.textPrimary)
            Text(item.reason, style = Fm.text.bodySmall, color = c.textSecondary)
        }
    }
}

private val badges = listOf("notifications" to -150.0, "sensors" to -30.0, "videocam" to 30.0, "photo" to 150.0)

@Composable
private fun PermissionsIllustration(modifier: Modifier = Modifier) {
    val c = Fm.colors
    val still = Fm.reducedMotion
    val transition = rememberInfiniteTransition(label = "permissions-float")
    val wave by transition.animateFloat(0f, 1f, infiniteRepeatable(tween(2400), RepeatMode.Reverse), label = "permissions-wave")
    val floating = if (still) 0f else wave
    Box(modifier.height(170.dp).clearAndSetSemantics {}, contentAlignment = Alignment.Center) {
        Box(Modifier.size(150.dp).background(c.accentSubtle, CircleShape))
        val ring = c.accent.copy(alpha = 0.18f)
        Canvas(Modifier.size(196.dp, 128.dp)) {
            drawOval(ring, size = Size(size.width, size.height), style = Stroke(width = 1.dp.toPx(), pathEffect = PathEffect.dashPathEffect(floatArrayOf(4.dp.toPx(), 5.dp.toPx()))))
        }
        FmIcon(
            "verified_user",
            size = 64.dp,
            tint = c.accent,
            modifier = Modifier.graphicsLayer {
                val k = 1f + 0.03f * floating
                scaleX = k
                scaleY = k
            },
        )
        badges.forEachIndexed { index, (icon, angle) ->
            val radians = Math.toRadians(angle)
            val lift = (if (index % 2 == 0) -4f else 4f) * floating
            val shadow = Color.Black.copy(alpha = if (c.dark) 0.3f else 0.08f)
            Box(
                Modifier
                    .offset(x = (cos(radians) * 82).toFloat().dp, y = (sin(radians) * 52).toFloat().dp + lift.dp)
                    .shadow(6.dp, CircleShape, ambientColor = shadow, spotColor = shadow)
                    .size(36.dp)
                    .background(c.bgRaised, CircleShape)
                    .border(1.dp, c.border, CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                FmIcon(icon, size = 16.dp, tint = c.accent)
            }
        }
    }
}
