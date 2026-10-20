package com.forkmesh.nativeapp.feature.genie

import android.speech.tts.TextToSpeech
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTag
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.composer.rememberSpeechDictation
import com.forkmesh.nativeapp.genie.GenieChat
import com.forkmesh.nativeapp.genie.GenieWords
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class GenieTalkLine(val text: String, val fromUser: Boolean)

object GenieTalkState {
    const val SHOWN_LINES = 3

    private val openState = MutableStateFlow(false)
    private val linesState = MutableStateFlow<List<GenieTalkLine>>(emptyList())
    val isOpen: StateFlow<Boolean> = openState.asStateFlow()
    val lines: StateFlow<List<GenieTalkLine>> = linesState.asStateFlow()
    private val chat = GenieChat()
    var speak: (String) -> Unit = {}

    fun open() {
        openState.value = true
        if (linesState.value.isEmpty()) add("Hi! Ask me anything, or say “say” and I'll repeat it.", false)
    }

    fun close() {
        openState.value = false
        GenieSpiderState.setMood(GenieMood.Idle)
    }

    private fun add(text: String, fromUser: Boolean) {
        linesState.value = (linesState.value + GenieTalkLine(text, fromUser)).takeLast(SHOWN_LINES)
    }

    fun heard(text: String) {
        val said = text.trim()
        if (said.isEmpty()) return
        add(said, true)
        if (GenieWords.stopsListening(said)) {
            reply(GenieChat.STOPPED_LISTENING)
            return
        }
        GenieSpiderState.setMood(GenieMood.Thinking)
        AppLifecycle.scope.launch {
            val writer = GenieFeature.cloudWriter(chat = true)
            val answer = withContext(Dispatchers.IO) { chat.answer(said, writer) }
            reply(answer)
        }
    }

    private fun reply(text: String) {
        add(text, false)
        GenieSpiderState.setMood(GenieMood.Speaking)
        speak(text)
        AppLifecycle.scope.launch {
            delay((1500 + text.length * 55L).coerceAtMost(12_000))
            if (GenieSpiderState.mood.value == GenieMood.Speaking) GenieSpiderState.setMood(GenieMood.Idle)
        }
    }
}

@Composable
fun GenieTalkBubble(modifier: Modifier = Modifier) {
    val open by GenieTalkState.isOpen.collectAsState()
    if (!open) return
    val lines by GenieTalkState.lines.collectAsState()
    val context = LocalContext.current
    val dictation = rememberSpeechDictation()
    var typed by remember { mutableStateOf("") }
    var latest by remember { mutableStateOf("") }
    DisposableEffect(context) {
        var ready = false
        val voice = TextToSpeech(context.applicationContext) { status -> ready = status == TextToSpeech.SUCCESS }
        voice.setPitch(0.92f)
        voice.language = Locale.getDefault()
        GenieTalkState.speak = { text -> if (ready) voice.speak(text, TextToSpeech.QUEUE_FLUSH, null, "genie") }
        onDispose {
            GenieTalkState.speak = {}
            voice.stop()
            voice.shutdown()
        }
    }
    Column(
        modifier
            .padding(horizontal = FmSpace.x3)
            .widthIn(max = 420.dp)
            .fillMaxWidth()
            .background(Brush.verticalGradient(listOf(GenieInk.night, GenieInk.dusk)), RoundedCornerShape(20.dp))
            .border(1.dp, GenieInk.glow.copy(alpha = 0.5f), RoundedCornerShape(20.dp))
            .padding(FmSpace.x3)
            .semantics { testTag = "genie-bubble" },
        verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
    ) {
        lines.forEach { line ->
            Text(
                line.text,
                color = if (line.fromUser) GenieInk.glow else Color.White,
                fontSize = 15.sp,
                modifier = Modifier.fillMaxWidth(),
            )
        }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            FmTextField(
                typed, { typed = it }, Modifier.weight(1f), hint = "Ask the Genie",
            )
            FmIconButton(if (typed.isBlank()) "mic" else "send", if (typed.isBlank()) "Speak" else "Send", {
                if (typed.isNotBlank()) {
                    GenieTalkState.heard(typed)
                    typed = ""
                } else if (dictation.listening) {
                    dictation.stop()
                } else {
                    GenieSpiderState.setMood(GenieMood.Listening)
                    dictation.start(
                        onWords = { words -> latest = words },
                        onError = { problem ->
                            GenieSpiderState.setMood(GenieMood.Idle)
                            GenieSpiderState.say(problem)
                        },
                        onQuiet = {
                            GenieTalkState.heard(latest)
                            latest = ""
                        },
                    )
                }
            }, color = GenieInk.glow)
            FmIconButton("close", "Close", { dictation.stop(); GenieTalkState.close() }, color = Color.White)
        }
    }
}
