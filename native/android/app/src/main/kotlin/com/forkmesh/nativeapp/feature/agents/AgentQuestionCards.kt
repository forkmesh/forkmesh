package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.control.AgentQuestion
import com.forkmesh.nativeapp.control.AgentQuestionField
import com.forkmesh.nativeapp.feature.code.FmFilledButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

@Composable
internal fun AgentQuestionCards(questions: List<AgentQuestion>, busy: Boolean, submit: (AgentQuestion, String) -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
        for (question in questions) {
            key(question.id) { AgentQuestionCard(question, busy, submit) }
        }
    }
}

@Composable
private fun AgentQuestionCard(question: AgentQuestion, busy: Boolean, submit: (AgentQuestion, String) -> Unit) {
    val c = Fm.colors
    var selections by remember(question.id) { mutableStateOf<Map<String, Set<String>>>(emptyMap()) }
    var others by remember(question.id) { mutableStateOf<Map<String, String>>(emptyMap()) }
    fun pick(field: AgentQuestionField, label: String) {
        val values = selections[field.id].orEmpty()
        selections = selections + (field.id to if (field.multiSelect) (if (label in values) values - label else values + label) else setOf(label))
        if (!field.multiSelect) others = others + (field.id to "")
    }
    val answer = question.answer(selections, others)
    val shape = RoundedCornerShape(FmRadius.lg)
    Column(
        Modifier
            .fillMaxWidth()
            .background(c.accentSubtle, shape)
            .border(BorderStroke(1.dp, c.border), shape)
            .padding(12.dp)
            .testTag("agent-question-card"),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            FmIcon("help_outline", size = 16.dp, tint = c.accent)
            Text("Agent needs your input", style = Fm.text.labelMedium.copy(color = c.accent, fontWeight = FmType.emphasis))
        }
        for (field in question.fields) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                Text(field.title, style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis))
                for (option in field.options) {
                    val on = option.label in selections[field.id].orEmpty()
                    val optionShape = RoundedCornerShape(FmRadius.md)
                    Column(
                        Modifier
                            .fillMaxWidth()
                            .heightIn(min = 44.dp)
                            .background(if (on) c.accentSubtle else c.bgOverlay, optionShape)
                            .clickable { pick(field, option.label) }
                            .padding(FmSpace.x2),
                        verticalArrangement = Arrangement.spacedBy(2.dp),
                    ) {
                        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            FmIcon(if (on) "check_circle" else "radio_button_unchecked", size = 16.dp, tint = if (on) c.accent else c.textSecondary)
                            Text(option.label, style = Fm.text.bodyMedium.copy(color = if (on) c.accent else c.textPrimary))
                        }
                        if (option.detail.isNotEmpty()) {
                            Text(option.detail, style = Fm.text.bodySmall.copy(color = c.textSecondary), modifier = Modifier.padding(start = 24.dp))
                        }
                    }
                }
                FmTextField(
                    value = others[field.id].orEmpty(),
                    onValueChange = { value ->
                        others = others + (field.id to value)
                        if (!field.multiSelect && value.isNotBlank()) selections = selections + (field.id to emptySet())
                    },
                    modifier = Modifier.fillMaxWidth(),
                    hint = if (field.isSecret) "Response" else "Other",
                    keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false),
                    visualTransformation = if (field.isSecret) PasswordVisualTransformation() else VisualTransformation.None,
                )
            }
        }
        FmFilledButton(
            "Send answer",
            onClick = if (answer.isEmpty() || busy) null else ({ submit(question, answer) }),
            modifier = Modifier.testTag("agent-question-send"),
            busy = busy,
        )
    }
}
