package com.forkmesh.nativeapp.feature.tools.vault

import android.app.Activity
import android.content.ClipData
import android.content.ClipDescription
import android.content.ClipboardManager
import android.content.Context
import android.content.ContextWrapper
import android.os.Build
import android.os.PersistableBundle
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFilledButton
import com.forkmesh.nativeapp.feature.account.AccountOutlinedButton
import com.forkmesh.nativeapp.feature.account.openExternalUrl
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCheckbox
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.vault.VaultCrypto
import com.forkmesh.nativeapp.vault.VaultFields
import com.forkmesh.nativeapp.vault.VaultItem
import com.forkmesh.nativeapp.vault.VaultRecipient
import com.forkmesh.nativeapp.vault.VaultScreenLogic
import com.forkmesh.nativeapp.vault.VaultShare
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

internal object VaultClipboard {
    private var clearJob: Job? = null

    fun copy(context: Context, text: String, expires: Boolean = true) {
        val clipboard = context.getSystemService(ClipboardManager::class.java) ?: return
        val clip = ClipData.newPlainText("ForkMesh Vault", text)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            clip.description.extras = PersistableBundle().apply { putBoolean(ClipDescription.EXTRA_IS_SENSITIVE, true) }
        }
        clipboard.setPrimaryClip(clip)
        clearJob?.cancel()
        if (!expires) return
        clearJob = AppLifecycle.scope.launch {
            delay(VaultScreenLogic.CLIPBOARD_CLEAR_MS)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) clipboard.clearPrimaryClip() else clipboard.setPrimaryClip(ClipData.newPlainText("", ""))
        }
    }
}

internal fun Context.findActivity(): Activity? {
    var current: Context? = this
    while (current is ContextWrapper) {
        if (current is Activity) return current
        current = current.baseContext
    }
    return null
}

private class VaultRunner(private val scope: CoroutineScope) {
    var busy by mutableStateOf(false)
    var error by mutableStateOf("")
    var notice by mutableStateOf("")

    fun run(notice: String = "", work: suspend () -> Unit, after: suspend () -> Unit = {}) {
        busy = true
        error = ""
        this.notice = ""
        scope.launch {
            try {
                work()
                this@VaultRunner.notice = notice
                after()
            } catch (thrown: Exception) {
                error = VaultScreenLogic.errorText(thrown)
            } finally {
                busy = false
            }
        }
    }
}

@Composable
private fun Banner(error: String, notice: String) {
    val c = Fm.colors
    if (error.isNotEmpty()) {
        Text(
            error,
            modifier = Modifier.fillMaxWidth().background(c.danger.copy(alpha = .12f)).padding(FmSpace.x3),
            style = Fm.text.bodySmall.copy(color = c.textPrimary),
        )
    } else if (notice.isNotEmpty()) {
        Text(
            notice,
            modifier = Modifier.fillMaxWidth().background(c.success.copy(alpha = .12f)).padding(FmSpace.x3),
            style = Fm.text.bodySmall.copy(color = c.textPrimary),
        )
    }
}

@Composable
private fun SecretField(value: String, onChange: (String) -> Unit, label: String, reveal: Boolean = false, readOnly: Boolean = false, trailing: (@Composable () -> Unit)? = null) {
    Column(Modifier.fillMaxWidth()) {
        Text(label, style = Fm.text.labelMedium.copy(color = Fm.colors.textSecondary))
        Spacer(Modifier.height(FmSpace.x1))
        FmTextField(
            value = value,
            onValueChange = onChange,
            readOnly = readOnly,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, autoCorrectEnabled = false),
            visualTransformation = if (reveal) VisualTransformation.None else PasswordVisualTransformation(),
            trailing = trailing,
        )
    }
}

@Composable
private fun LabeledField(value: String, onChange: (String) -> Unit, label: String, readOnly: Boolean = false, hint: String? = null, minLines: Int = 1, maxLines: Int = 1, keyboardType: KeyboardType = KeyboardType.Text) {
    Column(Modifier.fillMaxWidth()) {
        Text(label, style = Fm.text.labelMedium.copy(color = Fm.colors.textSecondary))
        Spacer(Modifier.height(FmSpace.x1))
        FmTextField(
            value = value,
            onValueChange = onChange,
            readOnly = readOnly,
            hint = hint,
            minLines = minLines,
            maxLines = maxLines,
            keyboardOptions = KeyboardOptions(keyboardType = keyboardType, autoCorrectEnabled = false),
        )
    }
}

class VaultScreen : Screen() {
    override val title: String get() = "Password Vault"

    private var filter by mutableStateOf("all")
    private var query by mutableStateOf("")
    private var recoveryCode by mutableStateOf<String?>(null)
    private var editing by mutableStateOf<VaultItem?>(null)
    private var creating by mutableStateOf(false)
    private var actionsFor by mutableStateOf<VaultItem?>(null)
    private var deleting by mutableStateOf<VaultItem?>(null)
    private var recovering by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val vault = Services.get<VaultService>() ?: return FmEmptyState("lock_outline", "The vault is unavailable")
        val scope = rememberCoroutineScope()
        val runner = remember { VaultRunner(scope) }
        val context = LocalContext.current
        val phase by vault.phase.collectAsState()
        LaunchedEffect(Unit) { runner.run(work = { vault.refresh() }) }
        Column(
            Modifier
                .fillMaxSize()
                .background(Fm.colors.bgBase)
                .pointerInput(Unit) {
                    awaitPointerEventScope {
                        while (true) {
                            awaitPointerEvent(PointerEventPass.Initial)
                            vault.touch()
                        }
                    }
                },
        ) {
            ToolsAppBar("Password Vault", navigator) {
                if (phase == VaultPhase.Unlocked) {
                    FmIconButton("lock_outline", "Lock", onPressed = { vault.lock() }, color = Fm.colors.textPrimary)
                    FmIconButton("settings_outlined", "Vault settings", onPressed = {
                        navigator.push(VaultSettingsScreen { code -> recoveryCode = code })
                    }, color = Fm.colors.textPrimary)
                    FmIconButton("add", "New login", onPressed = { creating = true }, color = Fm.colors.textPrimary)
                }
            }
            Banner(runner.error, runner.notice)
            Box(Modifier.weight(1f)) {
                when (phase) {
                    VaultPhase.Loading -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                    VaultPhase.SignedOut -> FmEmptyState("lock_outline", "Sign in to open your vault", message = "The vault belongs to your ForkMesh account.")
                    VaultPhase.NotSetUp -> SetupForm(runner.busy) { password ->
                        var code = ""
                        runner.run(work = { code = vault.create(password) }, after = { recoveryCode = code })
                    }
                    VaultPhase.Locked -> UnlockForm(
                        busy = runner.busy,
                        quickUnlock = vault.quickUnlockEnabled.collectAsState().value,
                        onUnlock = { runner.run(work = { vault.unlock(it) }) },
                        onQuickUnlock = { runner.run(work = { vault.unlockWithDevice() }) },
                        onRecover = { recovering = true },
                    )
                    VaultPhase.Unlocked -> ItemList(vault, runner, context)
                }
            }
        }
        recoveryCode?.let { code -> RecoveryCodeDialog(code) { recoveryCode = null } }
        if (recovering) {
            RecoverSheet(onDismiss = { recovering = false }) { code, password ->
                recovering = false
                var renewed = ""
                runner.run(work = { renewed = vault.recover(code, password) }, after = { recoveryCode = renewed })
            }
        }
        if (creating || editing != null) {
            ItemSheet(editing, onDismiss = { creating = false; editing = null }) { fields ->
                val id = editing?.id ?: ""
                creating = false
                editing = null
                runner.run(notice = "Saved, encrypted.", work = { vault.save(fields, id) })
            }
        }
        actionsFor?.let { item ->
            ItemActions(item, onDismiss = { actionsFor = null }) { action ->
                actionsFor = null
                when (action) {
                    "username" -> {
                        VaultClipboard.copy(context, item.username)
                        runner.notice = VaultScreenLogic.copiedNotice("Username")
                    }
                    "password" -> {
                        VaultClipboard.copy(context, item.password)
                        runner.notice = VaultScreenLogic.copiedNotice("Password")
                    }
                    "open" -> openExternalUrl(context, VaultCrypto.loginHref(item.url))
                    "edit" -> editing = item
                    "share" -> navigator.push(VaultShareScreen(item.id))
                    "delete" -> deleting = item
                }
            }
        }
        deleting?.let { item ->
            FmConfirmDialog(
                title = if (item.owned) "Delete this login?" else "Remove from your vault?",
                body = if (item.owned) "Everyone \"${item.title}\" is shared with loses it too. This cannot be undone." else null,
                confirmLabel = if (item.owned) "Delete" else "Remove",
                destructive = true,
                onConfirm = {
                    deleting = null
                    runner.run(work = { vault.remove(item.id) })
                },
                onDismissRequest = { deleting = null },
            )
        }
    }

    @Composable
    private fun ItemList(vault: VaultService, runner: VaultRunner, context: Context) {
        val all by vault.items.collectAsState()
        val shown = VaultScreenLogic.filter(all, filter, query)
        Column(Modifier.fillMaxSize()) {
            Column(Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x3, bottom = FmSpace.x2)) {
                FmTextField(
                    value = query,
                    onValueChange = { query = it },
                    hint = "Search titles, usernames, and sites",
                    leading = { FmIcon("search", size = 20.dp, tint = Fm.colors.textTertiary) },
                )
                Spacer(Modifier.height(FmSpace.x2))
                val filters = listOf("all" to "All", "mine" to "Mine", "shared" to "Shared with me")
                FmSegmentTabBar(
                    tabs = filters.map { FmSegmentTab(it.second) },
                    index = filters.indexOfFirst { it.first == filter }.coerceAtLeast(0),
                    onChanged = { filter = filters[it].first },
                )
            }
            if (shown.isEmpty()) {
                FmEmptyState(
                    "key_outlined",
                    "Nothing here yet",
                    message = "Add a login with the + button. Turn on autofill in Vault settings to fill them in other apps.",
                )
            } else {
                LazyColumn(Modifier.fillMaxSize()) {
                    items(shown, key = { it.id }) { item ->
                        if (!item.readable) {
                            FmListRow(
                                title = "Unreadable item",
                                subtitle = "This item could not be opened with your keys (${item.error.replace('_', ' ')}).",
                                leading = { FmIcon("error_outline", tint = Fm.colors.danger) },
                                showChevron = false,
                                onLongPress = { deleting = item },
                            )
                        } else {
                            val detail = listOf(item.username, item.host, if (item.owned) "" else "from ${item.owner}").filter { it.isNotEmpty() }.joinToString(" · ")
                            FmListRow(
                                title = item.title,
                                subtitle = detail.ifEmpty { null },
                                leading = { FmIcon(if (item.owned) "key_outlined" else "people_outline", tint = Fm.colors.textSecondary) },
                                trailing = {
                                    FmIconButton("copy_outlined", "Copy password", onPressed = {
                                        VaultClipboard.copy(context, item.password)
                                        runner.notice = VaultScreenLogic.copiedNotice("Password")
                                    })
                                },
                                showChevron = false,
                                onTap = { actionsFor = item },
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun FormColumn(content: @Composable ColumnScope.() -> Unit) {
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).imePadding().padding(FmSpace.x4),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
        content = content,
    )
}

@Composable
private fun SetupForm(busy: Boolean, onSubmit: (String) -> Unit) {
    var password by remember { mutableStateOf("") }
    var confirm by remember { mutableStateOf("") }
    var problem by remember { mutableStateOf<String?>(null) }
    FormColumn {
        Text("Create your vault", style = Fm.text.titleMedium)
        Text(
            "Choose a master password. It never leaves this phone and ForkMesh cannot reset it, so it must be one you will not forget and do not use anywhere else. Four or more unrelated words work well.",
            style = Fm.text.bodySmall,
        )
        SecretField(password, { password = it }, "Master password")
        SecretField(confirm, { confirm = it }, "Type it again")
        problem?.let { Text(it, style = Fm.text.bodySmall.copy(color = Fm.colors.danger)) }
        AccountFilledButton("Create vault", "lock_outline", enabled = !busy, onClick = {
            problem = VaultScreenLogic.newPasswordProblem(password, confirm)
            if (problem == null) onSubmit(password)
        })
    }
}

@Composable
private fun UnlockForm(busy: Boolean, quickUnlock: Boolean, onUnlock: (String) -> Unit, onQuickUnlock: () -> Unit, onRecover: () -> Unit) {
    var password by remember { mutableStateOf("") }
    FormColumn {
        Text("Unlock your vault", style = Fm.text.titleMedium)
        Text("Stretching the master password takes a few seconds on purpose: it is what makes guessing it expensive.", style = Fm.text.bodySmall)
        SecretField(password, { password = it }, "Master password")
        AccountFilledButton(if (busy) "Unlocking…" else "Unlock", "lock_open", enabled = !busy && password.isNotEmpty(), onClick = {
            val entered = password
            password = ""
            onUnlock(entered)
        })
        if (quickUnlock) AccountOutlinedButton("Unlock with this device", if (busy) null else onQuickUnlock, icon = "fingerprint")
        FmTextButton(onClick = onRecover, enabled = !busy) { Text("Forgot it? Use your recovery code") }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun VaultUnlockDialog(vault: VaultService, purpose: String, onUnlocked: () -> Unit, onCancel: () -> Unit) {
    var password by remember { mutableStateOf("") }
    var problem by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    val quickUnlock by vault.quickUnlockEnabled.collectAsState()
    val scope = rememberCoroutineScope()
    fun attempt(work: suspend () -> Boolean) {
        busy = true
        problem = null
        scope.launch {
            try {
                if (work()) onUnlocked()
            } catch (thrown: Exception) {
                problem = VaultScreenLogic.errorText(thrown)
            } finally {
                busy = false
            }
        }
    }
    FmModalBottomSheet(onDismissRequest = { if (!busy) onCancel() }, isScrollControlled = true) {
        Column(Modifier.imePadding().padding(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
            Text("Unlock Password Vault", style = Fm.text.titleLarge)
            Text("$purpose Type your master password to unlock it.", style = Fm.text.bodySmall)
            SecretField(password, { password = it }, "Master password")
            problem?.let { Text(it, style = Fm.text.bodySmall.copy(color = Fm.colors.danger)) }
            AccountFilledButton(if (busy) "Unlocking…" else "Unlock", "lock_open", enabled = !busy && password.isNotEmpty(), onClick = {
                val entered = password
                password = ""
                attempt {
                    vault.unlock(entered)
                    true
                }
            })
            if (quickUnlock) AccountOutlinedButton("Unlock with this device", if (busy) null else ({ attempt { vault.unlockWithDevice() } }), icon = "fingerprint")
            FmTextButton(onClick = onCancel, enabled = !busy) { Text("Cancel") }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun RecoverSheet(onDismiss: () -> Unit, onSubmit: (String, String) -> Unit) {
    var code by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var confirm by remember { mutableStateOf("") }
    var problem by remember { mutableStateOf<String?>(null) }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding().padding(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
            Text("Recover your vault", style = Fm.text.titleLarge)
            Text(
                "The recovery code opens the vault once. You set a new master password and get a new code; the old one stops working.",
                style = Fm.text.bodySmall,
            )
            LabeledField(code, { code = it }, "Recovery code", minLines = 2, maxLines = 3)
            SecretField(password, { password = it }, "New master password")
            SecretField(confirm, { confirm = it }, "Type it again")
            problem?.let { Text(it, style = Fm.text.bodySmall.copy(color = Fm.colors.danger)) }
            AccountFilledButton("Recover", "restore", onClick = {
                problem = VaultScreenLogic.newPasswordProblem(password, confirm)
                if (problem == null) onSubmit(code, password)
            })
        }
    }
}

@Composable
private fun RecoveryCodeDialog(code: String, onDone: () -> Unit) {
    val context = LocalContext.current
    var saved by remember { mutableStateOf(false) }
    FmAlertDialog(
        onDismissRequest = {},
        title = "Your recovery code",
        actions = {
            FmTextButton(onClick = { VaultClipboard.copy(context, code) }) { Text("Copy") }
            FmTextButton(onClick = onDone, enabled = saved) { Text("Done") }
        },
    ) {
        Text(
            "This is the only way back into the vault if the master password is forgotten. It is shown once. Write it down and keep it somewhere safe, away from this phone. Do not store it in the vault.",
            style = Fm.text.bodyMedium,
        )
        Spacer(Modifier.height(FmSpace.x3))
        SelectionContainer { Text(code, style = Fm.mono(size = Fm.text.bodyLarge.fontSize)) }
        Spacer(Modifier.height(FmSpace.x2))
        Row(verticalAlignment = Alignment.CenterVertically) {
            FmCheckbox(checked = saved, onCheckedChange = { saved = it })
            Text("I have saved this code.", style = Fm.text.bodyMedium)
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ItemActions(item: VaultItem, onDismiss: () -> Unit, onAction: (String) -> Unit) {
    FmModalBottomSheet(onDismissRequest = onDismiss) {
        Column(Modifier.padding(bottom = FmSpace.x4)) {
            Text(item.title, modifier = Modifier.padding(FmSpace.x4), style = Fm.text.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
            @Composable
            fun action(key: String, icon: String, label: String, danger: Boolean = false) {
                FmListRow(
                    title = label,
                    titleColor = if (danger) Fm.colors.danger else null,
                    leading = { FmIcon(icon, tint = if (danger) Fm.colors.danger else Fm.colors.textSecondary) },
                    showChevron = false,
                    onTap = { onAction(key) },
                )
            }
            if (item.username.isNotEmpty()) action("username", "person_outline", "Copy username")
            action("password", "copy_outlined", "Copy password")
            if (VaultCrypto.loginHref(item.url).isNotEmpty()) action("open", "open_in_new", "Open site")
            action("edit", if (item.canEdit) "edit_outlined" else "visibility_outlined", if (item.canEdit) "Edit" else "View")
            if (item.owned) action("share", "ios_share", "Share")
            action("delete", "delete_outline", if (item.owned) "Delete" else "Remove from my vault", danger = true)
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ItemSheet(item: VaultItem?, onDismiss: () -> Unit, onSave: (Map<String, Any?>) -> Unit) {
    val readOnly = item != null && !item.canEdit
    var title by remember { mutableStateOf(item?.title ?: "") }
    var url by remember { mutableStateOf(item?.url ?: "") }
    var username by remember { mutableStateOf(item?.username ?: "") }
    var password by remember { mutableStateOf(item?.password ?: "") }
    var notes by remember { mutableStateOf(item?.notes ?: "") }
    var reveal by remember { mutableStateOf(false) }
    fun cut(text: String, name: String) = text.take(VaultFields.limit(name))
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(
            Modifier.verticalScroll(rememberScrollState()).imePadding().padding(FmSpace.x4),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
        ) {
            Text(if (item == null) "New login" else if (readOnly) "Login" else "Edit login", style = Fm.text.titleLarge)
            if (item != null && !item.owned) Text(VaultScreenLogic.sharedBy(item), style = Fm.text.bodySmall)
            LabeledField(title, { title = cut(it, "title") }, "Title", readOnly)
            LabeledField(url, { url = cut(it, "url") }, "Website", readOnly, hint = "https://", keyboardType = KeyboardType.Uri)
            LabeledField(username, { username = cut(it, "username") }, "Username", readOnly, keyboardType = KeyboardType.Email)
            SecretField(password, { password = cut(it, "password") }, "Password", reveal = reveal, readOnly = readOnly, trailing = {
                FmIconButton(if (reveal) "visibility_off_outlined" else "visibility_outlined", if (reveal) "Hide" else "Show", onPressed = { reveal = !reveal })
            })
            if (!readOnly) {
                FmTextButton(onClick = {
                    password = VaultCrypto.generatePassword()
                    reveal = true
                }) {
                    FmIcon("auto_awesome", size = 18.dp, tint = Fm.colors.accent)
                    Spacer(Modifier.width(FmSpace.x1))
                    Text("Generate a password")
                }
            }
            LabeledField(notes, { notes = cut(it, "notes") }, "Notes", readOnly, minLines = 3, maxLines = 6)
            if (!readOnly) {
                AccountFilledButton("Save", "check", modifier = Modifier.fillMaxWidth(), onClick = {
                    onSave(linkedMapOf("title" to title, "username" to username, "password" to password, "url" to url, "notes" to notes))
                })
            }
        }
    }
}

class VaultShareScreen(private val itemId: String) : Screen() {
    override val title: String get() = "Share"

    private var kind by mutableStateOf("user")
    private var target by mutableStateOf("")
    private var permission by mutableStateOf("read")
    private var organizations by mutableStateOf<List<String>>(emptyList())
    private var preview by mutableStateOf<List<VaultRecipient>?>(null)
    private var accepted by mutableStateOf<List<String>>(emptyList())
    private var stopping by mutableStateOf<VaultShare?>(null)

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val vault = Services.get<VaultService>() ?: return
        val scope = rememberCoroutineScope()
        val runner = remember { VaultRunner(scope) }
        vault.items.collectAsState().value
        val item = vault.find(itemId)
        LaunchedEffect(Unit) {
            organizations = try {
                vault.organizations()
            } catch (_: Exception) {
                emptyList()
            }
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar(if (item == null) "Share" else "Share ${item.title}", navigator)
            if (item == null) {
                FmEmptyState("lock_outline", "The vault is locked")
                return@Column
            }
            FormColumn {
                Text(
                    "The login's key is sealed to each person's own public keys (X25519 and post-quantum ML-KEM-768). ForkMesh passes the sealed key along and cannot open it.",
                    style = Fm.text.bodySmall,
                )
                FmSectionHeader("Shared with")
                if (item.shares.isEmpty()) Text("Not shared with anyone.", style = Fm.text.bodyMedium)
                for (share in item.shares) {
                    FmListRow(
                        title = "${if (share.kind == "organization") "Organization " else ""}${share.name}",
                        subtitle = if (share.permission == "write") "Can view and edit" else "Can view and use",
                        trailing = { FmTextButton(onClick = { stopping = share }, contentColor = Fm.colors.danger) { Text("Stop sharing") } },
                        showChevron = false,
                    )
                }
                FmSectionHeader("Share with")
                FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    FmToneButton("A person", onPressed = { kind = "user"; preview = null }, tone = if (kind == "user") FmTone.Accent else FmTone.Neutral, filled = kind == "user")
                    FmToneButton(
                        "An organization",
                        onPressed = if (organizations.isEmpty()) null else ({ kind = "organization"; preview = null; target = organizations.first() }),
                        tone = if (kind == "organization") FmTone.Accent else FmTone.Neutral,
                        filled = kind == "organization",
                    )
                }
                if (kind == "user") {
                    LabeledField(target, { target = it; preview = null }, "Username")
                } else {
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                        for (org in organizations) {
                            FmToneButton(org, onPressed = { target = org; preview = null }, tone = if (target == org) FmTone.Accent else FmTone.Neutral, filled = target == org)
                        }
                    }
                }
                FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    FmToneButton("View and use", onPressed = { permission = "read" }, tone = if (permission == "read") FmTone.Accent else FmTone.Neutral, filled = permission == "read")
                    FmToneButton("View and edit", onPressed = { permission = "write" }, tone = if (permission == "write") FmTone.Accent else FmTone.Neutral, filled = permission == "write")
                }
                AccountOutlinedButton("Check keys", if (runner.busy || target.isBlank()) null else ({
                    runner.run(work = { preview = vault.previewShare(kind, target) })
                }))
                preview?.let { members ->
                    if (members.isEmpty()) Text("Nobody else is in this organization yet.", style = Fm.text.bodyMedium)
                    for (member in members) {
                        val label = when (member.state) {
                            "pinned" -> "Keys match what you pinned"
                            "new" -> "First share"
                            "changed" -> "Keys changed"
                            "no_vault" -> "No vault yet"
                            else -> "Keys could not be verified"
                        }
                        FmListRow(
                            title = member.name,
                            subtitleContent = {
                                Column {
                                    Text(label, style = Fm.text.bodySmall)
                                    if (member.fingerprint.isNotEmpty()) Text(member.fingerprint, style = Fm.mono(color = Fm.colors.textTertiary))
                                }
                            },
                            trailing = if (member.state == "changed") {
                                {
                                    FmSwitch(checked = member.name in accepted, onCheckedChange = { on ->
                                        accepted = if (on) accepted + member.name else accepted - member.name
                                    })
                                }
                            } else {
                                null
                            },
                            showChevron = false,
                        )
                    }
                    val changed = members.filter { it.state == "changed" }.map { it.name }
                    val advice = VaultScreenLogic.shareAdvice(kind, changed, members)
                    if (advice.isNotEmpty()) Text(advice, style = Fm.text.bodySmall)
                    AccountFilledButton("Share", "ios_share", enabled = !runner.busy, onClick = {
                        val name = target
                        runner.run(work = {
                            val outcome = vault.share(item.id, kind, name, permission, accepted)
                            runner.notice = VaultScreenLogic.shareOutcome(name, outcome)
                            preview = null
                        })
                    })
                }
                Banner(runner.error, runner.notice)
            }
        }
        stopping?.let { share ->
            FmConfirmDialog(
                title = "Stop sharing?",
                body = "The login is re-encrypted under a new key. If ${share.name} already saw the password, change it on the website too.",
                confirmLabel = "Stop sharing",
                destructive = true,
                onConfirm = {
                    stopping = null
                    runner.run(work = { vault.unshare(itemId, share.kind, share.name) })
                },
                onDismissRequest = { stopping = null },
            )
        }
    }
}

class VaultSettingsScreen(private val onRecoveryCode: (String) -> Unit) : Screen() {
    override val title: String get() = "Vault settings"

    private var fingerprint by mutableStateOf("")
    private var password by mutableStateOf("")
    private var confirm by mutableStateOf("")
    private var destroying by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val vault = Services.get<VaultService>() ?: return
        val scope = rememberCoroutineScope()
        val runner = remember { VaultRunner(scope) }
        val context = LocalContext.current
        val autofill by vault.autofillEnabled.collectAsState()
        val status by vault.autofillStatus.collectAsState()
        val autoLock by vault.autoLockMinutes.collectAsState()
        val quick by vault.quickUnlockEnabled.collectAsState()
        val quickAvailable by vault.quickUnlockAvailable.collectAsState()
        val foreground by AppLifecycle.foreground.collectAsState()
        LaunchedEffect(Unit) {
            fingerprint = try {
                vault.myFingerprint()
            } catch (_: Exception) {
                ""
            }
        }
        LaunchedEffect(foreground) { if (foreground) vault.refreshAutofillStatus() }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Vault settings", navigator)
            FormColumn {
                FmSectionHeader("Fill logins in other apps")
                if (!status.supported) {
                    Text("Filling logins into other apps is available on Android 8 and newer. On this device, copy a login from the vault instead.", style = Fm.text.bodySmall)
                } else {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text("Autofill from this vault", style = Fm.text.titleSmall)
                            Text(
                                if (status.deviceSecure) {
                                    "Keeps your logins on this phone, encrypted under a key in its secure hardware that only your fingerprint, face, or screen lock releases. Each fill asks for it."
                                } else {
                                    "Set a screen lock on this phone first: it is what protects the logins autofill keeps here."
                                },
                                style = Fm.text.bodySmall,
                            )
                        }
                        FmSwitch(checked = autofill, enabled = status.deviceSecure, onCheckedChange = { on -> runner.run(work = { vault.setAutofill(on) }) })
                    }
                    if (autofill) {
                        Text(
                            if (status.enabled) VaultScreenLogic.autofillSummary(status.logins) else "One more step: choose ForkMesh as the autofill provider in system settings.",
                            style = Fm.text.bodySmall,
                        )
                        if (!status.enabled) AccountOutlinedButton("Open autofill settings", { vault.openAutofillSettings(context.findActivity()) })
                    }
                }
                FmSectionHeader("Locking")
                Text("Lock after", style = Fm.text.labelMedium)
                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    for (minutes in VaultService.AUTO_LOCK_CHOICES) {
                        FmToneButton(
                            "$minutes minute${if (minutes == 1) "" else "s"}",
                            onPressed = { runner.run(work = { vault.setAutoLockMinutes(minutes) }) },
                            tone = if (autoLock == minutes) FmTone.Accent else FmTone.Neutral,
                            filled = autoLock == minutes,
                        )
                    }
                }
                Text("The vault also locks after a minute in the background and when you sign out.", style = Fm.text.bodySmall)
                if (quickAvailable) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text("Unlock with this device", style = Fm.text.titleSmall)
                            Text(
                                "Keeps your vault keys in this phone's secure storage and opens them with your fingerprint, face, or screen lock instead of the master password. Anyone who can unlock this phone can then open the vault.",
                                style = Fm.text.bodySmall,
                            )
                        }
                        FmSwitch(checked = quick, onCheckedChange = { on -> runner.run(work = { vault.setQuickUnlock(on) }) })
                    }
                }
                FmSectionHeader("Your key fingerprint")
                SelectionContainer { Text(fingerprint, style = Fm.mono()) }
                Text("Someone sharing with you for the first time can read this back to you to be sure the keys they were given are yours.", style = Fm.text.bodySmall)
                FmSectionHeader("Master password")
                SecretField(password, { password = it }, "New master password")
                SecretField(confirm, { confirm = it }, "Type it again")
                AccountOutlinedButton("Change master password", {
                    val problem = VaultScreenLogic.newPasswordProblem(password, confirm)
                    if (problem != null) {
                        runner.error = problem
                    } else {
                        val next = password
                        password = ""
                        confirm = ""
                        runner.run(
                            notice = "Master password changed. Other devices need the new one at their next unlock.",
                            work = { vault.changePassword(next) },
                        )
                    }
                })
                AccountOutlinedButton("New recovery code", {
                    var code = ""
                    runner.run(work = { code = vault.renewRecoveryCode() }, after = {
                        onRecoveryCode(code)
                        navigator.pop()
                    })
                })
                AccountOutlinedButton("Copy encrypted export", {
                    runner.run(notice = "Encrypted export copied. It opens only with your master password or recovery code.", work = {
                        val payload = vault.exportData()
                        VaultClipboard.copy(context, Json.stringify(payload["export"]), expires = false)
                    })
                })
                Banner(runner.error, runner.notice)
                FmTextButton(onClick = { destroying = true }, contentColor = Fm.colors.danger) { Text("Delete vault") }
            }
        }
        if (destroying) {
            FmConfirmDialog(
                title = "Delete your vault?",
                body = "Every login in it is erased here and for everyone you shared it with. The encrypted export is copied to the clipboard first; it opens only with your master password or recovery code.",
                confirmLabel = "Delete",
                destructive = true,
                onConfirm = {
                    destroying = false
                    runner.run(work = {
                        val payload = vault.destroy()
                        VaultClipboard.copy(context, Json.stringify(payload["export"]), expires = false)
                    }, after = { navigator.pop() })
                },
                onDismissRequest = { destroying = false },
            )
        }
    }
}
