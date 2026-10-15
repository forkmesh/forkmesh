package com.forkmesh.nativeapp.feature.coves

import com.forkmesh.nativeapp.feature.market.rememberScreenPluginMenu
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.coves.Cove
import com.forkmesh.nativeapp.coves.CoveActionException
import com.forkmesh.nativeapp.coves.CoveDocument
import com.forkmesh.nativeapp.coves.CoveEditorKind
import com.forkmesh.nativeapp.coves.CoveFiles
import com.forkmesh.nativeapp.coves.CoveRepo
import com.forkmesh.nativeapp.coves.CoveStore
import com.forkmesh.nativeapp.coves.CovesModel
import com.forkmesh.nativeapp.coves.CovesPhase
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.feature.tools.copyToClipboard
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.DateFormat
import java.util.Date

@Composable
private fun CoveNote(text: String, color: androidx.compose.ui.graphics.Color = Fm.colors.textSecondary) {
    Text(text, modifier = Modifier.padding(horizontal = FmSpace.x1, vertical = FmSpace.x2), style = Fm.text.bodySmall.copy(color = color))
}

@Composable
private fun CenteredProgress() {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
}

internal fun documentIcon(document: CoveDocument): String = when (CoveFiles.editorKind(document.name, document.mime)) {
    CoveEditorKind.Spreadsheet -> "table_chart_outlined"
    CoveEditorKind.Pipeline -> "view_kanban_outlined"
    CoveEditorKind.Text -> if (document.name.lowercase().endsWith(".json")) "data_object" else "description_outlined"
}

class CovesRepoPickerScreen : Screen() {
    override val title: String get() = "Coves"

    private var repos by mutableStateOf<List<Repository>>(emptyList())
    private var loading by mutableStateOf(true)
    private var error by mutableStateOf<String?>(null)
    private var query by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        LaunchedEffect(Unit) {
            val api = CovesFeature.api
            if (api == null) {
                loading = false
                error = "Sign in to browse repositories."
                return@LaunchedEffect
            }
            loading = true
            try {
                repos = withContext(Dispatchers.IO) { api.repositories() }
                error = null
            } catch (failure: Exception) {
                error = failure.message ?: failure.toString()
            }
            loading = false
        }
        val me = CoveStore.normalizedAccount(CovesFeature.account)
        val needle = query.trim().lowercase()
        val filtered = repos
            .sortedWith(compareBy<Repository>({ if (it.displayOwner.lowercase() == me) 0 else 1 }, { it.fullName.lowercase() }))
            .filter { needle.isEmpty() || "${it.displayOwner}/${it.name}".lowercase().contains(needle) }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Coves", navigator)
            FmTextField(
                value = query,
                onValueChange = { query = it },
                modifier = Modifier.fillMaxWidth().padding(FmSpace.x4),
                hint = "Search repositories",
            )
            when {
                loading && repos.isEmpty() -> CenteredProgress()
                !loading && filtered.isEmpty() -> FmEmptyState("inventory_2_outlined", "No repositories", message = error ?: "Coves live in a repository you can read.")
                else -> LazyColumn(contentPadding = PaddingValues(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
                    error?.let { item { CoveNote(it, Fm.colors.danger) } }
                    items(filtered, key = { it.fullName }) { repo ->
                        FmCard(
                            modifier = Modifier.fillMaxWidth().padding(bottom = FmSpace.x2),
                            onTap = { navigator.push(CovesScreen(CoveRepo(repo.displayOwner, repo.name))) },
                        ) {
                            Column {
                                Text("${repo.displayOwner}/${repo.name}", style = Fm.text.bodyLarge)
                                if (repo.description.isNotEmpty()) {
                                    Text(repo.description, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
                                }
                            }
                        }
                    }
                    item {
                        CoveNote("Coves are encrypted files inside a repository. Only people with the password, or accounts the creator invited, can read them.")
                    }
                }
            }
        }
    }
}

@Composable
internal fun CoveRow(cove: Cove, pending: Boolean) {
    val c = Fm.colors
    val subtitle = when {
        !cove.unlocked -> "Encrypted · ${cove.slug.take(8)}"
        cove.accountScoped -> {
            val docs = if (cove.documents.size == 1) "1 document" else "${cove.documents.size} documents"
            val people = if (cove.members.size == 1) "1 person" else "${cove.members.size} people"
            "$docs · $people"
        }
        else -> (if (cove.documents.size == 1) "1 document" else "${cove.documents.size} documents") + " · password"
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        FmIcon(
            if (!cove.unlocked) "lock" else if (cove.accountScoped) "group" else "lock_open",
            size = 22.dp,
            tint = if (cove.unlocked) c.accent else c.textSecondary,
        )
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.weight(1f)) {
            Text(cove.displayName, style = Fm.text.bodyLarge)
            Text(subtitle, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        }
        if (pending) {
            Text(
                "Pending review",
                modifier = Modifier.background(c.warning.copy(alpha = 0.18f), RoundedCornerShape(FmRadius.full)).padding(horizontal = 6.dp, vertical = 2.dp),
                style = Fm.text.labelSmall.copy(fontWeight = FontWeight.SemiBold),
            )
        }
    }
}

internal suspend fun unlockCove(messenger: FmMessenger, model: CovesModel, cove: Cove) {
    var problem: String? = null
    while (true) {
        val password = showPasswordDialog(
            messenger,
            "Unlock cove",
            "The password is shared by the cove's creator, out of band. It never leaves this phone.",
            "Unlock",
            problem,
        ) ?: return
        val opened = withContext(Dispatchers.Default) { model.unlock(cove.id, password) }
        if (opened) return
        problem = "That password didn't unlock this cove."
    }
}

class CovesScreen(private val repo: CoveRepo) : Screen() {
    override val title: String get() = "Coves"

    @Composable
    override fun Content(navigator: Navigator) {
        val model = remember { CovesFeature.model(repo) }
        if (model == null) {
            Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
                ToolsAppBar("Coves", navigator)
                FmEmptyState("lock", "Sign in to open coves", message = "Coves belong to your ForkMesh account.")
            }
            return
        }
        rememberModelRevision(model)
        val messenger = LocalFmMessenger.current
        val status = rememberFmStatus()
        val scope = rememberCoroutineScope()
        var menu by remember { mutableStateOf(false) }
        LaunchedEffect(model) {
            withContext(Dispatchers.IO) { if (model.phase == CovesPhase.Idle) model.load() else model.unlockAccountCoves() }
        }
        fun create() {
            scope.launch {
                val request = showNewCoveDialog(messenger, repo.slug) ?: return@launch
                status.clear()
                try {
                    val created = withContext(Dispatchers.IO) {
                        if (request.password != null) model.createPasswordCove(request.name, request.password, request.notifyOnOpen)
                        else model.createAccountCove(request.name)
                    }
                    messenger.showConfirmDialog(
                        "Cove",
                        "Proposed “${created.name}”. It appears for others once the repository owner merges it.",
                        hideCancel = true,
                    )
                } catch (failure: Exception) {
                    status.error(failure.message ?: failure.toString())
                }
            }
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            val plugin = rememberScreenPluginMenu()
            ToolsAppBar("Coves", navigator, pluginMenu = false) {
                plugin?.Host()
                FmIconButton("refresh", "Refresh", onPressed = { scope.launch(Dispatchers.IO) { model.load() } })
                FmIconButton("add", "New cove", onPressed = if (model.busy) null else ({ create() }))
                Box {
                    FmIconButton("more_vert", "More", onPressed = { menu = true })
                    FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                        FmDropdownMenuItem(text = "Lock all", onClick = {
                            menu = false
                            model.lockAll()
                        })
                        plugin?.Items(close = { menu = false }, divided = true)
                    }
                }
            }
            FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1))
            val phase = model.phase
            when {
                (phase == CovesPhase.Loading || phase == CovesPhase.Idle) && model.entries.isEmpty() -> CenteredProgress()
                phase == CovesPhase.Loaded && model.entries.isEmpty() -> Column(
                    Modifier.fillMaxSize().padding(FmSpace.x5),
                    verticalArrangement = Arrangement.Center,
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    FmIcon("shield_outlined", size = 40.dp, tint = Fm.colors.textSecondary)
                    Text("No coves yet", modifier = Modifier.padding(top = FmSpace.x3), style = Fm.text.titleMedium)
                    Text(
                        "A cove is an encrypted folder of notes, sheets and pipelines inside ${repo.slug}.",
                        modifier = Modifier.padding(vertical = FmSpace.x2),
                        style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
                    )
                    FmTextButton(onClick = { create() }) { Text("New cove") }
                }
                else -> LazyColumn(contentPadding = PaddingValues(FmSpace.x4)) {
                    if (phase is CovesPhase.Failed) {
                        item {
                            FmCard(modifier = Modifier.fillMaxWidth().padding(bottom = FmSpace.x3)) {
                                Column {
                                    Text("Couldn't load coves. ${phase.message}", style = Fm.text.bodyMedium.copy(color = Fm.colors.danger))
                                    FmTextButton(onClick = { scope.launch(Dispatchers.IO) { model.load() } }) { Text("Retry") }
                                }
                            }
                        }
                    }
                    val open = model.unlockedCoves
                    if (open.isNotEmpty()) {
                        item { FmSectionHeader(title = "Open") }
                        items(open, key = { "open-${it.id}" }) { cove ->
                            FmCard(
                                modifier = Modifier.fillMaxWidth().padding(vertical = FmSpace.x1),
                                onTap = { navigator.push(CoveScreen(repo, cove.id)) },
                            ) { CoveRow(cove, model.entry(cove.id)?.pendingReview == true) }
                        }
                    }
                    val locked = model.lockedCoves
                    if (locked.isNotEmpty()) {
                        item { Box(Modifier.padding(top = FmSpace.x4)) { FmSectionHeader(title = "Locked") } }
                        items(locked, key = { "locked-${it.id}" }) { cove ->
                            FmCard(
                                modifier = Modifier.fillMaxWidth().padding(vertical = FmSpace.x1),
                                onTap = { scope.launch { unlockCove(messenger, model, cove) } },
                            ) { CoveRow(cove, false) }
                        }
                        item { CoveNote("Locked coves show nothing about themselves: not a name, not who can open them.") }
                    }
                    if (model.skipped > 0) item { CoveNote("${model.skipped} cove file(s) could not be read.") }
                }
            }
        }
    }
}

class CoveLinkScreen(private val id: String) : Screen() {
    override val title: String get() = "Cove"

    @Composable
    override fun Content(navigator: Navigator) {
        val target = remember { CoveFiles.parseRouteId(id) }
        if (target == null) {
            Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
                ToolsAppBar("Cove", navigator)
                FmEmptyState("lock", "Cove link not recognised", message = "Open coves from their repository, or from a forkmesh://cove link.")
            }
            return
        }
        val repo = CoveRepo(target.owner, target.repository)
        val model = remember { CovesFeature.model(repo) }
        if (model == null) {
            Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
                ToolsAppBar("Cove", navigator)
                FmEmptyState("lock", "Sign in to open coves")
            }
            return
        }
        rememberModelRevision(model)
        val cove = model.cove(target.coveId)
        if (cove != null && cove.unlocked) {
            CoveBody(navigator, model, repo, target.coveId)
            return
        }
        val messenger = LocalFmMessenger.current
        val scope = rememberCoroutineScope()
        LaunchedEffect(model) { if (model.phase == CovesPhase.Idle) withContext(Dispatchers.IO) { model.load() } }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Cove", navigator)
            when {
                model.phase == CovesPhase.Loading || model.phase == CovesPhase.Idle -> CenteredProgress()
                cove != null -> Column(
                    Modifier.fillMaxSize().padding(FmSpace.x5),
                    verticalArrangement = Arrangement.Center,
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    FmIcon("lock", size = 40.dp, tint = Fm.colors.textSecondary)
                    Text("This cove is locked", modifier = Modifier.padding(top = FmSpace.x3), style = Fm.text.titleMedium)
                    Text(
                        if (cove.accountScoped || cove.accessMode.isEmpty()) "Your account has no key grant for it, or it needs its password."
                        else "Enter the cove's password to open it.",
                        modifier = Modifier.padding(vertical = FmSpace.x2),
                        style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
                    )
                    FmTextButton(onClick = { scope.launch { unlockCove(messenger, model, cove) } }) { Text("Enter password") }
                }
                else -> FmEmptyState("help_outline", "Cove not found", message = "It is not in ${repo.slug}, or its change has not been merged yet.")
            }
        }
    }
}

class CoveScreen(private val repo: CoveRepo, private val coveId: String) : Screen() {
    override val title: String get() = "Cove"

    @Composable
    override fun Content(navigator: Navigator) {
        val model = remember { CovesFeature.model(repo) }
        if (model == null) {
            Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
                ToolsAppBar("Cove", navigator)
                FmEmptyState("lock", "Sign in to open coves")
            }
            return
        }
        rememberModelRevision(model)
        CoveBody(navigator, model, repo, coveId)
    }
}

@Composable
private fun CoveBody(navigator: Navigator, model: CovesModel, repo: CoveRepo, coveId: String) {
    rememberModelRevision(model)
    val c = Fm.colors
    val context = LocalContext.current
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    var menu by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var notice by remember { mutableStateOf<String?>(null) }
    val cove = model.cove(coveId)
    fun run(work: () -> Unit) {
        scope.launch {
            try {
                withContext(Dispatchers.IO) { work() }
                error = null
            } catch (failure: Exception) {
                error = failure.message ?: failure.toString()
            }
        }
    }
    Column(Modifier.fillMaxSize().background(c.bgBase)) {
        ToolsAppBar(cove?.displayName ?: "Cove", navigator) {
            if (cove != null && cove.unlocked) {
                FmIconButton("note_add_outlined", "Add document", onPressed = if (model.busy) null else ({
                    scope.launch { showTemplatePicker(messenger, model, coveId) }
                }))
                Box {
                    FmIconButton("more_vert", "More", onPressed = { menu = true })
                    FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                        if (cove.accountScoped) {
                            FmDropdownMenuItem(text = "Share", onClick = {
                                menu = false
                                scope.launch { showShareSheet(messenger, model, coveId) }
                            })
                        }
                        FmDropdownMenuItem(text = "Copy link", onClick = {
                            menu = false
                            copyToClipboard(context, model.shareLink(coveId))
                            notice = "Link copied."
                        })
                        FmDropdownMenuItem(text = "Access log", onClick = {
                            menu = false
                            scope.launch { showAccessLog(messenger, cove) }
                        })
                        FmDropdownMenuItem(text = "Lock", onClick = {
                            menu = false
                            model.lock(coveId)
                            navigator.pop()
                        })
                    }
                }
            }
        }
        if (cove == null || !cove.unlocked) {
            FmEmptyState("lock", "Cove locked", message = "Open it again from the list.")
            return@Column
        }
        val documents = cove.documents.sortedBy { it.name.lowercase() }
        LazyColumn(contentPadding = PaddingValues(FmSpace.x4)) {
            if (model.entry(coveId)?.pendingReview == true) {
                item { CoveNote("Your changes are proposed and wait for the repository owner to merge them.") }
            }
            error?.let { item { CoveNote(it, c.danger) } }
            notice?.let { item { FmInlineStatusLine(it, isError = false, modifier = Modifier.padding(horizontal = FmSpace.x1), onDismiss = { notice = null }) } }
            if (documents.isEmpty()) {
                item {
                    Column(Modifier.fillMaxWidth().padding(vertical = FmSpace.x6), horizontalAlignment = Alignment.CenterHorizontally) {
                        FmIcon("note_add_outlined", size = 40.dp, tint = c.textSecondary)
                        Text("Empty cove", modifier = Modifier.padding(top = FmSpace.x3), style = Fm.text.titleMedium)
                        Text(
                            "Add a note, a spreadsheet, a lead pipeline or another starter document.",
                            modifier = Modifier.padding(vertical = FmSpace.x2),
                            style = Fm.text.bodyMedium.copy(color = c.textSecondary),
                        )
                        FmTextButton(onClick = { scope.launch { showTemplatePicker(messenger, model, coveId) } }) { Text("Add document") }
                    }
                }
            } else {
                item { FmSectionHeader(title = "Documents") }
                items(documents, key = { it.name }) { document ->
                    var rowMenu by remember { mutableStateOf(false) }
                    FmCard(
                        modifier = Modifier.fillMaxWidth().padding(vertical = FmSpace.x1),
                        onTap = { navigator.push(CoveDocumentScreen(repo, coveId, document.name)) },
                    ) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            FmIcon(documentIcon(document), size = 22.dp, tint = c.textSecondary)
                            Spacer(Modifier.width(FmSpace.x3))
                            Column(Modifier.weight(1f)) {
                                Text(document.name, style = Fm.text.bodyLarge)
                                if (document.updatedAtMs > 0) {
                                    Text(
                                        DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(document.updatedAtMs)),
                                        style = Fm.text.bodySmall.copy(color = c.textSecondary),
                                    )
                                }
                            }
                            Box {
                                FmIconButton("more_vert", "Document actions", onPressed = { rowMenu = true })
                                FmDropdownMenu(expanded = rowMenu, onDismissRequest = { rowMenu = false }) {
                                    FmDropdownMenuItem(text = "Rename", onClick = {
                                        rowMenu = false
                                        scope.launch {
                                            val newName = messenger.showPromptDialog("Rename", initialValue = document.name, confirmLabel = "Rename")
                                                ?: return@launch
                                            run { model.renameDocument(coveId, document.name, newName) }
                                        }
                                    })
                                    FmDropdownMenuItem(text = "Delete", onClick = {
                                        rowMenu = false
                                        scope.launch {
                                            val confirmed = messenger.showConfirmDialog(
                                                "Delete ${document.name}?",
                                                "The deletion is proposed like any other cove change.",
                                                confirmLabel = "Delete",
                                                destructive = true,
                                            )
                                            if (confirmed) run { model.deleteDocument(coveId, document.name) }
                                        }
                                    })
                                }
                            }
                        }
                    }
                }
            }
            item { CoveNote("Encrypted with AES-256 before it leaves this phone. The repository only ever holds ciphertext.") }
        }
    }
}

private suspend fun showTemplatePicker(messenger: FmMessenger, model: CovesModel, coveId: String) {
    messenger.showModalBottomSheet<Unit>(isScrollControlled = true) { close ->
        val scope = rememberCoroutineScope()
        var chosen by remember { mutableStateOf<com.forkmesh.nativeapp.coves.CoveFileTemplate?>(null) }
        var name by remember { mutableStateOf("") }
        var working by remember { mutableStateOf(false) }
        var error by remember { mutableStateOf<String?>(null) }
        val c = Fm.colors
        Column(Modifier.fillMaxWidth().padding(FmSpace.x4)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Quick create", modifier = Modifier.weight(1f), style = Fm.text.titleMedium)
                FmTextButton(onClick = { close(null) }, enabled = !working) { Text("Cancel") }
                FmTextButton(
                    onClick = {
                        val template = chosen ?: return@FmTextButton
                        working = true
                        scope.launch {
                            try {
                                withContext(Dispatchers.IO) { model.addDocument(coveId, template, name) }
                                close(Unit)
                            } catch (failure: Exception) {
                                error = failure.message ?: failure.toString()
                            }
                            working = false
                        }
                    },
                    enabled = chosen != null && !working,
                ) { Text(if (working) "Creating…" else "Create") }
            }
            error?.let { Text(it, style = Fm.text.bodySmall.copy(color = c.danger)) }
            LazyColumn(Modifier.fillMaxWidth().padding(top = FmSpace.x2)) {
                items(CoveFiles.templates(), key = { it.id }) { template ->
                    val selected = chosen?.id == template.id
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .clickable {
                                chosen = template
                                name = template.defaultPath
                            }
                            .padding(vertical = FmSpace.x2),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        FmIcon(template.icon, size = 22.dp, tint = c.textSecondary)
                        Spacer(Modifier.width(FmSpace.x3))
                        Column(Modifier.weight(1f)) {
                            Text(template.title, style = Fm.text.bodyLarge)
                            Text(template.description, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                        }
                        if (selected) FmIcon("check", size = 20.dp, tint = c.accent)
                    }
                }
                if (chosen != null) {
                    item {
                        FmTextField(
                            value = name,
                            onValueChange = { name = it },
                            modifier = Modifier.fillMaxWidth().padding(top = FmSpace.x3),
                            hint = "File name",
                        )
                    }
                }
            }
        }
    }
}

private suspend fun showShareSheet(messenger: FmMessenger, model: CovesModel, coveId: String) {
    messenger.showModalBottomSheet<Unit>(isScrollControlled = true) { close ->
        rememberModelRevision(model)
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        var invitee by remember { mutableStateOf("") }
        var working by remember { mutableStateOf(false) }
        var status by remember { mutableStateOf<String?>(null) }
        var failed by remember { mutableStateOf(false) }
        val c = Fm.colors
        val cove = model.cove(coveId)
        Column(Modifier.fillMaxWidth().padding(FmSpace.x4)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Share cove", modifier = Modifier.weight(1f), style = Fm.text.titleMedium)
                FmTextButton(onClick = { close(Unit) }) { Text("Done") }
            }
            if (cove != null) {
                FmSectionHeader(title = "People with access")
                for (person in cove.members) {
                    Row(Modifier.fillMaxWidth().padding(vertical = FmSpace.x1)) {
                        Text("@$person", modifier = Modifier.weight(1f), style = Fm.text.bodyMedium)
                        Text(
                            if (person == CoveStore.normalizedAccount(cove.creatorAccount)) "Owner · can edit" else "Can edit",
                            style = Fm.text.bodySmall.copy(color = c.textSecondary),
                        )
                    }
                }
                CoveNote("Only these accounts receive an encryption grant. The link carries no key and no contents.")
                if (cove.createdBy(model.account)) {
                    FmSectionHeader(title = "Add a person")
                    FmTextField(value = invitee, onValueChange = { invitee = it }, modifier = Modifier.fillMaxWidth(), hint = "ForkMesh account")
                    FmTextButton(
                        onClick = {
                            val name = invitee
                            working = true
                            scope.launch {
                                try {
                                    withContext(Dispatchers.IO) { model.invite(coveId, name) }
                                    invitee = ""
                                    failed = false
                                    status = "Added @${CoveStore.normalizedAccount(name)}. Send them the link once the owner merges the change."
                                } catch (failure: CoveActionException) {
                                    failed = true
                                    status = failure.message
                                } catch (failure: Exception) {
                                    failed = true
                                    status = failure.message ?: failure.toString()
                                }
                                working = false
                            }
                        },
                        enabled = !working && invitee.isNotBlank(),
                    ) { Text(if (working) "Adding…" else "Add person") }
                } else {
                    CoveNote("Only the cove creator can change sharing.")
                }
                FmTextButton(onClick = {
                    copyToClipboard(context, model.shareLink(coveId))
                    status = "Link copied. It opens only for an account listed above."
                    failed = false
                }) { Text("Copy link") }
                status?.let { CoveNote(it, if (failed) c.danger else c.textSecondary) }
            }
        }
    }
}

private suspend fun showAccessLog(messenger: FmMessenger, cove: Cove) {
    messenger.showModalBottomSheet<Unit>(isScrollControlled = true) { close ->
        val c = Fm.colors
        Column(Modifier.fillMaxWidth().padding(FmSpace.x4)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Access log", modifier = Modifier.weight(1f), style = Fm.text.titleMedium)
                FmTextButton(onClick = { close(Unit) }) { Text("Done") }
            }
            if (cove.accessLog.isEmpty()) {
                CoveNote("No activity yet. Edits are recorded inside the encrypted cove.")
            } else {
                LazyColumn {
                    items(cove.accessLog.reversed()) { entry ->
                        Row(Modifier.fillMaxWidth().padding(vertical = FmSpace.x2)) {
                            Column(Modifier.weight(1f)) {
                                Text(entry.name.ifEmpty { "Someone" }, style = Fm.text.bodyMedium)
                                Text(if (entry.action == "edit") "Edited" else "Opened", style = Fm.text.bodySmall.copy(color = c.textSecondary))
                            }
                            if (entry.ts > 0) {
                                Text(DateFormat.getDateInstance(DateFormat.MEDIUM).format(Date(entry.ts)), style = Fm.text.bodySmall.copy(color = c.textSecondary))
                            }
                        }
                    }
                }
            }
        }
    }
}
