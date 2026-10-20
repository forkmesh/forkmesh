package com.forkmesh.nativeapp.feature.agents

import com.forkmesh.nativeapp.ui.kit.confirmAction
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.AnnotatedString
import com.forkmesh.nativeapp.agents.AGENT_LOGIN_PROVIDERS
import com.forkmesh.nativeapp.agents.DesktopAgentTerminal
import com.forkmesh.nativeapp.agents.agentProviderLabel
import com.forkmesh.nativeapp.control.AgentControlCenter
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.FmFilledButton
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal class DesktopTerminalScreen(
    attach: String = "",
    private val session: Int = 0,
    owner: String = "",
    name: String = "",
    provider: String = "",
    private val account: String = "",
    private val accountLabel: String = "",
    private val mode: String = "",
) : Screen() {
    private val signIn: Boolean get() = mode == "login" || mode == "logout"
    private var terminal by mutableStateOf(
        when {
            session > 0 -> DesktopAgentTerminal(AgentsFeature.hub, owner = owner, name = name, provider = provider, session = session)
            mode == "login" || mode == "logout" -> DesktopAgentTerminal(AgentsFeature.hub, provider = provider, mode = mode, account = account)
            attach.isEmpty() -> null
            else -> DesktopAgentTerminal(AgentsFeature.hub, terminal = attach)
        },
    )
    private var repo by mutableStateOf<String?>(null)
    private var provider by mutableStateOf(provider.ifEmpty { "claude-code" })
    private var ending by mutableStateOf(false)
    private val status = FmStatus()

    private val who: String get() = if (accountLabel.isEmpty()) "" else " ($accountLabel)"

    override val title: String
        get() = when (mode) {
            "login" -> "Log in to ${agentProviderLabel(provider)}$who"
            "logout" -> "Log out of ${agentProviderLabel(provider)}$who"
            else -> terminal?.title?.let { if (accountLabel.isEmpty()) it else "$it · $accountLabel" } ?: "Agent in a terminal"
        }

    override fun onDisposed() {
        val last = terminal ?: return
        val close = signIn && !last.finished && last.terminalId.isNotEmpty()
        Thread {
            if (close) runCatching { last.end() }
            last.detach()
        }.start()
    }

    private fun start(center: AgentControlCenter, owner: String, name: String) {
        val previous = terminal
        terminal = DesktopAgentTerminal(
            AgentsFeature.hub, owner = owner, name = name, provider = provider, account = account,
            onStarted = { center.refreshDesktopAsync() },
        )
        previous?.let { Thread { it.detach() }.start() }
    }

    private fun restartSignIn() {
        val previous = terminal
        terminal = DesktopAgentTerminal(AgentsFeature.hub, provider = provider, mode = mode, account = account)
        previous?.let { Thread { it.detach() }.start() }
    }

    private fun backToForm() {
        val previous = terminal ?: return
        terminal = null
        Thread { previous.detach() }.start()
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val clipboard = LocalClipboardManager.current
        val uri = LocalUriHandler.current
        val center = AgentsFeature.control
        val centerTick = rememberCenterTick(center)
        val current = terminal
        val tick = rememberTerminalTick(current)
        if (current == null) {
            DetailScaffold(title, navigator) {
                centerTick.let { }
                StartForm(center)
            }
            return
        }
        fun end() {
            scope.launch {
                val confirmed = messenger.confirmAction(
                    "End ${current.providerLabel}?",
                    "End",
                    "The desktop closes its terminal, which stops the agent. Leaving this screen instead keeps it running.",
                )
                if (!confirmed) return@launch
                ending = true
                status.clear()
                try {
                    withContext(Dispatchers.IO) { current.end() }
                    status.show("Ended ${current.providerLabel}.")
                    center.refreshDesktopAsync()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    status.error(controlError(error))
                } finally {
                    ending = false
                }
            }
        }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("copy_all_outlined", "Copy terminal text", onPressed = {
                clipboard.setText(AnnotatedString(current.plainText))
                status.show("Terminal text copied.")
            })
            if (!current.finished) {
                FmIconButton("stop_circle", "End the agent", onPressed = if (ending) null else ({ end() }), color = c.danger)
            }
        }) {
            Column(Modifier.fillMaxSize()) {
                tick.let { }
                Row(Modifier.fillMaxWidth().padding(FmSpace.x3), verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        current.statusText,
                        Modifier.weight(1f),
                        style = Fm.text.bodySmall.copy(color = terminalStatusColor(current.statusLevel)),
                        minLines = 2,
                    )
                    if (current.failed && signIn) {
                        FmToneButton("Try again", onPressed = { restartSignIn() }, icon = "refresh")
                    } else if (current.failed && session == 0) {
                        FmToneButton("Change and retry", onPressed = { backToForm() }, icon = "refresh")
                    }
                }
                FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x3).padding(bottom = FmSpace.x2))
                val link = if (signIn) remember(tick) { current.screen.links().lastOrNull().orEmpty() } else ""
                if (link.isNotEmpty()) {
                    Row(Modifier.padding(horizontal = FmSpace.x3), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                        FmFilledButton("Open sign-in page", onClick = { runCatching { uri.openUri(link) } }, icon = "open_in_new")
                        FmToneButton("Copy link", onPressed = { clipboard.setText(AnnotatedString(link)) }, icon = "content_copy")
                    }
                    Spacer(Modifier.height(FmSpace.x2))
                }
                AgentTerminalPane(current, tick, "Type, then send to ${current.providerLabel}", Modifier.weight(1f).fillMaxWidth())
            }
        }
    }

    @Composable
    private fun StartForm(center: AgentControlCenter) {
        val c = Fm.colors
        LaunchedEffect(Unit) { center.refreshDesktopAsync() }
        val snapshot = center.desktopSnapshot
        val repos = snapshot?.repositories ?: emptyList()
        val chosenRepo = repo?.takeIf { pick -> repos.any { it.label == pick } } ?: repos.firstOrNull()?.label
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(FmSpace.x4),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            Text(center.hub.reach.label, style = Fm.text.bodySmall.copy(color = c.textSecondary))
            if (accountLabel.isNotEmpty()) {
                Text("Runs as the ${agentProviderLabel(provider)} account $accountLabel.", style = Fm.text.bodySmall.copy(color = c.textSecondary))
            }
            Text(
                "Start Claude Code or Codex in a terminal on your desktop, in a repository it has open. You watch and type " +
                    "into that terminal here. It keeps running when you leave this screen and shows in your Agents list until it ends.",
                style = Fm.text.bodyMedium.copy(color = c.textPrimary),
            )
            if (center.desktopNote.isNotEmpty()) Text(center.desktopNote, style = Fm.text.bodySmall.copy(color = c.warning))
            if (snapshot == null) {
                if (center.loading) FmLinearProgressIndicator(Modifier.fillMaxWidth())
                return@Column
            }
            if (repos.isEmpty()) {
                Text(
                    "The desktop has no repository open, so it has nowhere to start an agent.",
                    style = Fm.text.bodySmall.copy(color = c.textSecondary),
                )
                return@Column
            }
            TerminalMenu("Repository", chosenRepo ?: "", repos.map { it.label to it.label }) { repo = it }
            if (account.isEmpty()) {
                TerminalMenu("Agent", agentProviderLabel(provider), AGENT_LOGIN_PROVIDERS.toList()) { provider = it }
            }
            Spacer(Modifier.height(FmSpace.x2))
            FmFilledButton("Start", onClick = if (chosenRepo == null) null else ({
                val parts = chosenRepo.split('/', limit = 2)
                start(center, parts[0], parts.getOrElse(1) { "" })
            }), icon = "terminal")
        }
    }

    @Composable
    private fun TerminalMenu(label: String, value: String, options: List<Pair<String, String>>, onPick: (String) -> Unit) {
        var open by remember { mutableStateOf(false) }
        Column {
            PickerRow("expand_more", value.ifEmpty { label }, label, onTap = { open = true })
            FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                for ((id, text) in options) {
                    FmDropdownMenuItem(text = text, onClick = {
                        open = false
                        onPick(id)
                    })
                }
            }
        }
    }
}
