package com.forkmesh.nativeapp.feature.booking

import android.content.Context
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.market.ErrorLine
import com.forkmesh.nativeapp.feature.market.MarketFeature
import com.forkmesh.nativeapp.feature.market.PageActivityCard
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.rssreader.shareText
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.feature.tools.copyToClipboard
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.market.Booking
import com.forkmesh.nativeapp.market.BookingErrors
import com.forkmesh.nativeapp.market.BookingExport
import com.forkmesh.nativeapp.market.BookingItem
import com.forkmesh.nativeapp.market.BookingOverview
import com.forkmesh.nativeapp.market.BookingSettings
import com.forkmesh.nativeapp.market.BookingType
import com.forkmesh.nativeapp.market.BookingWindow
import com.forkmesh.nativeapp.market.PackagesClient
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.market.booking
import com.forkmesh.nativeapp.market.cancelBooking
import com.forkmesh.nativeapp.market.checkBookingSlug
import com.forkmesh.nativeapp.market.exportBooking
import com.forkmesh.nativeapp.market.installBooking
import com.forkmesh.nativeapp.market.saveBooking
import com.forkmesh.nativeapp.market.uninstallBooking
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.ZoneId

object BookingFeature {
    private const val OWNER = "WS15"

    fun install() {
        NavigationFeature.register(Route.Kind.Booking, OWNER) { route -> BookingScreen((route as Route.Booking).booking) }
    }
}

internal class BookingStore(val client: PackagesClient) {
    var overview by mutableStateOf<BookingOverview?>(null)
    var error by mutableStateOf("")
    var notice by mutableStateOf("")
    var loading by mutableStateOf(false)
    var busy by mutableStateOf(false)

    fun work(scope: CoroutineScope, task: () -> BookingOverview?, done: (BookingOverview?) -> Unit = {}) {
        busy = true
        error = ""
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching(task) }
            busy = false
            result.onSuccess { reply ->
                if (reply != null) overview = reply
                done(reply)
            }.onFailure { error = BookingErrors.describe(it) }
        }
    }
}

private fun localZone(): String = runCatching { ZoneId.systemDefault().id }.getOrDefault("UTC")

private fun parseHex(hex: String): Color =
    runCatching { Color(android.graphics.Color.parseColor(hex)) }.getOrDefault(Color(0xFF2563EB))

class BookingScreen(private val requested: String? = null) : Screen() {
    override val title: String get() = "Booking"

    private val store = MarketFeature.client()?.let(::BookingStore)
    private var tab by mutableIntStateOf(0)
    private var slug by mutableStateOf("")
    private var cancelling by mutableStateOf<BookingItem?>(null)
    private var confirmUninstall by mutableStateOf(false)
    private var menu by mutableStateOf(false)
    private var pendingExport: BookingExport? = null
    private var focused = false

    @Composable
    override fun Content(navigator: Navigator) {
        val store = store
        if (store == null) {
            Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
                ToolsAppBar(title, navigator)
                FmEmptyState("cloud_off", "Booking is unavailable", message = PackagesException.sentence("offline"))
            }
            return
        }
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val list = rememberLazyListState()
        fun reload() {
            if (store.loading) return
            store.loading = true
            scope.launch {
                val result = withContext(Dispatchers.IO) { runCatching { store.client.booking() } }
                store.loading = false
                result.onSuccess { store.overview = it; store.error = "" }.onFailure { store.error = BookingErrors.describe(it) }
                val current = store.overview
                if (current != null && !current.installed && slug.isEmpty()) slug = current.suggestedSlug
            }
        }
        LaunchedEffect(Unit) { if (store.overview == null) reload() }
        val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri: Uri? ->
            val export = pendingExport
            pendingExport = null
            if (uri == null || export == null) return@rememberLauncherForActivityResult
            scope.launch {
                val ok = withContext(Dispatchers.IO) {
                    runCatching { context.contentResolver.openOutputStream(uri, "wt")?.use { it.write(Json.stringify(export.json).toByteArray()) } != null }.getOrDefault(false)
                }
                store.notice = if (ok) "Export saved." else "Could not save the export."
            }
        }
        val overview = store.overview
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator) {
                if (overview?.installed == true) {
                    FmIconButton("settings", "Booking settings", onPressed = { navigator.push(BookingSettingsScreen(store)) }, color = Fm.colors.textPrimary)
                    Box {
                        FmIconButton("more_vert", "More", onPressed = { menu = true }, color = Fm.colors.textPrimary)
                        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            FmDropdownMenuItem("Export my data", onClick = {
                                menu = false
                                store.work(scope, { pendingExport = store.client.exportBooking(); null }) {
                                    pendingExport?.let { save.launch(it.filename) }
                                }
                            }, icon = "download")
                            FmDropdownMenuItem("Uninstall…", onClick = { menu = false; confirmUninstall = true }, icon = "delete")
                        }
                    }
                }
                FmIconButton("refresh", "Refresh", onPressed = if (store.loading) null else ({ store.notice = ""; reload() }), color = Fm.colors.textPrimary)
            }
            ErrorLine(store.error)
            if (store.notice.isNotEmpty()) Text(store.notice, Modifier.padding(horizontal = 16.dp, vertical = 4.dp), style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
            when {
                overview == null && store.loading -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                overview == null -> FmEmptyState("event", "Couldn't load Booking", message = "Tap refresh to try again.")
                !overview.installed -> InstallPrompt(overview, slug, store.busy, { slug = it }) {
                    store.work(scope, { store.client.installBooking(slug, localZone()) }) {
                        store.notice = "Your booking page is live. Copy the link and send it to anyone."
                    }
                }
                else -> {
                    val now = System.currentTimeMillis()
                    val zone = runCatching { ZoneId.systemDefault() }.getOrDefault(ZoneId.of("UTC"))
                    val shown = if (tab == 0) overview.upcoming else overview.past
                    val groups = Booking.groupByDay(shown, zone)
                    LaunchedEffect(overview, requested) {
                        val wanted = requested
                        if (focused || wanted == null || overview.booking(wanted) == null) return@LaunchedEffect
                        focused = true
                        tab = if (overview.inPast(wanted)) 1 else 0
                        delay(50)
                        var index = 5
                        for ((_, items) in Booking.groupByDay(if (tab == 0) overview.upcoming else overview.past, zone)) {
                            val at = items.indexOfFirst { it.id == wanted }
                            if (at >= 0) { index += at + 1; break }
                            index += items.size + 1
                        }
                        list.animateScrollToItem(index)
                    }
                    LazyColumn(Modifier.fillMaxSize(), state = list, contentPadding = PaddingValues(bottom = 32.dp)) {
                        item(key = "link") {
                            LinkCard(overview, store.busy, context,
                                onToggle = {
                                    val turningOff = overview.enabled
                                    store.work(scope, { store.client.saveBooking(mapOf("enabled" to !turningOff)) }) {
                                        store.notice = if (turningOff) "Your page is paused. The link shows a paused notice until you turn it back on." else "Your page is live again."
                                    }
                                },
                            )
                        }
                        item(key = "stats") { StatTiles(overview) }
                        item(key = "activity") { PageActivityCard(overview.activity, Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) }
                        item(key = "tabs") {
                            FmSegmentTabBar(
                                listOf(FmSegmentTab("Upcoming", overview.upcoming.size), FmSegmentTab("Past & cancelled", overview.past.size)),
                                tab, onChanged = { tab = it },
                            )
                        }
                        if (groups.isEmpty()) {
                            item(key = "empty") {
                                if (tab == 0) FmEmptyState("event", "No meetings booked yet", message = "Send your link to someone. When they book, the meeting appears here and on your calendar.")
                                else FmEmptyState("history", "Nothing here yet", message = "Meetings from the last 90 days and cancelled ones show up here.")
                            }
                        }
                        for ((day, items) in groups) {
                            item(key = "day-$tab-$day") { FmSectionHeader(Booking.dayLabel(day), Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp)) }
                            items(items, key = { "b-$tab-${it.id}" }) { booking ->
                                BookingRow(booking, now, zone, overview.settings, highlighted = booking.id == requested,
                                    onCancel = if (tab == 0 && !booking.cancelled) ({ cancelling = booking }) else null,
                                    onCalendar = if (booking.calendarEventId.isNotEmpty() && !booking.cancelled) ({ NavigationFeature.open(Route.Event(booking.calendarEventId)) }) else null,
                                    onEmail = { NavigationFeature.openInBrowser(context, "mailto:" + booking.guest.email) },
                                )
                            }
                        }
                    }
                }
            }
        }
        cancelling?.let { booking -> CancelSheet(booking, overview?.settings, store.busy, onDismiss = { cancelling = null }) { reason, notify ->
            store.work(scope, { store.client.cancelBooking(booking.id, reason, notify) }) {
                cancelling = null
                store.notice = if (notify) "Cancelled. We emailed ${booking.guest.name.ifEmpty { "your guest" }}." else "Cancelled."
            }
        } }
        if (confirmUninstall) {
            FmConfirmDialog(
                "Uninstall Booking?",
                onConfirm = {
                    store.work(scope, {
                        pendingExport = store.client.exportBooking()
                        store.client.uninstallBooking()
                        store.client.booking()
                    }) {
                        confirmUninstall = false
                        slug = it?.suggestedSlug.orEmpty()
                        pendingExport?.let { export -> save.launch(export.filename) }
                        store.notice = "Booking uninstalled."
                    }
                },
                onDismissRequest = { confirmUninstall = false },
                body = "Your export downloads first, then your booking page and every booking are deleted and the link stops working.",
                confirmLabel = "Uninstall",
                destructive = true,
                busy = store.busy,
            )
        }
    }
}

@Composable
private fun InstallPrompt(overview: BookingOverview, slug: String, busy: Boolean, onSlug: (String) -> Unit, onInstall: () -> Unit) {
    Column(
        Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        FmIcon("event", size = 40.dp, tint = Fm.colors.textTertiary)
        Text("Create your booking page", style = Fm.text.titleMedium, color = Fm.colors.textPrimary, textAlign = TextAlign.Center)
        Text(
            "Pick a short link, set the hours you take meetings, and send the link to anyone. They choose a free time, the meeting goes on your ForkMesh calendar, and ForkMesh emails the details to both of you.",
            style = Fm.text.bodySmall, color = Fm.colors.textSecondary, textAlign = TextAlign.Center,
        )
        val valid = Booking.slugIsValid(slug.trim().lowercase())
        FmTextField(
            slug, { onSlug(it.lowercase().filter { c -> c.isLetterOrDigit() || c == '-' }.take(63)) }, Modifier.fillMaxWidth(),
            hint = "Your link", helperText = overview.prefix + slug.ifEmpty { "you" },
            errorText = if (slug.isNotEmpty() && !valid) BookingErrors.sentence("invalid_slug") else null,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
        )
        FmToneButton(if (busy) "Creating…" else "Create booking page", onPressed = if (busy || !valid) null else onInstall, tone = FmTone.Accent, filled = true, icon = "add")
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun LinkCard(overview: BookingOverview, busy: Boolean, context: Context, onToggle: () -> Unit) {
    FmCard(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp)) {
        Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Your booking link", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                FmStatusBadge(if (overview.enabled) "Live" else "Paused", if (overview.enabled) Fm.colors.success else Fm.colors.warning)
            }
            Text(Booking.displayLink(overview.link), style = Fm.text.titleMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                FmToneButton("Copy", onPressed = { copyToClipboard(context, overview.link) }, icon = "content_copy")
                FmToneButton("Share", onPressed = { shareText(context, "Book a meeting with ${overview.settings.displayName}", overview.link) }, icon = "share")
                FmToneButton("Open", onPressed = { NavigationFeature.openInBrowser(context, overview.link) }, icon = "open_in_new")
                FmToneButton(if (overview.enabled) "Pause page" else "Turn page on", onPressed = if (busy) null else onToggle, icon = if (overview.enabled) "pause" else "play_arrow")
            }
            for (type in overview.settings.types) {
                val link = overview.links[type.id].orEmpty()
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(type.title + " · " + Booking.minutesLabel(type.minutes) + if (type.enabled) "" else " · hidden",
                            style = Fm.text.bodyMedium, color = Fm.colors.textPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(Booking.displayLink(link), style = Fm.text.labelSmall, color = Fm.colors.accent, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                    if (link.isNotEmpty()) {
                        FmIconButton("content_copy", "Copy the link for ${type.title}", onPressed = { copyToClipboard(context, link) }, color = Fm.colors.textSecondary)
                        FmIconButton("share", "Share the link for ${type.title}", onPressed = { shareText(context, type.title, link) }, color = Fm.colors.textSecondary)
                    }
                }
            }
        }
    }
}

@Composable
private fun StatTile(label: String, value: Int, detail: String, modifier: Modifier) {
    FmCard(modifier, padding = PaddingValues(12.dp)) {
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(label, style = Fm.text.labelSmall, color = Fm.colors.textTertiary, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(value.toString(), style = Fm.text.titleMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
            Text(detail, style = Fm.text.labelSmall, color = Fm.colors.textSecondary, maxLines = 1)
        }
    }
}

@Composable
private fun StatTiles(overview: BookingOverview) {
    val stats = overview.stats
    Column(Modifier.padding(horizontal = 16.dp, vertical = 4.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            StatTile("Upcoming", stats.upcoming, "", Modifier.weight(1f))
            StatTile("Next 7 days", stats.next7Days, if (stats.bookedMinutes7Days > 0) Booking.minutesLabel(stats.bookedMinutes7Days) + " booked" else "", Modifier.weight(1f))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            StatTile("Booked, last 30 days", stats.booked30Days, "", Modifier.weight(1f))
            StatTile("Cancelled, last 30 days", stats.cancelled30Days, "", Modifier.weight(1f))
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun BookingRow(
    booking: BookingItem,
    now: Long,
    zone: ZoneId,
    settings: BookingSettings,
    highlighted: Boolean,
    onCancel: (() -> Unit)?,
    onCalendar: (() -> Unit)?,
    onEmail: () -> Unit,
) {
    FmCard(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp), color = if (highlighted) Fm.colors.accentSubtle else null) {
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(Booking.timeRange(booking.startAt, booking.endAt, zone, settings.twentyFour), style = Fm.text.titleSmall.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
                Text(Booking.minutesLabel(booking.minutes), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
            }
            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(booking.guest.name.ifEmpty { "Guest" }, style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
                Text(booking.typeTitle, style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                val badge = booking.badge(now)
                if (badge.isNotEmpty()) FmStatusBadge(badge, if (booking.cancelled) Fm.colors.danger else Fm.colors.textSecondary)
                if (booking.rescheduledFrom > 0 && !booking.cancelled) FmStatusBadge("Moved", Fm.colors.accent)
            }
            if (booking.guest.email.isNotEmpty()) {
                Text(booking.guest.email, Modifier.clickable(onClick = onEmail), style = Fm.text.bodySmall, color = Fm.colors.accent)
            }
            listOf(booking.guest.phone, booking.where()).filter { it.isNotEmpty() }.distinct().forEach {
                Text(it, style = Fm.text.bodySmall, color = Fm.colors.textSecondary, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
            if (booking.guest.timezone.isNotEmpty() && booking.guest.timezone != settings.timezone) {
                Text("Their time zone: " + booking.guest.timezone.replace('_', ' '), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
            }
            if (booking.guest.notes.isNotEmpty()) Text(booking.guest.notes, style = Fm.text.bodySmall, color = Fm.colors.textPrimary)
            if (booking.cancelled && booking.cancelReason.isNotEmpty()) {
                Text("“${booking.cancelReason}”", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
            }
            if (onCalendar != null || onCancel != null) {
                Row(Modifier.padding(top = 4.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (onCalendar != null) FmToneButton("Calendar", onPressed = onCalendar, icon = "calendar_month")
                    if (onCancel != null) FmToneButton("Cancel", onPressed = onCancel, tone = FmTone.Danger)
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CancelSheet(booking: BookingItem, settings: BookingSettings?, busy: Boolean, onDismiss: () -> Unit, onConfirm: (String, Boolean) -> Unit) {
    var reason by remember { mutableStateOf("") }
    var notify by remember { mutableStateOf(true) }
    val zone = runCatching { ZoneId.systemDefault() }.getOrDefault(ZoneId.of("UTC"))
    val day = java.time.Instant.ofEpochMilli(booking.startAt).atZone(zone).toLocalDate()
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Cancel this meeting?", style = Fm.text.titleMedium, color = Fm.colors.textPrimary)
            Text(
                "${booking.typeTitle} with ${booking.guest.name.ifEmpty { "your guest" }}, ${Booking.dayLabel(day)} at " +
                    Booking.timeRange(booking.startAt, booking.endAt, zone, settings?.twentyFour == true) + ".",
                style = Fm.text.bodyMedium, color = Fm.colors.textSecondary,
            )
            FmTextField(reason, { reason = it.take(600) }, Modifier.fillMaxWidth(), hint = "Message to your guest", minLines = 2, maxLines = 5)
            SwitchRow("Email the guest that it is cancelled", notify) { notify = it }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End)) {
                FmToneButton("Keep it", onPressed = onDismiss)
                FmToneButton(if (busy) "Cancelling…" else "Cancel meeting", onPressed = if (busy) null else ({ onConfirm(reason, notify) }), tone = FmTone.Danger, filled = true)
            }
        }
    }
}

@Composable
internal fun SwitchRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().clickable { onChange(!checked) }.padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
        FmSwitch(checked, onChange)
    }
}

@Composable
internal fun <T> ChoiceRow(label: String, value: T, options: List<T>, text: (T) -> String, onPick: (T) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        Row(
            Modifier.fillMaxWidth().clickable { open = true }.padding(vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(label, Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
            Text(text(value), style = Fm.text.bodyMedium, color = Fm.colors.accent)
        }
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            options.forEach { option -> FmDropdownMenuItem(text(option), onClick = { open = false; onPick(option) }, icon = if (option == value) "check" else null) }
        }
    }
}

class BookingSettingsScreen internal constructor(private val store: BookingStore) : Screen() {
    override val title: String get() = "Booking settings"

    private var settings by mutableStateOf(store.overview?.settings ?: BookingSettings())
    private var slug by mutableStateOf(store.overview?.slug.orEmpty())
    private var slugNote by mutableStateOf("")
    private var zonePicker by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val overview = store.overview ?: return
        val scope = rememberCoroutineScope()
        LaunchedEffect(slug) {
            val wanted = slug.trim().lowercase()
            if (wanted.isEmpty() || wanted == overview.slug) { slugNote = ""; return@LaunchedEffect }
            delay(350)
            slugNote = withContext(Dispatchers.IO) { runCatching { store.client.checkBookingSlug(wanted).sentence }.getOrDefault("") }
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator) {
                FmTextButton(onClick = {
                    store.work(scope, { store.client.saveBooking(settings.body() + ("slug" to slug.trim().lowercase())) }) {
                        store.notice = "Booking settings saved."
                        navigator.pop()
                    }
                }, enabled = !store.busy) { Text(if (store.busy) "Saving…" else "Save") }
            }
            ErrorLine(store.error)
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                FmSectionHeader("Page")
                FmTextField(
                    slug, { slug = it.lowercase().filter { c -> c.isLetterOrDigit() || c == '-' }.take(63) }, Modifier.fillMaxWidth(),
                    hint = "Link", helperText = overview.prefix + slug + if (slugNote.isNotEmpty()) " · $slugNote" else "",
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                )
                FmTextField(settings.displayName, { settings = settings.copy(displayName = it.take(80)) }, Modifier.fillMaxWidth(), hint = "Name shown")
                FmTextField(settings.headline, { settings = settings.copy(headline = it.take(140)) }, Modifier.fillMaxWidth(), hint = "Headline")
                FmTextField(settings.welcome, { settings = settings.copy(welcome = it.take(1500)) }, Modifier.fillMaxWidth(), hint = "Welcome message", minLines = 2, maxLines = 6)
                Text("Colour", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    overview.options.accents.forEach { (name, hex) ->
                        val chosen = name == settings.accent
                        Box(
                            Modifier.size(32.dp).clip(CircleShape).background(parseHex(hex))
                                .then(if (chosen) Modifier.border(3.dp, Fm.colors.accent, CircleShape) else Modifier)
                                .clickable { settings = settings.copy(accent = name) },
                            contentAlignment = Alignment.Center,
                        ) { if (chosen) FmIcon("check", size = 18.dp, tint = Color.White) }
                    }
                }
                ChoiceRow("Clock", settings.timeFormat, listOf("12h", "24h"), { if (it == "24h") "24-hour (14:30)" else "12-hour (2:30 PM)" }) { settings = settings.copy(timeFormat = it) }

                FmSectionHeader("Hours", Modifier.padding(top = 12.dp))
                Row(Modifier.fillMaxWidth().clickable { zonePicker = true }.padding(vertical = 10.dp)) {
                    Text("Time zone", Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                    Text(settings.timezone.replace('_', ' '), style = Fm.text.bodyMedium, color = Fm.colors.accent)
                }
                ChoiceRow("Shortest notice", settings.minNoticeMinutes, overview.options.notices, Booking::noticeLabel) { settings = settings.copy(minNoticeMinutes = it) }
                ChoiceRow("Book up to", settings.maxDaysAhead, overview.options.horizons, Booking::horizonLabel) { settings = settings.copy(maxDaysAhead = it) }
                ChoiceRow("Gap between meetings", settings.bufferMinutes, overview.options.buffers, Booking::gapLabel) { settings = settings.copy(bufferMinutes = it) }
                ChoiceRow("Start times every", settings.slotStepMinutes, overview.options.steps, Booking::stepLabel) { settings = settings.copy(slotStepMinutes = it) }
                ChoiceRow("Most meetings a day", settings.dailyLimit, (0..12).toList(), Booking::dailyLimitLabel) { settings = settings.copy(dailyLimit = it) }
                for ((day, label) in Booking.DAYS) DayEditor(day, label, settings) { settings = it }
                SwitchRow("Never offer times that clash with my ForkMesh calendar", settings.checkCalendar) { settings = settings.copy(checkCalendar = it) }

                FmSectionHeader("Meeting types", Modifier.padding(top = 12.dp))
                settings.types.forEachIndexed { index, type ->
                    TypeEditor(type, overview.options.durations, overview.options.locations,
                        onChange = { change -> settings = settings.type(index) { change } },
                        onRemove = if (settings.types.size > 1) ({ settings = settings.removeType(index) }) else null)
                }
                FmToneButton("Add meeting type", onPressed = { settings = settings.addType() }, icon = "add")

                FmSectionHeader("Email and calendar", Modifier.padding(top = 12.dp))
                FmTextField(settings.replyTo, { settings = settings.copy(replyTo = it.trim()) }, Modifier.fillMaxWidth(), hint = "Guests reply to",
                    helperText = "Confirmations come from ForkMesh; replies go to this address. Leave it empty to keep your address private.",
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email))
                FmTextField(settings.notifyEmail, { settings = settings.copy(notifyEmail = it.trim()) }, Modifier.fillMaxWidth(), hint = "Send my booking emails to",
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email))
                SwitchRow("Email me when someone books, moves or cancels", settings.emailOwner) { settings = settings.copy(emailOwner = it) }
                SwitchRow("Ping me when someone opens my page or starts picking a time", settings.notifyVisits) { settings = settings.copy(notifyVisits = it) }
                SwitchRow("Add booked meetings to my ForkMesh calendar", settings.addToCalendar) { settings = settings.copy(addToCalendar = it) }
                Text(
                    "You always get a ForkMesh notification. Guests always get a confirmation, a reminder the day before, and an email when the meeting moves or is cancelled.",
                    style = Fm.text.labelSmall, color = Fm.colors.textTertiary,
                )
                Spacer(Modifier.height(24.dp))
            }
        }
        if (zonePicker) ZonePicker(settings.timezone, onDismiss = { zonePicker = false }) { settings = settings.copy(timezone = it); zonePicker = false }
    }
}

@Composable
private fun DayEditor(day: String, label: String, settings: BookingSettings, onChange: (BookingSettings) -> Unit) {
    val windows = settings.windows(day)
    Column(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(label, Modifier.weight(1f), style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.Medium), color = Fm.colors.textPrimary)
            if (windows.isEmpty()) Text("Unavailable", Modifier.padding(end = 8.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
            FmSwitch(windows.isNotEmpty(), { onChange(settings.withDay(day, it)) })
        }
        windows.forEachIndexed { index, window ->
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                ClockField(window.start, "From", Modifier.weight(1f)) { value -> onChange(settings.window(day, index) { it.copy(start = value) }) }
                Text("–", color = Fm.colors.textTertiary)
                ClockField(window.end, "Until", Modifier.weight(1f)) { value -> onChange(settings.window(day, index) { it.copy(end = value) }) }
                FmIconButton("close", "Remove these hours", onPressed = { onChange(settings.removeWindow(day, index)) }, color = Fm.colors.textSecondary)
            }
            if (!window.valid) Text("Use HH:MM, with the end after the start.", style = Fm.text.labelSmall, color = Fm.colors.danger)
        }
        if (windows.isNotEmpty()) FmTextButton(onClick = { onChange(settings.addWindow(day)) }) { Text("Add hours") }
    }
}

@Composable
private fun ClockField(value: String, hint: String, modifier: Modifier, onChange: (String) -> Unit) {
    FmTextField(value, { onChange(it.filter { c -> c.isDigit() || c == ':' }.take(5)) }, modifier, hint = hint,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
}

@Composable
private fun TypeEditor(type: BookingType, durations: List<Int>, locations: List<String>, onChange: (BookingType) -> Unit, onRemove: (() -> Unit)?) {
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            FmTextField(type.title, { onChange(type.copy(title = it.take(80))) }, Modifier.fillMaxWidth(), hint = "Name")
            ChoiceRow("Length", type.minutes, durations, Booking::minutesLabel) { onChange(type.copy(minutes = it)) }
            ChoiceRow("Where", type.location.kind, locations, { Booking.LOCATION_LABELS[it] ?: it }) { onChange(type.copy(location = type.location.copy(kind = it))) }
            if (type.location.kind != "none") {
                FmTextField(type.location.detail, { onChange(type.copy(location = type.location.copy(detail = it.take(500)))) }, Modifier.fillMaxWidth(),
                    hint = "Details", helperText = Booking.LOCATION_HINTS[type.location.kind])
            }
            FmTextField(type.description, { onChange(type.copy(description = it.take(600))) }, Modifier.fillMaxWidth(), hint = "Description", minLines = 2, maxLines = 4)
            SwitchRow("Show on my page", type.enabled) { onChange(type.copy(enabled = it)) }
            if (onRemove != null) FmToneButton("Remove", onPressed = onRemove, tone = FmTone.Danger)
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ZonePicker(current: String, onDismiss: () -> Unit, onPick: (String) -> Unit) {
    var query by remember { mutableStateOf("") }
    val zones = remember { ZoneId.getAvailableZoneIds().filter { it.contains('/') && !it.startsWith("Etc/") && !it.startsWith("SystemV/") }.sorted() + "UTC" }
    val shown = zones.filter { query.isBlank() || it.replace('_', ' ').contains(query.trim(), ignoreCase = true) }.take(200)
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Time zone", style = Fm.text.titleMedium, color = Fm.colors.textPrimary)
            FmTextField(query, { query = it }, Modifier.fillMaxWidth(), hint = "Search a city or region", autofocus = true)
            LazyColumn(Modifier.fillMaxWidth().height(420.dp)) {
                items(shown, key = { it }) { zone ->
                    Row(Modifier.fillMaxWidth().clickable { onPick(zone) }.padding(vertical = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(zone.replace('_', ' '), Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                        if (zone == current) FmIcon("check", size = 18.dp, tint = Fm.colors.accent)
                    }
                }
            }
            Spacer(Modifier.width(1.dp))
        }
    }
}
