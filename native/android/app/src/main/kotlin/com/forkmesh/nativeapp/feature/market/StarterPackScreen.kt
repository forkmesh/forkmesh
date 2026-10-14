package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.RadioButton
import androidx.compose.material3.RadioButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.market.StarterPack
import com.forkmesh.nativeapp.ui.kit.FmElevatedButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace

@Composable
fun StarterPackScreen(onContinue: (StarterPack) -> Unit) {
    var selectedId by rememberSaveable { mutableStateOf(StarterPack.FALLBACK.id) }
    val selected = StarterPack.fromId(selectedId)
    val colors = Fm.colors
    Column(
        Modifier.fillMaxSize().background(colors.bgBase).safeDrawingPadding(),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Column(
            Modifier.weight(1f).widthIn(max = 520.dp).fillMaxWidth().verticalScroll(rememberScrollState())
                .padding(horizontal = FmSpace.x5).padding(top = FmSpace.x7),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x5),
        ) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                Text("Choose how to start", style = Fm.text.headlineMedium, color = colors.textPrimary)
                Text(
                    "Pick what this project starts with. You can add or remove any package later in the Market.",
                    style = Fm.text.bodyMedium,
                    color = colors.textSecondary,
                )
            }
            Column(Modifier.selectableGroup(), verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                StarterPack.values().forEach { pack ->
                    StarterPackOption(pack, pack == selected) { selectedId = pack.id }
                }
            }
        }
        FmElevatedButton(
            onClick = { onContinue(selected) },
            modifier = Modifier.widthIn(max = 520.dp).fillMaxWidth().padding(horizontal = FmSpace.x5, vertical = FmSpace.x4)
                .heightIn(min = 52.dp).testTag("starter-pack-continue"),
        ) {
            Text("Continue", style = Fm.text.labelLarge)
        }
    }
}

@Composable
private fun StarterPackOption(pack: StarterPack, selected: Boolean, onSelect: () -> Unit) {
    val colors = Fm.colors
    val shape = RoundedCornerShape(FmRadius.lg)
    Row(
        Modifier.fillMaxWidth().clip(shape)
            .background(if (selected) colors.accentSubtle else colors.bgRaised)
            .border(BorderStroke(if (selected) 1.5.dp else 1.dp, if (selected) colors.accent else colors.border), shape)
            .selectable(selected = selected, role = Role.RadioButton, onClick = onSelect)
            .padding(FmSpace.x4)
            .testTag("starter-pack-${pack.id}"),
        verticalAlignment = Alignment.Top,
    ) {
        RadioButton(
            selected = selected,
            onClick = null,
            colors = RadioButtonDefaults.colors(selectedColor = colors.accent, unselectedColor = colors.textTertiary),
        )
        Spacer(Modifier.padding(start = FmSpace.x3))
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            Text(pack.label, style = Fm.text.titleSmall, color = colors.textPrimary)
            Text(pack.summary, style = Fm.text.bodySmall, color = colors.textSecondary)
        }
    }
}
