package com.forkmesh.nativeapp.feature.openwrt

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.tools.vault.VaultPhase
import com.forkmesh.nativeapp.feature.tools.vault.VaultService
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.openwrt.OpenWrtApplyOutcome
import com.forkmesh.nativeapp.openwrt.OpenWrtClient
import com.forkmesh.nativeapp.openwrt.OpenWrtDisk
import com.forkmesh.nativeapp.openwrt.OpenWrtModel
import com.forkmesh.nativeapp.openwrt.OpenWrtResult
import com.forkmesh.nativeapp.openwrt.OpenWrtSnapshot
import com.forkmesh.nativeapp.openwrt.RouterHttp
import com.forkmesh.nativeapp.openwrt.UciEdit
import com.forkmesh.nativeapp.vault.VaultItem
import com.forkmesh.nativeapp.vault.VaultLoginAnswer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

data class OpenWrtVaultPrompt(val purpose: String, val action: Action) {
    sealed class Action {
        data class Save(val title: String, val url: String, val username: String, val password: String, val done: String) : Action()
        data class Fill(val url: String) : Action()
    }
}

class OpenWrtState {
    var address by mutableStateOf("")
    var user by mutableStateOf("root")
    var password by mutableStateOf("")
    var signedIn by mutableStateOf(false)
    var busy by mutableStateOf(false)
    var message by mutableStateOf("")
    var messageIsError by mutableStateOf(false)
    var pendingCertificate by mutableStateOf("")
    var rollback by mutableStateOf(true)
    var snapshot by mutableStateOf(OpenWrtSnapshot())
    var packageOutput by mutableStateOf("")
    var storageLog by mutableStateOf("")
    var storageBusy by mutableStateOf(false)
    var routerHost by mutableStateOf("")
    var saveToVault by mutableStateOf(true)
        private set
    var vaultPrompt by mutableStateOf<OpenWrtVaultPrompt?>(null)
    var vaultChoices by mutableStateOf<List<VaultItem>>(emptyList())

    private var client: OpenWrtClient? = null
    private var loginUrl = ""
    private var loginUser = ""
    private var filledFromVault = ""
    private var certificateKey = ""
    private var prefs: SharedPreferences? = null

    fun attach(context: Context) {
        if (prefs != null) return
        val store = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        prefs = store
        address = store.getString("address", "").orEmpty()
        user = store.getString("user", "root").orEmpty().ifEmpty { "root" }
        saveToVault = store.getBoolean("saveLogin", true)
    }

    val vault: VaultService? get() = Services.get<VaultService>()

    private val vaultOffered: Boolean get() = vault?.let { it.phase.value != VaultPhase.SignedOut } ?: false

    fun changeSaveToVault(on: Boolean) {
        saveToVault = on
        prefs?.edit()?.putBoolean("saveLogin", on)?.apply()
    }

    val addressNote: String
        get() {
            val parsed = OpenWrtModel.parseAddress(address.ifBlank { "192.168.1.1" })
            return when {
                !parsed.valid -> parsed.error
                parsed.encrypted -> "HTTPS: the first time, you are asked to check the router's certificate fingerprint."
                else -> "Plain HTTP: the password crosses your Wi-Fi unencrypted. Install luci-ssl on the router and use https:// to encrypt it."
            }
        }

    fun show(text: String, error: Boolean = false) {
        message = text
        messageIsError = error
    }

    fun report(result: OpenWrtResult, success: String) {
        if (result.ok) show(success) else show(result.error, true)
    }

    suspend fun connect() {
        var fromVault = false
        var saved = ""
        val parsed = OpenWrtModel.parseAddress(address.ifBlank { "192.168.1.1" })
        if (!parsed.valid) return show(parsed.error, true)
        busy = true
        show("Connecting to ${parsed.host}…")
        try {
            var base = parsed.base
            if (!OpenWrtModel.isLiteralAddress(parsed.host)) {
                val addresses = withContext(Dispatchers.IO) { OpenWrtClient.resolve(parsed.host) }
                if (addresses.isEmpty()) return show("${parsed.host} could not be found on your network.", true)
                if (!addresses.all(OpenWrtModel::isLocalHost)) {
                    return show("${parsed.host} is not on your local network. Only routers on your own network can be managed, so the router password never crosses the internet.", true)
                }
                base = OpenWrtModel.baseWithHost(parsed, addresses.firstOrNull { OpenWrtModel.ipv4Bytes(it) != null } ?: addresses.first())
            }
            val port = if (parsed.port > 0) parsed.port else if (parsed.encrypted) 443 else 80
            certificateKey = "certificates/${parsed.host}:$port"
            val router = OpenWrtClient(RouterHttp(prefs?.getString(certificateKey, "").orEmpty()), base)
            val account = user.trim().ifEmpty { "root" }
            val typed = password
            val result = withContext(Dispatchers.IO) { router.signIn(account, typed) }
            if (!result.ok) {
                pendingCertificate = result.certificate
                return show(result.error, true)
            }
            client = router
            pendingCertificate = ""
            password = ""
            routerHost = parsed.host
            prefs?.edit()?.putString("address", address)?.putString("user", account)?.apply()
            signedIn = true
            loginUrl = parsed.base
            loginUser = account
            fromVault = filledFromVault.isNotEmpty() && typed == filledFromVault
            filledFromVault = ""
            saved = typed
            show("Connected to ${parsed.host}.")
        } finally {
            busy = false
        }
        refresh()
        if (signedIn && saved.isNotEmpty() && !fromVault && saveToVault && vaultOffered) saveInVault(saved, "Connected to $routerHost.")
    }

    suspend fun saveInVault(secret: String, done: String) {
        if (loginUrl.isEmpty() || secret.isEmpty()) return
        val host = OpenWrtModel.parseAddress(loginUrl).host.ifEmpty { routerHost }
        perform(
            OpenWrtVaultPrompt.Action.Save("OpenWrt router ($host)", loginUrl, loginUser.ifEmpty { "root" }, secret, done),
            "Router asks to save the password for $loginUrl in your Password Vault.",
        )
    }

    suspend fun signInWithVault() {
        val parsed = OpenWrtModel.parseAddress(address.ifBlank { "192.168.1.1" })
        if (!parsed.valid) return show(parsed.error, true)
        perform(OpenWrtVaultPrompt.Action.Fill(parsed.base), "Router asks for the password you saved for ${parsed.base}.")
    }

    suspend fun vaultUnlocked() {
        val prompt = vaultPrompt ?: return
        vaultPrompt = null
        perform(prompt.action, prompt.purpose)
    }

    fun vaultPromptCancelled() {
        val prompt = vaultPrompt ?: return
        vaultPrompt = null
        val action = prompt.action
        if (action is OpenWrtVaultPrompt.Action.Save) show("${action.done} The password was not saved in Password Vault.") else show("")
    }

    suspend fun useVaultLogin(item: VaultItem) {
        vaultChoices = emptyList()
        user = item.username.ifEmpty { "root" }
        password = item.password
        filledFromVault = item.password
        connect()
    }

    private suspend fun perform(action: OpenWrtVaultPrompt.Action, purpose: String) {
        val vault = vault ?: return
        when (action) {
            is OpenWrtVaultPrompt.Action.Save -> {
                val done = action.done
                when (val answer = vault.saveLogin(action.title, action.url, action.username, action.password)) {
                    is VaultLoginAnswer.Saved -> show(if (answer.changed) "$done The password is saved in Password Vault." else "$done The password was already saved in Password Vault.")
                    VaultLoginAnswer.Locked -> vaultPrompt = OpenWrtVaultPrompt(purpose, action)
                    VaultLoginAnswer.NoVault -> show("$done The password was not saved: create a Password Vault first in Tools, Vault.", true)
                    VaultLoginAnswer.SignedOut -> show("$done Sign in to ForkMesh to save the password in Password Vault.", true)
                    is VaultLoginAnswer.Failed -> show("$done The password was not saved in Password Vault: ${answer.text}", true)
                    is VaultLoginAnswer.Found -> Unit
                }
            }
            is OpenWrtVaultPrompt.Action.Fill -> when (val answer = vault.findLogins(action.url)) {
                is VaultLoginAnswer.Found -> choose(answer.logins, action.url)
                VaultLoginAnswer.Locked -> vaultPrompt = OpenWrtVaultPrompt(purpose, action)
                VaultLoginAnswer.NoVault -> show("There is no Password Vault on this account yet. Create one in Tools, Vault.", true)
                VaultLoginAnswer.SignedOut -> show("Sign in to ForkMesh to use Password Vault.", true)
                is VaultLoginAnswer.Failed -> show(answer.text, true)
                is VaultLoginAnswer.Saved -> Unit
            }
        }
    }

    private suspend fun choose(logins: List<VaultItem>, url: String) {
        if (logins.isEmpty()) return show("Password Vault has no password for $url yet. Type it once with “Save the password in Password Vault” on.", true)
        val typed = user.trim().ifEmpty { "root" }
        val same = logins.firstOrNull { it.username == typed }
        when {
            same != null -> useVaultLogin(same)
            logins.size == 1 -> useVaultLogin(logins.first())
            else -> vaultChoices = logins
        }
    }

    suspend fun trustCertificate() {
        if (pendingCertificate.isEmpty() || certificateKey.isEmpty()) return
        prefs?.edit()?.putString(certificateKey, pendingCertificate)?.apply()
        connect()
    }

    suspend fun signOut() {
        val current = client
        if (current != null) withContext(Dispatchers.IO) { current.signOut() }
        dropSession("")
    }

    private fun dropSession(text: String) {
        client = null
        signedIn = false
        snapshot = OpenWrtSnapshot()
        packageOutput = ""
        storageLog = ""
        storageBusy = false
        routerHost = ""
        loginUrl = ""
        loginUser = ""
        show(text)
    }

    suspend fun refresh() {
        val current = client ?: return
        busy = true
        val before = current.renewals
        val tool = snapshot.packageTool
        val fresh = withContext(Dispatchers.IO) { current.loadSnapshot(preferredTool = tool) }
        busy = false
        if (current.renewals > before && snapshot.pendingCount > 0) {
            show("The router session timed out, so the router dropped the changes you had not applied. ForkMesh signed in again.", true)
        } else if (fresh.problems.isNotEmpty()) {
            show(fresh.problems.first(), true)
        }
        snapshot = fresh
    }

    suspend fun reloadConfig(config: String) {
        val current = client ?: return
        val loaded = withContext(Dispatchers.IO) { current.call("uci", "get", mapOf("config" to config)) to current.changes() }
        snapshot = snapshot.copy(
            configs = if (loaded.first.ok) snapshot.configs + (config to loaded.first.data.obj("values")) else snapshot.configs,
            missingConfigs = if (loaded.first.ok) snapshot.missingConfigs - config else snapshot.missingConfigs,
            changes = loaded.second,
        )
    }

    suspend fun stage(config: String, section: String, edit: UciEdit, what: String): Boolean {
        val current = client ?: return false
        val result = withContext(Dispatchers.IO) { current.stage(config, section, edit) }
        reloadConfig(config)
        report(result, "$what saved. Apply to make it take effect.")
        return result.ok
    }

    suspend fun add(config: String, type: String, values: Map<String, Any?>, what: String, name: String = ""): Boolean {
        val current = client ?: return false
        val result = withContext(Dispatchers.IO) { current.addSection(config, type, name, values) }
        reloadConfig(config)
        report(result, "$what added. Apply to make it take effect.")
        return result.ok
    }

    suspend fun delete(config: String, section: String, what: String) {
        val current = client ?: return
        val result = withContext(Dispatchers.IO) { current.deleteSection(config, section) }
        reloadConfig(config)
        report(result, "$what deleted. Apply to make it take effect.")
    }

    suspend fun apply() {
        val current = client ?: return
        busy = true
        show("Applying changes…")
        val wanted = rollback
        val outcome = withContext(Dispatchers.IO) { current.apply(wanted) { seconds -> Thread.sleep((seconds * 1000).toLong()) } }
        busy = false
        show(outcome.message, outcome.isError)
        if (outcome == OpenWrtApplyOutcome.Applied) delay(3_000)
        refresh()
    }

    suspend fun discard() {
        val current = client ?: return
        val changes = snapshot.changes
        report(withContext(Dispatchers.IO) { current.discard(changes) }, "Changes discarded.")
        refresh()
    }

    suspend fun restartInterface(name: String) {
        val current = client ?: return
        val result = withContext(Dispatchers.IO) {
            val down = current.call("network.interface", "down", mapOf("interface" to name))
            if (down.ok) current.call("network.interface", "up", mapOf("interface" to name)) else down
        }
        report(result, "Interface $name restarted.")
        delay(3_000)
        refresh()
    }

    suspend fun serviceAction(name: String, action: String) {
        val current = client ?: return
        val result = withContext(Dispatchers.IO) { current.call("rc", "init", mapOf("name" to name, "action" to action), 60_000) }
        report(result, "$name: $action done.")
        val services = withContext(Dispatchers.IO) { OpenWrtModel.parseServices(current.call("rc", "list").data) }
        snapshot = snapshot.copy(services = services)
    }

    suspend fun packageAction(action: String, names: List<String>) {
        val current = client ?: return
        val tool = snapshot.packageTool
            ?: return show("The router does not let ForkMesh run its package manager. Install luci-app-package-manager (or luci-app-opkg) on the router.", true)
        busy = true
        packageOutput = "Running ${(listOf(tool.program) + OpenWrtModel.packageArguments(tool, action, names)).joinToString(" ")}…"
        val result = withContext(Dispatchers.IO) { current.packageAction(tool, action, names) }
        busy = false
        val output = listOf(result.data.str("stdout"), result.data.str("stderr"), if (result.ok) "" else result.error).map { it.trim() }.filter { it.isNotEmpty() }
        packageOutput = if (output.isEmpty()) "Done." else output.joinToString("\n")
        val subject = names.joinToString(", ")
        report(result, when (action) {
            "update" -> "Package lists updated."
            "install" -> "$subject installed."
            else -> "$subject removed."
        })
        if (action != "update") {
            val listed = withContext(Dispatchers.IO) { current.listPackages(tool) }
            snapshot = snapshot.copy(packages = listed.second)
        }
        if (action == "install" && result.ok) refresh()
    }

    suspend fun unmount(disk: OpenWrtDisk) {
        val current = client ?: return
        report(withContext(Dispatchers.IO) { current.exec("/bin/umount", listOf(disk.mount), 60_000) }, "${disk.mount} unmounted. You can unplug the disk now.")
        refresh()
    }

    suspend fun mountAll() {
        val current = client ?: return
        if (snapshot.changes.containsKey("fstab")) show("Apply your storage changes first: the router mounts what is saved, not what is waiting.", true)
        report(withContext(Dispatchers.IO) { current.exec("/sbin/block", listOf("mount"), 60_000) }, "The router mounted its disks.")
        refresh()
    }

    suspend fun setAutoMount(on: Boolean) {
        val current = client ?: return
        if (snapshot.fstabMissing) {
            if (!on || storageBusy) return
            storageBusy = true
            show("The router has no storage settings file (/etc/config/fstab) yet. Creating it…")
            val created = withContext(Dispatchers.IO) { current.createFstab() }
            storageBusy = false
            if (!created.ok) {
                show(created.error, true)
                checkStorage()
                return
            }
            appendStorageLog("Created /etc/config/fstab on the router.")
            val fstab = withContext(Dispatchers.IO) { current.call("uci", "get", mapOf("config" to "fstab")) }
            if (!fstab.ok) return show("ForkMesh created /etc/config/fstab, but the router cannot read it: ${fstab.error}", true)
            snapshot = snapshot.copy(configs = snapshot.configs + ("fstab" to fstab.data.obj("values")), missingConfigs = snapshot.missingConfigs - "fstab")
        }
        val values = mapOf("anon_mount" to if (on) "1" else "0", "auto_mount" to "1")
        val global = snapshot.sections("fstab", "global").firstOrNull()
        val saved = if (global == null) add("fstab", "global", values, "Automatic mounting") else stage("fstab", global.name, UciEdit(values), "Automatic mounting")
        if (saved) {
            show(if (on) "Automatic mounting saved. Apply the change, then press Mount disks now." else "Automatic mounting turned off. Apply to make it take effect.")
        } else {
            show("Could not change automatic mounting: $message", true)
            checkStorage()
        }
    }

    fun appendStorageLog(line: String) {
        storageLog = if (storageLog.isEmpty()) line else "$storageLog\n$line"
    }

    suspend fun checkStorage(keepLog: Boolean = false) {
        val current = client ?: return
        if (storageBusy) return
        storageBusy = true
        try {
            if (keepLog) appendStorageLog("\nChecking USB storage…") else storageLog = "Checking USB storage…"
            val start = snapshot
            val (check, fresh) = withContext(Dispatchers.IO) { current.checkStorage(start) }
            snapshot = fresh
            val report = OpenWrtModel.storageReport(check)
            if (keepLog) appendStorageLog(report) else storageLog = report
        } finally {
            storageBusy = false
        }
    }

    suspend fun installStorageSupport() {
        val current = client ?: return
        if (storageBusy) return
        val tool = snapshot.packageTool
        if (tool == null) {
            show("The router does not let ForkMesh run its package manager, so USB storage support cannot be installed from here. Install luci-app-package-manager (or luci-app-opkg on older releases) on the router.", true)
            checkStorage()
            return
        }
        storageBusy = true
        val failed = mutableListOf<String>()
        try {
            show("Installing USB storage support…")
            storageLog = "Updating the router's package lists…"
            val update = withContext(Dispatchers.IO) { current.packageAction(tool, "update", emptyList()) }
            if (update.ok) {
                appendStorageLog("Package lists updated.")
            } else {
                appendStorageLog("Updating the package lists failed; the router may have no internet connection. Trying to install anyway.")
                OpenWrtModel.outputTail(update.error).forEach { appendStorageLog("  $it") }
            }
            for (name in OpenWrtModel.usbStoragePackages) {
                appendStorageLog("Installing $name…")
                val result = withContext(Dispatchers.IO) { current.packageAction(tool, "install", listOf(name)) }
                if (result.ok) {
                    appendStorageLog("  $name is installed.")
                } else {
                    failed += name
                    appendStorageLog("  $name failed:")
                    val output = (result.data.str("stdout") + "\n" + result.data.str("stderr")).trim()
                    OpenWrtModel.outputTail(output.ifEmpty { result.error }).forEach { appendStorageLog("    $it") }
                }
            }
        } finally {
            storageBusy = false
        }
        refresh()
        if (failed.isEmpty()) show("USB storage support installed.")
        else show("Some storage packages could not be installed: ${failed.joinToString(", ")}. The log below says why.", true)
        checkStorage(keepLog = true)
    }

    suspend fun installNetworkSharing() {
        val current = client ?: return
        if (storageBusy) return
        val tool = snapshot.packageTool
            ?: return show("The router does not let ForkMesh run its package manager. Install luci-app-package-manager (or luci-app-opkg) on the router.", true)
        storageBusy = true
        var failure = ""
        try {
            show("Installing ${OpenWrtModel.networkSharePackages.joinToString(" and ")}…")
            storageLog = "Updating the router's package lists…"
            val update = withContext(Dispatchers.IO) { current.packageAction(tool, "update", emptyList()) }
            if (update.ok) {
                appendStorageLog("Package lists updated.")
            } else {
                appendStorageLog("Updating the package lists failed; the router may have no internet connection. Trying to install anyway.")
                OpenWrtModel.outputTail(update.error).forEach { appendStorageLog("  $it") }
            }
            for (name in OpenWrtModel.networkSharePackages) {
                appendStorageLog("Installing $name…")
                val result = withContext(Dispatchers.IO) { current.packageAction(tool, "install", listOf(name)) }
                if (result.ok) {
                    appendStorageLog("  $name is installed.")
                } else {
                    val output = (result.data.str("stdout") + "\n" + result.data.str("stderr")).trim().ifEmpty { result.error }
                    val tail = OpenWrtModel.outputTail(output)
                    tail.forEach { appendStorageLog("    $it") }
                    if (failure.isEmpty()) failure = "Could not install $name: ${tail.joinToString(" ").ifEmpty { result.error }}"
                }
            }
        } finally {
            storageBusy = false
        }
        refresh()
        if (failure.isEmpty()) show("Network sharing is installed. Share a mounted disk below.") else show(failure, true)
    }

    suspend fun applyAndMount() {
        apply()
        if (snapshot.pendingCount > 0) return
        mountAll()
        checkStorage()
    }

    suspend fun mountAtBoot(disk: OpenWrtDisk, path: String) {
        OpenWrtModel.mountPathProblem(path)?.let { return show(it, true) }
        if (disk.fstabSection.isEmpty()) {
            add("fstab", "mount", OpenWrtModel.mountSectionValues(disk.block, path), "Mount for ${disk.title}")
        } else {
            stage("fstab", disk.fstabSection, UciEdit(mapOf("target" to path, "enabled" to "1")), "Mount for ${disk.title}")
        }
    }

    suspend fun share(disk: OpenWrtDisk, name: String, guests: Boolean) {
        val config = snapshot.shareConfig
        if (config.isEmpty()) return show("Install ksmbd-server and luci-app-ksmbd on the Packages screen to share disks.", true)
        OpenWrtModel.shareNameProblem(name)?.let { return show(it, true) }
        add(config, OpenWrtModel.shareSectionType(config), OpenWrtModel.shareValues(name, disk.mount, guests), "Share $name")
    }

    suspend fun reboot() {
        val current = client ?: return
        val result = withContext(Dispatchers.IO) { current.call("system", "reboot") }
        if (!result.ok && !result.unreachable) return show(result.error, true)
        dropSession("The router is restarting. Connect again in a minute or two.")
    }

    suspend fun changePassword(password: String, confirm: String): Boolean {
        val current = client ?: return false
        OpenWrtModel.passwordProblem(password, confirm)?.let {
            show(it, true)
            return false
        }
        val account = current.user.ifEmpty { "root" }
        val result = withContext(Dispatchers.IO) { current.call("luci", "setPassword", mapOf("username" to account, "password" to password)) }
        if (result.ok) current.rememberPassword(password)
        report(result, "Router password changed.")
        if (result.ok && saveToVault && vaultOffered) saveInVault(password, "Router password changed.")
        return result.ok
    }

    companion object {
        const val PREFS = "market.packages.openwrt"
        val criticalServices = setOf("network", "firewall", "dropbear", "uhttpd", "rpcd", "dnsmasq", "odhcpd", "ubus", "log")
        val shared = OpenWrtState()
    }
}
