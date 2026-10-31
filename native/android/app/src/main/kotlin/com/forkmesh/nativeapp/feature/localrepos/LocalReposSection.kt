package com.forkmesh.nativeapp.feature.localrepos

import com.forkmesh.nativeapp.ui.kit.FmSheetAction
import com.forkmesh.nativeapp.ui.kit.showActionSheet
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.localrepos.LocalRepoRecord
import com.forkmesh.nativeapp.localrepos.LocalRepoStore
import com.forkmesh.nativeapp.localrepos.LocalRepoSyncRun
import com.forkmesh.nativeapp.localrepos.PublishService
import com.forkmesh.nativeapp.localrepos.describeLocalRepoFailure
import com.forkmesh.nativeapp.localrepos.localRepoAuthor
import com.forkmesh.nativeapp.localrepos.localRepoTileMeta
import com.forkmesh.nativeapp.localrepos.suggestedImportName
import com.forkmesh.nativeapp.localrepos.syncAllLabel
import com.forkmesh.nativeapp.localrepos.syncableRecords
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmDialogActions
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private data class NewRepoRequest(val name: String, val description: String)

@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun LocalReposSectionContent(store: LocalRepoStore, navigator: Navigator) {
    val c = Fm.colors
    val context = LocalContext.current
    val messenger = LocalFmMessenger.current
    val status = rememberFmStatus()
    val requireAccount = AccountFeature.rememberRequireAccount()
    val mirrorFromProvider = CodeFeature.rememberMirrorFromProvider(status)
    val scope = rememberCoroutineScope()
    var version by remember(store) { mutableIntStateOf(0) }
    var loadError by remember(store) { mutableStateOf<String?>(null) }
    DisposableEffect(store) {
        val listener: () -> Unit = { version++ }
        store.addListener(listener)
        onDispose { store.removeListener(listener) }
    }
    LaunchedEffect(store) {
        if (!store.isLoaded) {
            try {
                withContext(Dispatchers.IO) { store.ensureLoaded() }
                version++
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                loadError = error.toString()
            }
        }
    }
    val records = remember(store, version) { store.records }
    val busy = remember(store, version) { store.isBusy }

    fun open(record: LocalRepoRecord) = navigator.push(LocalRepoScreen(store, record))

    val publisher = Services.get<PublishService>()
    val canSync = publisher != null && syncableRecords(records).isNotEmpty()

    fun sync(record: LocalRepoRecord?) = scope.launch {
        val service = publisher ?: return@launch
        if (!requireAccount("sync")) return@launch
        val targets = if (record != null) listOf(record) else syncableRecords(store.records)
        if (targets.isEmpty()) return@launch
        val run = LocalRepoSyncRun(store, service)
        status.clear()
        val label = busyLabel("Syncing ${targets.first().name}")
        val summary = messenger.runBusy(label, onCancel = run::cancel) {
            withContext(Dispatchers.IO) { run.run(targets) { text -> label.value = text } }
        }
        status.show(summary)
    }

    fun actions(record: LocalRepoRecord) = scope.launch {
        val choice = messenger.showModalBottomSheet<String> { close ->
            Spacer(Modifier.height(FmSpace.x2))
            FmListRow(
                title = "Open",
                leading = { FmIconTile("folder_open_outlined", tint = c.accent) },
                onTap = { close("open") },
            )
            if (record.isPublished && canSync) {
                FmRowDivider()
                FmListRow(
                    title = "Sync from ForkMesh",
                    subtitle = record.publishedAs,
                    leading = { FmIconTile("sync", tint = c.accent) },
                    onTap = { close("sync") },
                )
            }
            Spacer(Modifier.height(FmSpace.x2))
        } ?: return@launch
        if (choice == "open") open(record) else sync(record)
    }

    val folderPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            val suggested = suggestedImportName(ImportSources.treeName(context, uri), zip = false)
            val name = askImportName(messenger, suggested) ?: return@launch
            val author = localRepoAuthor(LocalReposFeature.session(), com.forkmesh.nativeapp.feature.account.settings.SettingsArea.prefs?.let(com.forkmesh.nativeapp.settings.DeviceSettings::localAuthorOverride))
            val source = DocumentTreeImportDirectory(context.contentResolver, uri)
            runStoreBusy(messenger, store, "Importing $name", status) {
                store.importTree(name = name, source = source, author = author)
            }?.let(::open)
        }
    }
    val zipPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            val suggested = suggestedImportName(ImportSources.displayName(context, uri), zip = true)
            val name = askImportName(messenger, suggested) ?: return@launch
            val author = localRepoAuthor(LocalReposFeature.session(), com.forkmesh.nativeapp.feature.account.settings.SettingsArea.prefs?.let(com.forkmesh.nativeapp.settings.DeviceSettings::localAuthorOverride))
            runStoreBusy(messenger, store, "Importing $name", status) {
                val copy = ImportSources.copyToCache(context, uri)
                try {
                    store.importZip(name = name, zipPath = copy.path, author = author)
                } finally {
                    copy.delete()
                }
            }?.let(::open)
        }
    }

    fun create() = scope.launch {
        if (!requireAccount("create a repository")) return@launch
        val request = messenger.showDialog<NewRepoRequest> { done -> NewRepositoryDialog(done) } ?: return@launch
        val author = localRepoAuthor(LocalReposFeature.session(), com.forkmesh.nativeapp.feature.account.settings.SettingsArea.prefs?.let(com.forkmesh.nativeapp.settings.DeviceSettings::localAuthorOverride))
        runStoreBusy(messenger, store, "Creating ${request.name}", status) {
            val branch = com.forkmesh.nativeapp.feature.account.settings.SettingsArea.prefs?.let(com.forkmesh.nativeapp.settings.DeviceSettings::localDefaultBranch) ?: "main"
            store.create(name = request.name, description = request.description, author = author, branch = branch)
        }?.let(::open)
    }

    fun import() = scope.launch {
        if (!requireAccount("import a repository")) return@launch
        val choice = messenger.showActionSheet(
            actions = listOf(
                FmSheetAction("Folder — copy a folder from Files into a new repository"),
                FmSheetAction("Zip archive — extract a .zip into a new repository"),
            ),
        ) ?: return@launch
        if (choice == 0) {
            folderPicker.launch(null)
        } else {
            zipPicker.launch(arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream"))
        }
    }

    Column(Modifier.fillMaxWidth()) {
        FmSectionHeader(
            title = "On this device",
            icon = "phone_iphone",
            count = records.size,
            modifier = Modifier.padding(start = FmSpace.x4, top = FmSpace.x2, end = FmSpace.x4, bottom = FmSpace.x2),
        )
        val error = loadError
        when {
            error != null -> Text(
                error,
                modifier = Modifier.padding(horizontal = FmSpace.x4),
                style = Fm.text.bodySmall.copy(color = c.danger),
            )
            !store.isLoaded -> Box(
                Modifier
                    .fillMaxWidth()
                    .padding(FmSpace.x3),
                contentAlignment = Alignment.Center,
            ) {
                FmCircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
            }
            records.isEmpty() -> Text(
                "No local repositories yet. Create one or import a folder from Files — everything stays on this phone until you publish.",
                modifier = Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x2),
                style = Fm.text.bodyMedium.copy(color = c.textSecondary),
            )
            else -> Column(
                Modifier
                    .fillMaxWidth()
                    .background(c.listSurface),
            ) {
                records.forEachIndexed { index, record ->
                    if (index > 0) FmRowDivider()
                    LocalRepoTile(record, onLongPress = { actions(record) }) { open(record) }
                }
            }
        }
        FmInlineStatus(status, Modifier.padding(start = FmSpace.x4, top = FmSpace.x2, end = FmSpace.x4))
        FlowRow(
            Modifier.padding(start = FmSpace.x4, top = FmSpace.x2, end = FmSpace.x4, bottom = FmSpace.x3),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            if (canSync) {
                FmToneButton(label = syncAllLabel(records), onPressed = if (busy) null else ({ sync(null) }), icon = "sync", tone = FmTone.Accent)
            }
            FmToneButton(label = "New repository", onPressed = if (busy) null else ({ create() }), icon = "add", tone = FmTone.Accent)
            FmToneButton(label = "Import from Files", onPressed = if (busy) null else ({ import() }), icon = "drive_folder_upload_outlined")
            FmToneButton(label = "Mirror from GitHub/GitLab/Codeberg/Gitea", onPressed = if (busy) null else mirrorFromProvider, icon = "cloud_sync_outlined")
        }
        HorizontalDivider(thickness = 1.dp, color = c.border)
        FmSectionHeader(
            title = "Network",
            icon = "public",
            modifier = Modifier.padding(start = FmSpace.x4, top = FmSpace.x3, end = FmSpace.x4, bottom = FmSpace.x2),
        )
    }
}

@Composable
private fun LocalRepoTile(record: LocalRepoRecord, onLongPress: () -> Unit, onTap: () -> Unit) {
    val c = Fm.colors
    val tint = when (record.syncLabel) {
        "Published" -> c.success
        "Cloned" -> c.accent
        else -> c.textSecondary
    }
    FmListRow(
        title = record.name,
        subtitle = localRepoTileMeta(record),
        leading = { FmIconTile(if (record.isCloned) "cloud_done_outlined" else "phone_iphone", tint = c.accent, size = 40.dp) },
        trailing = { FmStatusBadge(record.syncLabel, tint) },
        onTap = onTap,
        onLongPress = onLongPress,
    )
}

@Composable
private fun NewRepositoryDialog(done: (NewRepoRequest?) -> Unit) {
    var name by remember { mutableStateOf("") }
    var description by remember { mutableStateOf("") }
    var problem by remember { mutableStateOf<String?>(null) }
    FmAlertDialog(
        onDismissRequest = { done(null) },
        title = "New repository",
        actions = {
            FmDialogActions(
                onCancel = { done(null) },
                onConfirm = {
                    val trimmed = name.trim()
                    val error = LocalRepoStore.validateName(trimmed)
                    if (error != null) problem = error else done(NewRepoRequest(trimmed, description))
                },
            )
        },
    ) {
        FmField("Name") {
            FmTextField(
                value = name,
                onValueChange = {
                    name = it
                    problem = null
                },
                hint = "my-project",
                errorText = problem,
                autofocus = true,
            )
        }
        Spacer(Modifier.height(FmSpace.x3))
        FmField("Description (optional)") { FmTextField(description, { description = it }, maxLines = 3) }
        Spacer(Modifier.height(FmSpace.x3))
        Text(
            "Creates README.md and an initial commit on main. Stays on this device until you publish it.",
            style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
        )
    }
}

private suspend fun askImportName(messenger: FmMessenger, initial: String): String? = messenger.showDialog { done ->
    RepoNameDialog(title = "Import from Files", initial = initial, hint = "Repository name", confirmLabel = "Import", done = done)
}

internal suspend fun <T> runStoreBusy(messenger: FmMessenger, store: LocalRepoStore, label: String, status: FmStatus, body: () -> T): T? {
    var error: String? = null
    status.clear()
    val result = messenger.runBusy(busyLabel(label), onCancel = store::cancelActive) {
        try {
            withContext(Dispatchers.IO) { body() }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            error = describeLocalRepoFailure(failure)
            null
        }
    }
    error?.let { status.error(it) }
    return result
}
