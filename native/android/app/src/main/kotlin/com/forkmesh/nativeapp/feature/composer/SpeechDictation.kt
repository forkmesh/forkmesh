package com.forkmesh.nativeapp.feature.composer

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.speech.RecognitionListener
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.ContextCompat

class SpeechDictation internal constructor(private val context: Context) {
    var listening by mutableStateOf(false)
        private set

    private var recognizer: SpeechRecognizer? = null
    private var onWords: (String) -> Unit = {}
    private var onQuiet: () -> Unit = {}
    private var onError: (String) -> Unit = {}
    internal var askPermission: (() -> Unit)? = null
    private var waitingForPermission = false

    val available: Boolean get() = SpeechRecognizer.isRecognitionAvailable(context)

    fun start(onWords: (String) -> Unit, onError: (String) -> Unit, onQuiet: () -> Unit = {}) {
        this.onWords = onWords
        this.onQuiet = onQuiet
        this.onError = onError
        if (!available) {
            onError("Dictation is not available on this device.")
            return
        }
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            waitingForPermission = true
            askPermission?.invoke() ?: onError("Allow the microphone for ForkMesh in Settings to dictate.")
            return
        }
        listen()
    }

    internal fun permissionResult(granted: Boolean) {
        if (!waitingForPermission) return
        waitingForPermission = false
        if (granted) listen() else onError("Dictation needs the microphone. Allow it for ForkMesh in Settings.")
    }

    private fun listen() {
        val speech = recognizer ?: SpeechRecognizer.createSpeechRecognizer(context).also { recognizer = it }
        speech.setRecognitionListener(object : RecognitionListener {
            override fun onReadyForSpeech(params: Bundle?) {}
            override fun onBeginningOfSpeech() {}
            override fun onRmsChanged(rmsdB: Float) {}
            override fun onBufferReceived(buffer: ByteArray?) {}
            override fun onEndOfSpeech() {}
            override fun onEvent(eventType: Int, params: Bundle?) {}

            override fun onPartialResults(partialResults: Bundle?) {
                words(partialResults)?.let(onWords)
            }

            override fun onResults(results: Bundle?) {
                val heard = words(results)
                heard?.let(onWords)
                val quiet = listening && !heard.isNullOrBlank()
                listening = false
                if (quiet) onQuiet()
            }

            override fun onError(error: Int) {
                listening = false
                when (error) {
                    SpeechRecognizer.ERROR_NO_MATCH, SpeechRecognizer.ERROR_SPEECH_TIMEOUT, SpeechRecognizer.ERROR_CLIENT -> {}
                    SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS ->
                        onError("Dictation needs the microphone. Allow it for ForkMesh in Settings.")
                    SpeechRecognizer.ERROR_NETWORK, SpeechRecognizer.ERROR_NETWORK_TIMEOUT ->
                        onError("Dictation stopped: the speech service could not be reached.")
                    else -> onError("Dictation stopped.")
                }
            }
        })
        val intent = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH)
            .putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
            .putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true)
            .putExtra(RecognizerIntent.EXTRA_CALLING_PACKAGE, context.packageName)
            .putExtra(RecognizerIntent.EXTRA_SPEECH_INPUT_COMPLETE_SILENCE_LENGTH_MILLIS, 4000L)
            .apply {
                val chosen = com.forkmesh.nativeapp.core.Services.get<com.forkmesh.nativeapp.api.PrefStore>()
                    ?.let(com.forkmesh.nativeapp.settings.DeviceSettings::dictationLocale).orEmpty()
                if (chosen.isNotEmpty()) putExtra(RecognizerIntent.EXTRA_LANGUAGE, chosen)
            }
        listening = true
        try {
            speech.startListening(intent)
        } catch (error: Exception) {
            listening = false
            onError("Dictation could not start: ${error.message ?: error}")
        }
    }

    private fun words(bundle: Bundle?): String? =
        bundle?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)?.firstOrNull()

    fun stop() {
        if (!listening) return
        listening = false
        recognizer?.stopListening()
    }

    internal fun release() {
        listening = false
        recognizer?.destroy()
        recognizer = null
    }
}

@Composable
fun rememberSpeechDictation(): SpeechDictation {
    val context = LocalContext.current
    val dictation = remember(context) { SpeechDictation(context) }
    val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        dictation.permissionResult(granted)
    }
    dictation.askPermission = { permission.launch(Manifest.permission.RECORD_AUDIO) }
    DisposableEffect(dictation) { onDispose { dictation.release() } }
    return dictation
}
