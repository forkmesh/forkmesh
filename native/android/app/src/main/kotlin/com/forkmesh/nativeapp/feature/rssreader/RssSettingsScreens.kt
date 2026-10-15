package com.forkmesh.nativeapp.feature.rssreader

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.rssreader.RssAppearance
import com.forkmesh.nativeapp.rssreader.RssErrors
import com.forkmesh.nativeapp.rssreader.RssFeedException
import com.forkmesh.nativeapp.rssreader.RssFont
import com.forkmesh.nativeapp.rssreader.RssFormat
import com.forkmesh.nativeapp.rssreader.RssLibrary
import com.forkmesh.nativeapp.rssreader.RssSettings
import com.forkmesh.nativeapp.rssreader.RssTheme
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
private fun <T> ChoiceRow(title: String, current: T, options: List<T>, label: (T) -> String, onSelect: (T) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        SettingsRow(title, value = label(current), onClick = { open = true })
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (option in options) {
                FmDropdownMenuItem(label(option), onClick = { open = false; onSelect(option) }, icon = if (option == current) "check" else null)
            }
        }
    }
}

@Composable
private fun Divider() {
    HorizontalDivider(Modifier.padding(start = FmSpace.x4), thickness = 1.dp, color = Fm.colors.border)
}

internal class RssSettingsScreen : Screen() {
    override val title: String get() = "Settings"
    private val status = FmStatus()

    @Composable
    override fun Content(navigator: Navigator) {
        val service = RssReaderFeature.service
        val version by service.version.collectAsState()
        val context = LocalContext.current
        val scope = rememberCoroutineScope()
        LaunchedEffect(Unit) { service.load() }
        val settings = remember(version) { service.library?.settings() } ?: RssSettings()
        val update = { change: (RssSettings) -> RssSettings -> service.mutate { it.updateSettings(change) } }
        val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            if (uri == null) return@rememberLauncherForActivityResult
            scope.launch {
                runCatching {
                    val text = withContext(Dispatchers.IO) {
                        context.contentResolver.openInputStream(uri)?.use { stream ->
                            val bytes = stream.readBytes()
                            if (bytes.size > 5 * 1024 * 1024) throw RssFeedException("too_large")
                            String(bytes, Charsets.UTF_8)
                        } ?: throw RssFeedException("opml_unreadable")
                    }
                    service.importOpml(text)
                }.onSuccess { result ->
                    status.show(
                        "Imported ${RssFormat.plural(result.imported, "subscription")}" +
                            (if (result.skipped > 0) "; ${result.skipped} already followed or over the limit." else ".") +
                            if (result.imported == 1) " Checking it now." else if (result.imported > 1) " Checking them now." else "",
                    )
                }.onFailure { error ->
                    status.error(if (error is RssFeedException) RssErrors.describe(error.code) else service.errorText(error))
                }
            }
        }
        val exporter = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("text/x-opml")) { uri: Uri? ->
            if (uri == null) return@rememberLauncherForActivityResult
            scope.launch {
                runCatching {
                    val text = service.exportOpml()
                    withContext(Dispatchers.IO) {
                        context.contentResolver.openOutputStream(uri, "wt")?.use { it.write(text.toByteArray(Charsets.UTF_8)) } ?: error("unwritable")
                    }
                }.onSuccess { status.show("Subscriptions exported.") }.onFailure { status.error("The file couldn't be written.") }
            }
        }
        RssScaffold("Settings", navigator) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x4))
                SettingsHeader("Feeds")
                SettingsRow("Import Subscriptions", subtitle = "Add feeds from an OPML file", icon = "download", onClick = {
                    importer.launch(arrayOf("text/x-opml", "text/xml", "application/xml", "application/octet-stream", "*/*"))
                })
                Divider()
                SettingsRow("Export Subscriptions", subtitle = "Save your feeds as an OPML file", icon = "upload", onClick = { exporter.launch("RSS Reader Subscriptions.opml") })
                Divider()
                SettingsRow("Add the ForkMesh Blog Feed", icon = "feed", onClick = {
                    status.show("Adding the ForkMesh Blog…")
                    scope.launch {
                        runCatching { service.addFeed(RssLibrary.FORKMESH_BLOG_FEED, "", "") }
                            .onSuccess { status.show("Subscribed to ${it.displayTitle}.") }
                            .onFailure { status.error(service.errorText(it)) }
                    }
                })

                SettingsHeader("Timeline")
                SettingsSwitch("Sort Oldest to Newest", settings.sortOldestFirst) { on -> update { it.copy(sortOldestFirst = on) } }
                Divider()
                SettingsSwitch("Group by Feed", settings.groupByFeed) { on -> update { it.copy(groupByFeed = on) } }
                Divider()
                SettingsSwitch("Refresh to Clear Read Articles", settings.refreshClearsRead) { on -> update { it.copy(refreshClearsRead = on) } }
                Divider()
                SettingsSwitch("Confirm Mark All as Read", settings.confirmMarkAllRead) { on -> update { it.copy(confirmMarkAllRead = on) } }
                Divider()
                ChoiceRow("Summary Lines", settings.timelineLines, (0..4).toList(), { if (it == 0) "None" else "$it" }) { lines -> update { it.copy(timelineLines = lines) } }
                Divider()
                SettingsSwitch("Show Feed Icons", settings.showFeedIcons) { on -> update { it.copy(showFeedIcons = on) } }

                SettingsHeader("Articles")
                ChoiceRow("Theme", settings.theme, RssTheme.entries.toList(), { it.label }) { theme -> update { it.copy(theme = theme) } }
                Divider()
                ChoiceRow("Font", settings.font, RssFont.entries.toList(), { it.label }) { font -> update { it.copy(font = font) } }
                Divider()
                SettingsRow("Text Size", value = "${settings.textSize} pt") {
                    Row {
                        FmIconButton("remove", "Smaller text", onPressed = if (settings.textSize > RssSettings.MIN_TEXT) ({ update { it.copy(textSize = it.textSize - 1) } }) else null)
                        FmIconButton("add", "Larger text", onPressed = if (settings.textSize < RssSettings.MAX_TEXT) ({ update { it.copy(textSize = it.textSize + 1) } }) else null)
                    }
                }
                Divider()
                SettingsSwitch("Load Remote Images", settings.loadRemoteImages, subtitle = "When off, images and media show as links") { on -> update { it.copy(loadRemoteImages = on) } }

                SettingsHeader("Appearance")
                ChoiceRow("Appearance", settings.appearance, RssAppearance.entries.toList(), { it.label }) { value -> update { it.copy(appearance = value) } }

                SettingsHeader("Troubleshooting")
                SettingsRow("Error Log", icon = "error_outline", onClick = { navigator.push(RssLogScreen(RssLogKind.ERRORS)) })
                Divider()
                SettingsRow("Activity Log", icon = "history", onClick = { navigator.push(RssLogScreen(RssLogKind.ACTIVITY)) })
                Divider()
                SettingsRow("Account Stats", icon = "bar_chart", onClick = { navigator.push(RssLogScreen(RssLogKind.STATS)) })

                SettingsHeader("About")
                SettingsRow("About RSS Reader", icon = "info", onClick = { navigator.push(RssAboutScreen()) })
                Text(
                    "Feeds are checked by this phone and stored only on it.",
                    Modifier.padding(FmSpace.x4),
                    style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
                )
            }
        }
    }
}

internal enum class RssLogKind(val title: String) { ERRORS("Error Log"), ACTIVITY("Activity Log"), STATS("Account Stats") }

internal class RssLogScreen(private val kind: RssLogKind) : Screen() {
    override val title: String get() = kind.title

    @Composable
    override fun Content(navigator: Navigator) {
        val service = RssReaderFeature.service
        val version by service.version.collectAsState()
        LaunchedEffect(Unit) { service.load() }
        val library = service.library
        RssScaffold(kind.title, navigator) {
            if (library == null) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                return@RssScaffold
            }
            when (kind) {
                RssLogKind.ERRORS -> {
                    val failing = remember(version) { library.failingFeeds() }
                    if (failing.isEmpty()) {
                        FmEmptyState("check_circle", "No errors", message = "Every feed answered the last time it was checked.")
                    } else {
                        LazyColumn(Modifier.fillMaxSize()) {
                            items(failing, key = { it.id }) { feed ->
                                SettingsRow(
                                    feed.displayTitle,
                                    subtitle = "${RssErrors.describe(feed.error)}\n${RssFormat.stamp(feed.errorAt)} · failed ${RssFormat.plural(feed.errorCount, "time")} in a row",
                                    onClick = { navigator.push(RssFeedInfoScreen(feed.id)) },
                                )
                                Divider()
                            }
                        }
                    }
                }
                RssLogKind.ACTIVITY -> {
                    val entries = remember(version) { library.activity() }
                    if (entries.isEmpty()) {
                        FmEmptyState("history", "No activity yet")
                    } else {
                        LazyColumn(Modifier.fillMaxSize()) {
                            items(entries.withIndex().toList(), key = { it.index }) { (_, entry) ->
                                SettingsRow(entry.text, subtitle = RssFormat.stamp(entry.at))
                                Divider()
                            }
                        }
                    }
                }
                RssLogKind.STATS -> {
                    val stats = remember(version) { library.stats() }
                    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                        SettingsHeader("On This Phone")
                        SettingsRow("Feeds", value = "${stats.feeds} of ${RssLibrary.MAX_FEEDS}")
                        Divider()
                        SettingsRow("Folders", value = "${stats.folders} of ${RssLibrary.MAX_FOLDERS}")
                        Divider()
                        SettingsRow("Articles", value = "${stats.articles}")
                        Divider()
                        SettingsRow("Unread", value = "${stats.unread}")
                        Divider()
                        SettingsRow("Starred", value = "${stats.starred}")
                        Divider()
                        SettingsRow("Feeds with errors", value = "${stats.failingFeeds}")
                        Divider()
                        SettingsRow("Oldest article", value = if (stats.oldestArticle > 0) RssFormat.stamp(stats.oldestArticle) else "None")
                        Divider()
                        SettingsRow("Last refresh", value = RssFormat.stamp(stats.lastRefresh))
                    }
                }
            }
        }
    }
}

internal class RssFeedInfoScreen(private val feedId: String) : Screen() {
    override val title: String get() = "Feed Info"
    private val status = FmStatus()

    @Composable
    override fun Content(navigator: Navigator) {
        val service = RssReaderFeature.service
        val version by service.version.collectAsState()
        val context = LocalContext.current
        LaunchedEffect(Unit) { service.load() }
        val library = service.library
        val feed = remember(version) { library?.feed(feedId) }
        var name by remember(feed?.id) { mutableStateOf(feed?.displayTitle.orEmpty()) }
        var folderMenu by remember { mutableStateOf(false) }
        var confirmRemove by remember { mutableStateOf(false) }
        RssScaffold("Feed Info", navigator) {
            if (library == null || feed == null) {
                FmEmptyState("feed", "This feed is no longer subscribed")
                return@RssScaffold
            }
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x4))
                Row(Modifier.fillMaxWidth().background(Fm.colors.bgRaised).padding(FmSpace.x4), verticalAlignment = Alignment.CenterVertically) {
                    FeedIcon(feed, 44.dp, 10.dp)
                    Spacer(Modifier.width(FmSpace.x3))
                    Column(Modifier.weight(1f)) {
                        Text(feed.displayTitle, style = Fm.text.titleMedium.copy(fontWeight = FontWeight.SemiBold))
                        if (feed.description.isNotEmpty()) Text(feed.description, style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary), maxLines = 3)
                    }
                }
                SettingsHeader("Name")
                Column(Modifier.fillMaxWidth().background(Fm.colors.bgRaised).padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
                    FmTextField(value = name, onValueChange = { name = it }, hint = feed.title.ifEmpty { "Name" }, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Text))
                    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                        FmTextButton(onClick = {
                            service.mutate { it.renameFeed(feed.id, name) }
                            status.show("Name saved.")
                        }, enabled = name.isNotBlank() && name != feed.displayTitle) { Text("Save Name") }
                        if (feed.customTitle.isNotEmpty()) {
                            FmTextButton(onClick = {
                                service.mutate { it.renameFeed(feed.id, "") }
                                name = feed.title
                            }) { Text("Use Feed's Name") }
                        }
                    }
                }
                SettingsHeader("Folder")
                Box {
                    val folders = library.folders()
                    SettingsRow("Folder", value = folders.firstOrNull { it.id == feed.folderId }?.name ?: "None", icon = "folder", onClick = { folderMenu = true })
                    FmDropdownMenu(expanded = folderMenu, onDismissRequest = { folderMenu = false }) {
                        FmDropdownMenuItem("None", onClick = { folderMenu = false; service.mutate { it.moveFeed(feed.id, "") } }, icon = if (feed.folderId.isEmpty()) "check" else null)
                        for (folder in folders) {
                            FmDropdownMenuItem(folder.name, onClick = { folderMenu = false; service.mutate { it.moveFeed(feed.id, folder.id) } }, icon = if (feed.folderId == folder.id) "check" else "folder")
                        }
                    }
                }
                SettingsHeader("Addresses")
                SettingsRow("Home Page", subtitle = feed.homePage.ifEmpty { "None" }, icon = "public", onClick = if (feed.homePage.isNotEmpty()) ({ openInBrowser(context, feed.homePage) }) else null)
                Divider()
                SettingsRow("Feed Address", subtitle = feed.url + if (feed.viaHttps) "\nFetched over HTTPS" else "", icon = "link")
                if (feed.previousUrls.isNotEmpty()) {
                    Divider()
                    SettingsRow("Previous Addresses", subtitle = feed.previousUrls.joinToString("\n"), icon = "history")
                }
                SettingsHeader("Status")
                SettingsRow("Last Check", value = RssFormat.stamp(feed.lastCheck))
                Divider()
                SettingsRow("Last Success", value = RssFormat.stamp(feed.lastSuccess))
                Divider()
                SettingsRow(
                    "Last Error",
                    subtitle = if (feed.error.isEmpty()) "None" else "${RssErrors.describe(feed.error)} (${RssFormat.stamp(feed.errorAt)})",
                    titleColor = if (feed.error.isEmpty()) null else Fm.colors.danger,
                )
                Divider()
                SettingsRow("Check Now", icon = "refresh", onClick = {
                    service.mutate { it.checkSoon(feed.id) }
                    service.refresh(force = false)
                    status.show("Checking…")
                })
                Spacer(Modifier.padding(FmSpace.x3))
                SettingsRow("Unsubscribe", titleColor = Fm.colors.danger, icon = "delete", onClick = { confirmRemove = true })
                Spacer(Modifier.padding(FmSpace.x5))
            }
            if (confirmRemove) {
                com.forkmesh.nativeapp.ui.kit.FmConfirmDialog(
                    title = "Unsubscribe from “${feed.displayTitle}”?",
                    body = "Its articles, including starred ones, are removed from this phone.",
                    confirmLabel = "Unsubscribe",
                    destructive = true,
                    onDismissRequest = { confirmRemove = false },
                    onConfirm = {
                        confirmRemove = false
                        service.mutate { it.unsubscribe(feed.id, System.currentTimeMillis()) }
                        navigator.pop()
                    },
                )
            }
        }
    }
}

internal class RssAboutScreen : Screen() {
    override val title: String get() = "About RSS Reader"

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        RssScaffold("About RSS Reader", navigator) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                Column(Modifier.fillMaxWidth().background(Fm.colors.bgRaised).padding(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        FmIcon("feed", size = 32.dp, tint = RssColors.today)
                        Spacer(Modifier.width(FmSpace.x3))
                        Text("RSS Reader", style = Fm.text.titleLarge)
                    }
                    Text(
                        "Follow sites by their RSS, Atom and JSON feeds. Your subscriptions and articles live on this phone, and the phone checks the feeds itself.",
                        style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
                    )
                }
                SettingsHeader("Credits")
                Column(Modifier.fillMaxWidth().background(Fm.colors.bgRaised).padding(FmSpace.x4)) {
                    Text(
                        "Inspired by NetNewsWire, the free and open source RSS reader created by Brent Simmons and developed by Ranchero Software and its contributors (MIT licensed). RSS Reader shares no code with NetNewsWire and is not affiliated with or endorsed by it.",
                        style = Fm.text.bodyMedium,
                    )
                }
                SettingsRow("netnewswire.com", icon = "open_in_new", onClick = { openInBrowser(context, "https://netnewswire.com/") })
                Divider()
                SettingsRow("NetNewsWire on GitHub", icon = "open_in_new", onClick = { openInBrowser(context, "https://github.com/Ranchero-Software/NetNewsWire") })
                Spacer(Modifier.padding(FmSpace.x5))
            }
        }
    }
}
