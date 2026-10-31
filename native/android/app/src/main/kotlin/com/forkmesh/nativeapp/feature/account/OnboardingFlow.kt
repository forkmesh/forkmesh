package com.forkmesh.nativeapp.feature.account

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.wrapContentWidth
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.market.PackageGateFeature
import com.forkmesh.nativeapp.feature.market.StarterPackPage
import com.forkmesh.nativeapp.market.StarterPack
import com.forkmesh.nativeapp.ui.kit.FmForcedDark
import com.forkmesh.nativeapp.ui.kit.FmOnboarding
import com.forkmesh.nativeapp.ui.kit.FmOnboardingBackButton
import com.forkmesh.nativeapp.ui.kit.FmOnboardingCanvas
import com.forkmesh.nativeapp.ui.kit.FmOnboardingProgress
import com.forkmesh.nativeapp.ui.kit.FmPillTextButton
import com.forkmesh.nativeapp.ui.theme.FmMotion
import com.forkmesh.nativeapp.ui.theme.FmSpace

private enum class OnboardingStep { Permissions, StarterPack }

@Composable
fun OnboardingFlow(needsPermissions: Boolean, needsStarterPack: Boolean) {
    val steps = remember {
        buildList {
            if (needsPermissions) add(OnboardingStep.Permissions)
            if (needsStarterPack) add(OnboardingStep.StarterPack)
        }
    }
    var index by rememberSaveable { mutableIntStateOf(0) }
    var selectedId by rememberSaveable { mutableStateOf(StarterPack.FALLBACK.id) }
    val selected = StarterPack.fromId(selectedId)
    val finish: (StarterPack) -> Unit = { pack ->
        if (OnboardingStep.StarterPack in steps) PackageGateFeature.chooseStarterPack(pack)
        if (OnboardingStep.Permissions in steps) PermissionsIntro.finish()
    }
    val advance: () -> Unit = {
        if (index + 1 < steps.size) index += 1 else finish(selected)
    }
    BackHandler(enabled = index > 0) { index -= 1 }
    val exit = FmMotion.effective(FmOnboarding.EXIT_MS)
    FmForcedDark {
        Box(Modifier.fillMaxSize()) {
            FmOnboardingCanvas()
            Column(Modifier.fillMaxSize().safeDrawingPadding()) {
                Box(
                    Modifier.fillMaxWidth().heightIn(min = 44.dp).padding(horizontal = 20.dp).padding(top = FmSpace.x2),
                    contentAlignment = Alignment.CenterStart,
                ) {
                    val slot by animateDpAsState(
                        if (index > 0) 56.dp else 0.dp,
                        tween(FmMotion.effective(450), easing = FmMotion.easeOutCubic),
                        label = "onboarding-back",
                    )
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Box(Modifier.width(slot).clipToBounds()) {
                            if (index > 0) {
                                FmOnboardingBackButton(
                                    onClick = { index -= 1 },
                                    Modifier.wrapContentWidth(Alignment.Start, unbounded = true),
                                )
                            }
                        }
                        FmOnboardingProgress(steps.size.coerceAtLeast(1), index)
                    }
                    FmPillTextButton(
                        "Skip",
                        { finish(StarterPack.FALLBACK) },
                        Modifier.align(Alignment.CenterEnd).testTag("onboarding-skip"),
                    )
                }
                AnimatedContent(
                    targetState = index,
                    modifier = Modifier.weight(1f).fillMaxWidth(),
                    transitionSpec = {
                        fadeIn(tween(50, delayMillis = exit)) togetherWith fadeOut(tween(exit, easing = FmMotion.easeOutCubic))
                    },
                    label = "OnboardingFlow",
                ) { page ->
                    when (steps.getOrNull(page)) {
                        OnboardingStep.Permissions -> PermissionsIntroPage(onContinue = advance, onSkip = { finish(StarterPack.FALLBACK) })
                        OnboardingStep.StarterPack -> StarterPackPage(selected, { selectedId = it.id }, advance)
                        null -> Unit
                    }
                }
            }
        }
    }
}
