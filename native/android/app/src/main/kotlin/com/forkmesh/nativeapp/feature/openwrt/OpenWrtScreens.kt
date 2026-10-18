package com.forkmesh.nativeapp.feature.openwrt

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import com.forkmesh.nativeapp.feature.files.FilesFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.openwrt.OpenWrtChoice
import com.forkmesh.nativeapp.openwrt.OpenWrtDisk
import com.forkmesh.nativeapp.openwrt.OpenWrtModel
import com.forkmesh.nativeapp.openwrt.OpenWrtProblem
import com.forkmesh.nativeapp.openwrt.UciEdit
import com.forkmesh.nativeapp.openwrt.UciSection
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

object OpenWrtFeature {
    private const val OWNER = "existing"

    fun install() {
        NavigationFeature.register(Route.Kind.OpenWrtRouter, OWNER) { OpenWrtScreen() }
    }
}

private val state get() = OpenWrtState.shared

private fun CoroutineScope.guard(block: suspend () -> Unit) = launch {
    try {
        block()
    } catch (problem: OpenWrtProblem) {
        state.show(problem.message ?: "", true)
    }
}

@Composable
private fun RouterPage(title: String, navigator: Navigator, root: Boolean = false, content: LazyListScope.() -> Unit) {
    Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
        ToolsAppBar(title, navigator, pluginMenu = root)
        if (state.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        LazyColumn(Modifier.fillMaxSize()) {
            if (state.message.isNotEmpty()) item(key = "message") { MessageLine() }
            content()
            item(key = "end") { Spacer(Modifier.height(FmSpace.x6)) }
        }
    }
}

@Composable
private fun MessageLine() {
    Text(
        state.message,
        style = Fm.text.bodyMedium,
        color = if (state.messageIsError) Fm.colors.danger else Fm.colors.textSecondary,
        modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
    )
}

@Composable
private fun Header(text: String) {
    Text(text, style = Fm.text.titleSmall, color = Fm.colors.textSecondary, modifier = Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x4, bottom = FmSpace.x1))
}

@Composable
private fun Note(text: String) {
    Text(text, style = Fm.text.bodySmall, color = Fm.colors.textTertiary, modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2))
}

@Composable
private fun Row2(title: String, detail: String = "", icon: String? = null, onClick: (() -> Unit)? = null, trailing: @Composable () -> Unit = {}) {
    Column {
        Row(
            Modifier.fillMaxWidth().let { if (onClick != null) it.clickable(onClick = onClick) else it }.padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (icon != null) {
                FmIcon(icon, tint = Fm.colors.textTertiary)
                Spacer(Modifier.width(FmSpace.x3))
            }
            Column(Modifier.weight(1f)) {
                Text(title, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary, maxLines = 2, overflow = TextOverflow.Ellipsis)
                if (detail.isNotEmpty()) Text(detail, style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
            }
            trailing()
            if (onClick != null && icon != null) FmIcon("chevron_right", tint = Fm.colors.textTertiary)
        }
        HorizontalDivider(color = Fm.colors.border)
    }
}

@Composable
private fun Field(label: String, value: String, onChange: (String) -> Unit, hint: String = "", secret: Boolean = false, keyboard: KeyboardType = KeyboardType.Text) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        placeholder = if (hint.isEmpty()) null else ({ Text(hint) }),
        singleLine = true,
        visualTransformation = if (secret) PasswordVisualTransformation() else VisualTransformation.None,
        keyboardOptions = KeyboardOptions(keyboardType = if (secret) KeyboardType.Password else keyboard, autoCorrectEnabled = false),
        modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
    )
}

@Composable
private fun Toggle(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth().clickable { onChange(!checked) }.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, style = Fm.text.bodyMedium, modifier = Modifier.weight(1f))
        Switch(checked = checked, onCheckedChange = onChange)
    }
}

@Composable
private fun Choice(label: String, choices: List<OpenWrtChoice>, value: String, onChange: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        Row2(label, OpenWrtModel.choiceLabel(choices, value), onClick = { open = true })
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (choice in choices) {
                DropdownMenuItem(text = { Text(choice.label) }, onClick = {
                    open = false
                    onChange(choice.value)
                })
            }
        }
    }
}

@Composable
private fun Actions(content: @Composable () -> Unit) {
    Row(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        content()
    }
}

@Composable
private fun Confirm(title: String, text: String, action: String, onConfirm: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = { Text(text) },
        confirmButton = { TextButton(onClick = { onDismiss(); onConfirm() }) { Text(action) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun Prompt(title: String, text: String, initial: String, action: String, onConfirm: (String) -> Unit, onDismiss: () -> Unit) {
    var value by remember { mutableStateOf(initial) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column {
                Text(text)
                OutlinedTextField(value = value, onValueChange = { value = it }, singleLine = true, modifier = Modifier.padding(top = FmSpace.x2))
            }
        },
        confirmButton = { TextButton(onClick = { onDismiss(); onConfirm(value.trim()) }) { Text(action) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

class OpenWrtScreen : Screen() {
    override val title: String get() = "Router"

    private var reviewing by mutableStateOf(false)
    private var discarding by mutableStateOf(false)
    private var rebooting by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        state.attach(LocalContext.current)
        val scope = rememberCoroutineScope()
        if (reviewing) {
            AlertDialog(
                onDismissRequest = { reviewing = false },
                title = { Text("Waiting changes") },
                text = { Text(state.snapshot.pendingLines.joinToString("\n"), fontFamily = FontFamily.Monospace) },
                confirmButton = { TextButton(onClick = { reviewing = false }) { Text("Done") } },
            )
        }
        if (discarding) {
            Confirm("Discard changes?", "The ${state.snapshot.pendingCount} changes you have not applied will be thrown away.", "Discard",
                { scope.guard { state.discard() } }, { discarding = false })
        }
        if (rebooting) {
            Confirm("Restart the router?", "Your network and internet go down for a minute or two while it restarts. Changes you have not applied are lost.", "Restart",
                { scope.guard { state.reboot() } }, { rebooting = false })
        }
        RouterPage("Router", navigator, root = true) {
            if (!state.signedIn) signIn(scope) else manager(navigator, scope)
        }
    }

    private fun LazyListScope.signIn(scope: CoroutineScope) {
        item(key = "signin") {
            Column {
                Header("Connect to your OpenWrt router")
                Field("Address", state.address, { state.address = it }, "192.168.1.1", keyboard = KeyboardType.Uri)
                Field("User", state.user, { state.user = it }, "root")
                Field("Password", state.password, { state.password = it }, secret = true)
                Note(state.addressNote)
                Actions {
                    Button(onClick = { scope.guard { state.connect() } }, enabled = !state.busy) { Text(if (state.busy) "Connecting…" else "Connect") }
                }
                if (state.pendingCertificate.isNotEmpty()) {
                    Header("Certificate fingerprint (SHA-256)")
                    Text(state.pendingCertificate, style = Fm.text.bodySmall, fontFamily = FontFamily.Monospace, modifier = Modifier.padding(horizontal = FmSpace.x4))
                    Note("Compare it with the fingerprint the router shows before you trust it.")
                    Actions { TextButton(onClick = { scope.guard { state.trustCertificate() } }) { Text("Trust this certificate") } }
                }
                Note("Manage an OpenWrt router on this Wi-Fi: its internet and local network, Wi-Fi, devices, port forwards, USB storage, services, packages and every setting. The password stays in memory while you use the router and is sent only to it.")
            }
        }
    }

    private fun LazyListScope.manager(navigator: Navigator, scope: CoroutineScope) {
        val snapshot = state.snapshot
        if (snapshot.pendingCount > 0) {
            item(key = "pending") {
                Column(Modifier.fillMaxWidth().background(Fm.colors.bgRaised).padding(vertical = FmSpace.x2)) {
                    Text(
                        if (snapshot.pendingCount == 1) "1 change is waiting. It takes effect when you apply it."
                        else "${snapshot.pendingCount} changes are waiting. They take effect when you apply them.",
                        style = Fm.text.bodyMedium,
                        modifier = Modifier.padding(horizontal = FmSpace.x4),
                    )
                    Toggle("Undo automatically if the router stops answering", state.rollback) { state.rollback = it }
                    Actions {
                        TextButton(onClick = { reviewing = true }) { Text("Review") }
                        TextButton(onClick = { discarding = true }) { Text("Discard") }
                        Spacer(Modifier.weight(1f))
                        Button(onClick = { scope.guard { state.apply() } }, enabled = !state.busy) { Text("Apply") }
                    }
                }
            }
        }
        item(key = "overview") {
            Column {
                Header("Overview")
                val release = snapshot.board.obj("release")
                val memory = snapshot.info.obj("memory")
                val root = snapshot.info.obj("root")
                val rows = mutableListOf(
                    "Hostname" to snapshot.hostname,
                    "Model" to snapshot.board.str("model"),
                    "Firmware" to release.str("description"),
                )
                if (snapshot.info.isNotEmpty()) {
                    rows += "Uptime" to OpenWrtModel.formatUptime(snapshot.info.long("uptime"))
                    rows += "Load" to OpenWrtModel.formatLoad(snapshot.info.list("load"))
                    val free = if (memory.containsKey("available")) memory.long("available") else memory.long("free")
                    rows += "Memory" to "${OpenWrtModel.formatBytes(free)} free of ${OpenWrtModel.formatBytes(memory.long("total"))}"
                    if (root.isNotEmpty()) rows += "Storage" to "${OpenWrtModel.formatBytes(root.long("avail") * 1024)} free of ${OpenWrtModel.formatBytes(root.long("total") * 1024)}"
                }
                for (status in snapshot.interfaces.filter { it.name != "loopback" }) {
                    rows += status.name to (listOf(if (status.up) "Up ${OpenWrtModel.formatUptime(status.uptime)}" else "Down") + status.ipv4).joinToString(" · ")
                }
                for ((label, value) in rows) if (value.isNotEmpty()) Row2(label, value)
            }
        }
        item(key = "sections") {
            Column {
                Header("Manage")
                Row2("Network", "Interfaces, addresses, DNS and DHCP", "lan") { navigator.push(OpenWrtNetworkScreen()) }
                Row2("Wi-Fi", "Radios and networks", "wifi") { navigator.push(OpenWrtWifiScreen()) }
                Row2("Devices", "Connected devices and reserved addresses", "devices") { navigator.push(OpenWrtDevicesScreen()) }
                Row2("Port forwards", "${snapshot.forwards.size} configured", "swap_horiz") { navigator.push(OpenWrtForwardsScreen()) }
                Row2("Storage", "${snapshot.disks.size} USB disks or cards", "usb") { navigator.push(OpenWrtStorageScreen()) }
                Row2("Services", "Start, stop and boot", "tune") { navigator.push(OpenWrtServicesScreen()) }
                Row2("Packages", "${snapshot.packages.size} installed", "inventory_2") { navigator.push(OpenWrtPackagesScreen()) }
                Row2("System", "Hostname and password", "settings") { navigator.push(OpenWrtSystemScreen()) }
                Row2("All settings", "Every UCI option", "list") { navigator.push(OpenWrtSettingsScreen()) }
            }
        }
        item(key = "actions") {
            Actions {
                TextButton(onClick = { scope.guard { state.refresh() } }) { Text("Refresh") }
                TextButton(onClick = { rebooting = true }) { Text("Restart router") }
                TextButton(onClick = { scope.guard { state.signOut() } }) { Text("Sign out") }
            }
        }
    }
}

class OpenWrtNetworkScreen : Screen() {
    override val title: String get() = "Network"

    @Composable
    override fun Content(navigator: Navigator) {
        RouterPage(title, navigator) {
            items(state.snapshot.interfaceSections, key = { it.name }) { section ->
                val status = state.snapshot.status(section.name)
                Row2(
                    section.name,
                    listOf(OpenWrtModel.choiceLabel(OpenWrtModel.protocolChoices, section.option("proto")), status?.let { if (it.up) "Up" else "Down" }.orEmpty(),
                        status?.ipv4.orEmpty().joinToString(", ")).filter { it.isNotEmpty() }.joinToString(" · "),
                    "lan",
                ) { navigator.push(OpenWrtInterfaceScreen(section)) }
            }
        }
    }
}

class OpenWrtInterfaceScreen(private val section: UciSection) : Screen() {
    override val title: String get() = "Interface ${section.name}"

    private val dhcp = state.snapshot.dhcpSection(section.name)
    private var proto by mutableStateOf(section.option("proto", "none"))
    private var ipaddr by mutableStateOf(section.option("ipaddr"))
    private var netmask by mutableStateOf(section.option("netmask"))
    private var gateway by mutableStateOf(section.option("gateway"))
    private var dns by mutableStateOf(section.option("dns"))
    private var pppoeUser by mutableStateOf(section.option("username"))
    private var pppoePassword by mutableStateOf("")
    private var dhcpOn by mutableStateOf(dhcp?.flag("ignore")?.not() ?: false)
    private var dhcpStart by mutableStateOf(dhcp?.option("start", "100") ?: "100")
    private var dhcpLimit by mutableStateOf(dhcp?.option("limit", "150") ?: "150")
    private var leaseTime by mutableStateOf(dhcp?.option("leasetime", "12h") ?: "12h")
    private var restarting by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        if (restarting) {
            Confirm("Restart ${section.name}?", "Devices using it lose their connection for a few seconds, and this screen may stop answering briefly.", "Restart",
                { scope.guard { state.restartInterface(section.name) } }, { restarting = false })
        }
        RouterPage(title, navigator) {
            item(key = "form") {
                Column {
                    Choice("Protocol", OpenWrtModel.protocolChoices, proto) { proto = it }
                    if (proto == "static") {
                        Field("IP address", ipaddr, { ipaddr = it }, "192.168.1.1", keyboard = KeyboardType.Uri)
                        Field("Netmask", netmask, { netmask = it }, "255.255.255.0", keyboard = KeyboardType.Uri)
                        Field("Gateway", gateway, { gateway = it }, "Optional", keyboard = KeyboardType.Uri)
                    }
                    if (proto == "pppoe") {
                        Field("PPPoE user", pppoeUser, { pppoeUser = it })
                        Field("PPPoE password", pppoePassword, { pppoePassword = it }, "Empty keeps the current one", secret = true)
                    }
                    if (proto != "none") Field("DNS servers", dns, { dns = it }, "Optional, for example 1.1.1.1 9.9.9.9", keyboard = KeyboardType.Uri)
                    if (proto == "static") {
                        Toggle("Hand out addresses on this network (DHCP server)", dhcpOn) { dhcpOn = it }
                        if (dhcpOn) {
                            Field("First address", dhcpStart, { dhcpStart = it }, "100", keyboard = KeyboardType.Number)
                            Field("Number of addresses", dhcpLimit, { dhcpLimit = it }, "150", keyboard = KeyboardType.Number)
                            Field("Lease time", leaseTime, { leaseTime = it }, "12h")
                        }
                    }
                    Actions {
                        Button(onClick = { scope.guard { save() } }) { Text("Save") }
                        TextButton(onClick = { restarting = true }) { Text("Restart interface") }
                    }
                    Note("Saved changes wait until you apply them on the Router screen.")
                }
            }
        }
    }

    private suspend fun save() {
        val edit = OpenWrtModel.interfaceEdit(section, proto, ipaddr, netmask, gateway, dns, pppoeUser, pppoePassword)
        if (!state.stage("network", section.name, edit, "Interface ${section.name}") || proto != "static") return
        val start = dhcpStart.toIntOrNull()?.takeIf { it in 1..65535 } ?: throw OpenWrtProblem("The first address is a number from 1 to 65535.")
        val limit = dhcpLimit.toIntOrNull()?.takeIf { it in 1..65535 } ?: throw OpenWrtProblem("The number of addresses is a number from 1 to 65535.")
        val pool = OpenWrtModel.dhcpEdit(dhcpOn, start, limit, leaseTime)
        val existing = state.snapshot.dhcpSection(section.name)
        if (existing != null) {
            state.stage("dhcp", existing.name, pool.copy(removed = pool.removed.filter { existing.options.containsKey(it) }), "DHCP server for ${section.name}")
        } else if (dhcpOn) {
            state.add("dhcp", "dhcp", pool.values + ("interface" to section.name), "DHCP server for ${section.name}",
                if (OpenWrtModel.validUciName(section.name)) section.name else "")
        }
    }
}

class OpenWrtWifiScreen : Screen() {
    override val title: String get() = "Wi-Fi"

    private var addingTo by mutableStateOf<String?>(null)
    private var newSsid by mutableStateOf("")
    private var newKey by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        addingTo?.let { radio ->
            AlertDialog(
                onDismissRequest = { addingTo = null },
                title = { Text("Add Wi-Fi network") },
                text = {
                    Column {
                        Text("The new network uses WPA2/WPA3 and connects to the local network.")
                        OutlinedTextField(newSsid, { newSsid = it }, label = { Text("Network name") }, singleLine = true)
                        OutlinedTextField(newKey, { newKey = it }, label = { Text("Password") }, singleLine = true, visualTransformation = PasswordVisualTransformation())
                    }
                },
                confirmButton = {
                    TextButton(onClick = {
                        addingTo = null
                        scope.guard {
                            val networks = state.snapshot.interfaceSections.map { it.name }
                            val values = OpenWrtModel.newWifiValues(radio, if ("lan" in networks) "lan" else networks.firstOrNull() ?: "lan", newSsid, newKey)
                            state.add("wireless", "wifi-iface", values, "Wi-Fi network $newSsid")
                        }
                    }) { Text("Add") }
                },
                dismissButton = { TextButton(onClick = { addingTo = null }) { Text("Cancel") } },
            )
        }
        RouterPage(title, navigator) {
            if (state.snapshot.radios.isEmpty()) item(key = "none") { Note("This router has no Wi-Fi radios, or the wireless settings could not be read.") }
            for (radio in state.snapshot.radios) {
                item(key = "radio-${radio.name}") {
                    Column {
                        Header(OpenWrtModel.wifiBand(radio))
                        Row2(radio.name, "channel ${radio.option("channel", "auto")}${if (radio.flag("disabled")) " · off" else ""}", "settings") {
                            navigator.push(OpenWrtRadioScreen(radio))
                        }
                        for (network in state.snapshot.wifiNetworks(radio.name)) {
                            Row2(
                                network.option("ssid", OpenWrtModel.sectionLabel(network)),
                                "${OpenWrtModel.choiceLabel(OpenWrtModel.encryptionChoices, network.option("encryption", "none"))} · ${network.option("network")}${if (network.flag("disabled")) " · off" else ""}",
                                "wifi",
                            ) { navigator.push(OpenWrtWifiNetworkScreen(network)) }
                        }
                        Actions {
                            TextButton(onClick = {
                                newSsid = ""
                                newKey = ""
                                addingTo = radio.name
                            }) { Text("Add Wi-Fi network") }
                        }
                    }
                }
            }
        }
    }
}

class OpenWrtRadioScreen(private val radio: UciSection) : Screen() {
    override val title: String get() = "${radio.name} · ${OpenWrtModel.wifiBand(radio)}"

    private var enabled by mutableStateOf(!radio.flag("disabled"))
    private var channel by mutableStateOf(radio.option("channel", "auto"))
    private var country by mutableStateOf(radio.option("country"))
    private var txpower by mutableStateOf(radio.option("txpower"))

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        RouterPage(title, navigator) {
            item(key = "form") {
                Column {
                    Toggle("Radio on", enabled) { enabled = it }
                    Field("Channel", channel, { channel = it }, "auto")
                    Field("Country", country, { country = it }, "US")
                    Field("Transmit power (dBm)", txpower, { txpower = it }, "Driver default", keyboard = KeyboardType.Number)
                    Actions {
                        Button(onClick = {
                            scope.guard { state.stage("wireless", radio.name, OpenWrtModel.radioEdit(radio, enabled, channel, country, txpower), "Radio ${radio.name}") }
                        }) { Text("Save") }
                    }
                    Note("Use the country you are in: it sets which channels and power levels are legal.")
                }
            }
        }
    }
}

class OpenWrtWifiNetworkScreen(private val iface: UciSection) : Screen() {
    override val title: String get() = iface.option("ssid", iface.name)

    private var enabled by mutableStateOf(!iface.flag("disabled"))
    private var ssid by mutableStateOf(iface.option("ssid"))
    private var encryption by mutableStateOf(iface.option("encryption", "none"))
    private var wifiKey by mutableStateOf(iface.option("key"))
    private var showKey by mutableStateOf(false)
    private var network by mutableStateOf(iface.option("network").substringBefore(' '))
    private var hidden by mutableStateOf(iface.flag("hidden"))
    private var isolate by mutableStateOf(iface.flag("isolate"))
    private var deleting by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        if (deleting) {
            Confirm("Delete $title?", "Devices connected to this network lose their connection once you apply.", "Delete", {
                scope.guard {
                    state.delete("wireless", iface.name, "Wi-Fi network $title")
                    navigator.pop()
                }
            }, { deleting = false })
        }
        RouterPage(title, navigator) {
            item(key = "form") {
                Column {
                    Toggle("Network on", enabled) { enabled = it }
                    Field("Name (SSID)", ssid, { ssid = it })
                    Choice("Security", OpenWrtModel.encryptionChoices, encryption) { encryption = it }
                    if (encryption != "none" && encryption != "owe") {
                        Field("Password", wifiKey, { wifiKey = it }, secret = !showKey)
                        Toggle("Show password", showKey) { showKey = it }
                    }
                    Choice("Connects to", state.snapshot.interfaceSections.map { OpenWrtChoice(it.name, it.name) }, network) { network = it }
                    Toggle("Hide the network name", hidden) { hidden = it }
                    Toggle("Keep devices apart (guest isolation)", isolate) { isolate = it }
                    Actions {
                        Button(onClick = {
                            scope.guard {
                                state.stage("wireless", iface.name, OpenWrtModel.wifiEdit(iface, ssid, encryption, wifiKey, network, enabled, hidden, isolate), "Wi-Fi network $ssid")
                            }
                        }) { Text("Save") }
                        TextButton(onClick = { deleting = true }) { Text("Delete network", color = Fm.colors.danger) }
                    }
                }
            }
        }
    }
}

class OpenWrtDevicesScreen : Screen() {
    override val title: String get() = "Devices"

    private var name by mutableStateOf("")
    private var mac by mutableStateOf("")
    private var ip by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val snapshot = state.snapshot
        val now = System.currentTimeMillis() / 1000
        val leased = snapshot.leases.map { it.mac }.toSet()
        RouterPage(title, navigator) {
            item(key = "connected") { Header("Connected") }
            if (snapshot.leases.isEmpty()) item(key = "none") { Note("No devices have an address from the router right now.") }
            items(snapshot.leases, key = { "lease-${it.mac}" }) { lease ->
                val reservation = snapshot.reservation(lease.mac)
                Row2(
                    lease.hostname.ifEmpty { reservation?.option("name") ?: lease.ip },
                    "${lease.ip} · ${lease.mac} · ${OpenWrtModel.leaseExpiry(lease.expires, now)}${if (reservation != null) " · reserved" else ""}",
                ) {
                    TextButton(onClick = {
                        scope.guard {
                            if (reservation != null) {
                                state.delete("dhcp", reservation.name, "Reservation for ${lease.ip}")
                            } else {
                                state.add("dhcp", "host", OpenWrtModel.reservationValues(OpenWrtModel.sanitizedHostname(lease.hostname, lease.mac), lease.mac, lease.ip), "Reservation of ${lease.ip}")
                            }
                        }
                    }) { Text(if (reservation != null) "Unreserve" else "Reserve") }
                }
            }
            val offline = snapshot.reservations.filter { it.option("mac").lowercase().substringBefore(' ') !in leased }
            if (offline.isNotEmpty()) {
                item(key = "offline") { Header("Reserved, not connected") }
                items(offline, key = { "host-${it.name}" }) { host ->
                    Row2(host.option("name", host.name), host.option("ip")) {
                        TextButton(onClick = { scope.guard { state.delete("dhcp", host.name, "Reservation ${host.option("name")}") } }) { Text("Remove") }
                    }
                }
            }
            item(key = "add") {
                Column {
                    Header("Reserve an address")
                    Field("Name", name, { name = it })
                    Field("MAC address", mac, { mac = it }, "aa:bb:cc:dd:ee:ff")
                    Field("IP address", ip, { ip = it }, "192.168.1.50", keyboard = KeyboardType.Uri)
                    Actions {
                        Button(onClick = {
                            scope.guard {
                                if (state.add("dhcp", "host", OpenWrtModel.reservationValues(name, mac, ip), "Reservation of $ip")) {
                                    name = ""
                                    mac = ""
                                    ip = ""
                                }
                            }
                        }) { Text("Add reservation") }
                    }
                    Note("A reserved device always gets the same address from the router.")
                }
            }
        }
    }
}

class OpenWrtForwardsScreen : Screen() {
    override val title: String get() = "Port forwards"

    @Composable
    override fun Content(navigator: Navigator) {
        RouterPage(title, navigator) {
            item(key = "new") { Actions { Button(onClick = { navigator.push(OpenWrtForwardScreen(null)) }) { Text("New port forward") } } }
            if (state.snapshot.forwards.isEmpty()) item(key = "none") { Note("No port forwards. A port forward lets a device on the internet reach a device on your network.") }
            items(state.snapshot.forwards, key = { it.name }) { redirect ->
                Row2(
                    redirect.option("name", OpenWrtModel.sectionLabel(redirect)),
                    "${OpenWrtModel.choiceLabel(OpenWrtModel.forwardProtocolChoices, redirect.option("proto", "tcp udp"))} ${redirect.option("src_dport")} → " +
                        "${redirect.option("dest_ip")}:${redirect.option("dest_port", redirect.option("src_dport"))}${if (redirect.flag("enabled", true)) "" else " · off"}",
                    "swap_horiz",
                ) { navigator.push(OpenWrtForwardScreen(redirect)) }
            }
        }
    }
}

class OpenWrtForwardScreen(private val existing: UciSection?) : Screen() {
    override val title: String get() = if (existing == null) "New port forward" else "Port forward"

    private var name by mutableStateOf(existing?.option("name").orEmpty())
    private var proto by mutableStateOf(existing?.option("proto", "tcp udp") ?: "tcp udp")
    private var external by mutableStateOf(existing?.option("src_dport").orEmpty())
    private var ip by mutableStateOf(existing?.option("dest_ip").orEmpty())
    private var target by mutableStateOf(existing?.option("dest_port").orEmpty())
    private var enabled by mutableStateOf(existing?.flag("enabled", true) ?: true)
    private var deleting by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        if (deleting && existing != null) {
            Confirm("Delete $name?", "The port stops reaching the device once you apply.", "Delete", {
                scope.guard {
                    state.delete("firewall", existing.name, "Port forward $name")
                    navigator.pop()
                }
            }, { deleting = false })
        }
        RouterPage(title, navigator) {
            item(key = "form") {
                Column {
                    Field("Name", name, { name = it }, "Game server")
                    Choice("Protocol", OpenWrtModel.forwardProtocolChoices, proto) { proto = it }
                    Field("External port", external, { external = it }, "8080 or 6000-6010", keyboard = KeyboardType.Uri)
                    Field("To device address", ip, { ip = it }, "192.168.1.20", keyboard = KeyboardType.Uri)
                    Field("To port", target, { target = it }, "Same as the external port", keyboard = KeyboardType.Uri)
                    Toggle("On", enabled) { enabled = it }
                    Actions {
                        Button(onClick = {
                            scope.guard {
                                val edit = OpenWrtModel.forwardEdit(existing, name, proto, external, ip, target, enabled)
                                val saved = if (existing != null) state.stage("firewall", existing.name, edit, "Port forward $name")
                                else state.add("firewall", "redirect", edit.values, "Port forward $name")
                                if (saved) navigator.pop()
                            }
                        }) { Text("Save") }
                        if (existing != null) TextButton(onClick = { deleting = true }) { Text("Delete", color = Fm.colors.danger) }
                    }
                }
            }
        }
    }
}

class OpenWrtStorageScreen : Screen() {
    override val title: String get() = "Storage"

    private var bootDisk by mutableStateOf<OpenWrtDisk?>(null)
    private var shareDisk by mutableStateOf<OpenWrtDisk?>(null)
    private var shareGuests by mutableStateOf(false)
    private var unmountDisk by mutableStateOf<OpenWrtDisk?>(null)
    private var installing by mutableStateOf(false)
    private var installingSharing by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val snapshot = state.snapshot
        val disks = snapshot.disks
        if (installingSharing) {
            Confirm("Install network sharing?", "This updates the router's package lists and installs ${OpenWrtModel.networkSharePackages.joinToString(" and ")}. The router needs internet access to download them.", "Install",
                { scope.guard { state.installNetworkSharing() } }, { installingSharing = false })
        }
        bootDisk?.let { disk ->
            Prompt("Mount at boot", "Folder on the router where ${disk.title} appears:",
                disk.bootTarget.ifEmpty { if (disk.mounted) disk.mount else OpenWrtModel.suggestedMountPath(disk.block) }, "Save",
                { path -> scope.guard { state.mountAtBoot(disk, path) } }, { bootDisk = null })
        }
        shareDisk?.let { disk ->
            Prompt("Share on network", if (shareGuests) "Anyone on your network can open it without a password. Name others see:" else "Name others see (a password is needed to open it):",
                (disk.block.label.ifEmpty { disk.block.name }).map { if (it.isLetterOrDigit() || it in "._-") it else '_' }.joinToString(""), "Share",
                { name -> scope.guard { state.share(disk, name, shareGuests) } }, { shareDisk = null })
        }
        unmountDisk?.let { disk ->
            Confirm("Unmount ${disk.title}?", "Shares and programs using the disk lose access. Afterwards it can be unplugged safely.", "Unmount",
                { scope.guard { state.unmount(disk) } }, { unmountDisk = null })
        }
        if (installing) {
            Confirm("Install USB storage support?", "This updates the router's package lists and installs ${OpenWrtModel.usbStoragePackages.joinToString(", ")}. The router needs internet access to download them.", "Install",
                { scope.guard { state.installStorageSupport() } }, { installing = false })
        }
        LaunchedEffect(Unit) {
            if (state.snapshot.disks.isEmpty() && state.storageLog.isEmpty()) scope.guard { state.checkStorage() }
        }
        RouterPage(title, navigator) {
            item(key = "top") {
                Column {
                    if (snapshot.pendingCount > 0) {
                        Note(if (snapshot.pendingCount == 1) "1 change is waiting. Disks mount by themselves only after it is applied."
                        else "${snapshot.pendingCount} changes are waiting. Disks mount by themselves only after they are applied.")
                        Actions {
                            TextButton(onClick = { scope.guard { state.applyAndMount() } }, enabled = !state.busy && !state.storageBusy) { Text("Apply and mount disks") }
                        }
                    }
                    Toggle("Mount any disk when it is plugged in", snapshot.autoMount) { on -> if (!state.storageBusy) scope.guard { state.setAutoMount(on) } }
                    Actions {
                        TextButton(onClick = { scope.guard { state.mountAll() } }) { Text("Mount disks now") }
                        TextButton(onClick = { installing = true }, enabled = !state.storageBusy) { Text("Install USB support") }
                        TextButton(onClick = { scope.guard { state.checkStorage() } }, enabled = !state.storageBusy) { Text("Check USB") }
                    }
                    if (state.storageBusy) LinearProgressIndicator(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4))
                    Note(
                        when {
                            snapshot.storageProblem.isNotEmpty() -> "The router cannot list its disks (${snapshot.storageProblem}). Install USB storage support, which adds block-mount and the USB and filesystem drivers."
                            disks.isEmpty() -> "No USB disk or memory card is listed. Plug one in and refresh, or press Check USB to see why."
                            else -> "Unmount a disk before you unplug it. Formatting is not offered: format the disk on a computer first."
                        },
                    )
                    if (state.storageLog.isNotEmpty()) {
                        Header("USB storage check")
                        SelectionContainer {
                            Text(state.storageLog, style = Fm.text.bodySmall, fontFamily = FontFamily.Monospace, modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2))
                        }
                    }
                    Header("Disks")
                }
            }
            items(disks, key = { "disk-${it.block.name}" }) { disk ->
                Column {
                    Row2(disk.title, diskDetail(disk), "usb")
                    Row(Modifier.padding(horizontal = FmSpace.x2)) {
                        TextButton(onClick = { bootDisk = disk }) { Text("Mount at boot") }
                        if (disk.fstabSection.isNotEmpty()) TextButton(onClick = { scope.guard { state.delete("fstab", disk.fstabSection, "Mount for ${disk.title}") } }) { Text("Not at boot") }
                        if (disk.mounted) {
                            TextButton(onClick = {
                                shareGuests = false
                                shareDisk = disk
                            }) { Text("Share") }
                            TextButton(onClick = {
                                shareGuests = true
                                shareDisk = disk
                            }) { Text("Share open") }
                            TextButton(onClick = { unmountDisk = disk }) { Text("Unmount", color = Fm.colors.danger) }
                        }
                    }
                }
            }
            item(key = "shares") {
                Column {
                    Header("Network shares")
                    if (snapshot.shareConfig.isEmpty()) {
                        Note("To share disks with computers and phones, install ksmbd-server and luci-app-ksmbd.")
                        Actions {
                            Button(onClick = { installingSharing = true }, enabled = state.signedIn && !state.storageBusy && !state.busy) { Text("Install network sharing") }
                        }
                    } else {
                        Note("Shared with the ${snapshot.shareConfig} file server (SMB). Shares with guest access need no password; others need a user set up on the router.")
                    }
                }
            }
            items(snapshot.shares, key = { "share-${it.section}" }) { share ->
                Column {
                    Row2(share.name, "${share.path}${if (share.readOnly) " · read only" else ""}${if (share.guests) " · guests" else ""}", "folder")
                    Row(Modifier.padding(horizontal = FmSpace.x2)) {
                        TextButton(onClick = { navigator.push(FilesFeature.browseShare(state.routerHost, share.name, share.guests)) }, enabled = state.routerHost.isNotEmpty()) { Text("Browse files") }
                        TextButton(onClick = { scope.guard { state.delete(snapshot.shareConfig, share.section, "Share ${share.name}") } }) { Text("Stop sharing", color = Fm.colors.danger) }
                    }
                }
            }
        }
    }

    private fun diskDetail(disk: OpenWrtDisk): String {
        val parts = mutableListOf(disk.block.type.ifEmpty { "Unknown filesystem" })
        if (disk.size > 0) parts += OpenWrtModel.formatBytes(disk.size)
        if (disk.usageKnown) parts += "${OpenWrtModel.formatBytes(disk.avail)} free"
        parts += if (disk.mounted) "at ${disk.mount}" else "not mounted"
        if (disk.fstabSection.isNotEmpty()) parts += if (disk.bootEnabled) "mounts at boot" else "boot mount off"
        return parts.joinToString(" · ")
    }
}

class OpenWrtServicesScreen : Screen() {
    override val title: String get() = "Services"

    private var menuFor by mutableStateOf<String?>(null)
    private var risky by mutableStateOf<Pair<String, String>?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        risky?.let { (name, action) ->
            Confirm("This service keeps the router reachable", "Stopping $name can cut your network off or lock you out until the router restarts.", "Continue",
                { scope.guard { state.serviceAction(name, action) } }, { risky = null })
        }
        fun run(name: String, action: String) {
            menuFor = null
            if ((action == "stop" || action == "disable") && name in OpenWrtState.criticalServices) risky = name to action
            else scope.guard { state.serviceAction(name, action) }
        }
        RouterPage(title, navigator) {
            items(state.snapshot.services, key = { it.name }) { service ->
                Box {
                    Row2(service.name, "${if (service.running) "Running" else "Stopped"}${if (service.enabled) " · starts at boot" else ""}", onClick = { menuFor = service.name })
                    DropdownMenu(expanded = menuFor == service.name, onDismissRequest = { menuFor = null }) {
                        DropdownMenuItem(text = { Text("Start") }, onClick = { run(service.name, "start") })
                        DropdownMenuItem(text = { Text("Restart") }, onClick = { run(service.name, "restart") })
                        DropdownMenuItem(text = { Text("Stop") }, onClick = { run(service.name, "stop") })
                        DropdownMenuItem(text = { Text("Start at boot") }, onClick = { run(service.name, "enable") })
                        DropdownMenuItem(text = { Text("Don't start at boot") }, onClick = { run(service.name, "disable") })
                    }
                }
            }
        }
    }
}

class OpenWrtPackagesScreen : Screen() {
    override val title: String get() = "Packages"

    private var query by mutableStateOf("")
    private var install by mutableStateOf("")
    private var removing by mutableStateOf<String?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        removing?.let { name ->
            Confirm("Remove $name?", "Removing a package the router depends on can break it until you reinstall the package or reset the router.", "Remove",
                { scope.guard { state.packageAction("remove", listOf(name)) } }, { removing = null })
        }
        val shown = state.snapshot.packages.filter { query.isBlank() || it.name.contains(query.trim(), ignoreCase = true) }
        RouterPage(title, navigator) {
            item(key = "install") {
                Column {
                    Field("Package to install", install, { install = it }, "luci-app-sqm")
                    Actions {
                        Button(onClick = { scope.guard { state.packageAction("install", install.split(' ').filter { it.isNotBlank() }) } }, enabled = install.isNotBlank() && !state.busy) { Text("Install") }
                        TextButton(onClick = { scope.guard { state.packageAction("update", emptyList()) } }, enabled = !state.busy) { Text("Update lists") }
                    }
                    if (state.packageOutput.isNotEmpty()) {
                        Text(state.packageOutput, style = Fm.text.bodySmall, fontFamily = FontFamily.Monospace, modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2))
                    }
                    Field("Filter installed packages", query, { query = it })
                    Header("Installed (${state.snapshot.packages.size})")
                }
            }
            items(shown, key = { it.name }) { pkg ->
                Row2(pkg.name, pkg.version) { TextButton(onClick = { removing = pkg.name }) { Text("Remove") } }
            }
        }
    }
}

class OpenWrtSystemScreen : Screen() {
    override val title: String get() = "System"

    private var hostname by mutableStateOf(state.snapshot.hostname)
    private var newPassword by mutableStateOf("")
    private var confirmPassword by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        RouterPage(title, navigator) {
            item(key = "form") {
                Column {
                    Header("Hostname")
                    Field("Hostname", hostname, { hostname = it }, "OpenWrt")
                    Actions {
                        Button(onClick = {
                            scope.guard {
                                OpenWrtModel.hostnameProblem(hostname)?.let { throw OpenWrtProblem(it) }
                                val system = state.snapshot.sections("system", "system").firstOrNull() ?: throw OpenWrtProblem("The router has no system settings section to change.")
                                state.stage("system", system.name, UciEdit(mapOf("hostname" to hostname)), "Hostname")
                            }
                        }) { Text("Save") }
                    }
                    Header("Router password")
                    Field("New password", newPassword, { newPassword = it }, secret = true)
                    Field("Again", confirmPassword, { confirmPassword = it }, secret = true)
                    Actions {
                        Button(onClick = {
                            scope.guard {
                                if (state.changePassword(newPassword, confirmPassword)) {
                                    newPassword = ""
                                    confirmPassword = ""
                                }
                            }
                        }) { Text("Change password") }
                    }
                    Note("The password for the account you signed in with. It also protects SSH and LuCI, and takes effect at once.")
                    Note("Backups, firmware upgrades and factory resets are on the ForkMesh desktop app, where the files live.")
                }
            }
        }
    }
}

class OpenWrtSettingsScreen : Screen() {
    override val title: String get() = "All settings"

    private var config by mutableStateOf("network")
    private var newType by mutableStateOf("")
    private var newName by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val snapshot = state.snapshot
        RouterPage(title, navigator) {
            item(key = "picker") {
                Column {
                    Choice("Configuration", snapshot.configNames.map { OpenWrtChoice(it, it) }, config) { picked ->
                        config = picked
                        if (snapshot.configs[picked] == null) scope.guard { state.reloadConfig(picked) }
                    }
                    Note("Every UCI setting on the router. Edits wait until you apply them.")
                    Header("Sections")
                }
            }
            items(snapshot.sections(config), key = { "$config-${it.name}" }) { section ->
                Row2(OpenWrtModel.sectionLabel(section), section.type, "list") { navigator.push(OpenWrtSectionScreen(config, section.name)) }
            }
            item(key = "add") {
                Column {
                    Header("Add a section")
                    Field("Type", newType, { newType = it }, "rule")
                    Field("Name", newName, { newName = it }, "Optional")
                    Actions {
                        Button(onClick = {
                            scope.guard {
                                if (!OpenWrtModel.validUciName(newType) || (newName.isNotEmpty() && !OpenWrtModel.validUciName(newName))) {
                                    throw OpenWrtProblem("A section type and name use letters, digits and underscores only.")
                                }
                                if (state.add(config, newType, emptyMap(), "Section ${newName.ifEmpty { newType }}", newName)) {
                                    newType = ""
                                    newName = ""
                                }
                            }
                        }) { Text("Add section") }
                    }
                }
            }
        }
    }
}

class OpenWrtSectionScreen(private val config: String, private val name: String) : Screen() {
    override val title: String get() = name

    private var editing by mutableStateOf<String?>(null)
    private var newOption by mutableStateOf("")
    private var newValue by mutableStateOf("")
    private var deleting by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val section = state.snapshot.section(config, name)
        editing?.let { key ->
            Prompt(key, "Empty removes the option. For a list, separate values with spaces.", section?.option(key).orEmpty(), "Save", { value ->
                scope.guard {
                    val edit = if (value.isEmpty()) UciEdit(removed = listOf(key)) else UciEdit(mapOf(key to OpenWrtModel.optionValue(value, section?.isList(key) == true)))
                    state.stage(config, name, edit, "$config.$name.$key")
                }
            }, { editing = null })
        }
        if (deleting) {
            Confirm("Delete $name?", "The whole section is removed once you apply.", "Delete", {
                scope.guard {
                    state.delete(config, name, "$config.$name")
                    navigator.pop()
                }
            }, { deleting = false })
        }
        RouterPage(title, navigator) {
            if (section == null) {
                item(key = "gone") { Note("This section no longer exists.") }
                return@RouterPage
            }
            item(key = "head") { Header("$config.${section.name} · ${section.type}") }
            items(section.options.keys.sorted(), key = { it }) { key ->
                Row2(key, if (OpenWrtModel.isSecretOption(key)) "••••••" else section.option(key), onClick = { editing = key })
            }
            item(key = "add") {
                Column {
                    Header("Add an option")
                    Field("Option", newOption, { newOption = it }, "option_name")
                    Field("Value", newValue, { newValue = it }, "Spaces make a list")
                    Actions {
                        Button(onClick = {
                            scope.guard {
                                if (!OpenWrtModel.validUciName(newOption)) throw OpenWrtProblem("An option name uses letters, digits and underscores only.")
                                state.stage(config, name, UciEdit(mapOf(newOption to OpenWrtModel.optionValue(newValue, newValue.contains(' ')))), "$config.$name.$newOption")
                                newOption = ""
                                newValue = ""
                            }
                        }) { Text("Set option") }
                        TextButton(onClick = { deleting = true }) { Text("Delete section", color = Fm.colors.danger) }
                    }
                }
            }
        }
    }
}
