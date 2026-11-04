package com.forkmesh.nativeapp.feature.reticulum

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.DisableSelection
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.feature.tools.copyToClipboard
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.reticulum.DestinationRecord
import com.forkmesh.nativeapp.reticulum.DestinationSort
import com.forkmesh.nativeapp.reticulum.InterfaceConfig
import com.forkmesh.nativeapp.reticulum.InterfaceKind
import com.forkmesh.nativeapp.reticulum.InterfaceView
import com.forkmesh.nativeapp.reticulum.LinkState
import com.forkmesh.nativeapp.reticulum.MapGroup
import com.forkmesh.nativeapp.reticulum.ReticulumConfig
import com.forkmesh.nativeapp.reticulum.ReticulumFormat
import com.forkmesh.nativeapp.reticulum.ReticulumSnapshot
import com.forkmesh.nativeapp.reticulum.Rns
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmHoldArea
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object ReticulumFeature {
    private const val OWNER = "existing"

    fun install() {
        NavigationFeature.register(Route.Kind.Reticulum, OWNER) { ReticulumScreen() }
    }
}

internal val state get() = ReticulumState.shared

private class InterfaceEdit(val config: InterfaceConfig, val creating: Boolean)

class ReticulumScreen : Screen() {
    override val title: String get() = "Reticulum"

    private var editing by mutableStateOf<InterfaceEdit?>(null)
    private var removing by mutableStateOf<InterfaceConfig?>(null)
    private var renewing by mutableStateOf(false)
    private var composing by mutableStateOf(false)
    private var deletingConversation by mutableStateOf(false)
    private var pendingAttachment: com.forkmesh.nativeapp.reticulum.LxmfAttachment? = null
    private var saveAttachment: (com.forkmesh.nativeapp.reticulum.LxmfAttachment) -> Unit = {}

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val scope = rememberCoroutineScope()
        state.attach(context)
        LaunchedEffect(Unit) { state.refresh() }
        val snapshot = state.snapshot
        val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            if (uri == null) return@rememberLauncherForActivityResult
            scope.launch {
                val text = withContext(Dispatchers.IO) {
                    runCatching {
                        context.contentResolver.openInputStream(uri)?.use { stream ->
                            val bytes = stream.readNBytes(ReticulumState.MAX_IMPORT_BYTES + 1)
                            if (bytes.size > ReticulumState.MAX_IMPORT_BYTES) null else String(bytes, Charsets.UTF_8)
                        }
                    }.getOrNull()
                }
                val name = uri.lastPathSegment?.substringAfterLast('/')?.ifEmpty { null } ?: "the file"
                if (text == null) state.show("Could not read $name.", true) else state.importConfig(text, name)
            }
        }
        val importConfig = { importer.launch(arrayOf("*/*")) }
        val saver = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri: Uri? ->
            val attachment = pendingAttachment
            pendingAttachment = null
            if (uri == null || attachment == null) return@rememberLauncherForActivityResult
            scope.launch {
                val saved = withContext(Dispatchers.IO) {
                    runCatching { context.contentResolver.openOutputStream(uri, "wt")?.use { it.write(attachment.data) } != null }.getOrDefault(false)
                }
                state.show(if (saved) "Saved ${attachment.name}." else "Could not save ${attachment.name}.", !saved)
            }
        }
        saveAttachment = { attachment ->
            pendingAttachment = attachment
            saver.launch(attachment.name.ifEmpty { "attachment" })
        }
        val watching = state.tab == ReticulumTab.Messages && state.currentPeer.isNotEmpty()
        androidx.compose.runtime.DisposableEffect(watching) {
            state.setWatching(watching)
            if (watching) ReticulumNotifier.cancel(context)
            onDispose { state.setWatching(false) }
        }
        if (composing) NewMessageDialog(snapshot, onDismiss = { composing = false }) { hex ->
            composing = false
            state.openConversation(hex)
        }
        if (deletingConversation) {
            Confirm(
                "Delete conversation",
                "Delete every message with ${state.conversations.firstOrNull { it.peerHex == state.currentPeer }?.name ?: "this contact"} from this phone?",
                "Delete",
                onConfirm = { state.deleteConversation() },
                onDismiss = { deletingConversation = false },
            )
        }
        val copy = { text: String, what: String ->
            if (text.isNotEmpty()) {
                copyToClipboard(context, text)
                state.show("$what copied.")
            }
        }
        editing?.let { edit ->
            InterfaceEditor(edit, onDismiss = { editing = null }) { saved ->
                editing = null
                if (edit.creating) state.addInterface(saved) else state.updateInterface(saved)
            }
        }
        removing?.let { config ->
            Confirm(
                "Remove interface",
                "Remove ${config.name}? Destinations heard through it stay in the list.",
                "Remove",
                onConfirm = { state.removeInterface(config.id) },
                onDismiss = { removing = null },
            )
        }
        if (renewing) {
            Confirm(
                "Make a new identity",
                "This phone gets a new destination hash. Anyone who knew the old one can no longer reach this phone through it. Continue?",
                "Make a new identity",
                onConfirm = { state.newIdentity() },
                onDismiss = { renewing = false },
            )
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar("Reticulum", navigator)
            LazyColumn(Modifier.fillMaxSize()) {
                item(key = "header") { Header(snapshot) }
                if (state.message.isNotEmpty()) {
                    item(key = "message") {
                        FmInlineStatusLine(
                            state.message,
                            state.messageIsError,
                            Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
                            onDismiss = { state.show("") },
                        )
                    }
                }
                val hasRemote = snapshot.interfaces.any { it.config.kind != InterfaceKind.SharedInstance }
                if (!hasRemote && snapshot.onlineCount == 0) item(key = "onboarding") { Onboarding(importConfig) }
                item(key = "tiles") { Tiles(snapshot) }
                item(key = "tabs") { TabStrip() }
                when (state.tab) {
                    ReticulumTab.Network -> networkTab(snapshot, copy)
                    ReticulumTab.Messages -> messagesTab(snapshot, copy, onNew = { composing = true }, onDeleteConversation = { deletingConversation = true }, onSave = { saveAttachment(it) })
                    ReticulumTab.Interfaces -> interfacesTab(snapshot, importConfig, onEdit = { editing = it }, onRemove = { removing = it })
                    ReticulumTab.Destinations -> destinationsTab(snapshot, copy)
                    ReticulumTab.Activity -> activityTab(snapshot)
                    ReticulumTab.Identity -> identityTab(snapshot, copy) { renewing = true }
                }
                item(key = "end") { Spacer(Modifier.height(FmSpace.x7)) }
            }
        }
    }
}

@Composable
private fun Header(snapshot: ReticulumSnapshot) {
    Column(Modifier.fillMaxWidth().padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x2, bottom = FmSpace.x1)) {
        Text(ReticulumFormat.summary(snapshot), style = Fm.text.bodySmall, color = Fm.colors.textSecondary, maxLines = 2, overflow = TextOverflow.Ellipsis)
        Spacer(Modifier.height(FmSpace.x2))
        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            FmToneButton("Announce", onPressed = if (snapshot.onlineCount > 0) ({ state.announceNow() }) else null, icon = "campaign", tooltip = "Tell the network this phone is here")
            FmToneButton(if (snapshot.running) "Disconnect" else "Connect", onPressed = { state.toggleNetwork() }, icon = if (snapshot.running) "link_off" else "link")
        }
    }
}

@Composable
private fun Onboarding(importConfig: () -> Unit) {
    FmCard(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2), padding = PaddingValues(FmSpace.x3)) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Text(
                "Reticulum is an encrypted mesh network that runs over the internet, LoRa radios, packet radio and local networks. " +
                    "Join a public entry point to see it, or connect to an app on this phone that shares its Reticulum instance, such as Sideband.",
                style = Fm.text.bodySmall,
                color = Fm.colors.textSecondary,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                FmToneButton("Join the public network", onPressed = { state.joinPublicNetwork() }, tone = FmTone.Accent, filled = true)
                FmToneButton("Import a Reticulum config", onPressed = importConfig)
            }
        }
    }
}

@Composable
private fun Tiles(snapshot: ReticulumSnapshot) {
    val tiles = ReticulumFormat.tiles(snapshot)
    Row(
        Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
    ) {
        StatTile("Connection", tiles.status, tiles.statusDetail, tone = tiles.statusTone)
        StatTile("Destinations", tiles.destinations, tiles.destinationsDetail)
        StatTile("Announces", tiles.announces, tiles.announcesDetail, spark = tiles.announceSpark)
        StatTile("Traffic", tiles.traffic, tiles.trafficDetail, spark = tiles.trafficSpark)
        val conversations = state.conversations.size
        StatTile(
            "Messages",
            if (state.unread > 0) "${state.unread} unread" else "$conversations",
            "${ReticulumFormat.plural(conversations, "conversation")} · ${ReticulumFormat.plural(state.activeLinks, "link")} open",
            tone = if (state.unread > 0) 1 else 0,
        )
        StatTile("Path lookups", tiles.paths, tiles.pathsDetail)
    }
}

@Composable
private fun TabStrip() {
    val colors = Fm.colors
    FmHoldArea {
        Row(
            Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = FmSpace.x4, vertical = FmSpace.x1)
                .height(38.dp)
                .fmGlass(RoundedCornerShape(FmRadius.lg), shadow = false)
                .padding(horizontal = FmSpace.x1),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            for (tab in ReticulumTab.entries) {
                val selected = state.tab == tab
                Column(
                    Modifier
                        .fillMaxHeight()
                        .selectable(selected = selected, role = Role.Tab, onClick = { state.tab = tab })
                        .padding(horizontal = FmSpace.x3),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                        Text(
                            tab.label,
                            style = Fm.text.titleSmall.copy(
                                color = if (selected) colors.textPrimary else colors.textTertiary,
                                fontWeight = if (selected) FmType.emphasis else FmType.regular,
                            ),
                        )
                    }
                    Box(Modifier.width(24.dp).height(2.dp).background(if (selected) colors.activeUnderline else Color.Transparent, RoundedCornerShape(2.dp)))
                }
            }
        }
    }
}

@Composable
internal fun Section(title: String, trailing: (@Composable () -> Unit)? = null) {
    FmSectionHeader(title, Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x4, bottom = FmSpace.x1), trailing = trailing)
}

@Composable
internal fun Note(text: String, color: Color = Fm.colors.textTertiary) {
    Text(text, style = Fm.text.bodySmall, color = color, modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1))
}

@Composable
internal fun Field(value: String, onChange: (String) -> Unit, hint: String, modifier: Modifier = Modifier, keyboard: KeyboardType = KeyboardType.Text, onDone: (() -> Unit)? = null) {
    FmTextField(
        value = value,
        onValueChange = onChange,
        hint = hint,
        keyboardOptions = KeyboardOptions(keyboardType = keyboard, autoCorrectEnabled = false),
        keyboardActions = androidx.compose.foundation.text.KeyboardActions(onDone = { onDone?.invoke() }),
        trailing = if (value.isEmpty()) null else {
            { FmIcon("close", size = 18.dp, tint = Fm.colors.textTertiary, modifier = Modifier.clickable { onChange("") }) }
        },
        modifier = modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
    )
}

@Composable
internal fun KeyValues(rows: List<Pair<String, String>>) {
    SelectionContainer {
        Column(Modifier.padding(horizontal = FmSpace.x4)) {
            for ((label, value) in rows) {
                Row(Modifier.padding(vertical = 3.dp)) {
                    Text(label, style = Fm.text.bodySmall, color = Fm.colors.textTertiary, modifier = Modifier.width(118.dp))
                    val mono = value.length >= 32 && value.all { it.isLetterOrDigit() }
                    Text(value, style = if (mono) Fm.mono(color = Fm.colors.textPrimary) else Fm.text.bodySmall, color = Fm.colors.textPrimary, modifier = Modifier.weight(1f))
                }
            }
        }
    }
}

@Composable
internal fun Actions(content: @Composable () -> Unit) {
    @OptIn(ExperimentalLayoutApi::class)
    FlowRow(
        Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
    ) { content() }
}

@Composable
internal fun Chip(label: String, selected: Boolean, group: MapGroup? = null, onSelected: () -> Unit) {
    val colors = Fm.colors
    val shape = RoundedCornerShape(FmRadius.md)
    Row(
        Modifier
            .height(30.dp)
            .clip(shape)
            .background(if (selected) colors.accentSubtle else colors.bgRaised)
            .border(1.dp, if (selected) colors.accent else colors.border, shape)
            .selectable(selected = selected, role = Role.RadioButton, onClick = onSelected)
            .padding(horizontal = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        if (group != null) GroupMarker(group)
        Text(label, maxLines = 1, style = Fm.text.bodyMedium.copy(color = if (selected) colors.accent else colors.textSecondary, fontSize = FmType.meta, fontWeight = FmType.emphasis))
    }
}

@Composable
internal fun Confirm(title: String, text: String, action: String, onConfirm: () -> Unit, onDismiss: () -> Unit) {
    DisableSelection {
        AlertDialog(
            onDismissRequest = onDismiss,
            title = { Text(title) },
            text = { Text(text) },
            confirmButton = { TextButton(onClick = { onDismiss(); onConfirm() }) { Text(action, color = Fm.colors.danger) } },
            dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
        )
    }
}

private fun stateColorTone(state: LinkState): FmTone = when (state) {
    LinkState.Online -> FmTone.Success
    LinkState.Connecting, LinkState.Waiting -> FmTone.Warning
    LinkState.Blocked, LinkState.Failed -> FmTone.Danger
    LinkState.Disabled -> FmTone.Neutral
}

@Composable
private fun stateColor(state: LinkState): Color = when (stateColorTone(state)) {
    FmTone.Success -> Fm.colors.success
    FmTone.Warning -> Fm.colors.warning
    FmTone.Danger -> Fm.colors.danger
    else -> Fm.colors.textTertiary
}

private fun LazyListScope.networkTab(snapshot: ReticulumSnapshot, copy: (String, String) -> Unit) {
    item(key = "map-header") {
        Section("Network map")
        Field(state.mapFilter, { state.mapFilter = it }, "Highlight by name or hash")
    }
    item(key = "map") {
        val spokes = remember(snapshot.interfaces) { snapshot.interfaces.map { MapSpoke(it.config.id, it.config.name, it.status.state == LinkState.Online) } }
        val records = remember(snapshot.destinations) { snapshot.destinations.values.toList() }
        NetworkMap(
            spokes = spokes,
            destinations = records,
            selfOnline = snapshot.onlineCount > 0,
            selected = state.selected,
            filter = state.mapFilter,
            pulses = state.pulses,
            nowMs = snapshot.at,
            onSelect = { state.select(it) },
            modifier = Modifier.fillMaxWidth().aspectRatio(0.92f).padding(horizontal = FmSpace.x2),
        )
    }
    item(key = "traffic") {
        FmCard(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2), padding = PaddingValues(FmSpace.x3)) {
            TrafficChart(snapshot.history)
        }
    }
    item(key = "detail") { SelectedDestination(snapshot, copy) }
}

@Composable
private fun SelectedDestination(snapshot: ReticulumSnapshot, copy: (String, String) -> Unit) {
    val record = snapshot.destinations[state.selected]
    Section("Selected destination")
    if (record == null) {
        Note("Tap a point on the map or a row in Destinations.")
        return
    }
    Row(Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1), verticalAlignment = Alignment.CenterVertically) {
        GroupMarker(MapGroup.of(record.kind))
        Spacer(Modifier.width(FmSpace.x2))
        SelectionContainer { Text(record.displayName.ifEmpty { record.label }, style = Fm.text.titleMedium) }
    }
    KeyValues(ReticulumFormat.detailRows(record, snapshot))
    Actions {
        if (record.aspect == com.forkmesh.nativeapp.reticulum.Lxmf.DELIVERY_ASPECT) {
            FmToneButton("Message", onPressed = { state.openConversation(record.hex) }, icon = "chat", tone = FmTone.Accent, filled = true)
        }
        FmToneButton("Find path", onPressed = if (snapshot.onlineCount > 0) ({ state.requestPath(record.hash) }) else null, icon = "route")
        FmToneButton("Copy hash", onPressed = { copy(record.hex, "Destination hash") }, icon = "content_copy")
        FmToneButton("Forget", onPressed = { state.forget(record.hex) }, icon = "delete", tone = FmTone.Danger)
    }
}

private fun LazyListScope.interfacesTab(
    snapshot: ReticulumSnapshot,
    importConfig: () -> Unit,
    onEdit: (InterfaceEdit) -> Unit,
    onRemove: (InterfaceConfig) -> Unit,
) {
    item(key = "if-actions") {
        var publicOpen by remember { mutableStateOf(false) }
        Actions {
            FmToneButton("Add interface", onPressed = { onEdit(InterfaceEdit(InterfaceConfig(id = InterfaceConfig.newId(), name = "New interface"), creating = true)) }, icon = "add", tone = FmTone.Accent, filled = true)
            Box {
                FmToneButton("Add public entry point", onPressed = { publicOpen = true }, icon = "language")
                FmDropdownMenu(expanded = publicOpen, onDismissRequest = { publicOpen = false }) {
                    for (entry in ReticulumConfig.publicEntryPoints()) {
                        FmDropdownMenuItem("${entry.name} (${entry.endpoint})", onClick = {
                            publicOpen = false
                            state.addPublicEntry(entry)
                        })
                    }
                }
            }
            FmToneButton("Import Reticulum config…", onPressed = importConfig, icon = "upload_file")
        }
    }
    if (snapshot.interfaces.isEmpty()) item(key = "if-empty") { Note("No interfaces yet. Add one, or join a public entry point.") }
    items(snapshot.interfaces, key = { "if-${it.config.id}" }) { view -> InterfaceRow(view, snapshot) }
    val selected = snapshot.interfaces.firstOrNull { it.config.id == state.selectedInterface }
    item(key = "if-detail") {
        Section(selected?.config?.name ?: "Interface details")
        if (selected == null) {
            Note("Tap an interface to see its details.")
        } else {
            SelectionContainer {
                Column(Modifier.padding(horizontal = FmSpace.x4)) {
                    for (line in ReticulumFormat.interfaceLines(selected, snapshot)) Text(line, style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                }
            }
            Actions {
                FmToneButton("Reconnect", onPressed = if (snapshot.running && selected.config.enabled) ({ state.reconnect(selected.config.id) }) else null, icon = "refresh")
                FmToneButton(if (selected.config.enabled) "Turn off" else "Turn on", onPressed = { state.toggleInterface(selected.config.id) }, icon = "power_settings_new")
                FmToneButton("Edit", onPressed = { onEdit(InterfaceEdit(selected.config, creating = false)) }, icon = "edit")
                FmToneButton("Remove", onPressed = { onRemove(selected.config) }, icon = "delete", tone = FmTone.Danger)
            }
            FmCard(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2), padding = PaddingValues(FmSpace.x3)) {
                TrafficChart(selected.status.history, title = "This interface, last 3 minutes")
            }
        }
    }
    val discovered = snapshot.destinations.values.filter { it.discovery != null }.sortedByDescending { it.lastHeard }
    item(key = "disc-header") {
        Section("Entry points announced on the network")
        if (discovered.isEmpty()) Note("Transport nodes that publish their interfaces show here with their proof of work.")
    }
    items(discovered, key = { "disc-${it.hex}" }) { record -> DiscoveredRow(record, snapshot, onEdit) }
}

@Composable
private fun InterfaceRow(view: InterfaceView, snapshot: ReticulumSnapshot) {
    val config = view.config
    val status = view.status
    val selected = state.selectedInterface == config.id
    val online = if (status.onlineSince > 0) ReticulumFormat.duration((snapshot.at - status.onlineSince) / 1000) else "—"
    FmListRow(
        config.name,
        subtitle = "${config.kindLabel} · ${config.endpoint}\n" +
            "Up $online · ↓ ${ReticulumFormat.bytes(status.rxBytes.toDouble())} · ↑ ${ReticulumFormat.bytes(status.txBytes.toDouble())} · " +
            "${status.rxPackets} / ${status.txPackets} packets · ${ReticulumFormat.plural(status.announces, "announce")}",
        leading = { FmIcon(if (config.kind == InterfaceKind.Udp) "wifi" else if (config.kind == InterfaceKind.SharedInstance) "hub" else "language", tint = stateColor(status.state)) },
        trailing = { FmStatusBadge(status.state.label, stateColor(status.state)) },
        onTap = { state.selectedInterface = if (selected) "" else config.id },
        showChevron = false,
        titleColor = if (selected) Fm.colors.accent else null,
    )
}

@Composable
private fun DiscoveredRow(record: DestinationRecord, snapshot: ReticulumSnapshot, onEdit: (InterfaceEdit) -> Unit) {
    val found = record.discovery ?: return
    val usable = found.connectable && found.stampValid
    Column {
        FmListRow(
            found.name.ifEmpty { "Unnamed" },
            subtitle = "${found.type} · ${ReticulumFormat.discoveredAddress(found)}\n" +
                "Transport ${if (found.transport) "yes" else "no"} · proof of work ${found.stampValue} bits${if (found.stampValid) "" else " (low)"} · " +
                "${ReticulumFormat.location(found)} · heard ${ReticulumFormat.age(record.lastHeard, snapshot.at)}",
            leading = { GroupMarker(MapGroup.Transport) },
            trailing = if (usable) ({ FmToneButton("Connect", onPressed = { onEdit(InterfaceEdit(ReticulumFormat.interfaceFromDiscovery(found), creating = true)) }) }) else null,
            showChevron = false,
            titleColor = if (usable) null else Fm.colors.textTertiary,
        )
        if (found.networkName.isNotEmpty()) Note("Needs the network name ${found.networkName}, which this connector does not support yet")
    }
}

private fun LazyListScope.destinationsTab(snapshot: ReticulumSnapshot, copy: (String, String) -> Unit) {
    item(key = "lookup") {
        Section("Find a path to")
        Field(state.lookupInput, { state.lookupInput = it; state.lookupError = "" }, "Destination hash, 32 hex characters", keyboard = KeyboardType.Ascii, onDone = { state.lookupTyped() })
        Actions { FmToneButton("Find path", onPressed = { state.lookupTyped() }, icon = "route", tone = FmTone.Accent, filled = true) }
        val lookup = Rns.parseHash(state.lookupInput)?.let { snapshot.lookups[Rns.hex(it)] }
        when {
            state.lookupError.isNotEmpty() -> Note(state.lookupError, Fm.colors.danger)
            lookup != null -> Note(ReticulumFormat.lookupText(lookup, snapshot), Fm.colors.textSecondary)
        }
    }
    val visible = ReticulumFormat.visibleDestinations(snapshot.destinations.values, state.destinationFilter, state.kindFilter, state.sort)
    item(key = "filters") {
        Section("Destinations", trailing = {
            Text(
                if (visible.size == snapshot.destinations.size) "${snapshot.destinations.size} destinations" else "${visible.size} of ${snapshot.destinations.size}",
                style = Fm.text.bodySmall,
                color = Fm.colors.textTertiary,
            )
        })
        Field(state.destinationFilter, { state.destinationFilter = it }, "Filter by name, application or hash")
        Row(
            Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            Chip("Every kind", state.kindFilter == null) { state.kindFilter = null }
            for (group in MapGroup.entries) Chip(group.label, state.kindFilter == group, group) { state.kindFilter = if (state.kindFilter == group) null else group }
        }
        var sortOpen by remember { mutableStateOf(false) }
        Actions {
            Box {
                FmToneButton("Sort: ${state.sort.label}", onPressed = { sortOpen = true }, icon = "sort")
                FmDropdownMenu(expanded = sortOpen, onDismissRequest = { sortOpen = false }) {
                    for (option in DestinationSort.entries) {
                        FmDropdownMenuItem(option.label, onClick = {
                            sortOpen = false
                            state.sort = option
                        }, icon = if (option == state.sort) "check" else null)
                    }
                }
            }
            FmToneButton("Clear list", onPressed = if (snapshot.destinations.isEmpty()) null else ({ state.clearDestinations() }), icon = "delete_sweep")
        }
    }
    items(visible, key = { "dest-${it.hex}" }) { record -> DestinationRow(record, snapshot, copy) }
}

@Composable
private fun DestinationRow(record: DestinationRecord, snapshot: ReticulumSnapshot, copy: (String, String) -> Unit) {
    var menu by remember { mutableStateOf(false) }
    Box {
        FmListRow(
            record.displayName.ifEmpty { "—" },
            subtitle = "${record.label} · ${ReticulumFormat.hops(record.hops)} via ${snapshot.interfaceName(record.interfaceId)} · " +
                "${ReticulumFormat.age(record.lastHeard, snapshot.at)} · ${ReticulumFormat.plural(record.announces, "announce")}\n${record.hex}",
            leading = { GroupMarker(MapGroup.of(record.kind)) },
            onTap = { state.select(record.hex, showMap = true) },
            onLongPress = { menu = true },
            titleColor = if (state.selected == record.hex) Fm.colors.accent else null,
        )
        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
            if (record.aspect == com.forkmesh.nativeapp.reticulum.Lxmf.DELIVERY_ASPECT) {
                FmDropdownMenuItem("Message", onClick = { menu = false; state.openConversation(record.hex) }, icon = "chat")
            }
            FmDropdownMenuItem("Show on map", onClick = { menu = false; state.select(record.hex, showMap = true) }, icon = "hub")
            FmDropdownMenuItem("Find path", onClick = { menu = false; state.requestPath(record.hash) }, icon = "route")
            FmDropdownMenuItem("Copy destination hash", onClick = { menu = false; copy(record.hex, "Destination hash") }, icon = "content_copy")
            FmDropdownMenuItem("Copy identity hash", onClick = { menu = false; copy(Rns.hex(record.identityHash), "Identity hash") }, icon = "content_copy")
            FmDropdownMenuItem("Forget", onClick = { menu = false; state.forget(record.hex) }, icon = "delete", textColor = Fm.colors.danger)
        }
    }
}

private val clockFormat: DateTimeFormatter = DateTimeFormatter.ofPattern("HH:mm:ss")

private fun LazyListScope.activityTab(snapshot: ReticulumSnapshot) {
    item(key = "counters") {
        Section("Packets since you connected")
        FmCard(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x1), padding = PaddingValues(FmSpace.x3)) {
            Column {
                for ((label, value) in ReticulumFormat.counterRows(snapshot.counters)) {
                    Row(Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
                        Text(label, style = Fm.text.bodySmall, color = Fm.colors.textSecondary, modifier = Modifier.weight(1f))
                        Text("$value", style = Fm.text.bodySmall.copy(fontFeatureSettings = FmType.tabular), color = Fm.colors.textPrimary)
                    }
                }
            }
        }
        Section("Activity")
        if (snapshot.events.isEmpty()) Note("Announces, path answers, interfaces coming and going and rejected packets show here.")
    }
    items(snapshot.events.size, key = { "ev-${snapshot.events[it].at}-$it" }) { index ->
        val event = snapshot.events[index]
        val time = clockFormat.format(Instant.ofEpochMilli(event.at).atZone(ZoneId.systemDefault()))
        val hex = Rns.hex(event.destination)
        val openable = event.destination.isNotEmpty() && snapshot.destinations.containsKey(hex)
        Row(
            Modifier
                .fillMaxWidth()
                .then(if (openable) Modifier.clickable { state.select(hex, showMap = true) } else Modifier)
                .padding(horizontal = FmSpace.x4, vertical = 6.dp),
        ) {
            Text(time, style = Fm.mono(color = Fm.colors.textTertiary), modifier = Modifier.width(72.dp))
            Text(event.text, style = Fm.text.bodySmall, color = if (event.kind == "rejected") Fm.colors.warning else Fm.colors.textPrimary, modifier = Modifier.weight(1f))
        }
    }
}

private fun LazyListScope.identityTab(snapshot: ReticulumSnapshot, copy: (String, String) -> Unit, onRenew: () -> Unit) {
    item(key = "identity") {
        Section("This phone on Reticulum")
        Note(
            "ForkMesh keeps a Reticulum identity for this phone: an X25519 key for encryption and an Ed25519 key for signing, " +
                "stored encrypted by the Android keystore. Announcing publishes its public key and the name below under the forkmesh.node " +
                "application, so others can find a path to it. Nothing is announced until you choose to.",
            Fm.colors.textSecondary,
        )
        HashRow("Destination", snapshot.destinationHash, copy, "Destination hash")
        HashRow("Identity", snapshot.identityHash, copy, "Identity hash")
        HashRow("Messaging address", state.address, copy, "Messaging address")
        Section("Name in announces")
        Field(state.announceName, { state.announceName = it.take(64) }, "Name shown to others, for example your handle", onDone = { state.saveAnnounceName() })
        Actions { FmToneButton("Save name", onPressed = { state.saveAnnounceName() }, icon = "check") }
        Section("Announce")
        IntervalPicker()
        Row(
            Modifier.fillMaxWidth().clickable { state.changeAnswerPaths(!state.answerPaths) }.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Answer path requests for this phone", style = Fm.text.bodyMedium, modifier = Modifier.weight(1f))
            FmSwitch(checked = state.answerPaths, onCheckedChange = { state.changeAnswerPaths(it) })
        }
        Note("Last announce: " + if (snapshot.lastAnnounce > 0) ReticulumFormat.age(snapshot.lastAnnounce, snapshot.at) else "Not announced since ForkMesh started", Fm.colors.textSecondary)
        Actions {
            FmToneButton("Announce now", onPressed = if (snapshot.onlineCount > 0) ({ state.announceNow() }) else null, icon = "campaign", tone = FmTone.Accent, filled = true)
            FmToneButton("Make a new identity", onPressed = onRenew, icon = "sync", tone = FmTone.Danger)
        }
    }
}

@Composable
internal fun HashRow(label: String, hash: ByteArray, copy: (String, String) -> Unit, what: String) {
    Row(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x1), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(label, style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
            SelectionContainer { Text(Rns.prettyHash(hash), style = Fm.mono(color = Fm.colors.textPrimary)) }
        }
        FmToneButton("Copy", onPressed = { copy(Rns.hex(hash), what) }, icon = "content_copy")
    }
}

@Composable
private fun IntervalPicker() {
    var open by remember { mutableStateOf(false) }
    val label = ReticulumFormat.announceIntervals.firstOrNull { it.first == state.announceMinutes }?.second.orEmpty()
    Box(Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1)) {
        FmToneButton(label, onPressed = { open = true }, icon = "schedule")
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for ((minutes, text) in ReticulumFormat.announceIntervals) {
                FmDropdownMenuItem(text, onClick = {
                    open = false
                    state.changeAnnounceMinutes(minutes)
                }, icon = if (minutes == state.announceMinutes) "check" else null)
            }
        }
    }
}

@Composable
private fun InterfaceEditor(edit: InterfaceEdit, onDismiss: () -> Unit, onSave: (InterfaceConfig) -> Unit) {
    val initial = edit.config
    var name by remember { mutableStateOf(initial.name) }
    var kind by remember { mutableStateOf(initial.kind) }
    var host by remember { mutableStateOf(initial.host) }
    var port by remember { mutableStateOf((if (initial.kind == InterfaceKind.TcpClient) initial.port else 4242).toString()) }
    var kiss by remember { mutableStateOf(initial.kiss) }
    var sharedPort by remember { mutableStateOf((if (initial.kind == InterfaceKind.SharedInstance) initial.port else InterfaceConfig.SHARED_INSTANCE_PORT).toString()) }
    var listenAddress by remember { mutableStateOf(initial.listenAddress) }
    var listenPort by remember { mutableStateOf(initial.listenPort.toString()) }
    var forwardAddress by remember { mutableStateOf(initial.forwardAddress) }
    var forwardPort by remember { mutableStateOf(initial.forwardPort.toString()) }
    var networkName by remember { mutableStateOf(initial.networkName) }
    var passphrase by remember { mutableStateOf(initial.passphrase) }
    var accessSize by remember { mutableStateOf(initial.accessCodeSize.toString()) }
    var error by remember { mutableStateOf("") }
    fun number(text: String) = text.trim().toIntOrNull() ?: 0
    DisableSelection {
        AlertDialog(
            onDismissRequest = onDismiss,
            title = { Text(if (edit.creating) "Add a Reticulum interface" else "Edit interface") },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                    EditorField("Name", name, { name = it.take(80) })
                    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x1), modifier = Modifier.horizontalScroll(rememberScrollState())) {
                        Chip("TCP", kind == InterfaceKind.TcpClient) { kind = InterfaceKind.TcpClient }
                        Chip("On this phone", kind == InterfaceKind.SharedInstance) { kind = InterfaceKind.SharedInstance }
                        Chip("UDP", kind == InterfaceKind.Udp) { kind = InterfaceKind.Udp }
                    }
                    when (kind) {
                        InterfaceKind.TcpClient -> {
                            EditorField("Host", host, { host = it.trim() }, "rmap.world", KeyboardType.Uri)
                            EditorField("Port", port, { port = it.filter(Char::isDigit).take(5) }, "4242", KeyboardType.Number)
                            Row(Modifier.fillMaxWidth().clickable { kiss = !kiss }.padding(vertical = FmSpace.x1), verticalAlignment = Alignment.CenterVertically) {
                                Text("KISS framing (for a TNC or modem reached over TCP)", style = Fm.text.bodySmall, modifier = Modifier.weight(1f))
                                FmSwitch(checked = kiss, onCheckedChange = { kiss = it })
                            }
                        }
                        InterfaceKind.SharedInstance -> {
                            EditorField("Port", sharedPort, { sharedPort = it.filter(Char::isDigit).take(5) }, "37428", KeyboardType.Number)
                            Text(
                                "Connects to 127.0.0.1 on this port, where an app running Reticulum with a shared instance (such as Sideband) listens. " +
                                    "ForkMesh then shares its interfaces, radios included, and its paths.",
                                style = Fm.text.bodySmall,
                                color = Fm.colors.textTertiary,
                            )
                        }
                        InterfaceKind.Udp -> {
                            EditorField("Listen on", listenAddress, { listenAddress = it.trim() }, "0.0.0.0", KeyboardType.Uri)
                            EditorField("Listen port", listenPort, { listenPort = it.filter(Char::isDigit).take(5) }, "4242", KeyboardType.Number)
                            EditorField("Send to", forwardAddress, { forwardAddress = it.trim() }, "255.255.255.255", KeyboardType.Uri)
                            EditorField("Send port", forwardPort, { forwardPort = it.filter(Char::isDigit).take(5) }, "4242", KeyboardType.Number)
                        }
                    }
                    Text("Interface access code", style = Fm.text.titleSmall, color = Fm.colors.textSecondary, modifier = Modifier.padding(top = FmSpace.x2))
                    EditorField("Network name", networkName, { networkName = it.take(256) }, "Leave empty for an open network")
                    EditorField("Passphrase", passphrase, { passphrase = it.take(256) }, "Leave empty for an open network", KeyboardType.Password, secret = true)
                    EditorField("Access code size (bytes)", accessSize, { accessSize = it.filter(Char::isDigit).take(2) }, "16", KeyboardType.Number)
                    Text(
                        "A virtual network on a shared medium (IFAC): only nodes with the same name and passphrase see each other's packets. The passphrase is sealed by the Android keystore.",
                        style = Fm.text.bodySmall,
                        color = Fm.colors.textTertiary,
                    )
                    if (error.isNotEmpty()) Text(error, style = Fm.text.bodySmall, color = Fm.colors.danger)
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    val result = initial.copy(
                        name = name.trim(),
                        kind = kind,
                        host = host.trim(),
                        kiss = kiss,
                        sharedOverTcp = true,
                        port = if (kind == InterfaceKind.SharedInstance) number(sharedPort) else number(port),
                        listenAddress = listenAddress.trim(),
                        listenPort = number(listenPort),
                        forwardAddress = forwardAddress.trim(),
                        forwardPort = number(forwardPort),
                        networkName = networkName.trim(),
                        passphrase = passphrase,
                        accessCodeSize = number(accessSize).takeIf { it > 0 } ?: 16,
                    )
                    val problem = result.validationError
                    if (problem.isNotEmpty()) error = problem else onSave(result)
                }) { Text("Save") }
            },
            dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
        )
    }
}

@Composable
private fun EditorField(label: String, value: String, onChange: (String) -> Unit, hint: String = "", keyboard: KeyboardType = KeyboardType.Text, secret: Boolean = false) {
    Text(label, style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
    FmTextField(
        value = value,
        onValueChange = onChange,
        hint = hint.ifEmpty { null },
        keyboardOptions = KeyboardOptions(keyboardType = keyboard, autoCorrectEnabled = false),
        visualTransformation = if (secret) androidx.compose.ui.text.input.PasswordVisualTransformation() else androidx.compose.ui.text.input.VisualTransformation.None,
        modifier = Modifier.fillMaxWidth(),
    )
}
