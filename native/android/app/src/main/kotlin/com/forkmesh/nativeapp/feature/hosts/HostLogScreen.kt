package com.forkmesh.nativeapp.feature.hosts

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.text.selection.SelectionContainer
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
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.hosts.HostLogLine
import com.forkmesh.nativeapp.hosts.HostLogStream
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.launch

internal class HostLogScreen(private val host: String) : Screen() {
    override val title: String get() = host
    private val stream: HostLogStream = HostsFeature.logStream(host)
    private var query by mutableStateOf("")
    private var follow by mutableStateOf(true)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val clipboard = LocalClipboardManager.current
        val status = rememberFmStatus()
        var tick by remember { mutableIntStateOf(0) }
        DisposableEffect(stream) {
            val listener: () -> Unit = { tick++ }
            stream.addListener(listener)
            onDispose { stream.removeListener(listener) }
        }
        LaunchedEffect(stream) { offMain { stream.start() } }
        val paused = stream.read { it.paused }
        val lines = remember(tick, query) { stream.read { it.filtered(query) } }
        val list = rememberLazyListState()
        LaunchedEffect(tick, follow) {
            if (follow && !paused && lines.isNotEmpty()) list.scrollToItem(lines.size - 1)
        }
        DetailScaffold(
            title,
            navigator,
            actions = {
                FmIconButton(
                    if (paused) "play_arrow" else "pause",
                    if (paused) "Resume" else "Pause",
                    onPressed = { stream.setPaused(!paused) },
                    color = c.textPrimary,
                )
                FmIconButton(
                    "vertical_align_bottom",
                    if (follow) "Stop following the end" else "Follow the end",
                    onPressed = { follow = !follow },
                    color = if (follow) c.accent else c.textTertiary,
                )
                FmIconButton("content_copy", if (query.isEmpty()) "Copy log" else "Copy matching lines", onPressed = {
                    clipboard.setText(AnnotatedString(stream.read { it.copyText(query) }))
                    status.show("Copied")
                }, color = c.textPrimary)
                FmIconButton("delete_outline", "Clear", onPressed = { stream.clear() }, color = c.textPrimary)
            },
        ) {
            Column(Modifier.fillMaxSize()) {
                StatusBar(tick)
                FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x3))
                FmTextField(
                    value = query,
                    onValueChange = { query = it },
                    hint = "Search the log",
                    modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x3, vertical = FmSpace.x1),
                    leading = { FmIcon("search", size = 18.dp, tint = c.textTertiary) },
                )
                LazyColumn(
                    Modifier.weight(1f).fillMaxWidth(),
                    state = list,
                    contentPadding = PaddingValues(horizontal = FmSpace.x3, vertical = FmSpace.x2),
                    verticalArrangement = Arrangement.spacedBy(2.dp),
                ) {
                    items(lines, key = { it.id }) { LogLine(it) }
                }
            }
        }
    }

    @Composable
    private fun StatusBar(tick: Int) {
        val c = Fm.colors
        tick.let { }
        val (count, held, dropped, paused) = stream.read { listOf(it.size, it.heldCount, it.dropped, if (it.paused) 1 else 0) }
        val text = if (stream.error.isNotEmpty()) stream.error else buildList {
            add(if (stream.live) (if (paused == 1) "Paused" else "Live") else "Connecting…")
            add("$count lines")
            if (paused == 1 && held > 0) add("$held waiting")
            if (dropped > 0) add("$dropped skipped")
        }.joinToString(" · ")
        Row(
            Modifier.fillMaxWidth().background(c.bgRaised).padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Dot(if (stream.live) (if (paused == 1) c.warning else c.success) else c.danger, 8)
            Spacer(Modifier.width(FmSpace.x2))
            Text(text, style = Fm.text.labelSmall.copy(color = c.textSecondary), maxLines = 1)
        }
    }

    @Composable
    private fun LogLine(line: HostLogLine) {
        val c = Fm.colors
        Row(verticalAlignment = Alignment.Top) {
            if (line.source.isNotEmpty()) {
                Text(
                    line.source,
                    style = Fm.text.labelSmall.copy(fontFamily = FmType.mono, fontSize = 10.sp, fontWeight = FontWeight.SemiBold, color = sourceColor(line.source)),
                    modifier = Modifier.width(48.dp),
                    maxLines = 1,
                )
            }
            SelectionContainer(Modifier.weight(1f)) {
                Text(line.text, style = Fm.text.labelSmall.copy(fontFamily = FmType.mono, fontSize = 11.sp, color = c.textPrimary))
            }
        }
    }

    @Composable
    private fun sourceColor(source: String): Color {
        val c = Fm.colors
        return when (source) {
            "node" -> c.success
            "mirror" -> c.accent
            "web" -> c.warning
            "ssh" -> Color(0xFF9C6ADE)
            "host" -> Color(0xFFD4A72C)
            else -> c.textTertiary
        }
    }

    @OptIn(kotlinx.coroutines.DelicateCoroutinesApi::class)
    override fun onDisposed() {
        GlobalScope.launch(Dispatchers.IO) { stream.stop() }
    }
}
