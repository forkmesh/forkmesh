package com.forkmesh.nativeapp.feature.account.settings

import android.net.Uri
import android.text.format.Formatter
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.code.FmFilledButton
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.platform.AppearanceStore
import com.forkmesh.nativeapp.settings.AccountDataBrowser
import com.forkmesh.nativeapp.settings.AccountDataField
import com.forkmesh.nativeapp.settings.AccountDataGroup
import com.forkmesh.nativeapp.settings.AccountDataModel
import com.forkmesh.nativeapp.settings.AccountDataRow
import com.forkmesh.nativeapp.settings.AccountDataSubject
import com.forkmesh.nativeapp.settings.AccountStoreItem
import com.forkmesh.nativeapp.settings.AccountSyncService
import com.forkmesh.nativeapp.settings.AccountTime
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmKeyValueRow
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.confirmAction
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
private fun DataIcon(name: String) {
    FmIcon(name, size = 22.dp, tint = Fm.colors.textSecondary)
}

private fun subjectIcon(subject: AccountDataSubject): String = when (subject.kind) {
    "account" -> "person_outline"
    "repositories" -> "folder_outlined"
    "organization" -> "groups_outlined"
    else -> "storage_outlined"
}

private fun storeIcon(item: AccountStoreItem): String = when (item.kind) {
    "layout" -> "grid_view_outlined"
    "subscriptions" -> "article_outlined"
    else -> "tune"
}

class AccountDataScreen : Screen() {
    override val title: String get() = "Data"

    private val model: AccountDataModel? by lazy { SettingsArea.endpoints?.let(::AccountDataModel) }
    private val browser: AccountDataBrowser? by lazy { SettingsArea.endpoints?.let(::AccountDataBrowser) }
    private var revision by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val exportStatus = rememberFmStatus()
        val cacheStatus = rememberFmStatus()
        val syncStatus = rememberFmStatus()
        val storeStatus = rememberFmStatus()
        val model = model
        val browser = browser
        val sync = Services.get<AccountSyncService>()
        val signedIn = SettingsArea.signedIn
        revision
        fun background(block: () -> Unit, after: () -> Unit = {}) {
            scope.launch {
                withContext(Dispatchers.IO) { block() }
                revision++
                after()
            }
        }
        if (browser != null && signedIn) {
            LaunchedEffect(browser) {
                if (browser.inventory == null) {
                    revision++
                    withContext(Dispatchers.IO) { browser.load() }
                    revision++
                }
            }
        }
        val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri: Uri? ->
            val file = model?.exportFile
            if (uri == null || file == null) return@rememberLauncherForActivityResult
            scope.launch {
                val ok = withContext(Dispatchers.IO) {
                    runCatching { context.contentResolver.openOutputStream(uri, "wt")?.use { it.write(file.json.toByteArray()) } != null }.getOrDefault(false)
                }
                model.clearExport()
                exportStatus.report(if (ok) "Export saved." else "Could not save the export.", !ok)
                revision++
            }
        }
        SettingsPage("Data", navigator) {
            if (sync != null) {
                item {
                    SettingsCard("Save to ForkMesh") {
                        Row(Modifier.testTag("account-sync-switch")) {
                            SettingsSwitch("Save my settings to ForkMesh", null, sync.enabled) { on ->
                                sync.enabled = on
                                revision++
                                if (on && signedIn) background({
                                    sync.saveNow()
                                    browser?.load(refresh = true)
                                })
                            }
                        }
                        SettingsNote(
                            "Your home screen layout, news reader subscriptions and app settings are saved to your account, so a new phone or a reinstall can get them back. They are encrypted with a key that belongs to your account alone, and that key is itself encrypted on the server. Sign-in keys, passwords and chat keys never leave this phone.",
                        )
                        FmKeyValueRow("Last saved", AccountTime.label(sync.lastSavedAt, System.currentTimeMillis()))
                        if (sync.error.isNotEmpty()) SettingsNote(sync.error, Fm.colors.danger)
                        FmInlineStatus(syncStatus)
                        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            FmFilledButton(if (sync.busy) "Saving…" else "Save now", onClick = if (sync.busy || !signedIn) null else ({
                                syncStatus.clear()
                                scope.launch {
                                    val saved = withContext(Dispatchers.IO) { sync.saveNow() }
                                    revision++
                                    if (sync.error.isEmpty()) {
                                        syncStatus.show(if (saved == 0) "Nothing to save yet" else if (saved == 1) "Saved 1 item" else "Saved $saved items")
                                        withContext(Dispatchers.IO) { browser?.load(refresh = true) }
                                        revision++
                                    }
                                }
                            }), icon = "cloud_upload_outlined")
                            FmOutlinedButton("Restore", onClick = if (sync.busy || !signedIn) null else ({
                                scope.launch {
                                    val sure = messenger.confirmAction(
                                        "Restore saved data?",
                                        "Restore",
                                        "This phone's home screen layout, news reader subscriptions and app settings are replaced with the copy saved on ForkMesh.",
                                        destructive = false,
                                    )
                                    if (!sure) return@launch
                                    syncStatus.clear()
                                    val applied = withContext(Dispatchers.IO) { sync.restore() }
                                    AppearanceStore.reload()
                                    revision++
                                    if (sync.error.isEmpty()) {
                                        syncStatus.show(
                                            if (applied == 0) "The saved copy matches this phone; nothing changed."
                                            else "Restored $applied ${if (applied == 1) "setting" else "settings and subscriptions"}. Some appear after you reopen ForkMesh.",
                                        )
                                    }
                                }
                            }), icon = "history")
                        }
                    }
                }
            }
            item {
                SettingsCard("Everything ForkMesh has about you") {
                    SettingsNote(
                        "Every place ForkMesh keeps something about you: your account and linked nodes, the repositories you own, and each organization you belong to. Open a section to see each item, and remove the ones that can be removed on their own.",
                    )
                    val inventory = browser?.inventory
                    when {
                        browser == null || !signedIn -> SettingsNote("Sign in to see what ForkMesh has about you.")
                        browser.loading && inventory == null -> FmSkeletonList(rows = 3, label = "Looking through your data")
                        inventory != null && inventory.subjects.isNotEmpty() -> Column {
                            inventory.subjects.forEachIndexed { index, subject ->
                                if (index > 0) FmRowDivider(indent = 0.dp)
                                FmListRow(
                                    subject.title,
                                    subtitle = subject.subtitle,
                                    leading = { DataIcon(subjectIcon(subject)) },
                                    onTap = { navigator.push(AccountDataSubjectScreen(browser, subject.id)) },
                                    padding = androidx.compose.foundation.layout.PaddingValues(vertical = FmSpace.x2),
                                )
                            }
                        }
                        else -> SettingsNote(if (browser.error.isEmpty()) "Nothing to show." else browser.error, if (browser.error.isEmpty()) Fm.colors.textSecondary else Fm.colors.danger)
                    }
                    if (browser != null && browser.error.isNotEmpty() && inventory != null) SettingsNote(browser.error, Fm.colors.danger)
                    if (browser != null && signedIn) {
                        FmTextButton(onClick = { background({ browser.load(refresh = true) }) }, enabled = !browser.loading) {
                            Text(if (browser.loading) "Checking…" else "Check again")
                        }
                    }
                }
            }
            item {
                SettingsCard("Saved from your phone") {
                    val items = browser?.inventory?.store.orEmpty()
                    if (items.isEmpty()) {
                        SettingsNote("Nothing saved yet. Switch on Save to ForkMesh to keep your layout, subscriptions and settings with your account.")
                    } else {
                        items.forEachIndexed { index, item ->
                            if (index > 0) FmRowDivider(indent = 0.dp)
                            FmListRow(
                                item.title,
                                subtitle = storeSubtitle(context, item),
                                leading = { DataIcon(storeIcon(item)) },
                                trailing = {
                                    FmIconButton("delete_outline", "Delete ${item.title}", onPressed = {
                                        scope.launch {
                                            val sure = messenger.confirmAction(
                                                "Delete ${item.title}?",
                                                "Delete",
                                                "The copy saved on ForkMesh is deleted. This phone keeps its own settings, and saves them again while Save to ForkMesh is on.",
                                            )
                                            if (!sure || browser == null) return@launch
                                            val ok = withContext(Dispatchers.IO) { browser.deleteStore(item.key) }
                                            if (ok) {
                                                sync?.forgetHashes()
                                                storeStatus.show("Deleted from ForkMesh")
                                            }
                                            revision++
                                        }
                                    }, color = Fm.colors.textTertiary, iconSize = 20.dp)
                                },
                                showChevron = false,
                                padding = androidx.compose.foundation.layout.PaddingValues(vertical = FmSpace.x2),
                            )
                        }
                        FmTextButton(onClick = {
                            scope.launch {
                                val sure = messenger.confirmAction(
                                    "Delete everything saved from your phone?",
                                    "Delete all",
                                    "Every copy saved on ForkMesh and its encryption key are deleted, and Save to ForkMesh is switched off. This phone keeps its own settings.",
                                )
                                if (!sure || browser == null) return@launch
                                val ok = withContext(Dispatchers.IO) { browser.deleteStore(null) }
                                if (ok) {
                                    sync?.enabled = false
                                    storeStatus.show("Deleted everything saved from your phone")
                                }
                                revision++
                            }
                        }, contentColor = Fm.colors.danger) { Text("Delete everything saved from your phone") }
                    }
                    FmInlineStatus(storeStatus)
                }
            }
            item {
                SettingsCard("Download your data") {
                    SettingsNote(
                        "Download everything listed above as one JSON file: your account, devices, SSH public keys, settings saved from your phone, and every item in every section. Password hashes, private keys and other credentials are never included.",
                    )
                    if (model == null) {
                        SettingsNote("Sign in to download your data.")
                    } else {
                        if (model.error.isNotEmpty()) SettingsNote(model.error, Fm.colors.danger)
                        FmInlineStatus(exportStatus)
                        FmFilledButton(if (model.busy) "Preparing…" else "Download all my data", onClick = if (model.busy || !signedIn) null else ({
                            exportStatus.clear()
                            scope.launch {
                                val file = withContext(Dispatchers.IO) { model.prepareExport(SettingsArea.account) }
                                revision++
                                if (file != null) save.launch(file.fileName)
                            }
                        }), icon = "download_outlined")
                    }
                }
            }
            item {
                SettingsCard("Data folder") {
                    SettingsNote(DataFolderFeature.summary())
                    FmTextButton(onClick = { navigator.push(DataFolderScreen()) }) { Text("Set up the data folder") }
                }
            }
            item {
                SettingsCard("This phone") {
                    SettingsNote("Cached repository pages, pictures and search results are rebuilt as you browse.")
                    FmTextButton(onClick = {
                        scope.launch {
                            withContext(Dispatchers.IO) { runCatching { context.cacheDir.listFiles()?.forEach { it.deleteRecursively() } } }
                            cacheStatus.show("Cache cleared.")
                        }
                    }) { Text("Clear the cache") }
                    FmInlineStatus(cacheStatus)
                }
            }
        }
    }

    private fun storeSubtitle(context: android.content.Context, item: AccountStoreItem): String {
        val parts = mutableListOf(Formatter.formatShortFileSize(context, item.bytes.toLong()))
        parts.add("saved " + AccountTime.label(item.updatedAt, System.currentTimeMillis()))
        if (item.device.isNotEmpty()) parts.add("from " + item.device)
        return parts.joinToString(" · ")
    }
}

internal class AccountDataSubjectScreen(private val browser: AccountDataBrowser, private val subjectId: String) : Screen() {
    override val title: String get() = browser.inventory?.subjects?.firstOrNull { it.id == subjectId }?.title ?: "Your data"

    private var revision by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        revision
        val subject = browser.inventory?.subjects?.firstOrNull { it.id == subjectId }
        val sections = browser.inventory?.groups(subjectId).orEmpty()
        LaunchedEffect(Unit) { revision++ }
        SettingsPage(subject?.title ?: "Your data", navigator) {
            if (subject != null) item { SettingsNote(subject.subtitle) }
            if (sections.isEmpty()) item { SettingsNote("Nothing here. ForkMesh keeps nothing in this section any more.") }
            items(sections, key = { it.category }) { section ->
                SettingsCard(section.category) {
                    section.groups.forEachIndexed { index, group ->
                        if (index > 0) FmRowDivider(indent = 0.dp)
                        FmListRow(
                            group.label,
                            subtitle = groupSubtitle(group),
                            leading = { DataIcon(groupIcon(group)) },
                            onTap = { navigator.push(AccountDataGroupScreen(browser, group)) },
                            padding = androidx.compose.foundation.layout.PaddingValues(vertical = FmSpace.x2),
                        )
                    }
                }
            }
        }
    }

    private fun groupIcon(group: AccountDataGroup): String = when {
        group.withheld -> "lock_outline"
        !group.readable -> "visibility_off_outlined"
        else -> "storage_outlined"
    }

    private fun groupSubtitle(group: AccountDataGroup): String {
        val count = if (group.rows == 1 && !group.truncated) "1 item" else "${group.countText} items"
        val note = when {
            group.withheld -> "credentials, not shown"
            !group.readable -> "organization only"
            group.deletable -> "removable"
            else -> ""
        }
        return if (note.isEmpty()) count else "$count · $note"
    }
}

internal class AccountDataGroupScreen(private val browser: AccountDataBrowser, private val group: AccountDataGroup) : Screen() {
    override val title: String get() = group.label

    private var revision by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val status = rememberFmStatus()
        var expanded by remember { mutableStateOf(emptySet<String>()) }
        revision
        LaunchedEffect(group.id) {
            if (browser.page(group) == null) {
                revision++
                withContext(Dispatchers.IO) { browser.loadRows(group) }
                revision++
            }
        }
        val page = browser.page(group)
        val loading = browser.loadingGroup == group.id
        SettingsPage(group.label, navigator) {
            if (group.note.isNotEmpty()) item { SettingsNote(group.note) }
            when {
                page == null && loading -> item { FmSkeletonList(rows = 4, hasLeading = false, label = "Loading items") }
                page == null -> Unit
                !page.readable -> item { SettingsCard { SettingsNote(page.reasonText) } }
                page.rows.isEmpty() -> item { SettingsNote("Nothing here. ForkMesh keeps nothing here any more.") }
                else -> {
                    items(page.rows, key = { it.id }) { row ->
                        val open = row.id in expanded
                        val trailing: (@Composable () -> Unit)? = if (row.deletable) {
                            {
                                FmIconButton("delete_outline", "Remove ${row.title}", onPressed = {
                                    scope.launch { remove(messenger, row, status) }
                                }, color = Fm.colors.textTertiary, iconSize = 20.dp)
                            }
                        } else {
                            null
                        }
                        SettingsCard {
                            FmListRow(
                                row.title,
                                subtitle = row.subtitle.ifEmpty { null },
                                leading = { DataIcon(if (open) "expand_less" else "expand_more") },
                                trailing = trailing,
                                onTap = { expanded = if (open) expanded - row.id else expanded + row.id },
                                showChevron = false,
                                padding = androidx.compose.foundation.layout.PaddingValues(vertical = FmSpace.x1),
                            )
                            if (open) {
                                FmRowDivider(indent = 0.dp)
                                Column(Modifier.fillMaxWidth().padding(vertical = FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                    row.fields.forEach { FieldView(it) }
                                }
                            }
                        }
                    }
                    if (page.next > 0) {
                        item {
                            FmOutlinedButton(if (loading) "Loading…" else "Show more", onClick = if (loading) null else ({
                                scope.launch {
                                    revision++
                                    withContext(Dispatchers.IO) { browser.loadRows(group, more = true) }
                                    revision++
                                }
                            }), icon = "expand_more")
                        }
                    }
                }
            }
            if (browser.rowsError.isNotEmpty()) item { SettingsNote(browser.rowsError, Fm.colors.danger) }
            item { FmInlineStatus(status) }
        }
    }

    private suspend fun remove(messenger: com.forkmesh.nativeapp.ui.kit.FmMessenger, row: AccountDataRow, status: com.forkmesh.nativeapp.ui.kit.FmStatus) {
        val sure = messenger.confirmAction("Remove ${row.title}?", "Remove", "ForkMesh deletes this item from its database. This cannot be undone.")
        if (!sure) return
        val ok = withContext(Dispatchers.IO) { browser.delete(row, group) }
        if (ok) status.show("Removed")
        revision++
    }

    @Composable
    private fun FieldView(field: AccountDataField) {
        val c = Fm.colors
        if (field.isStructured || field.text.length > 60) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                Text(field.label, style = Fm.text.labelMedium.copy(color = c.textTertiary))
                SelectionContainer {
                    Text(field.text, style = Fm.text.bodySmall.copy(color = c.textPrimary, fontFamily = if (field.isStructured) FmType.mono else null))
                }
            }
        } else {
            FmKeyValueRow(field.label, field.text)
        }
    }
}
