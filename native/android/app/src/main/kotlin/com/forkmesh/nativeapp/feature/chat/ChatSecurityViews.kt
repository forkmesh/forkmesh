package com.forkmesh.nativeapp.feature.chat

import android.app.Activity
import com.forkmesh.nativeapp.ui.theme.FmType
import android.content.Context
import android.content.ContextWrapper
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
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.chat.DirectMessageFrames
import com.forkmesh.nativeapp.chat.DirectSafetyScan
import com.forkmesh.nativeapp.chat.DirectTrustNotice
import com.forkmesh.nativeapp.chat.DirectTrustText
import com.forkmesh.nativeapp.feature.account.settings.QrScannerScreen
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmDialogActions
import com.forkmesh.nativeapp.ui.kit.FmElevatedButton
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.QrImage
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private tailrec fun Context.hostActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.hostActivity()
    else -> null
}

@Composable
fun ChatLockGate(content: @Composable () -> Unit) {
    val enabled by ChatPrivacy.lockEnabled.collectAsState()
    val locked by ChatPrivacy.chatLocked.collectAsState()
    if (!enabled || !locked) {
        content()
        return
    }
    val context = LocalContext.current
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        if (result.resultCode == Activity.RESULT_OK) ChatPrivacy.unlocked()
    }
    val ask = {
        val activity = context.hostActivity()
        if (!ChatPrivacy.deviceSecure(context)) {
            ChatPrivacy.unlocked()
        } else if (ChatPrivacy.usesPrompt() && activity != null) {
            ChatPrivacy.prompt(activity) { ok -> if (ok) ChatPrivacy.unlocked() }
        } else {
            ChatPrivacy.credentialIntent(context)?.let { runCatching { launcher.launch(it) } } ?: ChatPrivacy.unlocked()
        }
    }
    LaunchedEffect(Unit) { ask() }
    Box(Modifier.fillMaxSize().background(Fm.colors.canvas), contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            FmIcon("lock_outline", size = 40.dp, tint = Fm.colors.textTertiary)
            Spacer(Modifier.height(FmSpace.x4))
            Text("Chats are locked", style = Fm.text.titleMedium)
            Spacer(Modifier.height(FmSpace.x3))
            FmElevatedButton(onClick = { ask() }) { Text("Unlock") }
        }
    }
}

@Composable
fun ChatPrivacySettings() {
    val context = LocalContext.current
    val hide by ChatPrivacy.hideInSwitcher.collectAsState()
    val lock by ChatPrivacy.lockEnabled.collectAsState()
    val secure = remember { ChatPrivacy.deviceSecure(context) }
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
        PrivacyRow(ChatPrivacy.HIDE_IN_SWITCHER_TITLE, ChatPrivacy.HIDE_IN_SWITCHER_NOTE, hide, enabled = true) { ChatPrivacy.setHideInSwitcher(it) }
        PrivacyRow(ChatPrivacy.LOCK_TITLE, if (secure) ChatPrivacy.LOCK_NOTE else ChatPrivacy.LOCK_UNAVAILABLE, lock, enabled = secure) {
            ChatPrivacy.setLockEnabled(it)
        }
    }
}

@Composable
private fun PrivacyRow(title: String, note: String, checked: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(title, style = Fm.text.bodyLarge)
            Text(note, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
        }
        Spacer(Modifier.width(FmSpace.x3))
        FmSwitch(checked = checked, onCheckedChange = onChange, enabled = enabled)
    }
}

@Composable
fun ChatSystemLine(text: String) {
    Text(
        text,
        modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x5, vertical = FmSpace.x2),
        textAlign = TextAlign.Center,
        style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
    )
}

@Composable
fun PendingDeviceBanner(conversation: String, revision: Int) {
    val scope = rememberCoroutineScope()
    val pending by produceState(emptyList<DirectTrustNotice>(), conversation, revision) {
        value = withContext(Dispatchers.IO) { ChatDepth.direct?.trustNotices(conversation).orEmpty() }
    }
    val notice = pending.firstOrNull { it.asksApproval } ?: return
    var working by remember(notice.id) { mutableStateOf(false) }
    var confirmBlock by remember(notice.id) { mutableStateOf(false) }
    val colors = Fm.colors
    fun decide(approve: Boolean) {
        if (working) return
        working = true
        scope.launch {
            withContext(Dispatchers.IO) {
                val direct = ChatDepth.direct
                for (kid in notice.kids) {
                    if (approve) direct?.approveDevice(conversation, kid) else direct?.blockDevice(conversation, kid)
                }
            }
            working = false
        }
    }
    Column(Modifier.fillMaxWidth().background(colors.bgRaised).padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
        Text(notice.text, style = Fm.text.bodySmall.copy(color = colors.textSecondary))
        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            FmTextButton(onClick = { decide(approve = true) }, enabled = !working) { Text("Approve") }
            FmTextButton(onClick = { confirmBlock = true }, enabled = !working, contentColor = colors.danger) { Text("Block") }
        }
    }
    if (confirmBlock) {
        FmAlertDialog(
            onDismissRequest = { confirmBlock = false },
            title = DirectTrustText.blockTitle(notice.kids.size),
            actions = {
                FmDialogActions(
                    onCancel = { confirmBlock = false },
                    onConfirm = {
                        confirmBlock = false
                        decide(approve = false)
                    },
                    confirmLabel = "Block",
                    destructive = true,
                )
            },
        ) {
            Text(DirectTrustText.blockNote(notice.kids.size), style = Fm.text.bodyMedium.copy(color = colors.textSecondary))
        }
    }
}

class SafetyNumberScreen(private val conversation: String) : Screen() {
    override val title: String get() = "Safety number"

    @Composable
    override fun Content(navigator: Navigator) {
        val revision by ChatDepth.revision.collectAsState()
        val scope = rememberCoroutineScope()
        var notice by remember { mutableStateOf<String?>(null) }
        val number by produceState<com.forkmesh.nativeapp.crypto.ChatV3.SafetyNumber?>(null, conversation, revision) {
            value = withContext(Dispatchers.IO) { ChatDepth.direct?.safetyNumber(conversation) }
        }
        val verified by produceState(false, conversation, revision) {
            value = withContext(Dispatchers.IO) { ChatDepth.direct?.isVerified(conversation) == true }
        }
        val other = remember(conversation) { ChatDepth.direct?.record(conversation)?.otherUser.orEmpty() }
        val current = number
        if (current == null) {
            FmEmptyState(icon = "lock_outline", title = "No safety number yet", message = "Open the conversation once so this phone learns the other devices.")
            return
        }
        val colors = Fm.colors
        Column(
            Modifier.fillMaxSize().background(colors.bgBase).verticalScroll(rememberScrollState()).padding(FmSpace.x4),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            FmSectionHeader(title = if (other.isEmpty()) "Safety number" else "Safety number with $other")
            Spacer(Modifier.height(FmSpace.x3))
            for (row in DirectMessageFrames.safetyGroups(current.digits).chunked(4)) {
                Text(
                    row.joinToString("  "),
                    style = Fm.text.bodyLarge.copy(fontFamily = FmType.mono, fontSize = 20.sp),
                )
            }
            Spacer(Modifier.height(FmSpace.x4))
            QrImage(current.qr, size = 220.dp, description = "Safety number QR code")
            Spacer(Modifier.height(FmSpace.x3))
            Text(
                DirectTrustText.safetyExplainer(other.ifEmpty { "the other person" }),
                textAlign = TextAlign.Center,
                style = Fm.text.bodySmall.copy(color = colors.textSecondary),
            )
            Spacer(Modifier.height(FmSpace.x4))
            if (verified) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmIcon("verified_user_outlined", size = 18.dp, tint = colors.success)
                    Spacer(Modifier.width(FmSpace.x2))
                    Text("Verified", style = Fm.text.bodyMedium)
                }
            } else {
                FmElevatedButton(onClick = {
                    scope.launch(Dispatchers.IO) { ChatDepth.direct?.markVerified(conversation) }
                }) { Text("Mark as verified") }
            }
            Spacer(Modifier.height(FmSpace.x2))
            FmTextButton(onClick = {
                navigator.push(
                    QrScannerScreen("Scan their safety number") { scanned ->
                        scope.launch {
                            val result = withContext(Dispatchers.IO) {
                                ChatDepth.direct?.verifySafetyCode(conversation, scanned) ?: DirectSafetyScan.NOT_SAFETY_CODE
                            }
                            notice = result.message
                        }
                    },
                )
            }) { Text("Scan their code") }
            notice?.let {
                Spacer(Modifier.height(FmSpace.x2))
                Text(it, textAlign = TextAlign.Center, style = Fm.text.bodySmall.copy(color = colors.textSecondary))
            }
        }
    }
}
