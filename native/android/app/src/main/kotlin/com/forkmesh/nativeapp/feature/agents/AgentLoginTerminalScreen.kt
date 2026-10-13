package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.agents.AgentLoginTerminal
import com.forkmesh.nativeapp.agents.AgentTerminalSession
import com.forkmesh.nativeapp.agents.TERMINAL_DEFAULT_COLOR
import com.forkmesh.nativeapp.agents.TERMINAL_TRUE_COLOR
import com.forkmesh.nativeapp.agents.TerminalKey
import com.forkmesh.nativeapp.agents.TerminalRow
import com.forkmesh.nativeapp.agents.TerminalStatusLevel
import com.forkmesh.nativeapp.agents.agentProviderLabel
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.FmFilledButton
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

private val ANSI = listOf(
    0xFF1E1E1E, 0xFFCD3131, 0xFF0DBC79, 0xFFE5E510, 0xFF2472C8, 0xFFBC3FBC, 0xFF11A8CD, 0xFFE5E5E5,
    0xFF666666, 0xFFF14C4C, 0xFF23D18B, 0xFFF5F543, 0xFF3B8EEA, 0xFFD670D6, 0xFF29B8DB, 0xFFFFFFFF,
)

private fun terminalColor(value: Int, fallback: Color): Color {
    if (value == TERMINAL_DEFAULT_COLOR) return fallback
    if (value >= TERMINAL_TRUE_COLOR) return Color(0xFF000000 or (value - TERMINAL_TRUE_COLOR).toLong())
    if (value < 16) return Color(ANSI[value])
    if (value < 232) {
        val n = value - 16
        fun level(x: Int) = if (x == 0) 0 else 55 + x * 40
        return Color(level(n / 36), level((n / 6) % 6), level(n % 6))
    }
    val gray = 8 + (value - 232) * 10
    return Color(gray, gray, gray)
}

private fun rowText(row: TerminalRow, fg: Color, bg: Color): AnnotatedString = buildAnnotatedString {
    var width = row.cells.size
    while (width > 0 && row.cells[width - 1].ch == " " && row.cells[width - 1].bg == TERMINAL_DEFAULT_COLOR) width--
    for (i in 0 until width) {
        val cell = row.cells[i]
        var foreground = terminalColor(cell.fg, fg)
        var background = terminalColor(cell.bg, Color.Transparent)
        if (cell.inverse) {
            val swap = foreground
            foreground = if (background == Color.Transparent) bg else background
            background = swap
        }
        if (cell.dim) foreground = foreground.copy(alpha = .6f)
        pushStyle(
            SpanStyle(
                color = foreground,
                background = background,
                fontWeight = if (cell.bold) FontWeight.Bold else null,
                textDecoration = if (cell.underline || cell.link.isNotEmpty()) TextDecoration.Underline else null,
            ),
        )
        append(cell.ch)
        pop()
    }
}

@Composable
internal fun terminalStatusColor(level: TerminalStatusLevel): Color {
    val c = Fm.colors
    return when (level) {
        TerminalStatusLevel.Pending -> c.textSecondary
        TerminalStatusLevel.Ok -> c.success
        TerminalStatusLevel.Error -> c.danger
    }
}

@Composable
internal fun rememberTerminalTick(terminal: AgentTerminalSession?): Int {
    var tick by remember { mutableIntStateOf(0) }
    DisposableEffect(terminal) {
        val listener: () -> Unit = { tick++ }
        terminal?.addListener(listener)
        onDispose { terminal?.removeListener(listener) }
    }
    return tick
}

@Composable
internal fun AgentTerminalPane(terminal: AgentTerminalSession, tick: Int, hint: String, modifier: Modifier = Modifier) {
    val clipboard = LocalClipboardManager.current
    val style = TextStyle(fontFamily = FmType.mono, fontSize = 12.sp, lineHeight = 15.sp)
    val measurer = rememberTextMeasurer()
    val density = LocalDensity.current
    var typed by remember(terminal) { mutableStateOf("") }
    Column(modifier) {
        BoxWithConstraints(Modifier.weight(1f).fillMaxWidth().background(Color(0xFF111111))) {
            val cell = remember(style) { measurer.measure("M", style) }
            val cols = with(density) { (maxWidth.toPx() / cell.size.width).toInt() }.coerceAtLeast(20)
            val rows = with(density) { (maxHeight.toPx() / cell.size.height).toInt() }.coerceAtLeast(5)
            LaunchedEffect(cols, rows, terminal) {
                terminal.start(cols, rows)
                terminal.resize(cols, rows)
            }
            tick.let { }
            val all = terminal.screen.allRows
            val list = rememberLazyListState()
            LaunchedEffect(all.size, tick) { if (all.isNotEmpty()) list.scrollToItem(all.size - 1) }
            LazyColumn(Modifier.fillMaxSize(), state = list) {
                itemsIndexed(all) { _, row ->
                    Text(rowText(row, Color(0xFFE5E5E5), Color(0xFF111111)), style = style, softWrap = false, maxLines = 1)
                }
            }
        }
        Row(
            Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(FmSpace.x2),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x1),
        ) {
            val keys = listOf(
                "Esc" to TerminalKey.Escape, "Tab" to TerminalKey.Tab, "↑" to TerminalKey.Up, "↓" to TerminalKey.Down,
                "←" to TerminalKey.Left, "→" to TerminalKey.Right, "⌫" to TerminalKey.Backspace, "Ctrl-C" to TerminalKey.CtrlC,
                "Enter" to TerminalKey.Enter,
            )
            for ((label, key) in keys) FmToneButton(label, onPressed = { terminal.key(key) }, tone = FmTone.Neutral)
            FmToneButton("Paste", onPressed = { clipboard.getText()?.text?.let(terminal::paste) }, icon = "content_paste")
        }
        Row(Modifier.fillMaxWidth().navigationBarsPadding().imePadding().padding(FmSpace.x2)) {
            FmTextField(
                value = typed,
                onValueChange = { typed = it },
                modifier = Modifier.weight(1f),
                hint = hint,
            )
            Spacer(Modifier.height(FmSpace.x1))
            FmToneButton("Send", onPressed = {
                terminal.submit(typed)
                typed = ""
            }, tone = FmTone.Accent)
        }
    }
}

internal class AgentLoginTerminalScreen(private val node: String, private val provider: String) : Screen() {
    private val signedIn = AgentsFeature.signedIn()
    private var terminal by mutableStateOf(AgentLoginTerminal(node, provider, AgentsFeature.loginChannel(node)))
    private val status = FmStatus()

    override val title: String get() = "Sign in to ${agentProviderLabel(provider)}"

    override fun onDisposed() {
        val last = terminal
        Thread { last.close() }.start()
    }

    private fun restart() {
        val old = terminal
        terminal = AgentLoginTerminal(node, provider, AgentsFeature.loginChannel(node))
        Thread { old.close() }.start()
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val uri = LocalUriHandler.current
        val clipboard = LocalClipboardManager.current
        val terminal = terminal
        val tick = rememberTerminalTick(terminal)
        if (!signedIn) {
            DetailScaffold(title, navigator) {
                FmEmptyState("lock_outline", "Sign in to ForkMesh first", message = "A node only streams its agent sign-in to the account that owns it.")
            }
            return
        }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("copy_all_outlined", "Copy terminal text", onPressed = {
                clipboard.setText(AnnotatedString(terminal.plainText))
                status.show("Terminal text copied.")
            })
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
                    if (terminal.canRestart) {
                        FmToneButton("Start again", onPressed = { restart() }, icon = "refresh")
                    }
                }
                FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x3).padding(bottom = FmSpace.x2))
                val link = terminal.signInLink
                if (link.isNotEmpty()) {
                    Row(Modifier.padding(horizontal = FmSpace.x3), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                        FmFilledButton("Open sign-in page", onClick = { runCatching { uri.openUri(link) } }, icon = "open_in_new")
                        FmToneButton("Copy link", onPressed = { clipboard.setText(AnnotatedString(link)) }, icon = "content_copy")
                    }
                    Spacer(Modifier.height(FmSpace.x2))
                }
                AgentTerminalPane(terminal, tick, "Type, then send (a code, a choice)", Modifier.weight(1f).fillMaxWidth())
            }
        }
    }
}
