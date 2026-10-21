package com.forkmesh.nativeapp.feature.djstudio

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTag
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.djstudio.DjStudio
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.theme.FmHaptics

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun DjNowPlayingChip() {
    val model = DjStudioHub.shown
    val deck = model?.takeIf { !it.onScreen }?.audibleDeck
    val haptics = FmHaptics.current
    var menu by remember { mutableStateOf(false) }
    AnimatedVisibility(deck != null, enter = scaleIn() + fadeIn(), exit = scaleOut() + fadeOut()) {
        val shown = deck ?: return@AnimatedVisibility
        Box {
            Box(
                Modifier
                    .clip(CircleShape)
                    .combinedClickable(
                        onClick = {
                            haptics.selection()
                            NavigationFeature.open(Route.VideoMixDjStudio)
                        },
                        onLongClick = {
                            haptics.palette()
                            menu = true
                        },
                    )
                    .semantics {
                        role = Role.Button
                        contentDescription = (if (shown.title.isEmpty()) "Deck ${shown.side.label} is playing" else "Deck ${shown.side.label} is playing ${shown.title}") + ". Opens Video Mix DJ Studio"
                        testTag = "top-dj-now-playing"
                    },
            ) {
                DjMiniRecord(shown, 44.dp)
            }
            FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                FmDropdownMenuItem("Stop the Music", icon = "stop", onClick = {
                    menu = false
                    model?.stopTheMusic()
                })
            }
        }
    }
}

@Composable
fun DjMiniRecord(deck: DjDeck, size: Dp) {
    var turn by remember { mutableDoubleStateOf(deck.position()) }
    LaunchedEffect(deck, deck.playing) {
        var last = 0L
        while (deck.playing) {
            withFrameNanos { now ->
                if (now - last >= 33_000_000L) {
                    last = now
                    turn = deck.position()
                }
            }
        }
    }
    Box(
        Modifier
            .size(size)
            .clip(CircleShape)
            .background(Color(0xFF111214))
            .graphicsLayer { rotationZ = DjStudio.turnDegrees(turn).toFloat() },
    ) {
        deck.artwork?.let { Image(it, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop) }
    }
}
