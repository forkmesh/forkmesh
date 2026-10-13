package com.forkmesh.nativeapp.feature.teammates

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
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
import com.forkmesh.nativeapp.feature.agents.ChipRow
import com.forkmesh.nativeapp.feature.agents.opsIo
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.FmFilledButton
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.teammates.AgentChoiceRow
import com.forkmesh.nativeapp.teammates.TeamErrorText
import com.forkmesh.nativeapp.teammates.TeammateAction
import com.forkmesh.nativeapp.teammates.TeammateDraft
import com.forkmesh.nativeapp.teammates.TeammateEditorLogic
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

internal class TeammateEditorScreen(private val id: String?) : Screen() {
    override val title: String get() = if (id == null) "Add special agent" else "Edit special agent"
    private var original by mutableStateOf<TeammateDraft?>(null)
    private var draft by mutableStateOf(TeammateDraft())
    private var skillsText by mutableStateOf("")
    private var eventsText by mutableStateOf("")
    private var loaded by mutableStateOf(false)
    private var saving by mutableStateOf(false)
    private var error by mutableStateOf("")

    private val currentDraft: TeammateDraft
        get() = draft.copy(
            skills = skillsText.split(',').map { it.trim() }.filter { it.isNotEmpty() },
            schedule = draft.schedule.copy(events = eventsText.split(',').map { it.trim() }.filter { it.isNotEmpty() }),
        )

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val store = remember { TeammatesFeature.teammates }
        val scope = rememberCoroutineScope()
        LaunchedEffect(id) {
            if (loaded) return@LaunchedEffect
            opsIo { store.loadOptions() }
            if (id != null) {
                try {
                    val detail = opsIo { store.detail(id) }
                    val start = TeammateDraft.from(detail)
                    original = start
                    draft = start
                    skillsText = start.skills.joinToString(", ")
                    eventsText = start.schedule.events.joinToString(", ")
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    error = TeamErrorText.describe(failure)
                }
            }
            loaded = true
        }
        val issues = TeammateEditorLogic.validate(currentDraft)
        val changes = TeammateEditorLogic.changes(original, currentDraft)
        fun save() {
            scope.launch {
                saving = true
                try {
                    val action = if (id == null) TeammateAction.Create(changes) else TeammateAction.Edit(id, changes)
                    opsIo { store.perform(action) }
                    navigator.pop()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    error = TeamErrorText.describe(failure)
                } finally {
                    saving = false
                }
            }
        }
        DetailScaffold(title, navigator) {
            if (!loaded) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
                return@DetailScaffold
            }
            val options = store.options
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                item { FmSectionHeader("Identity") }
                item { FmTextField(draft.name, { draft = draft.copy(name = it) }, hint = "Name") }
                item { FmTextField(draft.specialty, { draft = draft.copy(specialty = it) }, hint = "Specialty") }
                item { FmTextField(draft.focus, { draft = draft.copy(focus = it) }, hint = "Focus") }
                item { FmTextField(draft.description, { draft = draft.copy(description = it) }, hint = "Description", minLines = 2, maxLines = 5) }
                item { FmTextField(draft.avatar, { draft = draft.copy(avatar = it) }, hint = "Avatar") }
                item { FmSectionHeader("Agent") }
                item { Picker("Provider", draft.provider, options.providers.ifEmpty { listOf(draft.provider) }, { it }) { draft = draft.copy(provider = it) } }
                item { FmTextField(draft.model, { draft = draft.copy(model = it) }, hint = "Model") }
                item { Picker("Mode", draft.mode, options.modes, { it.ifEmpty { "Desktop default" } }) { draft = draft.copy(mode = it) } }
                item { Toggle("Enabled", draft.enabled) { draft = draft.copy(enabled = it) } }
                item { Toggle("Learns from its runs", draft.selfImproving) { draft = draft.copy(selfImproving = it) } }
                item { FmTextField(draft.instructions, { draft = draft.copy(instructions = it) }, hint = "Instructions", minLines = 3, maxLines = 10) }
                item { FmTextField(skillsText, { skillsText = it }, hint = "Skills, separated by commas") }
                item { FmSectionHeader("Backup models") }
                item {
                    ChoiceList(draft.backupModels, withTask = false) { draft = draft.copy(backupModels = it) }
                }
                item { FmSectionHeader("Task agents") }
                item {
                    ChoiceList(draft.taskAgents, withTask = true, taskTypes = options.taskTypes.map { it.id to it.label }) { draft = draft.copy(taskAgents = it) }
                }
                item { FmSectionHeader("Schedule") }
                item {
                    Picker("Runs", draft.schedule.mode, options.scheduleModes, TeammateEditorLogic::scheduleModeLabel) {
                        draft = draft.copy(schedule = draft.schedule.copy(mode = it))
                    }
                }
                if (TeammateEditorLogic.usesTime(draft.schedule.mode)) {
                    item { FmTextField(draft.schedule.time, { draft = draft.copy(schedule = draft.schedule.copy(time = it)) }, hint = "Time (HH:MM)") }
                }
                if (TeammateEditorLogic.usesDays(draft.schedule.mode)) {
                    item {
                        ChipRow {
                            for ((day, short) in TeammateEditorLogic.weekdays) {
                                val on = day in draft.schedule.days
                                FilterChip(short, on) {
                                    val days = if (on) draft.schedule.days - day else (draft.schedule.days + day).sorted()
                                    draft = draft.copy(schedule = draft.schedule.copy(days = days))
                                }
                            }
                        }
                    }
                }
                if (TeammateEditorLogic.usesEvents(draft.schedule.mode)) {
                    item { FmTextField(eventsText, { eventsText = it }, hint = "Events, separated by commas") }
                }
                item { Toggle("Schedule on", draft.schedule.active) { draft = draft.copy(schedule = draft.schedule.copy(active = it)) } }
                item { FmTextField(draft.relayChannel, { draft = draft.copy(relayChannel = it) }, hint = "Relay room, like #team") }
                for (issue in issues) item { Text(issue.message, style = Fm.text.bodySmall.copy(color = c.warning)) }
                item { Banner(error, true) }
                item {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        FmFilledButton(
                            if (id == null) "Add teammate" else "Save changes",
                            onClick = if (saving || issues.isNotEmpty() || changes.isEmpty()) null else ({ save() }),
                            icon = "check",
                        )
                        if (saving) CircularProgressIndicator(Modifier.padding(start = FmSpace.x2))
                    }
                }
                if (changes.isEmpty() && id != null) item { Text("Nothing changed.", style = Fm.text.bodySmall.copy(color = c.textTertiary)) }
            }
        }
    }
}

@Composable
private fun Toggle(label: String, on: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(label, style = Fm.text.bodyMedium.copy(color = Fm.colors.textPrimary), modifier = Modifier.weight(1f))
        FmSwitch(checked = on, onCheckedChange = onChange)
    }
}

@Composable
private fun Picker(label: String, value: String, choices: List<String>, title: (String) -> String, onPick: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(label, style = Fm.text.bodyMedium.copy(color = Fm.colors.textPrimary), modifier = Modifier.weight(1f))
        Box {
            FmOutlinedButton(title(value), onClick = { open = true }, icon = "expand_more")
            FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                for (choice in choices) {
                    FmDropdownMenuItem(title(choice), icon = if (choice == value) "check" else null, onClick = {
                        open = false
                        onPick(choice)
                    })
                }
            }
        }
    }
}

@Composable
private fun ChoiceList(
    rows: List<AgentChoiceRow>,
    withTask: Boolean,
    taskTypes: List<Pair<String, String>> = emptyList(),
    onChange: (List<AgentChoiceRow>) -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        rows.forEachIndexed { index, row ->
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                if (withTask) {
                    Picker("Task", row.taskType, taskTypes.map { it.first }.ifEmpty { listOf(row.taskType) }, { id ->
                        taskTypes.firstOrNull { it.first == id }?.second ?: id.ifEmpty { "Choose" }
                    }) { picked -> onChange(rows.toMutableList().also { it[index] = row.copy(taskType = picked) }) }
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmTextField(row.provider, { value -> onChange(rows.toMutableList().also { it[index] = row.copy(provider = value) }) }, Modifier.weight(1f), hint = "provider")
                    FmTextField(row.model, { value -> onChange(rows.toMutableList().also { it[index] = row.copy(model = value) }) }, Modifier.weight(1f), hint = "model")
                    FmIconButton("delete_outline", "Remove", onPressed = { onChange(rows.toMutableList().also { it.removeAt(index) }) })
                }
            }
        }
        if (rows.size < 10) {
            FmOutlinedButton("Add", onClick = { onChange(rows + AgentChoiceRow(provider = "", model = "")) }, icon = "add")
        }
    }
}
