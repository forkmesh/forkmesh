package com.forkmesh.nativeapp.feature.market

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.wrapContentWidth
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.market.StarterPack
import com.forkmesh.nativeapp.ui.kit.FmAgentOrb
import com.forkmesh.nativeapp.ui.kit.FmOnboarding
import com.forkmesh.nativeapp.ui.kit.FmPillButton
import com.forkmesh.nativeapp.ui.kit.FmRevealLine
import com.forkmesh.nativeapp.ui.kit.fmBrighten
import com.forkmesh.nativeapp.ui.kit.fmReveal
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmMotion
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlin.math.roundToInt

private val StarterPack.icon: String
    get() = when (this) {
        StarterPack.MINIMAL -> "code"
        StarterPack.STANDARD -> "auto_awesome"
        StarterPack.EVERYTHING -> "apps"
    }

private val StarterPack.orb: List<Color>
    get() = when (this) {
        StarterPack.MINIMAL -> listOf(Color(0xFF27C76F), Color(0xFF0B6B46), Color(0xFF1456E0), Color(0xFF0A2BC2))
        StarterPack.STANDARD -> listOf(Color(0xFF2F6BFF), Color(0xFF16C8A8), Color(0xFF0C2CD6), Color(0xFF6B3DF2))
        StarterPack.EVERYTHING -> listOf(Color(0xFFFF5A2A), Color(0xFFD8146A), Color(0xFF7A1FE0), Color(0xFFFFA42E))
    }

private val cardWidth = 214.dp
private val cardHeight = 238.dp
private val cardGap = 16.dp

@Composable
fun StarterPackPage(selected: StarterPack, onSelect: (StarterPack) -> Unit, onContinue: () -> Unit) {
    val enter = FmOnboarding.ENTER_MS
    val packs = StarterPack.values().toList()
    Column(Modifier.fillMaxSize(), horizontalAlignment = Alignment.CenterHorizontally) {
        Column(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 28.dp)
                .padding(top = FmSpace.x3)
                .semantics(mergeDescendants = true) {
                    heading()
                    contentDescription = "Choose how to start"
                },
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            FmRevealLine("Choose how", delayMs = enter)
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(7.dp)) {
                FmRevealLine("to start", delayMs = enter + 160)
                FmAgentOrb(26.dp, StarterPack.STANDARD.orb, Modifier.fmReveal(enter + 340, rise = 6.dp, scale = 0.3f))
            }
        }
        Row(
            Modifier.fillMaxWidth().selectableGroup().padding(horizontal = 28.dp).padding(top = FmSpace.x4),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            packs.forEachIndexed { index, pack -> PackChip(pack, pack == selected, index) { onSelect(pack) } }
        }
        Spacer(Modifier.weight(1f))
        PackCarousel(packs, selected, onSelect, Modifier.fmReveal(enter + 40, rise = 24.dp, scale = 0.95f))
        Text(
            selected.summary,
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = 44.dp)
                .padding(horizontal = 28.dp)
                .padding(top = FmSpace.x4)
                .fmReveal(enter + 200, rise = 6.dp, scale = 1f),
            textAlign = TextAlign.Center,
            style = Fm.text.bodyMedium.copy(color = Color.White),
        )
        PackDots(packs, selected, Modifier.padding(top = FmSpace.x2))
        Spacer(Modifier.weight(1f))
        FmRevealLine(
            "Add or remove any package later in the Market.",
            Modifier.padding(horizontal = 28.dp),
            style = Fm.text.bodySmall,
            color = FmOnboarding.secondaryText,
            textAlign = TextAlign.Center,
            delayMs = enter + 500,
            durationMs = 600,
        )
        FmPillButton(
            "Start with ${selected.label}",
            onContinue,
            Modifier
                .widthIn(max = 520.dp)
                .padding(horizontal = 28.dp, vertical = FmSpace.x3)
                .testTag("starter-pack-continue")
                .fmBrighten(enter + 100),
        )
    }
}

@Composable
private fun PackChip(pack: StarterPack, selected: Boolean, index: Int, onSelect: () -> Unit) {
    val shape = CircleShape
    val tint = if (selected) Color.White else FmOnboarding.secondaryText
    Row(
        Modifier
            .fmReveal(FmOnboarding.ENTER_MS + 300 + index * 90, rise = 0.dp, scale = 0.94f)
            .height(36.dp)
            .background(if (selected) FmOnboarding.raised else FmOnboarding.surface, shape)
            .border(1.dp, FmOnboarding.hairline, shape)
            .selectable(selected = selected, role = Role.RadioButton, onClick = onSelect)
            .padding(horizontal = 13.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        FmIcon(pack.icon, size = 14.dp, tint = tint)
        Text(pack.label, maxLines = 1, style = Fm.text.labelMedium.copy(color = tint))
    }
}

@Composable
private fun PackCard(pack: StarterPack, selected: Boolean, onSelect: () -> Unit) {
    val duration = FmMotion.effective(320)
    val focus by animateFloatAsState(if (selected) 1f else 0f, tween(duration, easing = FmMotion.easeOutCubic), label = "pack-focus")
    val outer = RoundedCornerShape(30.dp)
    val inner = RoundedCornerShape(24.dp)
    Column(
        Modifier
            .width(cardWidth)
            .graphicsLayer {
                val k = 0.88f + 0.12f * focus
                scaleX = k
                scaleY = k
                alpha = 0.42f + 0.58f * focus
            }
            .background(FmOnboarding.surface, outer)
            .clickable(role = Role.Button, onClickLabel = pack.label, onClick = onSelect)
            .semantics(mergeDescendants = true) { contentDescription = "${pack.label}. ${pack.summary}" }
            .testTag("starter-pack-${pack.id}")
            .padding(start = 8.dp, top = 8.dp, end = 8.dp, bottom = 12.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        Box(
            Modifier
                .fillMaxWidth()
                .height(172.dp)
                .background(Color.Black.copy(alpha = 0.55f), inner)
                .border(1.dp, FmOnboarding.hairline, inner),
            contentAlignment = Alignment.Center,
        ) {
            FmAgentOrb(112.dp, pack.orb)
        }
        Row(
            Modifier
                .height(30.dp)
                .background(Color.White.copy(alpha = 0.05f), CircleShape)
                .border(1.dp, FmOnboarding.hairline, CircleShape)
                .padding(horizontal = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            FmIcon(pack.icon, size = 14.dp, tint = FmOnboarding.secondaryText)
            Text(pack.label, style = Fm.text.labelMedium.copy(color = FmOnboarding.secondaryText))
        }
    }
}

@Composable
private fun PackCarousel(packs: List<StarterPack>, selected: StarterPack, onSelect: (StarterPack) -> Unit, modifier: Modifier = Modifier) {
    val index = packs.indexOf(selected)
    val duration = FmMotion.effective(320)
    val shift by animateFloatAsState((packs.size - 1) / 2f - index, tween(duration, easing = FmMotion.easeOutCubic), label = "pack-shift")
    var drag by remember { mutableFloatStateOf(0f) }
    val latestIndex by androidx.compose.runtime.rememberUpdatedState(index)
    BoxWithConstraints(
        modifier
            .fillMaxWidth()
            .height(cardHeight)
            .clipToBounds()
            .pointerInput(packs) {
                var travel = 0f
                detectHorizontalDragGestures(
                    onDragStart = { travel = 0f },
                    onDragEnd = {
                        val threshold = 40.dp.toPx()
                        val step = if (travel < -threshold) 1 else if (travel > threshold) -1 else 0
                        onSelect(packs[(latestIndex + step).coerceIn(0, packs.size - 1)])
                        drag = 0f
                    },
                    onDragCancel = { drag = 0f },
                ) { _, amount ->
                    travel += amount
                    drag += amount * 0.6f
                }
            },
    ) {
        Row(
            Modifier
                .wrapContentWidth(Alignment.CenterHorizontally, unbounded = true)
                .offset {
                    val spacing = (cardWidth + cardGap).toPx()
                    IntOffset((shift * spacing + drag).roundToInt(), 0)
                }
                .align(Alignment.Center),
            horizontalArrangement = Arrangement.spacedBy(cardGap),
        ) {
            packs.forEach { pack -> PackCard(pack, pack == selected) { onSelect(pack) } }
        }
    }
}

@Composable
private fun PackDots(packs: List<StarterPack>, selected: StarterPack, modifier: Modifier = Modifier) {
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        packs.forEach { pack ->
            val active = pack == selected
            Box(
                Modifier
                    .size(if (active) 14.dp else 5.dp, 4.dp)
                    .background(Color.White.copy(alpha = if (active) 1f else 0.28f), CircleShape),
            )
        }
    }
}
