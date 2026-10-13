package com.forkmesh.nativeapp.feature.admin

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.CircularProgressIndicator
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
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.admin.AdminClient
import com.forkmesh.nativeapp.admin.AdminOverview
import com.forkmesh.nativeapp.admin.AdminRelay
import com.forkmesh.nativeapp.admin.AdminSection
import com.forkmesh.nativeapp.admin.AdminUser
import com.forkmesh.nativeapp.admin.AdminUserAction
import com.forkmesh.nativeapp.admin.AdminUsers
import com.forkmesh.nativeapp.admin.CronSummary
import com.forkmesh.nativeapp.admin.DatabaseInventory
import com.forkmesh.nativeapp.admin.DatabasePage
import com.forkmesh.nativeapp.admin.DatabaseTable
import com.forkmesh.nativeapp.admin.ErrorGroup
import com.forkmesh.nativeapp.admin.ErrorReport
import com.forkmesh.nativeapp.admin.PendingVerification
import com.forkmesh.nativeapp.admin.StorageUsage
import com.forkmesh.nativeapp.feature.account.projects.UserPresence
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.insights.BarChart
import com.forkmesh.nativeapp.feature.insights.InsightsFeature
import com.forkmesh.nativeapp.feature.insights.io
import com.forkmesh.nativeapp.feature.insights.toneColor
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.insights.InsightsException
import com.forkmesh.nativeapp.insights.InsightsLoad
import com.forkmesh.nativeapp.insights.StatsFormat
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

object AdminFeature {
    private var appRef: App? = null

    val app: App? get() = appRef

    fun install(app: App) {
        appRef = app
        NavigationFeature.register(Route.Kind.Admin, "WS20") { route -> AdminScreen(AdminSection.of((route as Route.Admin).section)) }
    }

    fun client(): AdminClient? = InsightsFeature.transport?.let(::AdminClient)

    fun adminScreen(section: AdminSection = AdminSection.Overview): Screen = AdminScreen(section)
}

private class PendingUserAction(val action: AdminUserAction, val target: String)

internal class AdminScreen(initial: AdminSection) : Screen() {
    override val title: String get() = "Admin"
    private var section by mutableStateOf(initial)
    private var overview by mutableStateOf<InsightsLoad<AdminOverview>>(InsightsLoad.Idle)
    private var errors by mutableStateOf<InsightsLoad<ErrorReport>>(InsightsLoad.Idle)
    private var users by mutableStateOf<InsightsLoad<AdminUsers>>(InsightsLoad.Idle)
    private var pending by mutableStateOf<InsightsLoad<List<PendingVerification>>>(InsightsLoad.Idle)
    private var inventory by mutableStateOf<InsightsLoad<DatabaseInventory>>(InsightsLoad.Idle)
    private var cron by mutableStateOf<InsightsLoad<CronSummary>>(InsightsLoad.Idle)
    private var storage by mutableStateOf<InsightsLoad<StorageUsage>>(InsightsLoad.Idle)
    private var relays by mutableStateOf<InsightsLoad<List<AdminRelay>>>(InsightsLoad.Idle)
    private var denied by mutableStateOf(false)
    private var lastAction by mutableStateOf("")
    private var actionError by mutableStateOf("")
    private var busy by mutableStateOf<Set<String>>(emptySet())
    private var query by mutableStateOf("")
    private var showQuarantined by mutableStateOf(false)
    private var sourceFilter by mutableStateOf("")
    private var confirmUser by mutableStateOf<PendingUserAction?>(null)
    private var purgeOwner by mutableStateOf("")
    private var purgeName by mutableStateOf("")
    private var purgeTyped by mutableStateOf("")

    private fun <T> track(result: InsightsLoad<T>): InsightsLoad<T> {
        val error = result.errorOrNull
        if (error != null && error.needsAdmin) denied = true
        return result
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val client = remember { AdminFeature.client() }
        fun reload() {
            val admin = client ?: return
            scope.launch {
                when (section) {
                    AdminSection.Overview -> {
                        if (overview.valueOrNull == null) overview = InsightsLoad.Loading
                        val current = overview
                        overview = track(io { InsightsLoad.attempt(current) { admin.overview() } })
                    }
                    AdminSection.Users -> {
                        if (users.valueOrNull == null) users = InsightsLoad.Loading
                        val current = users
                        users = track(io { InsightsLoad.attempt(current) { admin.users() } })
                        val p = pending
                        pending = track(io { InsightsLoad.attempt(p) { admin.pending() } })
                    }
                    AdminSection.Errors -> {
                        if (errors.valueOrNull == null) errors = InsightsLoad.Loading
                        val current = errors
                        errors = track(io { InsightsLoad.attempt(current) { admin.errors() } })
                    }
                    AdminSection.Database -> {
                        if (inventory.valueOrNull == null) inventory = InsightsLoad.Loading
                        val current = inventory
                        inventory = track(io { InsightsLoad.attempt(current) { admin.inventory() } })
                    }
                    AdminSection.Usage -> {
                        if (cron.valueOrNull == null) cron = InsightsLoad.Loading
                        val a = cron
                        cron = track(io { InsightsLoad.attempt(a) { admin.cron() } })
                        val b = storage
                        storage = track(io { InsightsLoad.attempt(b) { admin.storage() } })
                        val d = relays
                        relays = track(io { InsightsLoad.attempt(d) { admin.relays() } })
                    }
                    AdminSection.Repos -> Unit
                }
            }
        }
        LaunchedEffect(section) { reload() }
        DetailScaffold(title, navigator, actions = { FmIconButton("refresh", "Refresh", onPressed = { reload() }, color = c.textPrimary) }) {
            Column(Modifier.fillMaxSize()) {
                FmSegmentTabBar(AdminSection.entries.map { FmSegmentTab(it.title) }, section.ordinal, onChanged = { section = AdminSection.entries[it] })
                LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    if (client == null) {
                        item { FmEmptyState(icon = "lock_outline", title = "Sign in first", message = "The admin console needs an administrator account.") }
                        return@LazyColumn
                    }
                    if (denied) {
                        item { FmEmptyState(icon = "lock_outline", title = "Administrators only", message = "This needs an administrator account.") }
                        return@LazyColumn
                    }
                    if (lastAction.isNotEmpty()) item { Text(lastAction, style = Fm.text.bodySmall.copy(color = c.success)) }
                    if (actionError.isNotEmpty()) item { Text(actionError, style = Fm.text.bodySmall.copy(color = c.danger)) }
                    when (section) {
                        AdminSection.Overview -> overviewItems()
                        AdminSection.Users -> usersItems()
                        AdminSection.Errors -> errorsItems(navigator)
                        AdminSection.Database -> databaseItems(navigator)
                        AdminSection.Usage -> usageItems()
                        AdminSection.Repos -> purgeItems(client)
                    }
                }
            }
        }
        confirmUser?.let { item ->
            val messenger = LocalFmMessenger.current
            FmConfirmDialog(
                title = "${item.action.title} ${item.target}?",
                body = when (item.action) {
                    AdminUserAction.Delete -> "The account and its data are deleted. This cannot be undone."
                    AdminUserAction.Quarantine -> "The account is signed out and cannot sign in until restored."
                    AdminUserAction.Unquarantine -> "The account can sign in again."
                },
                confirmLabel = item.action.title,
                destructive = item.action != AdminUserAction.Unquarantine,
                onDismissRequest = { confirmUser = null },
                onConfirm = {
                    confirmUser = null
                    val admin = client ?: return@FmConfirmDialog
                    scope.launch {
                        if (item.action.needsDeviceAuth) {
                            val context = AdminFeature.app
                            if (context == null || !UserPresence.confirm(context, messenger, "${item.action.title} ${item.target}")) {
                                actionError = "Your fingerprint, face or screen lock is needed for this."
                                return@launch
                            }
                        }
                        busy = busy + item.target
                        try {
                            io { admin.perform(item.action, item.target) }
                            actionError = ""
                            lastAction = "${item.action.pastTense} ${item.target}"
                            val current = users
                            users = io { InsightsLoad.attempt(current) { admin.users() } }
                        } catch (cancelled: CancellationException) {
                            throw cancelled
                        } catch (failure: Exception) {
                            actionError = InsightsException.from(failure).message
                        } finally {
                            busy = busy - item.target
                        }
                    }
                },
            )
        }
    }

    private fun <T> LazyListScope.state(load: InsightsLoad<T>, content: LazyListScope.(T) -> Unit) {
        val value = load.valueOrNull
        when {
            value != null -> content(value)
            load is InsightsLoad.Failed -> item { Text(load.error.message, style = Fm.text.bodySmall.copy(color = Fm.colors.danger)) }
            else -> item { CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp) }
        }
    }

    private fun LazyListScope.overviewItems() = state(overview) { value ->
        item { if (value.actor.isNotEmpty()) Text("Signed in as ${value.actor}", style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary)) }
        val facts = listOf(
            "Accounts" to value.users,
            "Quarantined" to value.quarantinedUsers,
            "Errors in 24 h" to value.errors24h,
            "Waiting for email verification" to value.pendingVerifications,
            "Relays awaiting approval" to value.relaysAwaitingApproval,
        )
        items(facts, key = { it.first }) { (label, count) ->
            FmCard(Modifier.fillMaxWidth()) {
                Row {
                    Text(label, style = Fm.text.bodyMedium.copy(color = Fm.colors.textPrimary), modifier = Modifier.weight(1f))
                    Text(count?.let { StatsFormat.compactNumber(it.toDouble()) } ?: "—", style = Fm.text.titleSmall.copy(color = Fm.colors.textPrimary))
                }
            }
        }
    }

    private fun LazyListScope.usersItems() = state(users) { value ->
        item { FmTextField(value = query, onValueChange = { query = it }, hint = "Search names or emails") }
        item {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Show quarantined (${value.quarantined.size})", style = Fm.text.bodyMedium.copy(color = Fm.colors.textPrimary), modifier = Modifier.weight(1f))
                FmSwitch(checked = showQuarantined, onCheckedChange = { showQuarantined = it })
            }
        }
        val rows = value.filtered(query, showQuarantined)
        item { FmSectionHeader(if (showQuarantined) "Quarantined" else "Accounts", count = rows.size) }
        items(rows.take(300), key = { it.name }) { user -> UserCard(user) }
        pending.valueOrNull?.takeIf { it.isNotEmpty() }?.let { list ->
            item { FmSectionHeader("Waiting for email verification", count = list.size) }
            items(list, key = { "p-" + it.name }) { entry ->
                FmListRow(entry.name, subtitle = if (entry.joinedAt > 0) "joined ${relativeTime(entry.joinedAt)}" else null, showChevron = false)
            }
        }
    }

    @Composable
    private fun UserCard(user: AdminUser) {
        val c = Fm.colors
        FmCard(Modifier.fillMaxWidth()) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(user.name, style = Fm.text.titleSmall.copy(color = c.textPrimary), modifier = Modifier.weight(1f))
                    for (badge in user.badges) {
                        FmStatusBadge(badge, if (badge == "Admin") c.accent else if (badge == "Quarantined") c.danger else c.textSecondary)
                        Spacer(Modifier.width(FmSpace.x1))
                    }
                }
                val facts = listOf(
                    user.email,
                    if (user.createdAt > 0) "joined ${relativeTime(user.createdAt)}" else "",
                    if (user.nodes > 0) "${user.nodes} node${if (user.nodes == 1) "" else "s"}" else "",
                    user.signupCountry,
                    user.status,
                ).filter { it.isNotEmpty() }
                if (facts.isNotEmpty()) Text(facts.joinToString(" · "), style = Fm.text.labelSmall.copy(color = c.textSecondary))
                if (!user.isAdmin) {
                    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                        val working = user.name in busy
                        if (user.quarantined) {
                            FmToneButton("Restore", onPressed = if (working) null else ({ confirmUser = PendingUserAction(AdminUserAction.Unquarantine, user.name) }))
                        } else {
                            FmToneButton("Quarantine", tone = FmTone.Warning, onPressed = if (working) null else ({ confirmUser = PendingUserAction(AdminUserAction.Quarantine, user.name) }))
                        }
                        FmToneButton("Delete", tone = FmTone.Danger, onPressed = if (working) null else ({ confirmUser = PendingUserAction(AdminUserAction.Delete, user.name) }))
                    }
                }
            }
        }
    }

    private fun LazyListScope.errorsItems(navigator: Navigator) = state(errors) { value ->
        item {
            Text(
                "${value.total} errors in 24 h · ${value.groupCount} groups · ${value.usersHit} accounts hit" + if (value.truncated) " · list shortened" else "",
                style = Fm.text.bodySmall.copy(color = Fm.colors.textPrimary),
            )
        }
        if (value.hours.isNotEmpty()) item { BarChart(value.hours.map { it.toDouble() }, Fm.colors.danger, height = 60.dp) }
        item {
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                for ((label, source) in listOf("All" to "", "Server" to "server", "Desktop" to "desktop", "Browser" to "javascript")) {
                    FmToneButton(label, filled = sourceFilter == source, tone = if (sourceFilter == source) FmTone.Accent else FmTone.Neutral, onPressed = { sourceFilter = source })
                }
            }
        }
        val groups = value.filtered(sourceFilter.ifEmpty { null })
        if (groups.isEmpty()) item { Text("No errors in this group.", style = Fm.text.bodySmall.copy(color = Fm.colors.success)) }
        items(groups, key = { it.id }) { group -> ErrorCard(group, navigator) }
    }

    @Composable
    private fun ErrorCard(group: ErrorGroup, navigator: Navigator) {
        val c = Fm.colors
        FmCard(Modifier.fillMaxWidth()) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmStatusBadge(group.status.ifEmpty { "?" }, c.danger)
                    Spacer(Modifier.width(FmSpace.x2))
                    Text(group.sourceLabel, style = Fm.text.labelSmall.copy(color = c.textSecondary), modifier = Modifier.weight(1f))
                    Text("×${group.count}", style = Fm.text.labelMedium.copy(color = c.textPrimary))
                }
                SelectionContainer { Text(group.title, style = Fm.text.bodySmall.copy(color = c.textPrimary), maxLines = 6) }
                if (group.requestLine.isNotEmpty()) Text(group.requestLine, style = Fm.text.labelSmall.copy(fontFamily = FmType.mono, color = c.textSecondary))
                val meta = listOf(group.affected, if (group.lastSeen > 0) "last ${relativeTime(group.lastSeen)}" else "").filter { it.isNotEmpty() }
                if (meta.isNotEmpty()) Text(meta.joinToString(" · "), style = Fm.text.labelSmall.copy(color = c.textTertiary))
                FmToneButton("Send to an agent", icon = "auto_awesome", tone = FmTone.Accent, onPressed = {
                    navigator.push(AgentsFeature.startSheetScreen(group.agentPrompt(InsightsFeature.hostLabel)))
                })
            }
        }
    }

    private fun LazyListScope.databaseItems(navigator: Navigator) = state(inventory) { value ->
        item { FmTextField(value = query, onValueChange = { query = it }, hint = "Filter tables") }
        item { Text("${value.tables.size} tables · ${StatsFormat.compactNumber(value.totalRows.toDouble())} rows", style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary)) }
        items(value.filtered(query), key = { it.name }) { table -> TableRow(table, navigator) }
    }

    @Composable
    private fun TableRow(table: DatabaseTable, navigator: Navigator) {
        val c = Fm.colors
        FmListRow(
            table.name,
            subtitle = listOf(
                "${StatsFormat.compactNumber(table.rows.toDouble())} rows",
                table.storage,
                if (table.plaintextData) "plaintext data" else "",
                if (table.restricted) "restricted" else "",
                table.auditCategory,
            ).filter { it.isNotEmpty() }.joinToString(" · "),
            onTap = if (table.restricted) null else ({ navigator.push(DatabaseRowsScreen(table.name)) }),
            showChevron = !table.restricted,
            titleColor = if (table.gaps.isNotEmpty()) c.warning else null,
        )
    }

    private fun LazyListScope.usageItems() {
        item { FmSectionHeader("Scheduled jobs") }
        state(cron) { value ->
            item {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmStatusBadge(
                        when {
                            value.unavailable.isNotEmpty() -> "Unavailable"
                            value.lastTickAt == 0L -> "No ticks yet"
                            else -> "Last tick ${relativeTime(value.lastTickAt)}"
                        },
                        toneColor(value.health),
                    )
                    Spacer(Modifier.width(FmSpace.x2))
                    Text("${value.ticks} ticks · ${value.failedTicks} failed", style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
                }
            }
            items(value.recent.take(15), key = { "t-" + it.ts }) { tick ->
                Text(
                    "${relativeTime(tick.ts)} · ${tick.ms} ms" + (if (tick.failed.isNotEmpty()) " · failed ${tick.failed.joinToString(", ")}" else ""),
                    style = Fm.text.labelSmall.copy(color = if (tick.failures > 0) Fm.colors.danger else Fm.colors.textSecondary),
                )
            }
        }
        item { FmSectionHeader("Storage") }
        state(storage) { value ->
            items(value.entries, key = { "s-" + it.label }) { entry ->
                Row {
                    Text(entry.label, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary), modifier = Modifier.weight(1f))
                    Text(entry.value, style = Fm.text.bodySmall.copy(color = Fm.colors.textPrimary))
                }
            }
        }
        item { FmSectionHeader("Relays") }
        state(relays) { value ->
            if (value.isEmpty()) item { Text("No relays registered.", style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary)) }
            items(value, key = { "r-" + it.baseUrl + it.label }) { relay ->
                FmListRow(relay.label.ifEmpty { relay.baseUrl }, subtitle = "${relay.status} · ${relay.baseUrl}", showChevron = false)
            }
        }
    }

    private fun LazyListScope.purgeItems(client: AdminClient) {
        item {
            Text(
                "Purging removes a repository and everything the relay stores for it: issues, pull requests, discussions, pins and catalog rows. Mirrors keep their own copies until they sync.",
                style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary),
            )
        }
        item { FmTextField(value = purgeOwner, onValueChange = { purgeOwner = it }, hint = "Owner", keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false)) }
        item { FmTextField(value = purgeName, onValueChange = { purgeName = it }, hint = "Repository", keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false)) }
        val owner = purgeOwner.trim()
        val name = purgeName.trim()
        val valid = AdminClient.validSegment(owner) && AdminClient.validSegment(name)
        val phrase = AdminClient.purgeConfirmation(owner, name)
        if (valid) {
            item {
                FmTextField(
                    value = purgeTyped,
                    onValueChange = { purgeTyped = it },
                    hint = phrase,
                    helperText = "Type $phrase to confirm",
                    keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false),
                    textStyle = Fm.text.bodyLarge.copy(fontFamily = FmType.mono),
                )
            }
        }
        item {
            PurgeButton(client, owner, name, enabled = valid && purgeTyped.trim() == phrase && phrase !in busy)
        }
    }

    @Composable
    private fun PurgeButton(client: AdminClient, owner: String, name: String, enabled: Boolean) {
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        FmToneButton("Purge repository", icon = "delete_forever", tone = FmTone.Danger, filled = true, onPressed = if (!enabled) null else ({
            val key = AdminClient.purgeConfirmation(owner, name)
            scope.launch {
                val context = AdminFeature.app
                if (context == null || !UserPresence.confirm(context, messenger, "Purge $key")) {
                    actionError = "Your fingerprint, face or screen lock is needed for this."
                    return@launch
                }
                busy = busy + key
                try {
                    io { client.purgeRepository(owner, name) }
                    actionError = ""
                    lastAction = "Purged $key"
                    purgeTyped = ""
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    actionError = InsightsException.from(failure).message
                } finally {
                    busy = busy - key
                }
            }
        }))
    }
}

internal class DatabaseRowsScreen(private val table: String) : Screen() {
    override val title: String get() = table
    private var pages by mutableStateOf<List<DatabasePage>>(emptyList())
    private var error by mutableStateOf("")
    private var loading by mutableStateOf(false)
    private var expanded by mutableStateOf<String?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val client = remember { AdminFeature.client() }
        fun loadMore() {
            val admin = client ?: return
            if (loading) return
            val cursor = pages.lastOrNull()?.nextCursor.orEmpty()
            if (pages.isNotEmpty() && cursor.isEmpty()) return
            loading = true
            scope.launch {
                try {
                    val page = io { admin.rows(table, cursor) }
                    pages = pages + page
                    error = ""
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    error = InsightsException.from(failure).message
                } finally {
                    loading = false
                }
            }
        }
        LaunchedEffect(table) { if (pages.isEmpty()) loadMore() }
        DetailScaffold(title, navigator) {
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                val rows = pages.flatMap { page -> page.rows.map { page to it } }
                items(rows.size) { index ->
                    val (_, row) = rows[index]
                    val key = "$index:${row.id}"
                    FmCard(Modifier.fillMaxWidth(), onTap = { expanded = if (expanded == key) null else key }) {
                        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                            Text(row.summary, style = Fm.text.labelMedium.copy(fontFamily = FmType.mono, color = c.textPrimary), maxLines = if (expanded == key) Int.MAX_VALUE else 2)
                            if (expanded == key) {
                                for (cell in row.cells) {
                                    SelectionContainer { Text("${cell.column}: ${cell.value}", style = Fm.text.labelSmall.copy(fontFamily = FmType.mono, color = c.textSecondary)) }
                                }
                                if (row.data.isNotEmpty()) SelectionContainer { Text(row.data, style = Fm.text.labelSmall.copy(fontFamily = FmType.mono, color = c.textSecondary)) }
                                if (row.walletKeys) Text("Holds wallet keys; values are withheld.", style = Fm.text.labelSmall.copy(color = c.warning))
                            }
                        }
                    }
                }
                val cursor = pages.lastOrNull()?.nextCursor.orEmpty()
                if (pages.isNotEmpty() && cursor.isNotEmpty()) item { FmToneButton("Load more", onPressed = if (loading) null else ({ loadMore() })) }
                if (loading) item { CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp) }
                if (pages.isNotEmpty() && rows.isEmpty()) item { Text("This table is empty.", style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                if (error.isNotEmpty()) item { Text(error, style = Fm.text.bodySmall.copy(color = c.danger)) }
            }
        }
    }
}
