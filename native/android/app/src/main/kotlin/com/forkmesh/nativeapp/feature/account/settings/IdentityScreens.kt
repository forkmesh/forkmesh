package com.forkmesh.nativeapp.feature.account.settings

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.text.KeyboardOptions
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.SettingsService
import com.forkmesh.nativeapp.feature.tools.copyToClipboard
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.settings.IdentityBackupStore
import com.forkmesh.nativeapp.settings.NodeLinkModel
import com.forkmesh.nativeapp.settings.NodeLinkOutcome
import com.forkmesh.nativeapp.settings.RecoveryKit
import com.forkmesh.nativeapp.settings.RecoveryKitException
import com.forkmesh.nativeapp.settings.SolanaAddress
import com.forkmesh.nativeapp.settings.TrustListSummary
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.QrImage
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.DateFormat
import java.util.Date

class IdentitySettingsScreen : Screen() {
    override val title: String get() = "Identity and recovery"

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val scope = rememberCoroutineScope()
        val keyCopy = rememberFmStatus()
        val approvalCopy = rememberFmStatus()
        val identity = Services.get<DeviceIdentity>()
        val key = identity?.publicKeyB64url.orEmpty()
        val store = SettingsArea.prefs?.let(::IdentityBackupStore)
        var backup by remember { mutableStateOf(store?.state) }
        var passphrase by remember { mutableStateOf("") }
        var confirm by remember { mutableStateOf("") }
        var status by remember { mutableStateOf<String?>(null) }
        var pendingKit by remember { mutableStateOf<String?>(null) }
        var checkPassphrase by remember { mutableStateOf("") }
        var checkResult by remember { mutableStateOf<String?>(null) }
        var trustText by remember { mutableStateOf("") }
        val wallet = Services.get<SettingsService>()?.solanaAddress?.value.orEmpty()
        val account = SettingsArea.account

        val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri: Uri? ->
            val kit = pendingKit
            pendingKit = null
            if (uri == null || kit == null) return@rememberLauncherForActivityResult
            scope.launch {
                val ok = withContext(Dispatchers.IO) {
                    runCatching { context.contentResolver.openOutputStream(uri, "wt")?.use { it.write(kit.toByteArray()) } != null }.getOrDefault(false)
                }
                if (ok) {
                    store?.markBackedUp(key, System.currentTimeMillis())
                    backup = store?.state
                    passphrase = ""
                    confirm = ""
                    status = "Recovery kit saved. Keep the file and the passphrase in different places."
                } else {
                    status = "Could not save the recovery kit."
                }
            }
        }
        val open = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            if (uri == null) return@rememberLauncherForActivityResult
            val secret = checkPassphrase
            scope.launch {
                checkResult = withContext(Dispatchers.IO) {
                    try {
                        val text = context.contentResolver.openInputStream(uri)?.use { String(it.readBytes().take(256 * 1024).toByteArray()) }.orEmpty()
                        val result = RecoveryKit.check(text, secret, key)
                        if (result.matchesThisDevice) "This kit restores this phone's key (${RecoveryKit.shortKey(result.publicKey)})."
                        else RecoveryKitException.differentKey(result.publicKey).message
                    } catch (failure: Exception) {
                        failure.message ?: "That file is not a ForkMesh recovery kit."
                    }
                }
            }
        }

        SettingsPage("Identity and recovery", navigator) {
            item {
                SettingsCard("This device's key") {
                    Text(key.ifEmpty { "No identity key yet." }, style = Fm.mono())
                    if (key.isNotEmpty() && account.isNotEmpty()) {
                        Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                            QrImage("forkmesh:identity/${account.lowercase()}/$key", size = 180.dp, description = "Identity QR code")
                        }
                    }
                    Row {
                        FmTextButton(onClick = {
                            copyToClipboard(context, key)
                            keyCopy.show("Key copied.")
                        }, enabled = key.isNotEmpty()) { Text("Copy") }
                    }
                    FmInlineStatus(keyCopy)
                    val state = backup
                    SettingsNote(
                        when {
                            state == null -> ""
                            state.needsBackup(key) -> "This key has no recovery kit yet."
                            else -> "Backed up " + DateFormat.getDateInstance(DateFormat.MEDIUM).format(Date(state.backedUpAt)) + "."
                        },
                    )
                }
            }
            item {
                SettingsCard("Deploy approval key") {
                    SettingsNote("Merging a pull request or an agent branch from this phone signs a deploy approval with this key.")
                    Text(key.ifEmpty { "No identity key yet." }, style = Fm.mono())
                    Row {
                        FmTextButton(onClick = {
                            copyToClipboard(context, key)
                            approvalCopy.show("Deploy approval key copied.")
                        }, enabled = key.isNotEmpty()) { Text("Copy") }
                    }
                    FmInlineStatus(approvalCopy)
                }
            }
            item {
                SettingsCard("Make a recovery kit") {
                    SettingsNote("The kit is your key encrypted with a passphrase. Anyone with both can act as this device, so keep them apart.")
                    FmTextField(passphrase, { passphrase = it }, Modifier.fillMaxWidth(), hint = "Passphrase (12 or more characters)",
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password), visualTransformation = PasswordVisualTransformation())
                    FmTextField(confirm, { confirm = it }, Modifier.fillMaxWidth(), hint = "Type it again",
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password), visualTransformation = PasswordVisualTransformation())
                    val strength = RecoveryKit.strength(passphrase)
                    if (passphrase.isNotEmpty()) SettingsNote(listOf("Too short", "Weak", "Good", "Strong")[strength])
                    FmTextButton(
                        onClick = {
                            val current = identity ?: return@FmTextButton
                            val phrase = passphrase
                            val again = confirm
                            scope.launch {
                                val kit = withContext(Dispatchers.Default) {
                                    try {
                                        RecoveryKit.export(current, phrase, again, System.currentTimeMillis())
                                    } catch (failure: Exception) {
                                        status = failure.message
                                        null
                                    }
                                } ?: return@launch
                                pendingKit = kit
                                save.launch(RecoveryKit.fileName(account, System.currentTimeMillis()))
                            }
                        },
                        enabled = identity != null && RecoveryKit.validatePassphrase(passphrase, confirm) == null,
                    ) { Text("Save recovery kit") }
                    status?.let { SettingsNote(it) }
                }
            }
            item {
                SettingsCard("Check a recovery kit") {
                    FmTextField(checkPassphrase, { checkPassphrase = it }, Modifier.fillMaxWidth(), hint = "Its passphrase",
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password), visualTransformation = PasswordVisualTransformation())
                    FmTextButton(onClick = { open.launch(arrayOf("application/json", "text/plain", "*/*")) }, enabled = checkPassphrase.length >= RecoveryKit.MIN_PASSPHRASE) {
                        Text("Choose the kit file")
                    }
                    checkResult?.let { SettingsNote(it) }
                }
            }
            if (wallet.isNotEmpty() && SolanaAddress.isValid(wallet)) {
                item {
                    SettingsCard("Payout wallet") {
                        Text(SolanaAddress.short(wallet), style = Fm.mono())
                        Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                            QrImage("solana:$wallet", size = 180.dp, description = "Wallet QR code")
                        }
                    }
                }
            }
            item {
                SettingsCard("Check a trust list") {
                    SettingsNote("Paste a signed trust list, revocation or appeal to see who issued it and what it covers.")
                    FmTextField(trustText, { trustText = it }, Modifier.fillMaxWidth(), hint = "Trust list JSON", minLines = 3, maxLines = 8)
                    if (trustText.isNotBlank()) {
                        val summary = remember(trustText) { TrustListSummary.summarize(trustText) }
                        summary.fold(
                            onSuccess = {
                                SettingsNote("${it.kind} · ${it.name.ifEmpty { "unnamed" }} by ${it.issuer.take(16)} · version ${it.version} · ${it.entries} entries")
                                SettingsNote(it.documentId)
                            },
                            onFailure = { SettingsNote("That document does not verify: ${it.message}", Fm.colors.danger) },
                        )
                    }
                }
            }
        }
    }
}

class LinkNodeScreen : Screen() {
    override val title: String get() = "Link a node"
    private val scanned = mutableStateOf<String?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val endpoints = SettingsArea.endpoints
        val model = remember { endpoints?.let { NodeLinkModel(it, { SettingsArea.account }) } }
        var link by remember { mutableStateOf("") }
        var revision by remember { mutableStateOf(0) }
        revision
        val scannedLink = scanned.value
        LaunchedEffect(scannedLink) {
            if (scannedLink != null && model != null) {
                link = scannedLink
                model.review(scannedLink)
                scanned.value = null
                revision++
            }
        }
        SettingsPage("Nodes on this account", navigator) {
            if (model == null) {
                item { SettingsNote("Sign in to link nodes to your account.") }
                return@SettingsPage
            }
            item {
                SettingsCard("Link a node") {
                    SettingsNote("On the desktop, open Settings > Account and choose Link to a user. Paste the link it shows. Codes expire after five minutes.")
                    FmTextField(link, { link = it }, Modifier.fillMaxWidth(), hint = "https://…?link_node=…", minLines = 2, maxLines = 4)
                    Row {
                        FmTextButton(onClick = {
                            model.review(link)
                            revision++
                        }, enabled = link.isNotBlank() && !model.busy, busy = model.busy) { Text("Review") }
                        FmTextButton(onClick = {
                            navigator.push(QrScannerScreen("Scan the desktop's code") { scanned.value = it })
                        }, enabled = !model.busy) { Text("Scan code") }
                    }
                    model.pending?.let { grant ->
                        SettingsNote("Link @${grant.node} to @${SettingsArea.account}?")
                        Row {
                            FmTextButton(onClick = {
                                scope.launch {
                                    withContext(Dispatchers.IO) { model.confirm() }
                                    revision++
                                }
                            }) { Text("Link") }
                            FmTextButton(onClick = {
                                model.cancel()
                                revision++
                            }) { Text("Cancel") }
                        }
                    }
                    when (val outcome = model.outcome) {
                        is NodeLinkOutcome.Linked -> SettingsNote("Linked @${outcome.node}. Your account now has ${outcome.nodes.size} node(s).")
                        is NodeLinkOutcome.AlreadyLinked -> SettingsNote("@${outcome.node} was already linked to your account.")
                        else -> Unit
                    }
                    if (model.error.isNotEmpty()) SettingsNote(model.error, Fm.colors.danger)
                }
            }
        }
    }
}
