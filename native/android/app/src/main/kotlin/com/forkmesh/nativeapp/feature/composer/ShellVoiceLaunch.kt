package com.forkmesh.nativeapp.feature.composer

import android.os.SystemClock
import androidx.activity.compose.BackHandler
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.LifecycleOwner
import com.forkmesh.nativeapp.agentwatch.VoiceLaunchChime
import com.forkmesh.nativeapp.composer.QuickLaunchOutcome
import com.forkmesh.nativeapp.composer.VoiceLaunchPhase
import com.forkmesh.nativeapp.composer.VoiceLaunchRules
import com.forkmesh.nativeapp.composer.VoiceMeter
import com.forkmesh.nativeapp.composer.VoiceRelease
import com.forkmesh.nativeapp.composer.VoiceTranscript
import com.forkmesh.nativeapp.shell.LaunchFlight
import com.forkmesh.nativeapp.ui.kit.FmSoundCues
import com.forkmesh.nativeapp.ui.kit.VoiceLaunchSounds
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

internal typealias VoiceSender = (text: String, onSent: () -> Unit, onOutcome: (QuickLaunchOutcome) -> Unit) -> Unit

@Stable
class ShellVoiceLaunch internal constructor(
    private val dictation: SpeechDictation,
    private val scope: CoroutineScope,
) {
    var phase by mutableStateOf<VoiceLaunchPhase>(VoiceLaunchPhase.Closed)
        private set
    var words by mutableStateOf("")
        private set
    var origin by mutableStateOf(Rect.Zero)
        private set
    var held by mutableStateOf(false)
        private set
    var openedAt by mutableLongStateOf(0L)
        private set
    var heardAt by mutableLongStateOf(0L)
        private set
    var sentAt by mutableLongStateOf(0L)
        private set
    var levels by mutableStateOf(FloatArray(VoiceMeter.SIZE))
        private set

    var orb: () -> Rect = { Rect.Zero }
    internal var haptics: FmHaptics? = null
    internal var sender: VoiceSender = { _, _, _ -> }
    internal var onType: (String) -> Unit = {}
    internal var onCommands: () -> Unit = {}

    private val meter = VoiceMeter()
    private val transcript = VoiceTranscript()
    private var attempt = 0
    private var fired = false
    private var finishing: Job? = null
    private var limit: Job? = null

    val isOpen: Boolean get() = phase.isOpen

    val heard: String get() = words.trim()

    val missed: String? get() = (phase as? VoiceLaunchPhase.Missed)?.message

    val loudness: Float get() = levels.takeLast(3).maxOrNull() ?: 0f

    fun press() {
        if (phase is VoiceLaunchPhase.Closed) dictation.prime()
    }

    fun open(from: Rect, pressed: Boolean) {
        if (phase !is VoiceLaunchPhase.Closed) return
        attempt++
        val mine = attempt
        origin = from
        words = ""
        heardAt = 0L
        openedAt = SystemClock.uptimeMillis()
        fired = false
        held = pressed
        transcript.clear()
        meter.clear()
        levels = meter.levels
        phase = VoiceLaunchPhase.Waking
        haptics?.feel(HapticFeedbackType.LongPress)
        dictation.prime()
        VoiceLaunchSounds.play(VoiceLaunchChime.Summon)
        val lead = VoiceLaunchChime.leadSeconds(pressed, FmSoundCues.enabled)
        scope.launch {
            delay((lead * 1000).toLong())
            if (attempt == mine && phase is VoiceLaunchPhase.Waking) listen(mine)
        }
    }

    fun letGo() {
        val release = VoiceLaunchRules.release(phase, held, heard, meter.voicing)
        val wasHeld = held
        held = false
        if (!wasHeld) return
        if (release != VoiceRelease.Send) return
        val mine = attempt
        phase = VoiceLaunchPhase.Sending
        haptics?.feel(HapticFeedbackType.Confirm)
        finish(mine, VoiceLaunchRules.TAIL_MS)
    }

    fun sendNow() {
        if (phase !is VoiceLaunchPhase.Listening || heard.isEmpty()) return
        held = false
        finish(attempt, 0L)
    }

    fun cancel() {
        if (!VoiceLaunchRules.canCancel(phase)) return
        attempt++
        held = false
        limit?.cancel()
        finishing?.cancel()
        dictation.cancel()
        phase = VoiceLaunchPhase.Leaving
        haptics?.feel(HapticFeedbackType.TextHandleMove)
        closeLater(VoiceLaunchRules.LEAVE_HOLD_MS)
    }

    fun dismiss() {
        attempt++
        held = false
        limit?.cancel()
        finishing?.cancel()
        dictation.cancel()
        phase = VoiceLaunchPhase.Closed
    }

    fun sent() {
        attempt++
        limit?.cancel()
        finishing?.cancel()
        sentAt = SystemClock.uptimeMillis()
        phase = VoiceLaunchPhase.Sent
        haptics?.feel(HapticFeedbackType.Confirm)
        VoiceLaunchSounds.play(VoiceLaunchChime.Launch)
        closeLater(VoiceLaunchRules.SENT_HOLD_MS)
    }

    private fun listen(mine: Int) {
        phase = VoiceLaunchPhase.Listening
        haptics?.feel(HapticFeedbackType.TextHandleMove)
        limit = scope.launch {
            delay(VoiceLaunchRules.LIMIT_SECONDS * 1000L)
            if (attempt == mine && phase is VoiceLaunchPhase.Listening) {
                held = false
                phase = VoiceLaunchPhase.Sending
                finish(mine, 0L)
            }
        }
        dictation.start(
            onWords = { spoken ->
                val open = phase is VoiceLaunchPhase.Listening || phase is VoiceLaunchPhase.Sending
                if (attempt == mine && open && transcript.partial(spoken)) {
                    words = transcript.text
                    heardAt = SystemClock.uptimeMillis()
                }
            },
            onError = { message -> miss(message, mine, closes = false) },
            session = DictationSession(
                holds = { held && attempt == mine },
                onSegment = { if (attempt == mine) transcript.commit() },
                onFinished = { ended(mine) },
                onLevel = { db ->
                    if (attempt == mine) {
                        meter.push(db)
                        levels = meter.levels
                    }
                },
                silenceMs = VoiceLaunchRules.PAUSE_SECONDS * 1000L,
            ),
        )
    }

    private fun finish(mine: Int, tail: Long) {
        finishing?.cancel()
        finishing = scope.launch {
            if (tail > 0) delay(tail)
            if (attempt != mine || fired) return@launch
            dictation.stop()
            delay(VoiceLaunchRules.FINISH_WAIT_MS)
            ended(mine)
        }
    }

    private fun ended(mine: Int) {
        val live = phase is VoiceLaunchPhase.Listening || phase is VoiceLaunchPhase.Sending
        if (attempt != mine || !live || fired) return
        fired = true
        held = false
        finishing?.cancel()
        limit?.cancel()
        val text = transcript.text
        if (text.isBlank()) {
            miss(VoiceLaunchRules.NOTHING_HEARD, mine, closes = true)
            return
        }
        if (phase is VoiceLaunchPhase.Listening) {
            phase = VoiceLaunchPhase.Sending
            haptics?.feel(HapticFeedbackType.Confirm)
        }
        sender(text, { sent() }) { outcome ->
            when (outcome) {
                is QuickLaunchOutcome.NeedsFix, is QuickLaunchOutcome.Failed -> {
                    haptics?.feel(HapticFeedbackType.Reject)
                    toComposer(text)
                }
                QuickLaunchOutcome.Ignored -> dismiss()
                else -> {}
            }
        }
    }

    private fun toComposer(text: String) {
        dismiss()
        onType(text)
    }

    fun type() {
        val text = heard
        dismiss()
        onType(text)
    }

    fun commands() {
        dismiss()
        onCommands()
    }

    private fun miss(message: String, mine: Int, closes: Boolean) {
        if (attempt != mine) return
        attempt++
        limit?.cancel()
        finishing?.cancel()
        held = false
        dictation.cancel()
        phase = VoiceLaunchPhase.Missed(message)
        haptics?.feel(HapticFeedbackType.Reject)
        VoiceLaunchSounds.play(VoiceLaunchChime.Fizzle)
        if (closes) closeLater(VoiceLaunchRules.MISS_HOLD_MS)
    }

    private fun closeLater(millis: Long) {
        val mine = attempt
        scope.launch {
            delay(millis)
            if (attempt != mine || phase is VoiceLaunchPhase.Closed) return@launch
            if (phase is VoiceLaunchPhase.Leaving || phase is VoiceLaunchPhase.Sent) {
                phase = VoiceLaunchPhase.Closed
                return@launch
            }
            phase = VoiceLaunchPhase.Leaving
            closeLater(VoiceLaunchRules.LEAVE_HOLD_MS)
        }
    }
}

@Composable
internal fun rememberShellVoiceLaunch(
    navigator: Navigator,
    onStarted: () -> Unit,
    openRun: (Screen) -> Unit,
    onType: (String) -> Unit,
    onCommands: () -> Unit,
): ShellVoiceLaunch {
    val scope = rememberCoroutineScope()
    val dictation = rememberSpeechDictation()
    val quick = remember { ComposerFeature.quick() }
    val repoContext by ComposerFeature.repoContext.collectAsState()
    val actions = rememberQuickAgentActions(navigator, quick, repoContext, onStarted, openRun)
    val latest by rememberUpdatedState(actions)
    val voice = remember(dictation) { ShellVoiceLaunch(dictation, scope) }
    voice.sender = { text, onSent, onOutcome ->
        val saved = LaunchFlight.source
        LaunchFlight.source = voice.orb().takeIf { !it.isEmpty } ?: voice.origin
        latest.sendReporting(text, emptyList(), onSent, onOutcome)
        LaunchFlight.source = saved
    }
    voice.haptics = FmHaptics.current
    voice.onType = onType
    voice.onCommands = onCommands
    val context = LocalContext.current
    LaunchedEffect(Unit) { VoiceLaunchSounds.warmUp() }
    val open = voice.isOpen
    DisposableEffect(open) {
        if (open) FmSoundCues.quieten(true)
        onDispose { if (open) FmSoundCues.quieten(false) }
    }
    DisposableEffect(context, voice) {
        val owner = context as? LifecycleOwner
        val observer = LifecycleEventObserver { _, event -> if (event == Lifecycle.Event.ON_STOP) voice.cancel() }
        owner?.lifecycle?.addObserver(observer)
        onDispose { owner?.lifecycle?.removeObserver(observer) }
    }
    BackHandler(enabled = voice.isOpen) { voice.cancel() }
    return voice
}
