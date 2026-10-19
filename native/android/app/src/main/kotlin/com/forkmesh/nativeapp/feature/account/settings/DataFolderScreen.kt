package com.forkmesh.nativeapp.feature.account.settings

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.forkmesh.nativeapp.settings.DataFolder
import com.forkmesh.nativeapp.settings.DataFolderNeed
import com.forkmesh.nativeapp.settings.RecoveryKit
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.launch

class DataFolderScreen : Screen() {
    override val title: String get() = "Data folder"

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val scope = rememberCoroutineScope()
        val revision by DataFolderFeature.revision.collectAsState()
        revision
        var passphrase by remember { mutableStateOf("") }
        var confirm by remember { mutableStateOf("") }
        val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri: Uri? ->
            if (uri != null) scope.launch { DataFolderFeature.choose(uri) }
        }
        val feature = DataFolderFeature
        val need = feature.need()
        SettingsPage("Data folder", navigator) {
            item {
                SettingsCard("Where your data is kept") {
                    SettingsNote(DataFolder.ALERT_MESSAGE)
                    SettingsNote(feature.summary(), if (need == DataFolderNeed.ReconnectFolder) Fm.colors.danger else Fm.colors.textPrimary)
                    if (!feature.chosen) SettingsNote(DataFolder.FOLDER_NAME_HINT)
                    FmTextButton(onClick = { picker.launch(null) }, enabled = !feature.working) {
                        Text(if (feature.chosen) "Choose another folder" else "Choose a folder")
                    }
                    if (feature.chosen) {
                        FmTextButton(onClick = { scope.launch { feature.useAutomaticFolder() } }, enabled = !feature.working) {
                            Text("Use the automatic folder")
                        }
                    }
                }
            }
            if (feature.restorable.isNotEmpty()) {
                item {
                    SettingsCard("Restore") {
                        val names = feature.restorable.map { it.name }.filter { it.isNotEmpty() }.joinToString(", ")
                        SettingsNote("Found $names. Enter the passphrase you saved the identity key with.")
                        FmTextField(passphrase, { passphrase = it }, Modifier.fillMaxWidth(), hint = "Passphrase",
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password), visualTransformation = PasswordVisualTransformation())
                        Row {
                            FmTextButton(onClick = {
                                val secret = passphrase
                                scope.launch {
                                    feature.restore(secret)
                                    if (feature.needsRestart) passphrase = ""
                                }
                            }, enabled = !feature.working && passphrase.length >= RecoveryKit.MIN_PASSPHRASE) {
                                Text(if (feature.working) "Restoring…" else "Restore")
                            }
                            FmTextButton(onClick = { scope.launch { feature.startFresh() } }, enabled = !feature.working) {
                                Text("Keep this install's data instead")
                            }
                        }
                    }
                }
            } else if (need == null && !feature.identitySaved()) {
                item {
                    SettingsCard("Identity key (optional)") {
                        SettingsNote("Optional: save your identity key in the folder too, encrypted with a passphrase of at least ${RecoveryKit.MIN_PASSPHRASE} characters, so a reinstall can bring the account back. Nobody can recover the passphrase for you.")
                        FmTextField(passphrase, { passphrase = it }, Modifier.fillMaxWidth(), hint = "Passphrase",
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password), visualTransformation = PasswordVisualTransformation())
                        FmTextField(confirm, { confirm = it }, Modifier.fillMaxWidth(), hint = "Type it again",
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password), visualTransformation = PasswordVisualTransformation())
                        FmTextButton(onClick = {
                            val secret = passphrase
                            val again = confirm
                            scope.launch {
                                feature.saveIdentity(secret, again)
                                if (feature.identitySaved()) {
                                    passphrase = ""
                                    confirm = ""
                                }
                            }
                        }, enabled = !feature.working && RecoveryKit.validatePassphrase(passphrase, confirm) == null) {
                            Text(if (feature.working) "Encrypting…" else "Save identity key")
                        }
                    }
                }
            }
            if (feature.status.isNotEmpty() || feature.needsRestart) {
                item {
                    SettingsCard {
                        if (feature.status.isNotEmpty()) {
                            SettingsNote(feature.status, if (feature.statusIsError) Fm.colors.danger else Fm.colors.textSecondary)
                        }
                        if (feature.needsRestart) {
                            FmTextButton(onClick = { DataFolderFeature.restart(context) }) { Text("Restart ForkMesh") }
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun DataFolderAlert() {
    val revision by DataFolderFeature.revision.collectAsState()
    revision
    var hidden by remember { mutableStateOf(false) }
    var setup by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) { DataFolderFeature.sync() }
    if (setup) {
        val local = remember { Navigator(DataFolderScreen()) }
        Dialog(onDismissRequest = { setup = false }, properties = DialogProperties(usePlatformDefaultWidth = false)) {
            Box(Modifier.fillMaxSize().background(Fm.colors.bgBase)) { local.top.Content(local) }
        }
        return
    }
    if (hidden || !DataFolderFeature.showsAlert()) return
    val need = DataFolderFeature.need() ?: return
    val dismiss = {
        DataFolderFeature.dismissedThisLaunch = true
        hidden = true
    }
    FmAlertDialog(
        onDismissRequest = dismiss,
        title = DataFolder.ALERT_TITLE,
        actions = {
            FmTextButton(onClick = dismiss) { Text("Not now") }
            FmTextButton(onClick = {
                DataFolderFeature.dismissedThisLaunch = true
                setup = true
            }) { Text("Set up") }
        },
    ) {
        Text(
            DataFolder.message(need),
            modifier = Modifier.padding(start = FmSpace.x5, end = FmSpace.x5, top = FmSpace.x3, bottom = FmSpace.x3),
            style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
        )
    }
}
