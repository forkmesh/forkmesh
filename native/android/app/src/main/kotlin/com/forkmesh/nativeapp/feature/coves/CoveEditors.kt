package com.forkmesh.nativeapp.feature.coves

import com.forkmesh.nativeapp.ui.kit.confirmAction
import androidx.compose.foundation.BorderStroke
import com.forkmesh.nativeapp.ui.theme.FmType
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.coves.CoveEditorKind
import com.forkmesh.nativeapp.coves.CoveFiles
import com.forkmesh.nativeapp.coves.CovePipeline
import com.forkmesh.nativeapp.coves.CovePipelineException
import com.forkmesh.nativeapp.coves.CovePipelineLead
import com.forkmesh.nativeapp.coves.CoveRepo
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class CoveDocumentScreen(private val repo: CoveRepo, private val coveId: String, private val documentName: String) : Screen() {
    override val title: String get() = documentName

    private var text by mutableStateOf("")
    private var original by mutableStateOf("")
    private var loaded by mutableStateOf(false)
    private var saving by mutableStateOf(false)
    private var error by mutableStateOf<String?>(null)
    private var navigatorRef: Navigator? = null
    private var messengerRef: FmMessenger? = null

    private val dirty: Boolean get() = loaded && text != original

    override fun onBack(): Boolean {
        if (!dirty) return false
        val messenger = messengerRef ?: return false
        com.forkmesh.nativeapp.platform.ProjectLifecycle.scope.launch {
            if (messenger.confirmAction("Discard your changes?", "Discard")) {
                text = original
                navigatorRef?.pop()
            }
        }
        return true
    }

    @Composable
    override fun Content(navigator: Navigator) {
        navigatorRef = navigator
        val messenger = LocalFmMessenger.current
        messengerRef = messenger
        val model = remember { CovesFeature.model(repo) }
        model?.let { rememberModelRevision(it) }
        val scope = rememberCoroutineScope()
        val document = model?.cove(coveId)?.document(documentName)
        if (document != null && !loaded) {
            text = document.body
            original = document.body
            loaded = true
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(documentName, navigator) {
                if (dirty) FmTextButton(onClick = { onBack() }) { Text("Discard") }
                FmTextButton(
                    onClick = {
                        val current = model ?: return@FmTextButton
                        saving = true
                        scope.launch {
                            try {
                                withContext(Dispatchers.IO) { current.saveDocument(coveId, documentName, text) }
                                original = text
                                error = null
                            } catch (failure: Exception) {
                                error = failure.message ?: failure.toString()
                            }
                            saving = false
                        }
                    },
                    enabled = dirty && !saving,
                    busy = saving,
                ) { Text(if (saving) "Saving…" else "Save") }
            }
            error?.let {
                Text(it, modifier = Modifier.fillMaxWidth().background(Fm.colors.danger).padding(FmSpace.x2), style = Fm.text.bodySmall.copy(color = Color.White))
            }
            if (document == null) {
                FmEmptyState("help_outline", "Document unavailable", message = "The cove was locked or the document was removed.")
                return@Column
            }
            when (CoveFiles.editorKind(document.name, document.mime)) {
                CoveEditorKind.Spreadsheet -> CoveSpreadsheetEditor(text, onChange = { text = it })
                CoveEditorKind.Pipeline -> CovePipelineEditor(text, model.account, onChange = { text = it })
                CoveEditorKind.Text -> {
                    val json = document.name.lowercase().endsWith(".json")
                    FmTextField(
                        value = text,
                        onValueChange = { text = it },
                        modifier = Modifier.fillMaxSize().padding(FmSpace.x3),
                        minLines = 12,
                        maxLines = Int.MAX_VALUE,
                        keyboardOptions = if (json) KeyboardOptions(autoCorrectEnabled = false, keyboardType = KeyboardType.Ascii) else KeyboardOptions.Default,
                        textStyle = if (json) Fm.text.bodyMedium.copy(fontFamily = FmType.mono) else Fm.text.bodyLarge,
                    )
                }
            }
        }
    }
}

@Composable
private fun CoveSpreadsheetEditor(text: String, onChange: (String) -> Unit) {
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    val rows = remember(text) { CoveFiles.parseCsv(text) }
    val columns = maxOf(1, rows.maxOfOrNull { it.size } ?: 1)
    val grid = rows.map { row -> List(columns) { row.getOrElse(it) { "" } } }
    val c = Fm.colors
    fun commit(next: List<List<String>>) = onChange(CoveFiles.serializeCsv(next))
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x2), horizontalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            FmTextButton(onClick = { commit(grid + listOf(List(columns) { "" })) }) { Text("Add row") }
            FmTextButton(onClick = { commit(grid.map { it + "" }) }) { Text("Add column") }
            FmTextButton(onClick = { commit(grid.dropLast(1)) }, enabled = grid.size > 1, busy = false) { Text("Delete last row") }
            FmTextButton(onClick = { commit(grid.map { it.dropLast(1) }) }, enabled = columns > 1, busy = false) { Text("Delete last column") }
        }
        Box(Modifier.fillMaxSize().horizontalScroll(rememberScrollState()).verticalScroll(rememberScrollState()).padding(FmSpace.x2)) {
            Column {
                grid.forEachIndexed { r, row ->
                    Row {
                        row.forEachIndexed { col, value ->
                            Box(
                                Modifier
                                    .width(140.dp)
                                    .heightIn(min = 40.dp)
                                    .background(if (r == 0) c.bgOverlay else c.bgRaised)
                                    .border(BorderStroke(0.5.dp, c.border))
                                    .clickable {
                                        scope.launch {
                                            val edited = messenger.showPromptDialog("Edit cell", initialValue = value, confirmLabel = "Done")
                                                ?: return@launch
                                            commit(grid.mapIndexed { ri, cells -> if (ri == r) cells.mapIndexed { ci, cell -> if (ci == col) edited else cell } else cells })
                                        }
                                    }
                                    .padding(FmSpace.x2),
                                contentAlignment = Alignment.CenterStart,
                            ) {
                                Text(
                                    value,
                                    maxLines = 2,
                                    overflow = TextOverflow.Ellipsis,
                                    style = Fm.text.bodySmall.copy(fontWeight = if (r == 0) FontWeight.SemiBold else FontWeight.Normal, color = c.textPrimary),
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

private fun parseColor(hex: String): Color? {
    val clean = hex.trim().removePrefix("#")
    if (clean.length != 6) return null
    return clean.toLongOrNull(16)?.let { Color(0xFF000000 or it) }
}

private fun formatValue(value: Double): String =
    if (value == Math.rint(value)) value.toLong().toString() else String.format("%.2f", value).trimEnd('0').trimEnd('.')

@Composable
private fun CovePipelineEditor(text: String, actor: String, onChange: (String) -> Unit) {
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    val c = Fm.colors
    var stageFilter by remember { mutableStateOf<String?>(null) }
    val parsed = remember(text) {
        try {
            CovePipeline.parse(text) to null
        } catch (failure: CovePipelineException) {
            null to failure.message
        }
    }
    val pipeline = parsed.first
    if (pipeline == null) {
        Column(Modifier.fillMaxSize().padding(FmSpace.x3)) {
            Text("Can't read this pipeline", style = Fm.text.titleMedium)
            Text(parsed.second.orEmpty(), style = Fm.text.bodySmall.copy(color = c.textSecondary))
            FmTextField(
                value = text,
                onValueChange = onChange,
                modifier = Modifier.fillMaxWidth().padding(top = FmSpace.x3),
                minLines = 10,
                maxLines = Int.MAX_VALUE,
                textStyle = Fm.text.bodySmall.copy(fontFamily = FmType.mono),
            )
        }
        return
    }
    fun mutate(change: (CovePipeline) -> Unit) {
        val current = CovePipeline.parse(text)
        change(current)
        onChange(current.body)
    }
    fun editLead(lead: CovePipelineLead) {
        scope.launch {
            val updated = showLeadEditor(messenger, lead) ?: return@launch
            mutate { it.update(updated) }
        }
    }
    val visible = pipeline.leads.filter { stageFilter == null || it.stage == stageFilter }
    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x3)) {
        item {
            Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                for (total in pipeline.totals()) {
                    val selected = stageFilter == total.stage.id
                    Column(
                        Modifier
                            .heightIn(min = 44.dp)
                            .border(if (selected) 2.dp else 1.dp, parseColor(total.stage.color) ?: c.textSecondary, RoundedCornerShape(FmRadius.md))
                            .clickable { stageFilter = if (selected) null else total.stage.id }
                            .padding(FmSpace.x2),
                    ) {
                        Text(total.stage.name, style = Fm.text.bodySmall.copy(fontWeight = FontWeight.SemiBold, color = c.textPrimary))
                        Text("${total.count} · ${formatValue(total.value)}", style = Fm.text.bodySmall.copy(color = c.textSecondary))
                    }
                }
            }
        }
        item { Box(Modifier.padding(top = FmSpace.x4)) { FmSectionHeader(title = "Leads") } }
        items(visible, key = { it.id }) { lead ->
            var stageMenu by remember { mutableStateOf(false) }
            var rowMenu by remember { mutableStateOf(false) }
            Row(Modifier.fillMaxWidth().padding(vertical = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f).clickable { editLead(lead) }) {
                    Text(lead.name, style = Fm.text.bodyLarge)
                    Text(listOf(lead.company, lead.owner).filter { it.isNotEmpty() }.joinToString(" · "), style = Fm.text.bodySmall.copy(color = c.textSecondary))
                }
                Box {
                    FmTextButton(onClick = { stageMenu = true }) { Text(pipeline.stageName(lead.stage)) }
                    FmDropdownMenu(expanded = stageMenu, onDismissRequest = { stageMenu = false }) {
                        for (stage in pipeline.stages) {
                            FmDropdownMenuItem(text = stage.name, onClick = {
                                stageMenu = false
                                mutate { it.move(lead.id, stage.id, actor) }
                            })
                        }
                    }
                }
                Box {
                    com.forkmesh.nativeapp.ui.kit.FmIconButton("more_vert", "Lead actions", onPressed = { rowMenu = true })
                    FmDropdownMenu(expanded = rowMenu, onDismissRequest = { rowMenu = false }) {
                        FmDropdownMenuItem(text = "Delete", onClick = {
                            rowMenu = false
                            mutate { it.delete(lead.id, actor) }
                        })
                    }
                }
            }
        }
        item {
            FmTextButton(onClick = {
                var added: CovePipelineLead? = null
                mutate { added = it.addLead(actor = actor) }
                added?.let { editLead(it) }
            }) { Text("Add lead") }
        }
        if (pipeline.history.isNotEmpty()) {
            item { Box(Modifier.padding(top = FmSpace.x4)) { FmSectionHeader(title = "History") } }
            items(pipeline.history.takeLast(20).reversed()) { entry ->
                val map = entry as? Map<*, *> ?: emptyMap<String, Any?>()
                val lead = map["lead"] as? String ?: ""
                val by = map["by"] as? String ?: ""
                val to = pipeline.stageName(map["to"] as? String ?: "")
                val line = when (map["action"] as? String ?: "") {
                    "created" -> "$by added $lead"
                    "deleted" -> "$by removed $lead"
                    else -> "$by moved $lead to $to"
                }
                Text(line, modifier = Modifier.padding(vertical = FmSpace.x1), style = Fm.text.bodySmall.copy(color = c.textSecondary))
            }
        }
    }
}

private suspend fun showLeadEditor(messenger: FmMessenger, lead: CovePipelineLead): CovePipelineLead? =
    messenger.showDialog<CovePipelineLead> { done ->
        var name by remember { mutableStateOf(lead.name) }
        var company by remember { mutableStateOf(lead.company) }
        var owner by remember { mutableStateOf(lead.owner) }
        var value by remember { mutableStateOf(if (lead.value == 0.0) "" else formatValue(lead.value)) }
        FmAlertDialog(
            onDismissRequest = { done(null) },
            title = "Lead",
            actions = {
                FmTextButton(onClick = { done(null) }) { Text("Cancel") }
                FmTextButton(
                    onClick = {
                        done(lead.copy(name = name.trim(), company = company.trim(), owner = owner.trim(), value = value.trim().toDoubleOrNull() ?: 0.0))
                    },
                    enabled = name.isNotBlank(),
                ) { Text("Done") }
            },
        ) {
            FmTextField(value = name, onValueChange = { name = it }, modifier = Modifier.fillMaxWidth(), hint = "Name")
            FmTextField(value = company, onValueChange = { company = it }, modifier = Modifier.fillMaxWidth().padding(top = FmSpace.x2), hint = "Company")
            FmTextField(value = owner, onValueChange = { owner = it }, modifier = Modifier.fillMaxWidth().padding(top = FmSpace.x2), hint = "Owner")
            FmTextField(
                value = value,
                onValueChange = { value = it },
                modifier = Modifier.fillMaxWidth().padding(top = FmSpace.x2),
                hint = "Value",
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
            )
        }
    }
