package com.forkmesh.nativeapp.feature.planning

import com.forkmesh.nativeapp.feature.market.rememberScreenPluginMenu
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
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
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.planning.CalendarEvent
import com.forkmesh.nativeapp.planning.CalendarEventDraft
import com.forkmesh.nativeapp.planning.CalendarMath
import com.forkmesh.nativeapp.planning.CalendarMode
import com.forkmesh.nativeapp.planning.CalendarModel
import com.forkmesh.nativeapp.planning.CalendarOccurrence
import com.forkmesh.nativeapp.planning.CalendarRecurrence
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmExtendedFloatingActionButton
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmKeyValueRow
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.Instant
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.time.format.FormatStyle

private val dayFormat = DateTimeFormatter.ofPattern("EEE d MMM")
private val monthFormat = DateTimeFormatter.ofPattern("MMMM yyyy")
private val timeFormat = DateTimeFormatter.ofLocalizedTime(FormatStyle.SHORT)
private val dateTimeFormat = DateTimeFormatter.ofLocalizedDateTime(FormatStyle.MEDIUM, FormatStyle.SHORT)

internal fun formatAt(ms: Long, zone: ZoneId, format: DateTimeFormatter): String = Instant.ofEpochMilli(ms).atZone(zone).format(format)

private fun occurrenceTime(occurrence: CalendarOccurrence, zone: ZoneId): String =
    if (occurrence.event.allDay) "All day" else formatAt(occurrence.startAt, zone, timeFormat) + " – " + formatAt(occurrence.endAt, zone, timeFormat)

private fun calendarModel(date: String?, mode: CalendarMode): CalendarModel? =
    Services.get<ApiService>()?.let { CalendarModel(it, mode, date) }

class CalendarScreen(private val date: String?, private val initialMode: CalendarMode) : Screen() {
    override val title: String get() = "Calendar"

    private val model = calendarModel(date, initialMode)
    private var version by mutableIntStateOf(0)
    private var loading by mutableStateOf(true)
    private var reload by mutableIntStateOf(1)

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model ?: return
        val top = navigator.top
        LaunchedEffect(top) { if (top === this@CalendarScreen) reload++ }
        LaunchedEffect(reload) {
            loading = true
            withContext(Dispatchers.IO) { model.load() }
            loading = false
            version++
        }
        val scope = rememberCoroutineScope()
        fun change(block: () -> Unit) {
            block()
            version++
            if (model.needsLoad()) scope.launch {
                loading = true
                withContext(Dispatchers.IO) { model.loadIfNeeded() }
                loading = false
                version++
            }
        }
        @Suppress("UNUSED_EXPRESSION") version
        val zone = model.zone
        Box(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            Column(Modifier.fillMaxSize()) {
                ToolsAppBar("Calendar", navigator) {
                    FmTextButton(onClick = { change { model.goToToday() } }) { Text("Today") }
                    FmIconButton("chevron_left", "Previous", onPressed = { change { model.step(-1) } }, color = Fm.colors.textPrimary)
                    FmIconButton("chevron_right", "Next", onPressed = { change { model.step(1) } }, color = Fm.colors.textPrimary)
                }
                val modes = CalendarMode.entries
                FmSegmentTabBar(modes.map { FmSegmentTab(it.label) }, modes.indexOf(model.mode), onChanged = { index -> change { model.mode = modes[index] } })
                Text(
                    if (model.mode == CalendarMode.MONTH) formatAt(model.anchor, zone, monthFormat) else formatAt(model.anchor, zone, dayFormat),
                    Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                    style = Fm.text.titleMedium,
                )
                if (loading) FmCircularProgressIndicator(Modifier.align(Alignment.CenterHorizontally).size(20.dp), strokeWidth = 2.dp)
                model.error?.let { Text(it, Modifier.padding(horizontal = 16.dp), color = Fm.colors.danger, style = Fm.text.bodySmall) }
                when (model.mode) {
                    CalendarMode.MONTH -> {
                        MonthGrid(model) { day -> change { model.select(day) } }
                        DayList(model, model.occurrencesOnDay(CalendarMath.startOfDay(model.anchor, zone)), navigator)
                    }
                    CalendarMode.WEEK -> {
                        WeekStrip(model) { day -> change { model.select(day) } }
                        DayList(model, model.occurrencesOnDay(CalendarMath.startOfDay(model.anchor, zone)), navigator)
                    }
                    CalendarMode.DAY -> DayList(model, model.occurrencesOnDay(CalendarMath.startOfDay(model.anchor, zone)), navigator)
                    CalendarMode.AGENDA -> AgendaList(model, navigator)
                }
            }
            FmExtendedFloatingActionButton(
                "New event",
                onClick = { navigator.push(EventEditorScreen(model, null, model.newDraft(model.anchor))) },
                modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp),
                icon = "add",
            )
        }
    }
}

@Composable
private fun MonthGrid(model: CalendarModel, onSelect: (Long) -> Unit) {
    val zone = model.zone
    val days = model.days
    val occurrences = model.occurrences
    val selected = CalendarMath.startOfDay(model.anchor, zone)
    val today = model.today
    Column(Modifier.padding(horizontal = 8.dp)) {
        Row {
            days.take(7).forEach { day ->
                Text(formatAt(day, zone, DateTimeFormatter.ofPattern("EEEEE")), Modifier.weight(1f), textAlign = TextAlign.Center, style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
            }
        }
        days.chunked(7).forEach { week ->
            Row {
                week.forEach { day ->
                    val count = occurrences.count { CalendarMath.overlaps(it, day, zone) }
                    val inMonth = model.isInAnchorMonth(day)
                    Column(
                        Modifier
                            .weight(1f)
                            .aspectRatio(1f)
                            .padding(2.dp)
                            .background(if (day == selected) Fm.colors.accentSubtle else Fm.colors.bgBase, RoundedCornerShape(8.dp))
                            .then(if (day == today) Modifier.border(1.dp, Fm.colors.accent, RoundedCornerShape(8.dp)) else Modifier)
                            .clickable { onSelect(day) },
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.Center,
                    ) {
                        Text(
                            Instant.ofEpochMilli(day).atZone(zone).dayOfMonth.toString(),
                            style = Fm.text.bodyMedium,
                            color = if (inMonth) Fm.colors.textPrimary else Fm.colors.textDisabled,
                        )
                        if (count > 0) {
                            Row(horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                                repeat(minOf(count, 3)) { Box(Modifier.size(4.dp).background(Fm.colors.accent, CircleShape)) }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun WeekStrip(model: CalendarModel, onSelect: (Long) -> Unit) {
    val zone = model.zone
    val selected = CalendarMath.startOfDay(model.anchor, zone)
    Row(Modifier.padding(horizontal = 8.dp)) {
        model.days.forEach { day ->
            val count = model.occurrencesOnDay(day).size
            Column(
                Modifier.weight(1f).padding(2.dp)
                    .background(if (day == selected) Fm.colors.accentSubtle else Fm.colors.bgRaised, RoundedCornerShape(8.dp))
                    .clickable { onSelect(day) }.padding(vertical = 8.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(formatAt(day, zone, DateTimeFormatter.ofPattern("EEE")), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                Text(Instant.ofEpochMilli(day).atZone(zone).dayOfMonth.toString(), style = Fm.text.titleMedium)
                Text(if (count == 0) " " else "$count", style = Fm.text.labelSmall, color = Fm.colors.accent)
            }
        }
    }
}

@Composable
private fun DayList(model: CalendarModel, items: List<CalendarOccurrence>, navigator: Navigator) {
    if (items.isEmpty()) {
        Text("Nothing on ${formatAt(model.anchor, model.zone, dayFormat)}.", Modifier.padding(16.dp), color = Fm.colors.textTertiary, style = Fm.text.bodyMedium)
        return
    }
    LazyColumn(Modifier.fillMaxWidth()) {
        items(items, key = { it.id }) { occurrence -> OccurrenceRow(model, occurrence, navigator) }
        item { Spacer(Modifier.height(88.dp)) }
    }
}

@Composable
private fun AgendaList(model: CalendarModel, navigator: Navigator) {
    val agenda = model.agenda
    if (agenda.isEmpty()) {
        FmEmptyState("calendar_month", "Nothing in the next 30 days")
        return
    }
    LazyColumn(Modifier.fillMaxSize()) {
        agenda.forEach { day ->
            item(key = day.day) {
                val start = CalendarMath.parseDayKey(day.day, model.zone) ?: 0L
                FmSectionHeader(formatAt(start, model.zone, dayFormat), Modifier.padding(horizontal = 16.dp, vertical = 8.dp), count = day.items.size)
            }
            items(day.items, key = { it.id }) { occurrence -> OccurrenceRow(model, occurrence, navigator) }
        }
        item { Spacer(Modifier.height(88.dp)) }
    }
}

@Composable
private fun OccurrenceRow(model: CalendarModel, occurrence: CalendarOccurrence, navigator: Navigator) {
    val event = occurrence.event
    val status = event.attendeeStatus(model.actor)
    FmListRow(
        event.title.ifEmpty { "(No title)" },
        subtitle = listOfNotNull(
            occurrenceTime(occurrence, model.zone),
            event.location.ifEmpty { null },
            if (event.calendar != "personal") event.calendar else null,
            status?.takeIf { !event.isOwnedBy(model.actor) }?.let { "You: $it" },
        ).joinToString(" · "),
        leading = { Box(Modifier.size(10.dp).background(if (event.scope == "organization") Fm.colors.accentEdge else Fm.colors.accent, CircleShape)) },
        onTap = { navigator.push(EventScreen(event.id, model)) },
    )
}

class EventScreen(private val eventId: String, private val shared: CalendarModel? = null) : Screen() {
    override val title: String get() = "Event"

    private val model = shared ?: calendarModel(null, CalendarMode.DAY)
    private var version by mutableIntStateOf(0)
    private var loading by mutableStateOf(shared?.event(eventId) == null)
    private var confirmDelete by mutableStateOf(false)
    private var menu by mutableStateOf(false)
    private val answerStatus = FmStatus()
    private val deleteStatus = FmStatus()

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model ?: return
        val scope = rememberCoroutineScope()
        LaunchedEffect(Unit) {
            if (model.event(eventId) == null) {
                withContext(Dispatchers.IO) { model.loadEvent(eventId) }
                loading = false
                version++
            }
        }
        @Suppress("UNUSED_EXPRESSION") version
        val event = model.event(eventId)
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            val plugin = rememberScreenPluginMenu()
            ToolsAppBar(event?.title ?: "Event", navigator, pluginMenu = false) {
                val editable = event != null && model.canEdit(event)
                if (editable || plugin != null) {
                    Box {
                        FmIconButton("more_vert", "More", onPressed = { menu = true }, color = Fm.colors.textPrimary)
                        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            if (event != null && editable) {
                                FmDropdownMenuItem("Edit", onClick = { menu = false; navigator.push(EventEditorScreen(model, event.id, CalendarEventDraft.of(event))) })
                                FmDropdownMenuItem("Delete", onClick = { menu = false; confirmDelete = true }, textColor = Fm.colors.danger)
                            }
                            plugin?.Items(close = { menu = false }, divided = editable)
                        }
                    }
                    plugin?.Host()
                }
            }
            when {
                loading && event == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                event == null -> FmEmptyState("event_busy", "Event unavailable", message = model.error)
                else -> Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp)) {
                    FmInlineStatus(deleteStatus, Modifier.padding(bottom = 8.dp))
                    FmCard(Modifier.fillMaxWidth()) {
                        Column {
                            FmKeyValueRow("Starts", if (event.allDay) formatAt(event.startAt, model.zone, dayFormat) else formatAt(event.startAt, model.zone, dateTimeFormat))
                            FmKeyValueRow("Ends", if (event.allDay) formatAt(event.endAt, model.zone, dayFormat) else formatAt(event.endAt, model.zone, dateTimeFormat))
                            if (event.recurrence != CalendarRecurrence.NONE) FmKeyValueRow("Repeats", event.recurrence.label)
                            FmKeyValueRow("Calendar", event.calendar)
                            if (event.location.isNotEmpty()) FmKeyValueRow("Where", event.location)
                            if (event.meetingUrl.isNotEmpty()) FmKeyValueRow("Link", event.meetingUrl)
                            if (event.owner.isNotEmpty()) FmKeyValueRow("Owner", "@${event.owner}")
                            if (event.reminders.isNotEmpty()) FmKeyValueRow("Reminders", event.reminders.sortedDescending().joinToString(", ") { CalendarEventDraft.reminderLabel(it) })
                        }
                    }
                    if (event.description.isNotEmpty()) {
                        Spacer(Modifier.height(12.dp))
                        Text(event.description, style = Fm.text.bodyMedium)
                    }
                    if (event.attendees.isNotEmpty()) {
                        FmSectionHeader("People", Modifier.padding(vertical = 12.dp), count = event.attendees.size)
                        event.attendees.forEach { FmListRow("@${it.name}", subtitle = it.status, showChevron = false) }
                    }
                    val mine = event.attendeeStatus(model.actor)
                    if (mine != null && !event.isOwnedBy(model.actor)) {
                        FmSectionHeader("Your answer", Modifier.padding(vertical = 12.dp))
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            listOf("accepted" to "Going", "tentative" to "Maybe", "declined" to "No").forEach { (status, label) ->
                                FmToneButton(label, onPressed = {
                                    answerStatus.clear()
                                    scope.launch {
                                        val ok = withContext(Dispatchers.IO) { model.rsvp(event.id, status) }
                                        version++
                                        if (!ok) answerStatus.error(model.error ?: "Your answer was not sent.")
                                    }
                                }, tone = if (mine == status) FmTone.Accent else FmTone.Neutral, filled = mine == status)
                            }
                        }
                        FmInlineStatus(answerStatus, Modifier.padding(top = 8.dp))
                    }
                }
            }
        }
        if (confirmDelete && event != null) {
            FmConfirmDialog(
                title = "Delete this event?",
                body = if (event.recurrence != CalendarRecurrence.NONE) "Every repeat is deleted." else null,
                confirmLabel = "Delete",
                destructive = true,
                onDismissRequest = { confirmDelete = false },
                onConfirm = {
                    confirmDelete = false
                    deleteStatus.clear()
                    scope.launch {
                        if (withContext(Dispatchers.IO) { model.delete(event.id) }) navigator.pop() else deleteStatus.error(model.error ?: "The event was not deleted.")
                    }
                },
            )
        }
    }
}

private val inputFormat = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm")

private fun toInput(ms: Long, zone: ZoneId): String = Instant.ofEpochMilli(ms).atZone(zone).format(inputFormat)

private fun fromInput(text: String, zone: ZoneId): Long? =
    runCatching { LocalDateTime.parse(text.trim(), inputFormat).atZone(zone).toInstant().toEpochMilli() }.getOrNull()

class EventEditorScreen(private val model: CalendarModel, private val eventId: String?, initial: CalendarEventDraft) : Screen() {
    override val title: String get() = if (eventId == null) "New event" else "Edit event"

    private var draft by mutableStateOf(initial)
    private var start by mutableStateOf(toInput(initial.startAt, model.zone))
    private var end by mutableStateOf(toInput(initial.endAt, model.zone))
    private var until by mutableStateOf(if (initial.recurrenceUntil > 0) toInput(initial.recurrenceUntil, model.zone).take(10) else "")
    private var attendees by mutableStateOf(initial.attendees.joinToString(", "))
    private var saving by mutableStateOf(false)
    private var problem by mutableStateOf<String?>(null)
    private var calendarMenu by mutableStateOf(false)
    private var repeatMenu by mutableStateOf(false)

    private fun built(): CalendarEventDraft? {
        val zone = model.zone
        val startMs = fromInput(start, zone) ?: return null.also { problem = "Write the start as YYYY-MM-DD HH:MM." }
        val endMs = fromInput(end, zone) ?: return null.also { problem = "Write the end as YYYY-MM-DD HH:MM." }
        val untilMs = if (until.isBlank()) 0L else (CalendarMath.parseDayKey(until.trim(), zone)?.let { CalendarMath.addDays(it, 1, zone) - 1 } ?: return null.also { problem = "Write the repeat end as YYYY-MM-DD." })
        return draft.copy(
            startAt = startMs,
            endAt = endMs,
            recurrenceUntil = untilMs,
            attendees = attendees.split(',').map { it.trim().removePrefix("@") }.filter { it.isNotEmpty() },
        )
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar(title, navigator) {
                FmTextButton(onClick = {
                    val next = built() ?: return@FmTextButton
                    next.validationError?.let { problem = it; return@FmTextButton }
                    saving = true
                    scope.launch {
                        val saved = withContext(Dispatchers.IO) { if (eventId == null) model.create(next) else model.update(eventId, next) }
                        saving = false
                        if (saved != null) navigator.pop() else problem = model.error
                    }
                }, enabled = !saving) { Text(if (saving) "Saving…" else "Save") }
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                problem?.let { Text(it, color = Fm.colors.danger, style = Fm.text.bodySmall) }
                FmTextField(draft.title, { draft = draft.copy(title = it) }, hint = "Title", autofocus = eventId == null)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("All day", Modifier.weight(1f), style = Fm.text.bodyMedium)
                    FmSwitch(checked = draft.allDay, onCheckedChange = { draft = draft.copy(allDay = it) })
                }
                FmTextField(start, { start = it }, hint = "Starts (YYYY-MM-DD HH:MM)")
                FmTextField(end, { end = it }, hint = "Ends (YYYY-MM-DD HH:MM)")
                Box {
                    FmListRow("Calendar", subtitle = draft.calendar, onTap = { calendarMenu = true })
                    FmDropdownMenu(expanded = calendarMenu, onDismissRequest = { calendarMenu = false }) {
                        model.calendars.filter { it.scope == "personal" || it.role in listOf("owner", "admin", "member", "") }.forEach { info ->
                            FmDropdownMenuItem(info.name, onClick = { calendarMenu = false; draft = draft.copy(calendar = if (info.scope == "personal") "personal" else info.name) })
                        }
                    }
                }
                Box {
                    FmListRow("Repeats", subtitle = draft.recurrence.label, onTap = { repeatMenu = true })
                    FmDropdownMenu(expanded = repeatMenu, onDismissRequest = { repeatMenu = false }) {
                        CalendarRecurrence.entries.forEach { value -> FmDropdownMenuItem(value.label, onClick = { repeatMenu = false; draft = draft.copy(recurrence = value) }) }
                    }
                }
                if (draft.recurrence != CalendarRecurrence.NONE) FmTextField(until, { until = it }, hint = "Repeat until (YYYY-MM-DD, optional)")
                FmTextField(draft.location, { draft = draft.copy(location = it) }, hint = "Location")
                FmTextField(draft.meetingUrl, { draft = draft.copy(meetingUrl = it) }, hint = "Meeting link (https://)")
                FmTextField(attendees, { attendees = it }, hint = "Invite (account names, comma separated)")
                FmTextField(draft.description, { draft = draft.copy(description = it) }, hint = "Description", minLines = 3, maxLines = 8)
                FmSectionHeader("Reminders", count = draft.reminders.size)
                CalendarEventDraft.ALLOWED_REMINDERS.forEach { minutes ->
                    val on = minutes in draft.reminders
                    Row(Modifier.fillMaxWidth().clickable {
                        draft = draft.copy(reminders = if (on) draft.reminders - minutes else draft.reminders + minutes)
                    }.padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(CalendarEventDraft.reminderLabel(minutes), Modifier.weight(1f), style = Fm.text.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        FmSwitch(checked = on, onCheckedChange = null)
                    }
                }
                Spacer(Modifier.width(1.dp))
            }
        }
    }
}
