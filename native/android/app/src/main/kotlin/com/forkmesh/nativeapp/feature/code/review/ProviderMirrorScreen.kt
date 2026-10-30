package com.forkmesh.nativeapp.feature.code.review

import com.forkmesh.nativeapp.ui.kit.confirmAction
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ci.ApiProviderMirrorClient
import com.forkmesh.nativeapp.ci.ProviderMirrorCandidate
import com.forkmesh.nativeapp.ci.ProviderMirrorDraft
import com.forkmesh.nativeapp.ci.ProviderMirrorModel
import com.forkmesh.nativeapp.ci.ProviderMirrorResource
import com.forkmesh.nativeapp.ci.ProviderMirrorState
import com.forkmesh.nativeapp.ci.ProviderMirrorText
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.InfoCard
import com.forkmesh.nativeapp.feature.code.LoadingCard
import com.forkmesh.nativeapp.feature.code.collab.openExternalUrl
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmCheckbox
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch

internal class ProviderMirrorScreen(private val repo: Repository) : Screen() {
    private val model = ProviderMirrorModel(ApiProviderMirrorClient(CodeServices.api), CodeServices.ref(repo))
    override val title: String get() = "Mirror to GitHub or GitLab"

    @Composable
    override fun Content(navigator: Navigator) {
        val tick = rememberTick()
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val context = LocalContext.current
        var draft by remember { mutableStateOf(ProviderMirrorDraft()) }
        var editing by remember { mutableStateOf(false) }
        var canEdit by remember { mutableStateOf<Boolean?>(null) }

        fun reset() {
            draft = model.mirror?.let { ProviderMirrorDraft.from(it) } ?: ProviderMirrorDraft(provider = draft.provider)
        }

        fun run(block: () -> Unit) {
            scope.launch {
                io { block() }
                tick.bump()
            }
        }

        LaunchedEffect(Unit) {
            val owner = io { CodeServices.isOwner(repo) }
            canEdit = owner
            if (owner) {
                io { model.load() }
                reset()
            }
            tick.bump()
        }
        tick.value
        val now = System.currentTimeMillis()
        DetailScaffold("Mirror · ${repo.name}", navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { run { model.load() } })
        }) {
            ScreenList {
                when {
                    canEdit == false -> item {
                        InfoCard("Mirror to GitHub or GitLab") {
                            Note("Only the repository owner can mirror it to GitHub or GitLab.")
                        }
                    }
                    !model.loaded -> item {
                        if (model.error != null) {
                            InfoCard("Mirror") { Note(model.error.orEmpty(), Fm.colors.danger) }
                        } else {
                            LoadingCard("Loading mirror settings…")
                        }
                    }
                    else -> {
                        if (model.error != null || model.notice != null) {
                            item {
                                Column(Modifier.padding(horizontal = FmSpace.x1)) {
                                    model.error?.let { Note(it, Fm.colors.danger) }
                                    model.notice?.let { Note(it, Fm.colors.success) }
                                }
                            }
                        }
                        val mirror = model.mirror
                        if (mirror != null && !editing) {
                            item {
                                InfoCard(mirror.remote) {
                                    Note(
                                        mirror.connectionError.ifEmpty {
                                            "${mirror.providerLabel} as @${mirror.login.ifEmpty { "unknown" }}" +
                                                (if (mirror.remotePrivate) " · private" else "") +
                                                (if (mirror.canPush) "" else " · token cannot push")
                                        },
                                        if (mirror.connectionError.isEmpty()) Fm.colors.textSecondary else Fm.colors.danger,
                                    )
                                    Row(verticalAlignment = Alignment.CenterVertically) {
                                        Text("Mirroring", Modifier.weight(1f), color = Fm.colors.textPrimary)
                                        FmSwitch(checked = mirror.active, onCheckedChange = { on -> run { model.setActive(on) } })
                                    }
                                    if (mirror.remoteUrl.isNotEmpty()) {
                                        FmToneButton("Open on ${mirror.providerLabel}", onPressed = { openExternalUrl(context, mirror.remoteUrl) }, icon = "open_in_new")
                                    }
                                }
                            }
                            item {
                                InfoCard("What is mirrored") {
                                    for (resource in model.resources.filter { it.supported(mirror.provider) }) {
                                        val setting = mirror.setting(resource.key)
                                        Row(verticalAlignment = Alignment.CenterVertically) {
                                            Text(resource.title(mirror.provider), Modifier.weight(1f), color = Fm.colors.textPrimary)
                                            Note(
                                                ProviderMirrorText.direction(setting.direction, mirror.providerLabel) +
                                                    if (setting.manual) " · picked" else "",
                                                if (setting.direction == "off") Fm.colors.textTertiary else Fm.colors.textSecondary,
                                            )
                                        }
                                    }
                                }
                            }
                            item {
                                InfoCard("Status") {
                                    StatusLine("Code", ProviderMirrorText.codeLine(mirror, model.codeHosted, now))
                                    StatusLine("Import", ProviderMirrorText.syncLine(mirror, now))
                                    if (model.pending > 0) Note("${model.pending} item${if (model.pending == 1) "" else "s"} waiting to be sent to ${mirror.providerLabel}.")
                                    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                        FmToneButton(if (model.busy) "Working…" else "Sync now", onPressed = if (model.busy) null else ({ run { model.sync() } }), icon = "sync", tone = FmTone.Accent, filled = true)
                                        FmToneButton("Check connection", onPressed = if (model.busy) null else ({ run { model.verify() } }))
                                        FmToneButton("Change settings", onPressed = {
                                            draft = ProviderMirrorDraft.from(mirror)
                                            editing = true
                                        }, icon = "tune")
                                        FmToneButton("Disconnect", onPressed = {
                                            scope.launch {
                                                if (messenger.confirmAction(
                                                        "Disconnect from ${mirror.providerLabel}?",
                                                        "Disconnect",
                                                        "The stored token is deleted and nothing more is mirrored. Nothing already copied is removed on either side.",
                                                    )
                                                ) {
                                                    io { model.disconnect() }
                                                    reset()
                                                    tick.bump()
                                                }
                                            }
                                        }, tone = FmTone.Danger)
                                    }
                                }
                            }
                            if (model.importKinds.isNotEmpty()) {
                                item {
                                    InfoCard("Bring into ForkMesh") {
                                        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                            for (resource in model.importKinds) {
                                                FmToneButton(
                                                    "Pick ${resource.title(mirror.provider).lowercase()}",
                                                    onPressed = { run { model.openCandidates(resource.key) } },
                                                    icon = "checklist",
                                                    tone = if (model.candidateKind == resource.key) FmTone.Accent else FmTone.Neutral,
                                                )
                                            }
                                        }
                                        if (model.candidateKind != null) {
                                            CandidateList(model, onToggle = { model.toggle(it); tick.bump() }, onOpen = { openExternalUrl(context, it) }, onPage = { page ->
                                                run { model.openCandidates(model.candidateKind.orEmpty(), page) }
                                            }, onImport = { run { model.importSelected() } }, onClose = { model.closeCandidates(); tick.bump() })
                                        }
                                    }
                                }
                            }
                            if (mirror.history.isNotEmpty()) {
                                item {
                                    InfoCard("Recent activity") {
                                        for (entry in mirror.history.take(8)) {
                                            Row(verticalAlignment = Alignment.CenterVertically) {
                                                Dot(if (entry.ok) Fm.colors.success else Fm.colors.danger)
                                                Text(entry.message.ifEmpty { entry.kind }, Modifier.weight(1f).padding(start = FmSpace.x2), color = Fm.colors.textPrimary, style = Fm.text.bodySmall)
                                                Note(ProviderMirrorText.ago(entry.atMs, now), Fm.colors.textTertiary)
                                            }
                                        }
                                    }
                                }
                            }
                        } else {
                            item {
                                MirrorForm(model, draft, onChange = { draft = it }, onOpen = { openExternalUrl(context, it) })
                            }
                            item {
                                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                    val label = model.provider(draft.provider)?.label ?: "GitHub"
                                    FmToneButton(
                                        when {
                                            model.busy -> "Checking the token…"
                                            mirror == null -> "Connect $label"
                                            else -> "Save mirror"
                                        },
                                        onPressed = if (model.busy) null else ({
                                            scope.launch {
                                                if (io { model.save(draft, repo.description) }) {
                                                    editing = false
                                                    reset()
                                                }
                                                tick.bump()
                                            }
                                        }),
                                        icon = "link",
                                        tone = FmTone.Accent,
                                        filled = true,
                                    )
                                    if (mirror != null) FmToneButton("Cancel", onPressed = { editing = false; reset() })
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun Dot(color: Color) {
    Box(Modifier.size(8.dp).background(color, CircleShape))
}

@Composable
private fun StatusLine(title: String, line: Pair<Boolean?, String>) {
    Row(verticalAlignment = Alignment.Top) {
        Box(Modifier.padding(top = 6.dp)) {
            Dot(
                when (line.first) {
                    true -> Fm.colors.success
                    false -> Fm.colors.danger
                    null -> Fm.colors.textTertiary
                },
            )
        }
        Column(Modifier.padding(start = FmSpace.x2)) {
            Text(title, color = Fm.colors.textPrimary, style = Fm.text.bodyMedium)
            Note(line.second)
        }
    }
}

@Composable
private fun Choice(label: String, options: List<Pair<String, String>>, selected: String, enabled: Boolean, onSelect: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Note(label, Fm.colors.textSecondary)
        Box(Modifier.padding(start = FmSpace.x2)) {
            FmToneButton(
                options.firstOrNull { it.first == selected }?.second ?: selected,
                onPressed = if (enabled) ({ open = true }) else null,
                icon = "expand_more",
            )
            FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                for ((value, text) in options) {
                    FmDropdownMenuItem(text = text, onClick = {
                        open = false
                        onSelect(value)
                    }, icon = if (value == selected) "check" else null)
                }
            }
        }
    }
}

@Composable
private fun MirrorForm(model: ProviderMirrorModel, draft: ProviderMirrorDraft, onChange: (ProviderMirrorDraft) -> Unit, onOpen: (String) -> Unit) {
    val providers = model.providers
    val provider = model.provider(draft.provider) ?: providers.first()
    val stored = model.mirror?.let { it.provider == draft.provider && it.tokenHint.isNotEmpty() } == true
    InfoCard("Mirror to") {
        Note("Keep a copy of this repository on GitHub or GitLab with your own access token, and choose for each part whether it is sent from ForkMesh, brought in, both, or neither.")
        FmSegmentTabBar(
            tabs = providers.map { FmSegmentTab(it.label) },
            index = providers.indexOfFirst { it.key == draft.provider }.coerceAtLeast(0),
            onChanged = { index -> onChange(draft.copy(provider = providers[index].key)) },
        )
        FmTextField(draft.remote, { onChange(draft.copy(remote = it)) }, hint = "owner/name", helperText = "${provider.host}/${draft.remote.ifEmpty { "owner/name" }}")
        FmTextField(
            draft.token,
            { onChange(draft.copy(token = it)) },
            hint = if (stored) "Saved (${model.mirror?.tokenHint.orEmpty()}). Leave blank to keep it" else "Personal access token",
            visualTransformation = PasswordVisualTransformation(),
        )
        Note("${provider.tokenHelp} The token is stored encrypted and never shown again.")
        FmToneButton("Create a token on ${provider.label}", onPressed = { onOpen(provider.tokenUrl) }, icon = "key")
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                "Create it on ${provider.label} if it does not exist" + if (model.isPrivate) " (private)" else "",
                Modifier.weight(1f),
                color = Fm.colors.textPrimary,
            )
            FmSwitch(checked = draft.createRemote, onCheckedChange = { onChange(draft.copy(createRemote = it)) })
        }
    }
    InfoCard("What to mirror") {
        for (resource in model.resources) {
            ResourceRow(model, resource, draft, provider.label, onChange)
        }
        Note("Items brought in from ${provider.label} keep a link to the original and are never sent back. Imported pull requests arrive as issues labelled pull-request.", Fm.colors.textTertiary)
    }
}

@Composable
private fun ResourceRow(
    model: ProviderMirrorModel,
    resource: ProviderMirrorResource,
    draft: ProviderMirrorDraft,
    providerLabel: String,
    onChange: (ProviderMirrorDraft) -> Unit,
) {
    val supported = resource.supported(draft.provider)
    val setting = draft.setting(resource)
    val name = resource.title(draft.provider)
    Column(Modifier.fillMaxWidth().padding(vertical = FmSpace.x1), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
        Text(name, color = if (supported) Fm.colors.textPrimary else Fm.colors.textTertiary, style = Fm.text.bodyMedium)
        Note(if (supported) resource.hint else "$providerLabel has no ${name.lowercase()}.", if (supported) Fm.colors.textSecondary else Fm.colors.textTertiary)
        if (supported) {
            Choice(
                "Direction",
                resource.directions.map { it to ProviderMirrorText.direction(it, providerLabel) },
                setting.direction,
                enabled = true,
            ) { onChange(draft.with(resource, setting.copy(direction = it))) }
            if (resource.canImport && setting.imports) {
                Choice(
                    "Import",
                    listOf("auto" to "Automatically", "manual" to "I pick what to import"),
                    setting.mode,
                    enabled = true,
                ) { onChange(draft.with(resource, setting.copy(mode = it))) }
            }
            if (resource.canImport && setting.direction != "off") {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Comments", Modifier.weight(1f), color = Fm.colors.textPrimary)
                    FmSwitch(checked = setting.comments, onCheckedChange = { onChange(draft.with(resource, setting.copy(comments = it))) })
                }
            }
            if (resource.key == "code" && setting.direction == "push") {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Delete branches there that no longer exist here", Modifier.weight(1f), color = Fm.colors.textPrimary)
                    FmSwitch(checked = draft.prune, onCheckedChange = { onChange(draft.copy(prune = it)) })
                }
                if (!model.codeHosted) {
                    Note("Code is pushed by the ForkMesh server that holds this repository; this one is served from its owner's node.", Fm.colors.textTertiary)
                }
            }
        }
    }
}

@Composable
private fun CandidateList(
    model: ProviderMirrorModel,
    onToggle: (ProviderMirrorCandidate) -> Unit,
    onOpen: (String) -> Unit,
    onPage: (Int) -> Unit,
    onImport: () -> Unit,
    onClose: () -> Unit,
) {
    Column(Modifier.fillMaxWidth().padding(top = FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
        when {
            model.candidatesLoading -> Note("Loading…")
            model.candidates.isEmpty() -> Note("Nothing here yet.")
        }
        for (item in model.candidates) {
            Row(
                Modifier.fillMaxWidth().clickable(enabled = item.selectable) { onToggle(item) },
                verticalAlignment = Alignment.Top,
            ) {
                FmCheckbox(checked = item.key in model.selected, onCheckedChange = if (item.selectable) ({ _ -> onToggle(item) }) else null, enabled = item.selectable)
                Column(Modifier.weight(1f).padding(start = FmSpace.x1)) {
                    Text(item.title, color = if (item.selectable) Fm.colors.textPrimary else Fm.colors.textTertiary, style = Fm.text.bodyMedium)
                    Note(
                        "#${item.number} · @${item.author} · ${item.stateLabel}" +
                            when {
                                item.imported -> " · already imported"
                                item.mirrored -> " · came from ForkMesh"
                                else -> ""
                            },
                    )
                    if (item.excerpt.isNotEmpty()) Note(item.excerpt, Fm.colors.textTertiary)
                }
                if (item.url.isNotEmpty()) FmIconButton("open_in_new", "Open", onPressed = { onOpen(item.url) })
            }
        }
        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
            if (model.candidatePage > 1) FmToneButton("Newer", onPressed = { onPage(model.candidatePage - 1) })
            if (model.candidatesMore) FmToneButton("Older", onPressed = { onPage(model.candidatePage + 1) })
            FmToneButton(
                if (model.selected.isEmpty()) "Import" else "Import ${model.selected.size}",
                onPressed = if (model.selected.isEmpty() || model.busy) null else onImport,
                icon = "download",
                tone = FmTone.Accent,
                filled = true,
            )
            FmToneButton("Close", onPressed = onClose)
        }
    }
}
