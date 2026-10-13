package com.forkmesh.nativeapp.feature.coves

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import com.forkmesh.nativeapp.coves.CovesModel
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCheckbox
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace

@Composable
internal fun rememberModelRevision(model: CovesModel?): Int {
    var revision by remember { mutableIntStateOf(0) }
    DisposableEffect(model) {
        val listener: () -> Unit = { revision++ }
        model?.addListener(listener)
        onDispose { model?.removeListener(listener) }
    }
    return revision
}

internal suspend fun showPasswordDialog(messenger: FmMessenger, title: String, body: String, confirmLabel: String, error: String? = null): String? =
    messenger.showDialog<String> { done ->
        var text by rememberSaveable { mutableStateOf("") }
        FmAlertDialog(
            onDismissRequest = { done(null) },
            title = title,
            actions = {
                FmTextButton(onClick = { done(null) }) { Text("Cancel") }
                FmTextButton(onClick = { done(text) }, enabled = text.isNotEmpty()) { Text(confirmLabel) }
            },
        ) {
            Text(body, style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
            FmTextField(
                value = text,
                onValueChange = { text = it },
                modifier = Modifier.fillMaxWidth().padding(top = FmSpace.x3),
                hint = "Password",
                autofocus = true,
                errorText = error,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                visualTransformation = PasswordVisualTransformation(),
            )
        }
    }

internal data class NewCoveRequest(val name: String, val password: String?, val notifyOnOpen: Boolean)

internal suspend fun showNewCoveDialog(messenger: FmMessenger, repoSlug: String): NewCoveRequest? =
    messenger.showDialog<NewCoveRequest> { done ->
        var name by rememberSaveable { mutableStateOf("") }
        var usePassword by rememberSaveable { mutableStateOf(false) }
        var password by rememberSaveable { mutableStateOf("") }
        var confirm by rememberSaveable { mutableStateOf("") }
        var notify by rememberSaveable { mutableStateOf(true) }
        val valid = name.isNotBlank() && (!usePassword || (password.isNotEmpty() && password == confirm))
        FmAlertDialog(
            onDismissRequest = { done(null) },
            title = "New cove",
            actions = {
                FmTextButton(onClick = { done(null) }) { Text("Cancel") }
                FmTextButton(
                    onClick = { done(NewCoveRequest(name.trim(), if (usePassword) password else null, usePassword && notify)) },
                    enabled = valid,
                ) { Text("Create") }
            },
        ) {
            FmTextField(value = name, onValueChange = { name = it }, modifier = Modifier.fillMaxWidth(), hint = "Name, e.g. Launch plans", autofocus = true)
            Row(
                Modifier.fillMaxWidth().padding(top = FmSpace.x3),
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
            ) {
                FmTextButton(onClick = { usePassword = false }, enabled = usePassword) { Text("Accounts I invite") }
                FmTextButton(onClick = { usePassword = true }, enabled = !usePassword) { Text("Anyone with a password") }
            }
            if (usePassword) {
                FmTextField(
                    value = password,
                    onValueChange = { password = it },
                    modifier = Modifier.fillMaxWidth(),
                    hint = "Password",
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    visualTransformation = PasswordVisualTransformation(),
                )
                FmTextField(
                    value = confirm,
                    onValueChange = { confirm = it },
                    modifier = Modifier.fillMaxWidth().padding(top = FmSpace.x2),
                    hint = "Type it again",
                    errorText = if (confirm.isNotEmpty() && confirm != password) "The passwords differ." else null,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    visualTransformation = PasswordVisualTransformation(),
                )
            }
            if (usePassword) {
                Row(Modifier.fillMaxWidth().padding(top = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                    FmCheckbox(checked = notify, onCheckedChange = { notify = it })
                    Text("Tell me when someone opens it", style = Fm.text.bodyMedium)
                }
            }
            Text(
                if (usePassword) "Share the password with your team out of band. ForkMesh never sees it."
                else "Each invited account gets its own encrypted key grant. You can invite people after creating it.",
                modifier = Modifier.padding(top = FmSpace.x2),
                style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary),
            )
            Column(Modifier.padding(top = FmSpace.x2)) {
                Text(
                    "The cove is proposed to $repoSlug as an encrypted file. The repository owner merges it like any change.",
                    style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary),
                )
            }
        }
    }
