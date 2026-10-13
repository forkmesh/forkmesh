package com.forkmesh.nativeapp.feature.agents

import com.forkmesh.nativeapp.feature.composer.ComposerFeature
import kotlinx.coroutines.delay
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import com.forkmesh.nativeapp.agents.AgentQueueApi
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.feature.composer.PromptComposer

@Composable
internal fun AgentPromptComposer(
    text: String,
    onTextChange: (String) -> Unit,
    hint: String,
    onSend: (() -> Unit)?,
    modifier: Modifier = Modifier,
    busy: Boolean = false,
    running: Boolean = false,
    onStop: (() -> Unit)? = null,
    attachments: List<PromptAttachment> = emptyList(),
    onAttachmentsChanged: ((List<PromptAttachment>) -> Unit)? = null,
    modelLabel: String? = null,
    onPickModel: (() -> Unit)? = null,
    extraActions: @Composable () -> Unit = {},
    autofocus: Boolean = false,
    padForKeyboard: Boolean = true,
    draftSurface: String = "",
    offersFix: Boolean = true,
) {
    if (draftSurface.isNotEmpty()) {
        val latestChange by rememberUpdatedState(onTextChange)
        val latestText by rememberUpdatedState(text)
        LaunchedEffect(draftSurface) {
            if (latestText.isEmpty()) {
                val saved = ComposerFeature.drafts?.load(draftSurface).orEmpty()
                if (saved.isNotEmpty()) latestChange(saved)
            }
        }
        LaunchedEffect(draftSurface, text) {
            delay(300)
            ComposerFeature.drafts?.save(draftSurface, text)
        }
    }
    PromptComposer(
        text = text,
        onTextChange = onTextChange,
        onSend = onSend,
        modifier = modifier,
        hint = hint,
        running = running,
        busy = busy,
        onStop = onStop,
        modelLabel = modelLabel,
        onPickModel = onPickModel,
        attachments = attachments,
        onAttachmentsChanged = onAttachmentsChanged,
        extraControls = { extraActions() },
        autofocus = autofocus,
        maxLength = AgentQueueApi.MAX_AGENT_PROMPT_CHARS,
        uploadsFiles = true,
        padForKeyboard = padForKeyboard,
        outerPadding = padForKeyboard,
        offersFix = offersFix,
    )
}
