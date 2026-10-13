package com.forkmesh.nativeapp.feature.coves

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import com.forkmesh.nativeapp.control.HubControlCalling
import com.forkmesh.nativeapp.coves.ActionsSecrets
import com.forkmesh.nativeapp.coves.ActionsSecretsClient
import com.forkmesh.nativeapp.coves.ActionsSecretsModel
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.DateFormat
import java.util.Date

private data class SecretDraft(val name: String, val value: String)

private suspend fun showSecretEditor(messenger: FmMessenger, name: String?): SecretDraft? = messenger.showDialog<SecretDraft> { done ->
    var draftName by remember { mutableStateOf(name.orEmpty()) }
    var value by remember { mutableStateOf("") }
    val valid = ActionsSecrets.validName(draftName.trim()) && value.toByteArray().size <= ActionsSecrets.MAX_VALUE_BYTES
    FmAlertDialog(
        onDismissRequest = { done(null) },
        title = if (name == null) "New variable" else "Replace $name",
        actions = {
            FmTextButton(onClick = { done(null) }) { Text("Cancel") }
            FmTextButton(onClick = { done(SecretDraft(draftName.trim(), value)) }, enabled = valid) { Text("Save") }
        },
    ) {
        if (name == null) {
            FmTextField(
                value = draftName,
                onValueChange = { draftName = it },
                modifier = Modifier.fillMaxWidth(),
                hint = "Name, e.g. DEPLOY_TOKEN",
                errorText = if (draftName.isNotBlank() && !ActionsSecrets.validName(draftName.trim())) {
                    "Use letters, digits and underscores, starting with a letter or underscore (at most 128)."
                } else {
                    null
                },
            )
        }
        FmTextField(
            value = value,
            onValueChange = { value = it },
            modifier = Modifier.fillMaxWidth().padding(top = FmSpace.x2),
            hint = "Value",
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
            visualTransformation = PasswordVisualTransformation(),
        )
        Text(
            "Values are written to the desktop and never read back. The phone only ever sees names.",
            modifier = Modifier.padding(top = FmSpace.x2),
            style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary),
        )
    }
}

class ActionsSecretsScreen : Screen() {
    override val title: String get() = "Actions secrets"

    private val model: ActionsSecretsModel? by lazy {
        runCatching { AgentsFeature.hub }.getOrNull()?.let { ActionsSecretsModel(ActionsSecretsClient(HubControlCalling(it))) }
    }
    private var revision by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val messenger = LocalFmMessenger.current
        val scope = rememberCoroutineScope()
        val model = model
        revision
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Actions secrets", navigator) {
                if (model != null) {
                    FmIconButton("refresh", "Refresh", onPressed = {
                        scope.launch {
                            withContext(Dispatchers.IO) { model.load() }
                            revision++
                        }
                    })
                    FmIconButton("add", "New variable", onPressed = if (model.busy) null else ({
                        scope.launch {
                            val draft = showSecretEditor(messenger, null) ?: return@launch
                            if (!CovesFeature.authenticate(context, messenger, "Change an Actions secret on your desktop")) return@launch
                            withContext(Dispatchers.IO) { model.save(draft.name, draft.value) }
                            revision++
                        }
                    }))
                }
            }
            if (model == null) {
                FmEmptyState("desktop_windows_outlined", "No desktop connected", message = "Pair a desktop on this network, or sign in so the relay can reach it.")
                return@Column
            }
            LaunchedEffect(model) {
                withContext(Dispatchers.IO) { model.load() }
                revision++
            }
            when {
                model.loading && model.rows.isEmpty() -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                else -> LazyColumn(contentPadding = PaddingValues(FmSpace.x4)) {
                    model.error?.let { item { Text(it, modifier = Modifier.padding(bottom = FmSpace.x2), style = Fm.text.bodySmall.copy(color = Fm.colors.danger)) } }
                    if (model.rows.isEmpty() && model.error == null) {
                        item { Text("No Actions variables on this desktop yet.", style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary)) }
                    }
                    items(model.rows, key = { it.name }) { row ->
                        FmCard(modifier = Modifier.fillMaxWidth().padding(vertical = FmSpace.x1)) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Column(Modifier.weight(1f)) {
                                    Text(row.name, style = Fm.text.bodyLarge)
                                    if (row.updatedAtMs > 0) {
                                        Text(
                                            "Updated " + DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(row.updatedAtMs)),
                                            style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary),
                                        )
                                    }
                                }
                                FmIconButton("edit_outlined", "Replace value", onPressed = {
                                    scope.launch {
                                        val draft = showSecretEditor(messenger, row.name) ?: return@launch
                                        if (!CovesFeature.authenticate(context, messenger, "Change an Actions secret on your desktop")) return@launch
                                        withContext(Dispatchers.IO) { model.save(draft.name, draft.value) }
                                        revision++
                                    }
                                })
                                FmIconButton("delete_outline", "Delete", onPressed = {
                                    scope.launch {
                                        val confirmed = messenger.showConfirmDialog(
                                            "Delete ${row.name}?",
                                            "Workflows that use it will stop finding it.",
                                            confirmLabel = "Delete",
                                            destructive = true,
                                        )
                                        if (!confirmed) return@launch
                                        if (!CovesFeature.authenticate(context, messenger, "Delete an Actions secret on your desktop")) return@launch
                                        withContext(Dispatchers.IO) { model.delete(row.name) }
                                        revision++
                                    }
                                })
                            }
                        }
                    }
                    item {
                        Text(
                            "Secrets stay on your desktop. The desktop asks for a fresh confirmation before every change.",
                            modifier = Modifier.padding(top = FmSpace.x3),
                            style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary),
                        )
                    }
                }
            }
        }
    }
}
