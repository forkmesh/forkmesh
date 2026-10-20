package com.forkmesh.nativeapp.feature.health

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTag
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.market.PackageGateFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.ui.kit.fmGlass

@Composable
fun StepsWalkChip() {
    val surfaces by PackageGateFeature.surfaces.collectAsState()
    val shown = WalkActivity.stepping && surfaces.shows("steps")
    AnimatedVisibility(shown, enter = scaleIn() + fadeIn(), exit = scaleOut() + fadeOut()) {
        Box(
            Modifier
                .fmGlass(CircleShape, interactive = true)
                .clickable { NavigationFeature.open(Route.Steps) }
                .semantics {
                    role = Role.Button
                    contentDescription = "${WalkActivity.gaitText}, ${WalkActivity.stepsPerMinute} steps a minute. Opens the Pedometer"
                    testTag = "top-walking"
                }
                .padding(5.dp),
        ) {
            StepsWalkBadge(WalkActivity.cadence, WalkActivity.running, 34.dp)
        }
    }
}
