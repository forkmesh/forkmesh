package com.forkmesh.nativeapp.feature.openwrt

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
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
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

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
    var routerHost by mutableStateOf("")

    private var client: OpenWrtClient? = null
    private var certificateKey = ""
    private var prefs: SharedPreferences? = null

    fun attach(context: Context) {
        if (prefs != null) return
        val store = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        prefs = store
        address = store.getString("address", "").orEmpty()
        user = store.getString("user", "root").orEmpty().ifEmpty { "root" }
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
            show("Connected to ${parsed.host}.")
        } finally {
            busy = false
        }
        refresh()
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
        routerHost = ""
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
        val values = mapOf("anon_mount" to if (on) "1" else "0", "auto_mount" to "1")
        val global = snapshot.sections("fstab", "global").firstOrNull()
        if (global == null) add("fstab", "global", values, "Automatic mounting") else stage("fstab", global.name, UciEdit(values), "Automatic mounting")
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
        return result.ok
    }

    companion object {
        const val PREFS = "market.packages.openwrt"
        val criticalServices = setOf("network", "firewall", "dropbear", "uhttpd", "rpcd", "dnsmasq", "odhcpd", "ubus", "log")
        val shared = OpenWrtState()
    }
}
