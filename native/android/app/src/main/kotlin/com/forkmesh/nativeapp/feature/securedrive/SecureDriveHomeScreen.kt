package com.forkmesh.nativeapp.feature.securedrive

import android.net.Uri
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmElevatedButton
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.kit.FmPromptDialog
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch

class SecureDriveHomeScreen : Screen() {
    override val title: String get() = "Secure Drive"

    @Composable
    override fun Content(navigator: Navigator) {
        val service = SecureDriveFeature.service
        val phase by service.phase.collectAsState()
        SdScaffold("Secure Drive", navigator, secure = phase == SdPhase.Unlocked) {
            when (phase) {
                SdPhase.Setup -> SetUpForm()
                SdPhase.Locked -> UnlockForm()
                SdPhase.Unlocked -> DriveList(navigator)
            }
        }
    }
}

@Composable
private fun FormColumn(content: @Composable () -> Unit) {
    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .imePadding()
            .padding(FmSpace.x4),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) { content() }
}

@Composable
private fun PassphraseField(label: String, value: String, onChange: (String) -> Unit, helper: String? = null) {
    FmField(label) {
        FmTextField(
            value = value,
            onValueChange = onChange,
            helperText = helper,
            visualTransformation = PasswordVisualTransformation(),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, autoCorrectEnabled = false),
        )
    }
}

@Composable
private fun SetUpForm() {
    val service = SecureDriveFeature.service
    val scope = rememberCoroutineScope()
    var name by rememberSaveable { mutableStateOf(Build.MODEL.orEmpty()) }
    var pass by remember { mutableStateOf("") }
    var again by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    val busy by service.busy.collectAsState()
    FormColumn {
        Text("Set up Secure Drive on this phone", style = Fm.text.titleMedium)
        Text(
            "Secure Drive keeps an end-to-end encrypted drive in one file on your local network, for example on a USB drive plugged into your router. " +
                "Your passphrase opens this phone's keys. It is never stored or sent anywhere.",
            style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
        )
        FmField("Phone name") { FmTextField(name, { name = it }, hint = "This phone") }
        PassphraseField("Passphrase", pass, { pass = it }, "At least 10 characters; a few unrelated words work well")
        PassphraseField("Type it again", again, { again = it })
        error?.let { Text(it, style = Fm.text.bodySmall.copy(color = Fm.colors.danger)) }
        FmElevatedButton(
            onClick = {
                error = null
                scope.launch {
                    service.setUp(name, pass, again).onSuccess {
                        pass = ""
                        again = ""
                    }.onFailure { error = it.sdText() }
                }
            },
            enabled = busy == null && pass.isNotEmpty(),
        ) { Text("Create this phone's keys") }
    }
}

@Composable
private fun UnlockForm() {
    val service = SecureDriveFeature.service
    val scope = rememberCoroutineScope()
    var pass by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    val notice by service.notice.collectAsState()
    val busy by service.busy.collectAsState()
    FormColumn {
        Text("Unlock Secure Drive", style = Fm.text.titleMedium)
        notice?.let { Text(it, style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary)) }
        PassphraseField("Passphrase", pass, { pass = it })
        error?.let { Text(it, style = Fm.text.bodySmall.copy(color = Fm.colors.danger)) }
        FmElevatedButton(
            onClick = {
                error = null
                scope.launch {
                    service.unlock(pass).onSuccess { pass = "" }.onFailure { error = it.sdText() }
                }
            },
            enabled = busy == null && pass.isNotEmpty(),
        ) { Text("Unlock") }
    }
}

private sealed class HomeDialog {
    data object Name : HomeDialog()
    data class Code(val uri: Uri) : HomeDialog()
    data object Passphrase : HomeDialog()
}

@Composable
private fun DriveList(navigator: Navigator) {
    val service = SecureDriveFeature.service
    val scope = rememberCoroutineScope()
    val drives by service.summaries.collectAsState()
    val busy by service.busy.collectAsState()
    var dialog by remember { mutableStateOf<HomeDialog?>(null) }
    var pendingLabel by rememberSaveable { mutableStateOf("") }
    var message by remember { mutableStateOf<String?>(null) }
    val create = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            service.createDrive(uri, pendingLabel)
                .onSuccess { id ->
                    message = "The drive is ready. Add files, or invite another device under Devices."
                    navigator.push(SecureDriveDriveScreen(id))
                }
                .onFailure { message = it.sdText() }
        }
    }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) dialog = HomeDialog.Code(uri)
    }
    FormColumn {
        Text(
            "This phone's keys are ready. Create a drive, or join one with an invitation code from a device that is already approved.",
            style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
        )
        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            FmElevatedButton(onClick = { dialog = HomeDialog.Name }, enabled = busy == null) { Text("Create drive") }
            FmTextButton(onClick = { pick.launch(arrayOf("*/*")) }, enabled = busy == null) { Text("Join a drive") }
        }
        message?.let { Text(it, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary)) }
        if (drives.isEmpty()) {
            Text("No drive yet", style = Fm.text.titleSmall.copy(color = Fm.colors.textTertiary))
        }
        for (drive in drives) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .clickable { navigator.push(SecureDriveDriveScreen(drive.id)) }
                    .padding(vertical = FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon("lock", size = 20.dp, tint = Fm.colors.textTertiary)
                Spacer(Modifier.width(FmSpace.x3))
                Column(Modifier.weight(1f)) {
                    Text(drive.label, style = Fm.text.bodyLarge)
                    if (drive.pending) Text("Waiting for approval", style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
                }
            }
            HorizontalDivider(color = Fm.colors.border)
        }
        SdSpacer()
        Text("This phone", style = Fm.text.titleSmall)
        Text(service.deviceName, style = Fm.text.bodyMedium)
        Text("Safety number", style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
        SdSafetyNumber(service.deviceSafety)
        FmTextButton(onClick = { dialog = HomeDialog.Passphrase }) { Text("Change passphrase") }
    }
    when (val open = dialog) {
        HomeDialog.Name -> FmPromptDialog(
            title = "Name the drive",
            body = "Next, save the drive file on the router's USB drive or network share.",
            initialValue = "Secure Drive",
            confirmLabel = "Continue",
            onConfirm = { label ->
                dialog = null
                pendingLabel = label.trim()
                create.launch("${label.trim().ifEmpty { "SecureDrive" }}.fmsd")
            },
            onDismissRequest = { dialog = null },
        )
        is HomeDialog.Code -> FmPromptDialog(
            title = "Join a drive",
            body = "Type the invitation code from a device that is already approved.",
            hint = "XXX-XXX-XXX-XXX-XXX-XXX-XXX-XXX-XXX",
            confirmLabel = "Ask to join",
            keyboardOptions = KeyboardOptions(autoCorrectEnabled = false),
            onConfirm = { code ->
                dialog = null
                scope.launch {
                    service.joinDrive(open.uri, code)
                        .onSuccess { id -> navigator.push(SecureDriveDriveScreen(id)) }
                        .onFailure { message = it.sdText() }
                }
            },
            onDismissRequest = { dialog = null },
        )
        HomeDialog.Passphrase -> ChangePassphraseDialog(onDone = { text ->
            dialog = null
            if (text != null) message = text
        })
        null -> Unit
    }
}

@Composable
private fun ChangePassphraseDialog(onDone: (String?) -> Unit) {
    val service = SecureDriveFeature.service
    val scope = rememberCoroutineScope()
    var current by remember { mutableStateOf("") }
    var next by remember { mutableStateOf("") }
    var again by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    FmConfirmDialog(
        title = "Change passphrase",
        confirmLabel = "Change",
        onConfirm = {
            scope.launch {
                service.changePassphrase(current, next, again)
                    .onSuccess { onDone("Passphrase changed.") }
                    .onFailure { error = it.sdText() }
            }
        },
        onDismissRequest = { onDone(null) },
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            PassphraseField("Current passphrase", current, { current = it })
            PassphraseField("New passphrase", next, { next = it })
            PassphraseField("Type it again", again, { again = it })
            error?.let { Text(it, style = Fm.text.bodySmall.copy(color = Fm.colors.danger)) }
        }
    }
}
