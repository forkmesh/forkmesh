package com.forkmesh.nativeapp.feature.account

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.account.PermissionPacing
import com.forkmesh.nativeapp.account.PermissionReason
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.ui.kit.FmOnboarding
import com.forkmesh.nativeapp.ui.kit.FmPillButton
import com.forkmesh.nativeapp.ui.kit.FmPillTextButton
import com.forkmesh.nativeapp.ui.kit.FmRevealLine
import com.forkmesh.nativeapp.ui.kit.FmRevealTitle
import com.forkmesh.nativeapp.ui.kit.fmBrighten
import com.forkmesh.nativeapp.ui.kit.fmReveal
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

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
fun PermissionsIntroPage(onContinue: () -> Unit, onSkip: () -> Unit) {
    val enter = FmOnboarding.ENTER_MS
    Column(Modifier.fillMaxSize(), horizontalAlignment = Alignment.CenterHorizontally) {
        Column(
            Modifier.weight(1f).widthIn(max = 520.dp).fillMaxWidth().verticalScroll(rememberScrollState())
                .padding(horizontal = 28.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            PermissionsHeroCard(
                Modifier.padding(top = FmSpace.x3).fmReveal(enter, rise = 26.dp, scale = 0.96f),
            )
            FmRevealTitle(PermissionPacing.titleLines, Modifier.padding(top = FmSpace.x5), delayMs = enter + 120)
            FmRevealLine(
                PermissionPacing.FOOTNOTE,
                Modifier.padding(top = FmSpace.x3),
                style = Fm.text.bodySmall,
                color = FmOnboarding.secondaryText,
                textAlign = TextAlign.Center,
                delayMs = enter + 420,
                durationMs = 600,
            )
        }
        Column(
            Modifier.widthIn(max = 520.dp).fillMaxWidth().padding(horizontal = 28.dp).padding(top = FmSpace.x3),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
        ) {
            FmPillButton(
                "Let's Get Started",
                onContinue,
                Modifier.testTag("permissions-intro-continue").fmBrighten(enter + 100),
            )
            FmPillTextButton(
                "skip and start",
                onSkip,
                Modifier.testTag("permissions-intro-skip").fmReveal(enter + 600, rise = 0.dp, scale = 1f),
            )
        }
    }
}

private val heroBand = Brush.linearGradient(
    listOf(Color(0xFF5B6CFF), Color(0xFF8E8BFF), Color(0xFFC39BF2), Color(0xFF7FE3D8)),
)

@Composable
private fun PermissionsHeroCard(modifier: Modifier = Modifier) {
    val enter = FmOnboarding.ENTER_MS
    val still = Fm.reducedMotion
    var asked by remember { mutableIntStateOf(if (still) 0 else PermissionPacing.reasons.size) }
    LaunchedEffect(still) {
        if (still) return@LaunchedEffect
        delay((enter + 500).toLong())
        while (asked > 0) {
            asked -= 1
            delay(170)
        }
    }
    val transition = rememberInfiniteTransition(label = "permissions-card")
    val wave by transition.animateFloat(-2.4f, 2.4f, infiniteRepeatable(tween(3400), RepeatMode.Reverse), label = "permissions-tilt")
    val shape = RoundedCornerShape(27.dp)
    Column(
        modifier
            .widthIn(max = 316.dp)
            .fillMaxWidth()
            .graphicsLayer {
                rotationY = if (still) 0f else wave
                cameraDistance = 16f * density
            }
            .background(FmOnboarding.surface, shape)
            .border(1.dp, FmOnboarding.hairline, shape)
            .padding(5.dp),
    ) {
        HeroTop()
        Column(
            Modifier.fillMaxWidth().background(heroBand, RoundedCornerShape(22.dp)).padding(5.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            HeroStrip()
            HeroCounter(asked)
            HeroList()
        }
    }
}

@Composable
private fun HeroTop() {
    Row(
        Modifier.fillMaxWidth().clearAndSetSemantics {}.padding(start = 14.dp, top = 12.dp, end = 12.dp, bottom = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
    ) {
        FmIcon("verified_user", size = 15.dp, tint = Color.White)
        FmRevealLine(
            "Asked one at a time",
            style = Fm.text.labelLarge,
            delayMs = FmOnboarding.ENTER_MS + 160,
            durationMs = 500,
        )
        Spacer(Modifier.weight(1f))
        Box(
            Modifier.size(28.dp).background(Color.White.copy(alpha = 0.08f), CircleShape).border(1.dp, FmOnboarding.hairline, CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            FmIcon("lock", size = 13.dp, tint = Color.White)
        }
    }
}

@Composable
private fun HeroStrip() {
    Row(
        Modifier.fillMaxWidth().clearAndSetSemantics {}.padding(start = 10.dp, top = 6.dp, end = 6.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            "Only when needed",
            maxLines = 1,
            style = Fm.text.labelMedium.copy(color = Color.White, fontWeight = FontWeight.Medium),
        )
        Spacer(Modifier.weight(1f))
        Row(horizontalArrangement = Arrangement.spacedBy((-7).dp)) {
            PermissionPacing.reasons.forEachIndexed { index, item ->
                Box(
                    Modifier
                        .fmReveal(FmOnboarding.ENTER_MS + 260 + index * 100, rise = 0.dp, scale = 0.4f)
                        .size(26.dp)
                        .background(Color(0xFF17171A), CircleShape)
                        .border(1.5.dp, Color.White, CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    FmIcon(item.icon, size = 12.dp, tint = Color.White)
                }
            }
        }
    }
}

@Composable
private fun HeroCounter(asked: Int) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 66.dp)
            .background(Color.Black, RoundedCornerShape(18.dp))
            .clearAndSetSemantics { contentDescription = PermissionPacing.SUMMARY }
            .padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        Box(
            Modifier.size(40.dp).background(Color.White.copy(alpha = 0.1f), RoundedCornerShape(12.dp)),
            contentAlignment = Alignment.Center,
        ) {
            FmIcon("back_hand", size = 18.dp, tint = Color.White)
        }
        Column {
            Text("$asked", style = Fm.text.titleLarge.copy(color = Color.White, fontWeight = FontWeight.SemiBold))
            Text("Asked up front", style = Fm.text.labelSmall.copy(color = FmOnboarding.secondaryText))
        }
    }
}

@Composable
private fun HeroList() {
    Column(
        Modifier.fillMaxWidth().background(Color.Black, RoundedCornerShape(18.dp)).padding(horizontal = 12.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        PermissionPacing.reasons.forEachIndexed { index, item -> HeroRow(item, index) }
    }
}

@Composable
private fun HeroRow(item: PermissionReason, index: Int) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 40.dp)
            .fmReveal(FmOnboarding.ENTER_MS + 340 + index * 70, rise = 8.dp, scale = 1f)
            .clearAndSetSemantics { contentDescription = "${item.title}. ${item.reason}" }
            .testTag("permissions-intro-${item.id}"),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        Box(
            Modifier.size(28.dp).background(Color.White.copy(alpha = 0.08f), RoundedCornerShape(9.dp)),
            contentAlignment = Alignment.Center,
        ) {
            FmIcon(item.icon, size = 14.dp, tint = Color.White)
        }
        Column {
            Text(
                item.title,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.labelLarge.copy(color = Color.White, fontWeight = FontWeight.Medium),
            )
            Text(
                item.hint,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.labelSmall.copy(color = FmOnboarding.secondaryText),
            )
        }
    }
}
