package com.forkmesh.nativeapp.feature.chat

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.apps.AppBlock
import com.forkmesh.nativeapp.apps.AppElement
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun AppBlocksView(message: ChatMessage) {
    val colors = Fm.colors
    val scope = rememberCoroutineScope()
    var busy by remember(message.id) { mutableStateOf("") }
    val act: (String, String) -> Unit = { actionId, value ->
        if (busy.isEmpty() && message.appMessageId.isNotEmpty()) {
            busy = actionId
            scope.launch {
                try {
                    withContext(Dispatchers.IO) { ChatDepth.hub?.appAction(message, actionId, value) }
                } finally {
                    busy = ""
                }
            }
        }
    }
    Column(Modifier.fillMaxWidth().padding(top = FmSpace.x1)) {
        for (block in message.blocks) {
            when (block) {
                is AppBlock.Section -> {
                    if (block.text.isNotEmpty()) {
                        SelectionContainer {
                            Text(block.text, modifier = Modifier.padding(vertical = 2.dp), style = Fm.text.bodyMedium.copy(color = colors.textPrimary))
                        }
                    }
                    if (block.fields.isNotEmpty()) {
                        FlowRow(
                            Modifier.fillMaxWidth().padding(vertical = 2.dp),
                            horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
                            verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
                        ) {
                            for (field in block.fields) {
                                Text(field, style = Fm.text.bodySmall.copy(color = colors.textSecondary))
                            }
                        }
                    }
                }
                is AppBlock.Context -> Text(
                    block.text,
                    modifier = Modifier.padding(vertical = 2.dp),
                    style = Fm.text.bodySmall.copy(color = colors.textTertiary),
                )
                AppBlock.Divider -> Box(
                    Modifier
                        .padding(vertical = FmSpace.x1)
                        .fillMaxWidth()
                        .height(1.dp)
                        .background(colors.border),
                )
                is AppBlock.Actions -> FlowRow(
                    Modifier.fillMaxWidth().padding(vertical = FmSpace.x1),
                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                    verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
                ) {
                    for (element in block.elements) {
                        val enabled = busy.isEmpty() && message.appMessageId.isNotEmpty()
                        when (element) {
                            is AppElement.Button -> AppButton(element, enabled) { act(element.actionId, element.value) }
                            is AppElement.Select -> AppSelect(element, enabled) { value -> act(element.actionId, value) }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun AppButton(element: AppElement.Button, enabled: Boolean, onClick: () -> Unit) {
    val colors = Fm.colors
    val fill = when (element.style) {
        "primary" -> colors.accent
        "danger" -> colors.danger
        else -> null
    }
    if (fill != null) {
        Button(
            onClick = onClick,
            enabled = enabled,
            colors = ButtonDefaults.buttonColors(containerColor = fill, contentColor = Color.White),
            contentPadding = ButtonDefaults.ContentPadding,
        ) {
            Text(element.text, maxLines = 1)
        }
    } else {
        OutlinedButton(
            onClick = onClick,
            enabled = enabled,
            border = BorderStroke(1.dp, colors.border),
            colors = ButtonDefaults.outlinedButtonColors(contentColor = colors.textPrimary),
        ) {
            Text(element.text, maxLines = 1)
        }
    }
}

@Composable
private fun AppSelect(element: AppElement.Select, enabled: Boolean, onPick: (String) -> Unit) {
    val colors = Fm.colors
    var open by remember { mutableStateOf(false) }
    Box {
        OutlinedButton(
            onClick = { open = true },
            enabled = enabled,
            border = BorderStroke(1.dp, colors.border),
            colors = ButtonDefaults.outlinedButtonColors(contentColor = colors.textPrimary),
        ) {
            Text(element.placeholder, maxLines = 1)
            Spacer(Modifier.width(FmSpace.x1))
            FmIcon("arrow_drop_down", size = 18.dp)
        }
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (option in element.options) {
                FmDropdownMenuItem(option.text, onClick = {
                    open = false
                    onPick(option.value)
                })
            }
        }
    }
}
