package com.forkmesh.nativeapp.feature.hosts

import com.forkmesh.nativeapp.ui.kit.confirmAction
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import com.forkmesh.nativeapp.agents.DesktopAgentTerminal
import com.forkmesh.nativeapp.feature.agents.AgentTerminalPane
import com.forkmesh.nativeapp.feature.agents.rememberTerminalTick
import com.forkmesh.nativeapp.feature.agents.terminalStatusColor
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal class HostTerminalScreen(private val host: String, terminal: String) : Screen() {
    private val terminal = DesktopAgentTerminal(HostsFeature.hub, terminal = terminal)
    private var closing by mutableStateOf(false)

    override val title: String get() = "SSH to $host"

    override fun onDisposed() {
        val last = terminal
        Thread { last.detach() }.start()
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val status = rememberFmStatus()
        val clipboard = LocalClipboardManager.current
        val tick = rememberTerminalTick(terminal)
        fun close() {
            scope.launch {
                val confirmed = messenger.confirmAction(
                    "Close the SSH session to $host?",
                    "Close",
                    "The desktop ends the connection and anything still running in that shell. Leaving this screen instead keeps the session open on the desktop.",
                )
                if (!confirmed) return@launch
                closing = true
                status.clear()
                try {
                    withContext(Dispatchers.IO) { terminal.end() }
                    status.show("SSH session closed.")
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    status.error(describe(error))
                } finally {
                    closing = false
                }
            }
        }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("copy_all_outlined", "Copy terminal text", onPressed = {
                clipboard.setText(AnnotatedString(terminal.plainText))
                status.show("Terminal text copied.")
            })
            if (!terminal.finished) {
                FmIconButton("close", "Close the SSH session", onPressed = if (closing) null else ({ close() }), color = c.danger)
            }
        }) {
            Column(Modifier.fillMaxSize()) {
                tick.let { }
                Row(Modifier.fillMaxWidth().padding(FmSpace.x3), verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        terminal.statusText,
                        Modifier.weight(1f),
                        style = Fm.text.bodySmall.copy(color = terminalStatusColor(terminal.statusLevel)),
                        minLines = 2,
                    )
                }
                FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x3))
                AgentTerminalPane(terminal, tick, "Type a command, then send", Modifier.weight(1f).fillMaxWidth())
            }
        }
    }
}
