package com.forkmesh.nativeapp.feature.rssreader

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import com.forkmesh.nativeapp.rssreader.RssFilter
import com.forkmesh.nativeapp.rssreader.RssFilterAction
import com.forkmesh.nativeapp.rssreader.RssFilterField
import com.forkmesh.nativeapp.rssreader.RssLibrary
import com.forkmesh.nativeapp.rssreader.RssMatcher
import com.forkmesh.nativeapp.rssreader.RssSmartFeed
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace

internal fun filterFeedName(library: RssLibrary, feedId: String): String =
    if (feedId.isEmpty()) "All Feeds" else library.feed(feedId)?.displayTitle ?: "All Feeds"

internal class RssFiltersScreen : Screen() {
    override val title: String get() = "Filters"

    @Composable
    override fun Content(navigator: Navigator) {
        val service = RssReaderFeature.service
        val version by service.version.collectAsState()
        LaunchedEffect(Unit) { service.load() }
        val library = service.library
        val filters = remember(version) { library?.filters().orEmpty() }
        RssScaffold(
            "Filters",
            navigator,
            actions = {
                FmIconButton(
                    "add",
                    "Add Filter",
                    onPressed = if (filters.size < RssLibrary.MAX_FILTERS) ({ navigator.push(RssFilterEditScreen(null)) }) else null,
                    color = Fm.colors.accent,
                )
            },
        ) {
            if (library == null || filters.isEmpty()) {
                FmEmptyState("filter_alt", "No filters", message = "Hide articles, or mark them as read when they arrive, by title, text or author.")
            } else {
                LazyColumn(Modifier.fillMaxSize()) {
                    items(filters, key = { it.id }) { filter ->
                        SettingsRow(
                            filter.pattern,
                            subtitle = listOf(
                                filter.field.label + if (filter.regex) " (Regular Expression)" else "",
                                filter.action.label,
                                filterFeedName(library, filter.feedId),
                            ).joinToString(" · "),
                            onClick = { navigator.push(RssFilterEditScreen(filter.id)) },
                        )
                        Divider()
                    }
                }
            }
        }
    }
}

internal class RssFilterEditScreen(private val filterId: String?) : Screen() {
    override val title: String get() = if (filterId == null) "New Filter" else "Edit Filter"
    private val status = FmStatus()

    @Composable
    override fun Content(navigator: Navigator) {
        val service = RssReaderFeature.service
        LaunchedEffect(Unit) { service.load() }
        val library = service.library
        val existing = remember { filterId?.let { id -> library?.filters()?.firstOrNull { it.id == id } } }
        var pattern by remember { mutableStateOf(existing?.pattern.orEmpty()) }
        var regex by remember { mutableStateOf(existing?.regex ?: false) }
        var field by remember { mutableStateOf(existing?.field ?: RssFilterField.TITLE) }
        var action by remember { mutableStateOf(existing?.action ?: RssFilterAction.HIDE) }
        var feedId by remember { mutableStateOf(existing?.feedId.orEmpty()) }
        var confirmDelete by remember { mutableStateOf(false) }
        val invalid = !RssMatcher.valid(pattern.trim(), regex)
        val feeds = remember { library?.feeds().orEmpty() }
        RssScaffold(
            title,
            navigator,
            actions = {
                FmTextButton(onClick = {
                    runCatching {
                        library?.saveFilter(RssFilter(existing?.id.orEmpty(), feedId, field, pattern, regex, action))
                        service.changed()
                    }.onSuccess { navigator.pop() }.onFailure { status.error(service.errorText(it)) }
                }, enabled = pattern.isNotBlank() && !invalid) { Text("Save") }
            },
        ) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x4))
                SettingsHeader("Match")
                Column(Modifier.fillMaxWidth().background(Fm.colors.bgRaised).padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
                    FmTextField(
                        value = pattern,
                        onValueChange = { pattern = it },
                        hint = if (regex) "Regular expression" else "Text to match",
                        autofocus = existing == null,
                        errorText = if (invalid) RssMatcher.INVALID_REGEX else null,
                    )
                }
                SettingsSwitch("Regular Expression", regex) { regex = it }
                Divider()
                ChoiceRow("Field", field, RssFilterField.entries.toList(), { it.label }) { field = it }
                Divider()
                ChoiceRow("Action", action, RssFilterAction.entries.toList(), { it.label }) { action = it }
                Divider()
                ChoiceRow("Feed", feedId, listOf("") + feeds.map { it.id }, { id -> if (id.isEmpty()) "All Feeds" else feeds.firstOrNull { it.id == id }?.displayTitle ?: "All Feeds" }) { feedId = it }
                Text(
                    "Matching ignores case. Starred articles are never hidden or marked as read by a filter. Mark as Read applies to new articles as they arrive.",
                    Modifier.padding(FmSpace.x4),
                    style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
                )
                if (existing != null) {
                    SettingsRow("Delete Filter", titleColor = Fm.colors.danger, icon = "delete", onClick = { confirmDelete = true })
                }
                Spacer(Modifier.padding(FmSpace.x5))
            }
            if (confirmDelete && existing != null) {
                FmConfirmDialog(
                    title = "Delete Filter?",
                    body = "Articles it hid come back.",
                    confirmLabel = "Delete",
                    destructive = true,
                    onDismissRequest = { confirmDelete = false },
                    onConfirm = {
                        confirmDelete = false
                        service.mutate { it.deleteFilter(existing.id) }
                        navigator.pop()
                    },
                )
            }
        }
    }
}

internal class RssSmartFeedEditScreen(private val smartId: String?) : Screen() {
    override val title: String get() = if (smartId == null) "New Smart Feed" else "Edit Smart Feed"
    private val status = FmStatus()

    private val ages = listOf(0, 1, 3, 7, 14, 30)

    private fun ageLabel(days: Int): String = when (days) {
        0 -> "Any Age"
        1 -> "1 Day"
        7 -> "1 Week"
        14 -> "2 Weeks"
        30 -> "1 Month"
        else -> "$days Days"
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val service = RssReaderFeature.service
        LaunchedEffect(Unit) { service.load() }
        val library = service.library
        val existing = remember { smartId?.let { library?.smartFeed(it) } }
        var name by remember { mutableStateOf(existing?.name.orEmpty()) }
        var words by remember { mutableStateOf(existing?.words.orEmpty()) }
        var regex by remember { mutableStateOf(existing?.regex ?: false) }
        var scope by remember { mutableStateOf(existing?.scope.orEmpty()) }
        var unreadOnly by remember { mutableStateOf(existing?.unreadOnly ?: false) }
        var starredOnly by remember { mutableStateOf(existing?.starredOnly ?: false) }
        var maxAgeDays by remember { mutableStateOf(existing?.maxAgeDays ?: 0) }
        val invalid = !RssMatcher.valid(words.trim(), regex)
        val folders = remember { library?.folders().orEmpty() }
        val feeds = remember { library?.feeds().orEmpty() }
        val scopes = listOf("") + folders.map { "folder:${it.id}" } + feeds.map { "feed:${it.id}" }
        val scopeLabel = { key: String ->
            when {
                key.startsWith("folder:") -> folders.firstOrNull { "folder:${it.id}" == key }?.let { "Folder: ${it.name}" } ?: "All Feeds"
                key.startsWith("feed:") -> feeds.firstOrNull { "feed:${it.id}" == key }?.displayTitle ?: "All Feeds"
                else -> "All Feeds"
            }
        }
        RssScaffold(
            title,
            navigator,
            actions = {
                FmTextButton(onClick = {
                    runCatching {
                        library?.saveSmartFeed(RssSmartFeed(existing?.id.orEmpty(), name, words, regex, scope, unreadOnly, starredOnly, maxAgeDays))
                        service.changed()
                    }.onSuccess { navigator.pop() }.onFailure { status.error(service.errorText(it)) }
                }, enabled = name.isNotBlank() && !invalid) { Text("Save") }
            },
        ) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x4))
                SettingsHeader("Name")
                Column(Modifier.fillMaxWidth().background(Fm.colors.bgRaised).padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
                    FmTextField(value = name, onValueChange = { name = it }, hint = "Name", autofocus = existing == null)
                }
                SettingsHeader("Words")
                Column(Modifier.fillMaxWidth().background(Fm.colors.bgRaised).padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
                    FmTextField(
                        value = words,
                        onValueChange = { words = it },
                        hint = if (regex) "Regular expression" else "Words in the title or text (optional)",
                        errorText = if (invalid) RssMatcher.INVALID_REGEX else null,
                    )
                }
                SettingsSwitch("Regular Expression", regex) { regex = it }
                SettingsHeader("Conditions")
                ChoiceRow("Feeds", scope, scopes, scopeLabel) { scope = it }
                Divider()
                SettingsSwitch("Unread Only", unreadOnly) { unreadOnly = it }
                Divider()
                SettingsSwitch("Starred Only", starredOnly) { starredOnly = it }
                Divider()
                ChoiceRow("Age", maxAgeDays, ages, ::ageLabel) { maxAgeDays = it }
                Spacer(Modifier.padding(FmSpace.x5))
            }
        }
    }
}
