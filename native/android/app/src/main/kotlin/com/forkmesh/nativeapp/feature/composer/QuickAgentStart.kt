package com.forkmesh.nativeapp.feature.composer

import com.forkmesh.nativeapp.ui.kit.FmSoundCues
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.theme.FmSpace
import androidx.compose.ui.Modifier
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.Column
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import com.forkmesh.nativeapp.agents.AgentQueueApi
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.composer.PromptDraftStore
import com.forkmesh.nativeapp.composer.QuickLaunchFix
import com.forkmesh.nativeapp.composer.QuickLaunchOutcome
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.agents.showAgentSetupSheet
import com.forkmesh.nativeapp.feature.localrepos.LocalReposFeature
import com.forkmesh.nativeapp.feature.surfaces.SurfacesFeature
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.shell.LaunchFlight
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal class QuickAgentActions(
    val status: FmStatus,
    val openContextPicker: () -> Unit,
    val runFix: (QuickLaunchFix) -> Unit,
    val send: (prompt: String, images: List<PromptAttachment>, onSent: () -> Unit) -> Unit,
)

@Composable
internal fun rememberQuickAgentActions(
    navigator: Navigator,
    quick: QuickSendState,
    repoContext: String?,
    onStarted: () -> Unit = {},
    openRun: (Screen) -> Unit = { navigator.push(it) },
): QuickAgentActions {
    val messenger = LocalFmMessenger.current
    val status = rememberFmStatus()
    val scope = rememberCoroutineScope()
    val requireAccount = AccountFeature.rememberRequireAccount()
    val cloneToDevice = LocalReposFeature.rememberCloneToDevice(navigator, status)
    val latestRepo by rememberUpdatedState(repoContext)
    val latestStarted by rememberUpdatedState(onStarted)
    val latestOpenRun by rememberUpdatedState(openRun)
    val latestClone by rememberUpdatedState(cloneToDevice)

    fun openContextPicker() {
        scope.launch {
            when (val picked = pickQuickContext(messenger, quick)) {
                is QuickPick.Repo -> quick.choose(picked.target)
                is QuickPick.Where -> quick.choose(picked.destination)
                null -> {}
            }
        }
    }

    fun runFix(fix: QuickLaunchFix) {
        status.clear()
        scope.launch {
            when (fix) {
                QuickLaunchFix.SignInAccount -> if (requireAccount("start an agent")) {
                    quick.clearFix()
                    quick.prepare(latestRepo)
                }
                QuickLaunchFix.ChooseRepository -> openContextPicker()
                is QuickLaunchFix.SignInProvider -> AgentsFeature.openLoginTerminal(navigator, fix.node, fix.provider)
                is QuickLaunchFix.SignInRunner -> {
                    quick.clearFix()
                    AgentsFeature.openRunnerSignIn(navigator, fix.runner, fix.provider)
                }
                QuickLaunchFix.SetUpDevice -> {
                    val current = withContext(Dispatchers.IO) { runCatching { AgentsFeature.credentials.read() }.getOrNull() }
                    if (showAgentSetupSheet(messenger, current) != null) {
                        quick.clearFix()
                        quick.credentialsChanged()
                    }
                }
                is QuickLaunchFix.MirrorToDevice -> {
                    val clone = latestClone
                    if (clone == null) {
                        status.error("This phone cannot mirror repositories yet.")
                    } else {
                        clone(quick.repository?.repo ?: Repository(fix.owner, fix.name))
                    }
                }
            }
        }
    }

    fun send(prompt: String, images: List<PromptAttachment>, onSent: () -> Unit) {
        status.clear()
        LaunchFlight.charge()
        scope.launch {
            try {
                when (val outcome = quick.send(prompt, images)) {
                    is QuickLaunchOutcome.Queued -> {
                        if (images.isNotEmpty()) {
                            AgentsFeature.sentPhotos.record(outcome.session.sessionId, prompt.trim().ifEmpty { "See the attached photos." }, images)
                        }
                        AgentsFeature.ring()
                        FmSoundCues.play(SoundCue.SUCCESS)
                        onSent()
                        latestStarted()
                        LaunchFlight.launch(SurfacesFeature.agents.value.active.size, outcome.message)
                        latestOpenRun(AgentsFeature.launchedSessionScreen(outcome.repo, outcome.session))
                    }
                    is QuickLaunchOutcome.QueuedWithoutSession -> {
                        AgentsFeature.ring()
                        FmSoundCues.play(SoundCue.SUCCESS)
                        onSent()
                        latestStarted()
                        LaunchFlight.launch(SurfacesFeature.agents.value.active.size, outcome.message)
                    }
                    is QuickLaunchOutcome.Device -> {
                        onSent()
                        navigator.push(AgentsFeature.onDeviceAgentScreen(outcome.record, outcome.prompt, outcome.images, autoStart = true))
                    }
                    is QuickLaunchOutcome.Desktop -> {
                        FmSoundCues.play(SoundCue.SUCCESS)
                        onSent()
                        latestStarted()
                        LaunchFlight.launch(SurfacesFeature.agents.value.active.size, outcome.message)
                        latestOpenRun(AgentsFeature.allAgentsScreen())
                    }
                    is QuickLaunchOutcome.NeedsFix, is QuickLaunchOutcome.Failed -> FmSoundCues.play(SoundCue.ERROR)
                    else -> {}
                }
            } finally {
                LaunchFlight.discharge()
            }
        }
    }

    return remember(quick, navigator) { QuickAgentActions(status, ::openContextPicker, ::runFix, ::send) }
}

@Composable
fun AgentQuickStart(navigator: Navigator, onStarted: () -> Unit = {}, onSent: () -> Unit = {}, autofocus: Boolean = false) {
    val scope = rememberCoroutineScope()
    val quick = remember { ComposerFeature.quick() }
    val drafts = remember { ComposerFeature.drafts }
    val repoContext by ComposerFeature.repoContext.collectAsState()
    val actions = rememberQuickAgentActions(navigator, quick, repoContext, onStarted)
    var text by remember { mutableStateOf(drafts?.load(PromptDraftStore.NEW_AGENT) ?: "") }
    var photos by remember { mutableStateOf<List<PromptAttachment>>(emptyList()) }

    LaunchedEffect(repoContext) { quick.prepare(repoContext) }

    fun updateText(next: String) {
        text = next
        drafts?.save(PromptDraftStore.NEW_AGENT, next)
    }

    PromptComposer(
        text = text,
        onTextChange = ::updateText,
        onSend = {
            actions.send(text, photos) {
                updateText("")
                photos = emptyList()
                onSent()
            }
        },
        hint = "New agent · type / for commands",
        busy = quick.sending,
        modelLabel = quick.model.label,
        modelChoices = quick.modelChoices,
        selectedModel = quick.model,
        onSelectModel = { picked -> scope.launch { quick.chooseModel(picked) } },
        attachments = photos,
        onAttachmentsChanged = { photos = it },
        header = { QuickActionsHeader(quick, actions) },
        sendDescription = "Start agent",
        autofocus = autofocus,
        maxLength = AgentQueueApi.MAX_AGENT_PROMPT_CHARS,
        uploadsFiles = true,
        padForKeyboard = false,
        outerPadding = false,
        offersFix = true,
    )
}

@Composable
internal fun QuickActionsHeader(quick: QuickSendState, actions: QuickAgentActions) {
    Column(Modifier.fillMaxWidth()) {
        QuickContextLine(quick, actions.openContextPicker, actions.runFix)
        FmInlineStatus(actions.status, Modifier.padding(bottom = FmSpace.x2))
    }
}

suspend fun showAgentQuickStartSheet(messenger: FmMessenger, navigator: Navigator, onStarted: () -> Unit = {}) {
    messenger.showModalBottomSheet<Unit>(showDragHandle = true, isScrollControlled = true) { close ->
        Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x3).padding(bottom = FmSpace.x3).imePadding()) {
            AgentQuickStart(navigator, onStarted = onStarted, onSent = { close(Unit) }, autofocus = true)
        }
    }
}
