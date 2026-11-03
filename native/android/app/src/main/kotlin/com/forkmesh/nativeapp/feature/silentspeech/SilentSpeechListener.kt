package com.forkmesh.nativeapp.feature.silentspeech

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.speech.tts.TextToSpeech
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.Prefs
import com.forkmesh.nativeapp.silentspeech.SilentChannel
import com.forkmesh.nativeapp.silentspeech.SilentDataset
import com.forkmesh.nativeapp.silentspeech.SilentEvaluation
import com.forkmesh.nativeapp.silentspeech.SilentEvaluator
import com.forkmesh.nativeapp.silentspeech.SilentMatch
import com.forkmesh.nativeapp.silentspeech.SilentModel
import com.forkmesh.nativeapp.silentspeech.SilentNoiseFloor
import com.forkmesh.nativeapp.silentspeech.SilentQuaternion
import com.forkmesh.nativeapp.silentspeech.SilentSample
import com.forkmesh.nativeapp.silentspeech.SilentSegmenter
import com.forkmesh.nativeapp.silentspeech.SilentSpeechPrefs
import com.forkmesh.nativeapp.silentspeech.SilentTake
import com.forkmesh.nativeapp.silentspeech.SilentTranscript
import com.forkmesh.nativeapp.silentspeech.SilentVector
import java.io.File
import java.util.Locale
import java.util.UUID
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

sealed class SilentHold {
    data class Train(val word: String) : SilentHold()
    data object Decode : SilentHold()
}

object SilentSpeechListener : SensorEventListener {
    private const val CALIBRATION_SECONDS = 2.0
    private const val HOLD_PRE_ROLL = 0.1
    private const val RECENT_SECONDS = 0.4

    var listening by mutableStateOf(false)
        private set
    var connected by mutableStateOf(false)
        private set
    var problem by mutableStateOf<String?>(null)
        private set
    var level by mutableStateOf(0.0)
        private set
    var rate by mutableStateOf<Double?>(null)
        private set
    var hold by mutableStateOf<SilentHold?>(null)
        private set
    var dataset by mutableStateOf(SilentDataset.STARTER)
        private set
    var evaluation by mutableStateOf<SilentEvaluation?>(null)
        private set
    var evaluating by mutableStateOf(false)
        private set
    var lastMatch by mutableStateOf<SilentMatch?>(null)
        private set
    var transcript by mutableStateOf(SilentTranscript())
        private set
    var noise by mutableStateOf<SilentNoiseFloor?>(null)
        private set
    var calibrating by mutableStateOf(false)
        private set
    var detecting by mutableStateOf(false)
        private set
    var selectedWord by mutableStateOf<String?>(null)
    var channels by mutableStateOf(SilentChannel.DEFAULTS - SilentChannel.ACCELERATION)
        private set
    var minMargin by mutableStateOf(SilentSpeechPrefs.DEFAULT_MIN_MARGIN)
        private set
    var maxDistance by mutableStateOf<Double?>(null)
        private set
    var speakAloud by mutableStateOf(false)
        private set
    var autoDetect by mutableStateOf(false)
        private set

    val hasAcceleration: Boolean = false

    private var app: Context? = null
    private var sensorManager: SensorManager? = null
    private var sensor: Sensor? = null
    private var model: SilentModel? = null
    private val recent = ArrayList<SilentSample>()
    private val held = ArrayList<SilentSample>()
    private val calibration = ArrayList<Double>()
    private var calibrationStart: Double? = null
    private var segmenter: SilentSegmenter? = null
    private val rateWindow = ArrayList<Double>()
    private var tts: TextToSpeech? = null
    private var ttsReady = false
    private var pendingSpeech: String? = null
    private val main = Handler(Looper.getMainLooper())

    private val dynamicCallback = object : SensorManager.DynamicSensorCallback() {
        override fun onDynamicSensorConnected(sensor: Sensor) {
            if (listening && SilentSpeechListener.sensor == null && isHeadTracker(sensor)) attach(sensor)
        }

        override fun onDynamicSensorDisconnected(sensor: Sensor) {
            if (sensor == SilentSpeechListener.sensor) detach()
        }
    }

    val supported: Boolean get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU

    fun install(context: Context) {
        val appContext = context.applicationContext
        app = appContext
        val prefs = Prefs(appContext, SilentSpeechPrefs.FILE)
        val saved = prefs.getStringList(SilentSpeechPrefs.CHANNELS).orEmpty().mapNotNull(SilentChannel::of).toSet() - SilentChannel.ACCELERATION
        channels = saved.ifEmpty { SilentChannel.DEFAULTS - SilentChannel.ACCELERATION }
        minMargin = prefs.getDouble(SilentSpeechPrefs.MIN_MARGIN, SilentSpeechPrefs.DEFAULT_MIN_MARGIN)
        maxDistance = if (prefs.contains(SilentSpeechPrefs.MAX_DISTANCE)) prefs.getDouble(SilentSpeechPrefs.MAX_DISTANCE, 0.0) else null
        speakAloud = prefs.getBool(SilentSpeechPrefs.SPEAK_ALOUD, false)
        autoDetect = prefs.getBool(SilentSpeechPrefs.AUTO_DETECT, false)
        AppLifecycle.scope.launch {
            val loaded = withContext(Dispatchers.IO) { runCatching { file(appContext).takeIf { it.isFile }?.readText() }.getOrNull()?.let(SilentDataset::decode) }
            if (loaded != null) dataset = loaded
            if (selectedWord == null) selectedWord = dataset.words.firstOrNull()?.word
        }
    }

    private fun prefs(): Prefs? = app?.let { Prefs(it, SilentSpeechPrefs.FILE) }

    private fun file(context: Context) = File(File(context.filesDir, "silent_speech"), "dataset.json")

    private fun isHeadTracker(sensor: Sensor): Boolean =
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && sensor.type == Sensor.TYPE_HEAD_TRACKER

    fun start() {
        if (listening) return
        val context = app ?: return
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            problem = "Head tracking needs Android 13 or later."
            return
        }
        val manager = context.getSystemService(SensorManager::class.java) ?: return
        sensorManager = manager
        problem = null
        listening = true
        manager.registerDynamicSensorCallback(dynamicCallback, main)
        manager.getDynamicSensorList(Sensor.TYPE_HEAD_TRACKER).firstOrNull()?.let(::attach)
        if (autoDetect && noise == null) calibrate()
    }

    fun stop() {
        if (!listening) return
        sensorManager?.unregisterDynamicSensorCallback(dynamicCallback)
        sensorManager?.unregisterListener(this)
        sensor = null
        listening = false
        connected = false
        level = 0.0
        rate = null
        hold = null
        held.clear()
        recent.clear()
        rateWindow.clear()
        segmenter?.reset()
        calibrating = false
        detecting = false
    }

    private fun attach(found: Sensor) {
        sensor = found
        connected = sensorManager?.registerListener(this, found, SensorManager.SENSOR_DELAY_GAME, main) == true
    }

    private fun detach() {
        sensorManager?.unregisterListener(this)
        sensor = null
        connected = false
        level = 0.0
        rate = null
        hold = null
        held.clear()
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    override fun onSensorChanged(event: SensorEvent) {
        if (event.values.size < 6) return
        val v = event.values
        val sample = SilentSample(
            event.timestamp / 1e9,
            SilentVector(v[3].toDouble(), v[4].toDouble(), v[5].toDouble()),
            SilentQuaternion.fromRotationVector(SilentVector(v[0].toDouble(), v[1].toDouble(), v[2].toDouble())),
        )
        receive(sample)
    }

    private fun receive(sample: SilentSample) {
        connected = true
        level = level * 0.6 + sample.energy * 0.4
        rateWindow.add(sample.t)
        rateWindow.removeAll { it < sample.t - 2 }
        if (rateWindow.size > 5 && rateWindow.last() > rateWindow.first()) {
            rate = (rateWindow.size - 1) / (rateWindow.last() - rateWindow.first())
        }
        recent.add(sample)
        recent.removeAll { it.t < sample.t - RECENT_SECONDS }
        if (hold != null) {
            held.add(sample)
            return
        }
        if (calibrating) {
            calibration.add(sample.energy)
            val start = calibrationStart ?: sample.t.also { calibrationStart = it }
            if (sample.t - start >= CALIBRATION_SECONDS) finishCalibration()
            return
        }
        val current = segmenter
        if (!autoDetect || current == null) return
        val segment = current.push(sample)
        detecting = current.active
        if (segment != null) decode(segment)
    }

    fun beginHold(kind: SilentHold) {
        if (!listening || hold != null) return
        if (kind is SilentHold.Train && dataset.words.none { it.word == kind.word }) return
        val now = recent.lastOrNull()?.t ?: 0.0
        held.clear()
        held.addAll(recent.filter { it.t >= now - HOLD_PRE_ROLL })
        hold = kind
    }

    fun endHold() {
        val kind = hold ?: return
        hold = null
        val samples = ArrayList(held)
        held.clear()
        segmenter?.reset()
        if (samples.size < 2 || samples.last().t - samples.first().t < 0.2) return
        when (kind) {
            is SilentHold.Train -> {
                dataset = dataset.addTake(SilentTake(UUID.randomUUID().toString(), kind.word, samples))
                changed()
            }
            SilentHold.Decode -> decode(samples)
        }
    }

    fun calibrate() {
        if (!listening) return
        calibration.clear()
        calibrationStart = null
        calibrating = true
    }

    private fun finishCalibration() {
        calibrating = false
        noise = SilentNoiseFloor.calibrate(calibration)
        segmenter = noise?.let { SilentSegmenter(it) }
        calibration.clear()
        calibrationStart = null
    }

    private fun decode(samples: List<SilentSample>) {
        val current = model ?: SilentModel(dataset.takes, channels).also { model = it }
        val match = current.match(samples, maxDistance, minMargin)
        lastMatch = match
        if (match == null || !match.accepted) return
        transcript = transcript.append(match.word)
        if (speakAloud) speak(dataset.phrase(match.word))
    }

    fun speak(text: String) {
        if (text.isBlank()) return
        val context = app ?: return
        val engine = tts
        if (engine != null && ttsReady) {
            engine.speak(text, TextToSpeech.QUEUE_FLUSH, null, "silent-speech")
            return
        }
        pendingSpeech = text
        if (engine == null) {
            tts = TextToSpeech(context) { status ->
                main.post {
                    ttsReady = status == TextToSpeech.SUCCESS
                    if (ttsReady) {
                        tts?.language = Locale.getDefault()
                        pendingSpeech?.let { tts?.speak(it, TextToSpeech.QUEUE_FLUSH, null, "silent-speech") }
                    }
                    pendingSpeech = null
                }
            }
        }
    }

    fun evaluate() {
        if (evaluating) return
        evaluating = true
        val snapshot = dataset
        val chosen = channels
        AppLifecycle.scope.launch {
            val result = withContext(Dispatchers.Default) { SilentEvaluator.evaluate(snapshot, chosen) }
            evaluation = result
            evaluating = false
            result?.suggestedMaxDistance?.let { updateMaxDistance(it) }
        }
    }

    fun toggleChannel(channel: SilentChannel, on: Boolean) {
        val next = if (on) channels + channel else channels - channel
        if (next.isEmpty()) return
        channels = next
        prefs()?.putStringList(SilentSpeechPrefs.CHANNELS, next.map { it.wire }.sorted())
        invalidate()
    }

    fun updateSpeakAloud(on: Boolean) {
        speakAloud = on
        prefs()?.putBool(SilentSpeechPrefs.SPEAK_ALOUD, on)
    }

    fun updateAutoDetect(on: Boolean) {
        autoDetect = on
        prefs()?.putBool(SilentSpeechPrefs.AUTO_DETECT, on)
        segmenter?.reset()
        if (on && noise == null) calibrate()
    }

    fun updateMaxDistance(limit: Double?) {
        maxDistance = limit
        if (limit != null) prefs()?.putDouble(SilentSpeechPrefs.MAX_DISTANCE, limit) else prefs()?.remove(SilentSpeechPrefs.MAX_DISTANCE)
    }

    fun addWord(word: String, phrase: String): Boolean {
        val next = dataset.addWord(word, phrase) ?: return false
        dataset = next
        selectedWord = SilentDataset.clean(word)
        changed()
        return true
    }

    fun removeWord(word: String) {
        dataset = dataset.removeWord(word)
        if (selectedWord == word) selectedWord = dataset.words.firstOrNull()?.word
        changed()
    }

    fun undoTake(word: String) {
        dataset = dataset.removeLastTake(word)
        changed()
    }

    fun undoWord() {
        transcript = transcript.undo()
    }

    fun clearTranscript() {
        transcript = transcript.clear()
        lastMatch = null
    }

    fun copyTranscript() {
        val context = app ?: return
        context.getSystemService(ClipboardManager::class.java)?.setPrimaryClip(ClipData.newPlainText("Silent Speech", transcript.text))
    }

    private fun changed() {
        invalidate()
        save()
    }

    private fun invalidate() {
        model = null
        evaluation = null
    }

    private fun save() {
        val context = app ?: return
        val text = dataset.encoded()
        AppLifecycle.scope.launch(Dispatchers.IO) {
            runCatching {
                val target = file(context)
                target.parentFile?.mkdirs()
                val temp = File(target.parentFile, "dataset.json.tmp")
                temp.writeText(text)
                if (!temp.renameTo(target)) {
                    target.writeText(text)
                    temp.delete()
                }
            }
        }
    }
}
