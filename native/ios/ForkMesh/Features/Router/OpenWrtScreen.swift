import FMBase
import FMFiles
import FMPackages
import SwiftUI

struct OpenWrtScreen: View {
    @State private var model = OpenWrtRouterModel()
    @State private var reviewing = false
    @State private var confirmDiscard = false
    @State private var confirmReboot = false

    var body: some View {
        Group {
            if model.signedIn { manager } else { signIn }
        }
        .navigationTitle("Router")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            if model.signedIn {
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Button("Refresh", systemImage: "arrow.clockwise") { Task { await model.refresh() } }
                        Button("Restart router", systemImage: "power", role: .destructive) { confirmReboot = true }
                        Button("Sign out", systemImage: "rectangle.portrait.and.arrow.right") { Task { await model.signOut() } }
                    } label: {
                        Image(systemName: "ellipsis.circle")
                    }
                    .accessibilityIdentifier("openwrt-menu")
                }
            }
        }
        .confirmationDialog("Restart the router?", isPresented: $confirmReboot, titleVisibility: .visible) {
            Button("Restart", role: .destructive) { Task { await model.reboot() } }
        } message: {
            Text("Your network and internet go down for a minute or two while it restarts. Changes you have not applied are lost.")
        }
        .confirmationDialog("Discard changes?", isPresented: $confirmDiscard, titleVisibility: .visible) {
            Button("Discard", role: .destructive) { Task { await model.discard() } }
        } message: {
            Text("The \(model.snapshot.pendingCount) changes you have not applied will be thrown away.")
        }
        .sheet(isPresented: $reviewing) {
            NavigationStack {
                List(model.snapshot.pendingLines, id: \.self) { line in
                    Text(line).font(.callout.monospaced())
                }
                .navigationTitle("Waiting changes")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { reviewing = false } } }
            }
        }
    }

    private var signIn: some View {
        Form {
            Section {
                TextField("192.168.1.1", text: $model.address)
                    .keyboardType(.URL)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .accessibilityIdentifier("openwrt-address")
                TextField("root", text: $model.user)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                SecureField("Password", text: $model.password)
                    .accessibilityIdentifier("openwrt-password")
                    .onSubmit { Task { await model.connect() } }
            } header: {
                Text("Connect to your OpenWrt router")
            } footer: {
                Text(model.addressNote)
            }
            Section {
                Button(model.busy ? "Connecting…" : "Connect") { Task { await model.connect() } }
                    .disabled(model.busy)
                    .accessibilityIdentifier("openwrt-connect")
            }
            if !model.pendingCertificate.isEmpty {
                Section {
                    Text(model.pendingCertificate).font(.caption.monospaced()).textSelection(.enabled)
                    Button("Trust this certificate") { Task { await model.trustCertificate() } }
                } header: {
                    Text("Certificate fingerprint (SHA-256)")
                } footer: {
                    Text("Compare it with the fingerprint the router shows before you trust it.")
                }
            }
            if !model.message.isEmpty { OpenWrtMessage(model: model) }
            Section {
                Text("Manage an OpenWrt router on this Wi-Fi: its internet and local network, Wi-Fi, devices, port forwards, USB storage, services, packages and every setting. The password stays in memory while you use the router and is sent only to it.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var manager: some View {
        List {
            if model.snapshot.pendingCount > 0 {
                Section {
                    Text(model.snapshot.pendingCount == 1 ? "1 change is waiting. It takes effect when you apply it."
                         : "\(model.snapshot.pendingCount) changes are waiting. They take effect when you apply them.")
                    Toggle("Undo automatically if the router stops answering", isOn: $model.rollback)
                    HStack {
                        Button("Review") { reviewing = true }.buttonStyle(.bordered)
                        Button("Discard") { confirmDiscard = true }.buttonStyle(.bordered)
                        Spacer()
                        Button("Apply") { Task { await model.apply() } }
                            .buttonStyle(.borderedProminent)
                            .disabled(model.busy)
                            .accessibilityIdentifier("openwrt-apply")
                    }
                }
            }
            if !model.message.isEmpty { OpenWrtMessage(model: model) }
            Section("Overview") { OpenWrtOverview(snapshot: model.snapshot) }
            Section {
                NavigationLink { OpenWrtNetworkScreen(model: model) } label: { Label("Network", systemImage: "network") }
                NavigationLink { OpenWrtWifiScreen(model: model) } label: { Label("Wi-Fi", systemImage: "wifi") }
                NavigationLink { OpenWrtDevicesScreen(model: model) } label: { Label("Devices", systemImage: "laptopcomputer.and.iphone") }
                NavigationLink { OpenWrtForwardsScreen(model: model) } label: { Label("Port forwards", systemImage: "arrow.left.arrow.right") }
                NavigationLink { OpenWrtStorageScreen(model: model) } label: { Label("Storage", systemImage: "externaldrive") }
                NavigationLink { OpenWrtServicesScreen(model: model) } label: { Label("Services", systemImage: "gearshape.2") }
                NavigationLink { OpenWrtPackagesScreen(model: model) } label: { Label("Packages", systemImage: "shippingbox") }
                NavigationLink { OpenWrtSystemScreen(model: model) } label: { Label("System", systemImage: "cpu") }
                NavigationLink { OpenWrtSettingsScreen(model: model) } label: { Label("All settings", systemImage: "slider.horizontal.3") }
            }
        }
        .refreshable { await model.refresh() }
        .overlay { if model.busy { ProgressView() } }
    }
}

struct OpenWrtMessage: View {
    let model: OpenWrtRouterModel

    var body: some View {
        Section {
            Text(model.message)
                .font(.callout)
                .foregroundStyle(model.messageIsError ? Color.red : Color.secondary)
                .textSelection(.enabled)
                .accessibilityIdentifier("openwrt-message")
        }
    }
}

struct OpenWrtOverview: View {
    let snapshot: OpenWrtSnapshot

    var body: some View {
        let release = snapshot.board.obj("release")
        let memory = snapshot.info.obj("memory")
        let root = snapshot.info.obj("root")
        row("Hostname", snapshot.hostname)
        row("Model", snapshot.board.str("model"))
        row("Firmware", release.str("description"))
        if !snapshot.info.isEmpty {
            row("Uptime", OpenWrtModel.formatUptime(snapshot.info.int64("uptime")))
            row("Load", OpenWrtModel.formatLoad(snapshot.info.list("load")))
            row("Memory", "\(OpenWrtModel.formatBytes(memory.contains("available") ? memory.int64("available") : memory.int64("free"))) free of \(OpenWrtModel.formatBytes(memory.int64("total")))")
            if !root.isEmpty {
                row("Storage", "\(OpenWrtModel.formatBytes(root.int64("avail") * 1024)) free of \(OpenWrtModel.formatBytes(root.int64("total") * 1024))")
            }
        }
        ForEach(snapshot.interfaces.filter { $0.name != "loopback" }) { status in
            row(status.name, ([status.up ? "Up \(OpenWrtModel.formatUptime(status.uptime))" : "Down"] + status.ipv4).joined(separator: " · "))
        }
    }

    @ViewBuilder
    private func row(_ label: String, _ value: String) -> some View {
        if !value.isEmpty {
            LabeledContent(label) { Text(value).multilineTextAlignment(.trailing).textSelection(.enabled) }
        }
    }
}

struct OpenWrtNetworkScreen: View {
    let model: OpenWrtRouterModel

    var body: some View {
        List(model.snapshot.interfaceSections) { section in
            NavigationLink {
                OpenWrtInterfaceEditor(model: model, section: section)
            } label: {
                let status = model.snapshot.status(of: section.name)
                VStack(alignment: .leading, spacing: 2) {
                    Text(section.name).font(.headline)
                    Text([OpenWrtModel.choiceLabel(OpenWrtModel.protocolChoices, section.option("proto")),
                          status.map { $0.up ? "Up" : "Down" } ?? "",
                          (status?.ipv4 ?? []).joined(separator: ", ")].filter { !$0.isEmpty }.joined(separator: " · "))
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
        }
        .navigationTitle("Network")
        .refreshable { await model.refresh() }
    }
}

struct OpenWrtInterfaceEditor: View {
    let model: OpenWrtRouterModel
    let section: UciSection
    @State private var proto = ""
    @State private var ipaddr = ""
    @State private var netmask = ""
    @State private var gateway = ""
    @State private var dns = ""
    @State private var pppoeUser = ""
    @State private var pppoePassword = ""
    @State private var dhcpOn = false
    @State private var dhcpStart = 100
    @State private var dhcpLimit = 150
    @State private var leaseTime = "12h"
    @State private var loaded = false
    @State private var confirmRestart = false

    var body: some View {
        Form {
            Section {
                Picker("Protocol", selection: $proto) {
                    ForEach(OpenWrtModel.protocolChoices) { Text($0.label).tag($0.value) }
                }
            }
            if proto == "static" {
                Section("Address") {
                    field("IP address", $ipaddr, "192.168.1.1")
                    field("Netmask", $netmask, "255.255.255.0")
                    field("Gateway", $gateway, "Optional")
                }
            }
            if proto == "pppoe" {
                Section("PPPoE") {
                    field("User", $pppoeUser, "From your internet provider")
                    SecureField("Password (empty keeps the current one)", text: $pppoePassword)
                }
            }
            if proto != "none" {
                Section("DNS servers") { field("Servers", $dns, "Optional, for example 1.1.1.1 9.9.9.9") }
            }
            if proto == "static" {
                Section("DHCP server") {
                    Toggle("Hand out addresses on this network", isOn: $dhcpOn)
                    if dhcpOn {
                        Stepper("First address: \(dhcpStart)", value: $dhcpStart, in: 1...65535)
                        Stepper("Addresses: \(dhcpLimit)", value: $dhcpLimit, in: 1...65535)
                        field("Lease time", $leaseTime, "12h")
                    }
                }
            }
            Section {
                Button("Save") { Task { await save() } }
                Button("Restart interface") { confirmRestart = true }
            } footer: {
                Text("Saved changes wait until you apply them on the Router screen.")
            }
        }
        .navigationTitle("Interface \(section.name)")
        .onAppear(perform: load)
        .confirmationDialog("Restart \(section.name)?", isPresented: $confirmRestart, titleVisibility: .visible) {
            Button("Restart") { Task { await model.restartInterface(section.name) } }
        } message: {
            Text("Devices using it lose their connection for a few seconds, and this screen may stop answering briefly.")
        }
    }

    private func field(_ label: String, _ text: Binding<String>, _ hint: String) -> some View {
        LabeledContent(label) {
            TextField(hint, text: text)
                .multilineTextAlignment(.trailing)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
        }
    }

    private func load() {
        guard !loaded else { return }
        loaded = true
        proto = section.option("proto", "none")
        ipaddr = section.option("ipaddr")
        netmask = section.option("netmask")
        gateway = section.option("gateway")
        dns = section.option("dns")
        pppoeUser = section.option("username")
        if let dhcp = model.snapshot.dhcpSection(for: section.name) {
            dhcpOn = !dhcp.flag("ignore")
            dhcpStart = Int(dhcp.option("start", "100")) ?? 100
            dhcpLimit = Int(dhcp.option("limit", "150")) ?? 150
            leaseTime = dhcp.option("leasetime", "12h")
        }
    }

    private func save() async {
        do {
            let edit = try OpenWrtModel.interfaceEdit(section, proto: proto, ipaddr: ipaddr, netmask: netmask, gateway: gateway, dns: dns,
                                                      pppoeUser: pppoeUser, pppoePassword: pppoePassword)
            guard await model.stage("network", section.name, edit, what: "Interface \(section.name)") else { return }
        } catch let problem as OpenWrtProblem {
            return model.show(problem.message, error: true)
        } catch {
            return model.show("\(error)", error: true)
        }
        guard proto == "static" else { return }
        let pool = OpenWrtModel.dhcpEdit(enabled: dhcpOn, start: dhcpStart, limit: dhcpLimit, leaseTime: leaseTime)
        if let dhcp = model.snapshot.dhcpSection(for: section.name) {
            var edit = pool
            edit.removed = edit.removed.filter { dhcp.options.contains($0) }
            await model.stage("dhcp", dhcp.name, edit, what: "DHCP server for \(section.name)")
        } else if dhcpOn {
            var values = pool.values
            values["interface"] = .string(section.name)
            await model.add("dhcp", type: "dhcp", name: OpenWrtModel.validUciName(section.name) ? section.name : "", values: values, what: "DHCP server for \(section.name)")
        }
    }
}

struct OpenWrtWifiScreen: View {
    let model: OpenWrtRouterModel
    @State private var addingTo: String?
    @State private var newSsid = ""
    @State private var newKey = ""

    var body: some View {
        List {
            ForEach(model.snapshot.radios) { radio in
                Section {
                    NavigationLink {
                        OpenWrtRadioEditor(model: model, radio: radio)
                    } label: {
                        LabeledContent(radio.name, value: "\(OpenWrtModel.wifiBand(radio)) · channel \(radio.option("channel", "auto"))\(radio.flag("disabled") ? " · off" : "")")
                    }
                    ForEach(model.snapshot.wifiNetworks(radio: radio.name)) { network in
                        NavigationLink {
                            OpenWrtWifiEditor(model: model, iface: network)
                        } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(network.option("ssid", OpenWrtModel.sectionLabel(network))).font(.headline)
                                Text("\(OpenWrtModel.choiceLabel(OpenWrtModel.encryptionChoices, network.option("encryption", "none"))) · \(network.option("network"))\(network.flag("disabled") ? " · off" : "")")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                        }
                    }
                    Button("Add Wi-Fi network") {
                        newSsid = ""
                        newKey = ""
                        addingTo = radio.name
                    }
                } header: {
                    Text(OpenWrtModel.wifiBand(radio))
                }
            }
            if model.snapshot.radios.isEmpty {
                Text("This router has no Wi-Fi radios, or the wireless settings could not be read.").foregroundStyle(.secondary)
            }
        }
        .navigationTitle("Wi-Fi")
        .refreshable { await model.refresh() }
        .alert("Add Wi-Fi network", isPresented: Binding(get: { addingTo != nil }, set: { if !$0 { addingTo = nil } })) {
            TextField("Network name", text: $newSsid)
            SecureField("Password (8 characters or more)", text: $newKey)
            Button("Add") {
                let radio = addingTo ?? ""
                Task {
                    do {
                        let networks = model.snapshot.interfaceSections.map(\.name)
                        let values = try OpenWrtModel.newWifiValues(radio: radio, network: networks.contains("lan") ? "lan" : networks.first ?? "lan", ssid: newSsid, key: newKey)
                        await model.add("wireless", type: "wifi-iface", values: values, what: "Wi-Fi network \(newSsid)")
                    } catch let problem as OpenWrtProblem {
                        model.show(problem.message, error: true)
                    } catch {}
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("The new network uses WPA2/WPA3 and connects to the local network.")
        }
    }
}

struct OpenWrtRadioEditor: View {
    let model: OpenWrtRouterModel
    let radio: UciSection
    @State private var enabled = true
    @State private var channel = ""
    @State private var country = ""
    @State private var txpower = ""
    @State private var loaded = false

    var body: some View {
        Form {
            Toggle("Radio on", isOn: $enabled)
            LabeledContent("Channel") { TextField("auto", text: $channel).multilineTextAlignment(.trailing).textInputAutocapitalization(.never) }
            LabeledContent("Country") { TextField("US", text: $country).multilineTextAlignment(.trailing).textInputAutocapitalization(.characters) }
            LabeledContent("Transmit power (dBm)") { TextField("Default", text: $txpower).multilineTextAlignment(.trailing).keyboardType(.numberPad) }
            Section {
                Button("Save") {
                    Task {
                        do {
                            let edit = try OpenWrtModel.radioEdit(radio, enabled: enabled, channel: channel, country: country, txpower: txpower)
                            await model.stage("wireless", radio.name, edit, what: "Radio \(radio.name)")
                        } catch let problem as OpenWrtProblem {
                            model.show(problem.message, error: true)
                        } catch {}
                    }
                }
            } footer: {
                Text("Use the country you are in: it sets which channels and power levels are legal.")
            }
        }
        .navigationTitle("\(radio.name) · \(OpenWrtModel.wifiBand(radio))")
        .onAppear {
            guard !loaded else { return }
            loaded = true
            enabled = !radio.flag("disabled")
            channel = radio.option("channel", "auto")
            country = radio.option("country")
            txpower = radio.option("txpower")
        }
    }
}

struct OpenWrtWifiEditor: View {
    let model: OpenWrtRouterModel
    let iface: UciSection
    @Environment(\.dismiss) private var dismiss
    @State private var enabled = true
    @State private var ssid = ""
    @State private var encryption = "sae-mixed"
    @State private var key = ""
    @State private var showKey = false
    @State private var network = ""
    @State private var hidden = false
    @State private var isolate = false
    @State private var loaded = false
    @State private var confirmDelete = false

    var body: some View {
        Form {
            Toggle("Network on", isOn: $enabled)
            LabeledContent("Name (SSID)") { TextField("Home", text: $ssid).multilineTextAlignment(.trailing).autocorrectionDisabled() }
            Picker("Security", selection: $encryption) {
                ForEach(OpenWrtModel.encryptionChoices) { Text($0.label).tag($0.value) }
            }
            if encryption != "none" && encryption != "owe" {
                LabeledContent("Password") {
                    Group {
                        if showKey { TextField("Password", text: $key) } else { SecureField("Password", text: $key) }
                    }
                    .multilineTextAlignment(.trailing)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                }
                Toggle("Show password", isOn: $showKey)
            }
            Picker("Connects to", selection: $network) {
                ForEach(model.snapshot.interfaceSections) { Text($0.name).tag($0.name) }
            }
            Toggle("Hide the network name", isOn: $hidden)
            Toggle("Keep devices apart (guest isolation)", isOn: $isolate)
            Section {
                Button("Save") {
                    Task {
                        do {
                            let edit = try OpenWrtModel.wifiEdit(iface, ssid: ssid, encryption: encryption, key: key, network: network, enabled: enabled, hidden: hidden, isolate: isolate)
                            await model.stage("wireless", iface.name, edit, what: "Wi-Fi network \(ssid)")
                        } catch let problem as OpenWrtProblem {
                            model.show(problem.message, error: true)
                        } catch {}
                    }
                }
                Button("Delete network", role: .destructive) { confirmDelete = true }
            }
        }
        .navigationTitle(iface.option("ssid", iface.name))
        .confirmationDialog("Delete \(iface.option("ssid", iface.name))?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                Task {
                    await model.delete("wireless", iface.name, what: "Wi-Fi network \(iface.option("ssid", iface.name))")
                    dismiss()
                }
            }
        } message: {
            Text("Devices connected to this network lose their connection once you apply.")
        }
        .onAppear {
            guard !loaded else { return }
            loaded = true
            enabled = !iface.flag("disabled")
            ssid = iface.option("ssid")
            encryption = iface.option("encryption", "none")
            if !OpenWrtModel.encryptionChoices.contains(where: { $0.value == encryption }) { encryption = "sae-mixed" }
            key = iface.option("key")
            network = String(iface.option("network").split(separator: " ").first ?? "")
            hidden = iface.flag("hidden")
            isolate = iface.flag("isolate")
        }
    }
}

struct OpenWrtDevicesScreen: View {
    let model: OpenWrtRouterModel
    @State private var name = ""
    @State private var mac = ""
    @State private var ip = ""

    var body: some View {
        let now = Int64(Date().timeIntervalSince1970)
        let leased = Set(model.snapshot.leases.map(\.mac))
        List {
            Section("Connected") {
                ForEach(model.snapshot.leases) { lease in
                    let reservation = model.snapshot.reservation(for: lease.mac)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(lease.hostname.isEmpty ? (reservation?.option("name") ?? lease.ip) : lease.hostname).font(.headline)
                        Text("\(lease.ip) · \(lease.mac) · \(OpenWrtModel.leaseExpiry(lease.expires, now: now))\(reservation == nil ? "" : " · reserved")")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    .swipeActions {
                        if let reservation {
                            Button("Remove reservation", role: .destructive) {
                                Task { await model.delete("dhcp", reservation.name, what: "Reservation for \(lease.ip)") }
                            }
                        } else {
                            Button("Reserve") {
                                Task {
                                    if let values = try? OpenWrtModel.reservationValues(name: OpenWrtModel.sanitizedHostname(lease.hostname, mac: lease.mac), mac: lease.mac, ip: lease.ip) {
                                        await model.add("dhcp", type: "host", values: values, what: "Reservation of \(lease.ip)")
                                    }
                                }
                            }
                            .tint(.accentColor)
                        }
                    }
                }
                if model.snapshot.leases.isEmpty { Text("No devices have an address from the router right now.").foregroundStyle(.secondary) }
            }
            let offline = model.snapshot.reservations.filter { !leased.contains(String($0.option("mac").lowercased().split(separator: " ").first ?? "")) }
            if !offline.isEmpty {
                Section("Reserved, not connected") {
                    ForEach(offline) { host in
                        LabeledContent(host.option("name", host.name), value: host.option("ip"))
                            .swipeActions {
                                Button("Remove", role: .destructive) { Task { await model.delete("dhcp", host.name, what: "Reservation \(host.option("name"))") } }
                            }
                    }
                }
            }
            Section {
                TextField("Name", text: $name).autocorrectionDisabled()
                TextField("MAC address", text: $mac).textInputAutocapitalization(.never).autocorrectionDisabled()
                TextField("IP address", text: $ip).keyboardType(.numbersAndPunctuation)
                Button("Add reservation") {
                    Task {
                        do {
                            let values = try OpenWrtModel.reservationValues(name: name, mac: mac, ip: ip)
                            if await model.add("dhcp", type: "host", values: values, what: "Reservation of \(ip)") {
                                name = ""
                                mac = ""
                                ip = ""
                            }
                        } catch let problem as OpenWrtProblem {
                            model.show(problem.message, error: true)
                        } catch {}
                    }
                }
            } header: {
                Text("Reserve an address")
            } footer: {
                Text("A reserved device always gets the same address. Swipe a connected device to reserve its current address.")
            }
        }
        .navigationTitle("Devices")
        .refreshable { await model.refresh() }
    }
}

struct OpenWrtForwardsScreen: View {
    let model: OpenWrtRouterModel

    var body: some View {
        List {
            ForEach(model.snapshot.forwards) { redirect in
                NavigationLink {
                    OpenWrtForwardEditor(model: model, existing: redirect)
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(redirect.option("name", OpenWrtModel.sectionLabel(redirect))).font(.headline)
                        Text("\(OpenWrtModel.choiceLabel(OpenWrtModel.forwardProtocolChoices, redirect.option("proto", "tcp udp"))) \(redirect.option("src_dport")) → \(redirect.option("dest_ip")):\(redirect.option("dest_port", redirect.option("src_dport")))\(redirect.flag("enabled", true) ? "" : " · off")")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
            if model.snapshot.forwards.isEmpty {
                Text("No port forwards. A port forward lets a device on the internet reach a device on your network.").foregroundStyle(.secondary)
            }
        }
        .navigationTitle("Port forwards")
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                NavigationLink { OpenWrtForwardEditor(model: model, existing: nil) } label: { Image(systemName: "plus") }
                    .accessibilityLabel("New port forward")
            }
        }
        .refreshable { await model.refresh() }
    }
}

struct OpenWrtForwardEditor: View {
    let model: OpenWrtRouterModel
    let existing: UciSection?
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var proto = "tcp udp"
    @State private var external = ""
    @State private var ip = ""
    @State private var target = ""
    @State private var enabled = true
    @State private var loaded = false
    @State private var confirmDelete = false

    var body: some View {
        Form {
            LabeledContent("Name") { TextField("Game server", text: $name).multilineTextAlignment(.trailing) }
            Picker("Protocol", selection: $proto) {
                ForEach(OpenWrtModel.forwardProtocolChoices) { Text($0.label).tag($0.value) }
            }
            LabeledContent("External port") { TextField("8080 or 6000-6010", text: $external).multilineTextAlignment(.trailing).keyboardType(.numbersAndPunctuation) }
            LabeledContent("To device") { TextField("192.168.1.20", text: $ip).multilineTextAlignment(.trailing).keyboardType(.numbersAndPunctuation) }
            LabeledContent("To port") { TextField("Same as external", text: $target).multilineTextAlignment(.trailing).keyboardType(.numbersAndPunctuation) }
            Toggle("On", isOn: $enabled)
            Section {
                Button("Save") {
                    Task {
                        do {
                            let edit = try OpenWrtModel.forwardEdit(existing, name: name, proto: proto, externalPort: external, ip: ip, internalPort: target, enabled: enabled)
                            var saved = false
                            if let existing {
                                saved = await model.stage("firewall", existing.name, edit, what: "Port forward \(name)")
                            } else {
                                saved = await model.add("firewall", type: "redirect", values: edit.values, what: "Port forward \(name)")
                            }
                            if saved { dismiss() }
                        } catch let problem as OpenWrtProblem {
                            model.show(problem.message, error: true)
                        } catch {}
                    }
                }
                if existing != nil { Button("Delete", role: .destructive) { confirmDelete = true } }
            } footer: {
                if !model.message.isEmpty && model.messageIsError { Text(model.message).foregroundStyle(.red) }
            }
        }
        .navigationTitle(existing == nil ? "New port forward" : "Port forward")
        .confirmationDialog("Delete \(name)?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                Task {
                    if let existing { await model.delete("firewall", existing.name, what: "Port forward \(name)") }
                    dismiss()
                }
            }
        } message: {
            Text("The port stops reaching the device once you apply.")
        }
        .onAppear {
            guard !loaded, let existing else { return }
            loaded = true
            name = existing.option("name")
            proto = existing.option("proto", "tcp udp")
            external = existing.option("src_dport")
            ip = existing.option("dest_ip")
            target = existing.option("dest_port")
            enabled = existing.flag("enabled", true)
        }
    }
}

struct OpenWrtStorageScreen: View {
    let model: OpenWrtRouterModel
    @State private var bootDisk: OpenWrtDisk?
    @State private var bootPath = ""
    @State private var shareDisk: OpenWrtDisk?
    @State private var shareName = ""
    @State private var shareGuests = false
    @State private var unmountDisk: OpenWrtDisk?
    @State private var confirmInstall = false
    @State private var confirmShareInstall = false

    var body: some View {
        let disks = model.snapshot.disks
        List {
            if !model.message.isEmpty { OpenWrtMessage(model: model) }
            if model.snapshot.pendingCount > 0 {
                Section {
                    Text(model.snapshot.pendingCount == 1 ? "1 change is waiting. Disks mount by themselves only after it is applied."
                         : "\(model.snapshot.pendingCount) changes are waiting. Disks mount by themselves only after they are applied.")
                    Button("Apply and mount disks") { Task { await model.applyAndMount() } }
                        .disabled(model.busy || model.storageBusy)
                }
            }
            Section {
                Toggle("Mount any disk when it is plugged in", isOn: Binding(
                    get: { model.snapshot.autoMount },
                    set: { on in Task { await model.setAutoMount(on) } }))
                    .disabled(model.storageBusy)
                Button("Mount disks now") { Task { await model.mountAll() } }
                Button("Install USB storage support") { confirmInstall = true }.disabled(model.storageBusy)
                Button("Check USB storage") { Task { await model.checkStorage() } }.disabled(model.storageBusy)
            } footer: {
                Text(!model.snapshot.storageProblem.isEmpty
                     ? "The router cannot list its disks (\(model.snapshot.storageProblem)). Install USB storage support, which adds block-mount and the USB and filesystem drivers."
                     : disks.isEmpty ? "No USB disk or memory card is listed. Plug one in and pull to refresh, or press Check USB storage to see why." : "Unmount a disk before you unplug it.")
            }
            if !model.storageLog.isEmpty {
                Section("USB storage check") {
                    Text(model.storageLog)
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                        .accessibilityIdentifier("openwrt-storage-log")
                }
            }
            Section("Disks") {
                ForEach(disks) { disk in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(disk.title).font(.headline)
                        Text(diskDetail(disk)).font(.caption).foregroundStyle(.secondary)
                    }
                    .contextMenu { diskActions(disk) }
                    .swipeActions { diskActions(disk) }
                }
            }
            Section {
                ForEach(model.snapshot.shares) { share in
                    NavigationLink {
                        NetworkShareEntryScreen(share: NetworkShare(address: model.routerHost, share: share.name, guest: share.guests))
                    } label: {
                        VStack(alignment: .leading, spacing: 2) {
                            LabeledContent(share.name, value: "\(share.path)\(share.readOnly ? " · read only" : "")\(share.guests ? " · guests" : "")")
                            Text("Browse files").font(.caption).foregroundStyle(.tint)
                        }
                    }
                    .accessibilityIdentifier("openwrt-share-browse-\(share.name)")
                    .swipeActions {
                        Button("Stop sharing", role: .destructive) {
                            Task { await model.delete(model.snapshot.shareConfig, share.section, what: "Share \(share.name)") }
                        }
                    }
                }
                if model.snapshot.shareConfig.isEmpty {
                    Button("Install network sharing") { confirmShareInstall = true }
                        .disabled(model.storageBusy || model.busy || !model.signedIn)
                        .accessibilityIdentifier("openwrt-install-sharing")
                }
            } header: {
                Text("Network shares")
            } footer: {
                Text(model.snapshot.shareConfig.isEmpty
                     ? "To share disks with computers and phones, install ksmbd-server and luci-app-ksmbd."
                     : "Shared with the \(model.snapshot.shareConfig) file server (SMB). Swipe a mounted disk to share it; tap a share to browse its files.")
            }
        }
        .navigationTitle("Storage")
        .refreshable { await model.refresh() }
        .overlay { if model.storageBusy { ProgressView() } }
        .task {
            if model.snapshot.disks.isEmpty && model.storageLog.isEmpty { await model.checkStorage() }
        }
        .alert("Mount at boot", isPresented: Binding(get: { bootDisk != nil }, set: { if !$0 { bootDisk = nil } })) {
            TextField("/mnt/usb", text: $bootPath).textInputAutocapitalization(.never).autocorrectionDisabled()
            Button("Save") {
                if let disk = bootDisk { Task { await model.mountAtBoot(disk, path: bootPath.trimmingCharacters(in: .whitespaces)) } }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Folder on the router where the disk appears.")
        }
        .alert("Share on network", isPresented: Binding(get: { shareDisk != nil }, set: { if !$0 { shareDisk = nil } })) {
            TextField("Name", text: $shareName).autocorrectionDisabled()
            Button("Share, password needed") {
                if let disk = shareDisk { Task { await model.share(disk, name: shareName.trimmingCharacters(in: .whitespaces), guests: false) } }
            }
            Button("Share, anyone on the network") {
                if let disk = shareDisk { Task { await model.share(disk, name: shareName.trimmingCharacters(in: .whitespaces), guests: true) } }
            }
            Button("Cancel", role: .cancel) {}
        }
        .confirmationDialog("Unmount \(unmountDisk?.title ?? "")?", isPresented: Binding(get: { unmountDisk != nil }, set: { if !$0 { unmountDisk = nil } }), titleVisibility: .visible) {
            Button("Unmount") { if let disk = unmountDisk { Task { await model.unmount(disk) } } }
        } message: {
            Text("Shares and programs using the disk lose access. Afterwards it can be unplugged safely.")
        }
        .confirmationDialog("Install USB storage support?", isPresented: $confirmInstall, titleVisibility: .visible) {
            Button("Install") { Task { await model.installStorageSupport() } }
        } message: {
            Text("This updates the router's package lists and installs \(OpenWrtModel.usbStoragePackages.joined(separator: ", ")). The router needs internet access to download them.")
        }
        .confirmationDialog("Install network sharing?", isPresented: $confirmShareInstall, titleVisibility: .visible) {
            Button("Install") { Task { await model.installShareSupport() } }
        } message: {
            Text("This updates the router's package lists and installs \(OpenWrtModel.shareSupportPackages.joined(separator: " and ")), the file server and its settings page. The router needs internet access to download them.")
        }
    }

    private func diskDetail(_ disk: OpenWrtDisk) -> String {
        var parts = [disk.block.type.isEmpty ? "Unknown filesystem" : disk.block.type]
        if disk.size > 0 { parts.append(OpenWrtModel.formatBytes(disk.size)) }
        if disk.usageKnown { parts.append("\(OpenWrtModel.formatBytes(disk.avail)) free") }
        parts.append(disk.mounted ? "at \(disk.mount)" : "not mounted")
        if !disk.fstabSection.isEmpty { parts.append(disk.bootEnabled ? "mounts at boot" : "boot mount off") }
        return parts.joined(separator: " · ")
    }

    @ViewBuilder
    private func diskActions(_ disk: OpenWrtDisk) -> some View {
        Button("Mount at boot…") {
            bootPath = disk.bootTarget.isEmpty ? (disk.mounted ? disk.mount : OpenWrtModel.suggestedMountPath(disk.block)) : disk.bootTarget
            bootDisk = disk
        }
        if !disk.fstabSection.isEmpty {
            Button("Don't mount at boot") { Task { await model.delete("fstab", disk.fstabSection, what: "Mount for \(disk.title)") } }
        }
        if disk.mounted {
            Button("Share…") {
                shareName = String((disk.block.label.isEmpty ? disk.block.name : disk.block.label).map { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "." ? $0 : "_" })
                shareDisk = disk
            }
            Button("Unmount", role: .destructive) { unmountDisk = disk }
        }
    }
}

struct OpenWrtServicesScreen: View {
    let model: OpenWrtRouterModel
    @State private var risky: (name: String, action: String)?

    var body: some View {
        List(model.snapshot.services) { service in
            Menu {
                Button("Start") { run(service.name, "start") }
                Button("Restart") { run(service.name, "restart") }
                Button("Stop", role: .destructive) { run(service.name, "stop") }
                Divider()
                Button("Start at boot") { run(service.name, "enable") }
                Button("Don't start at boot") { run(service.name, "disable") }
            } label: {
                HStack {
                    Text(service.name).foregroundStyle(.primary)
                    Spacer()
                    Text("\(service.running ? "Running" : "Stopped")\(service.enabled ? " · at boot" : "")").font(.caption).foregroundStyle(.secondary)
                }
            }
        }
        .navigationTitle("Services")
        .refreshable { await model.refresh() }
        .confirmationDialog("This service keeps the router reachable",
                            isPresented: Binding(get: { risky != nil }, set: { if !$0 { risky = nil } }), titleVisibility: .visible) {
            Button("Continue", role: .destructive) {
                if let risky { Task { await model.serviceAction(risky.name, risky.action) } }
            }
        } message: {
            Text("Stopping \(risky?.name ?? "it") can cut your network off or lock you out until the router restarts.")
        }
    }

    private func run(_ name: String, _ action: String) {
        if (action == "stop" || action == "disable") && OpenWrtRouterModel.criticalServices.contains(name) {
            risky = (name, action)
        } else {
            Task { await model.serviceAction(name, action) }
        }
    }
}

struct OpenWrtPackagesScreen: View {
    let model: OpenWrtRouterModel
    @State private var query = ""
    @State private var install = ""
    @State private var removing: String?

    var body: some View {
        let shown = query.isEmpty ? model.snapshot.packages : model.snapshot.packages.filter { $0.name.localizedCaseInsensitiveContains(query) }
        List {
            Section {
                TextField("Package to install, for example luci-app-sqm", text: $install)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                HStack {
                    Button("Install") { Task { await model.packageAction("install", install.split(separator: " ").map(String.init)) } }
                        .disabled(install.trimmingCharacters(in: .whitespaces).isEmpty || model.busy)
                    Spacer()
                    Button("Update lists") { Task { await model.packageAction("update", []) } }.disabled(model.busy)
                }
                .buttonStyle(.borderless)
                if !model.packageOutput.isEmpty {
                    Text(model.packageOutput).font(.caption.monospaced()).textSelection(.enabled)
                }
            }
            Section("Installed (\(model.snapshot.packages.count))") {
                ForEach(shown) { package in
                    LabeledContent(package.name, value: package.version)
                        .swipeActions { Button("Remove", role: .destructive) { removing = package.name } }
                }
            }
        }
        .navigationTitle("Packages")
        .searchable(text: $query, prompt: "Filter installed packages")
        .confirmationDialog("Remove \(removing ?? "")?", isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } }), titleVisibility: .visible) {
            Button("Remove", role: .destructive) { if let removing { Task { await model.packageAction("remove", [removing]) } } }
        } message: {
            Text("Removing a package the router depends on can break it until you reinstall the package or reset the router.")
        }
    }
}

struct OpenWrtSystemScreen: View {
    let model: OpenWrtRouterModel
    @State private var hostname = ""
    @State private var newPassword = ""
    @State private var confirmPassword = ""
    @State private var loaded = false

    var body: some View {
        Form {
            Section("Hostname") {
                TextField("OpenWrt", text: $hostname).textInputAutocapitalization(.never).autocorrectionDisabled()
                Button("Save") {
                    Task {
                        if let problem = OpenWrtModel.hostnameProblem(hostname) { return model.show(problem, error: true) }
                        guard let system = model.snapshot.sections("system", type: "system").first else { return }
                        await model.stage("system", system.name, UciEdit(values: ["hostname": .string(hostname)]), what: "Hostname")
                    }
                }
            }
            Section {
                SecureField("New password", text: $newPassword)
                SecureField("Again", text: $confirmPassword)
                Button("Change password") {
                    Task {
                        if await model.changePassword(newPassword, confirm: confirmPassword) {
                            newPassword = ""
                            confirmPassword = ""
                        }
                    }
                }
            } header: {
                Text("Router password")
            } footer: {
                Text("The password for the account you signed in with. It also protects SSH and LuCI, and takes effect at once.")
            }
            Section {
                Text("Backups, firmware upgrades and factory resets are on the ForkMesh desktop app, where the files live.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
        }
        .navigationTitle("System")
        .onAppear {
            guard !loaded else { return }
            loaded = true
            hostname = model.snapshot.hostname
        }
    }
}

struct OpenWrtSettingsScreen: View {
    let model: OpenWrtRouterModel
    @State private var config = "network"
    @State private var newType = ""
    @State private var newName = ""

    var body: some View {
        List {
            Section {
                Picker("Configuration", selection: $config) {
                    ForEach(model.snapshot.configNames, id: \.self) { Text($0).tag($0) }
                }
            } footer: {
                Text("Every UCI setting on the router. Edits wait until you apply them.")
            }
            Section("Sections") {
                ForEach(model.snapshot.sections(config)) { section in
                    NavigationLink {
                        OpenWrtSectionEditor(model: model, config: config, name: section.name)
                    } label: {
                        LabeledContent(OpenWrtModel.sectionLabel(section), value: section.type)
                    }
                }
            }
            Section("Add a section") {
                TextField("Type, for example rule", text: $newType).textInputAutocapitalization(.never).autocorrectionDisabled()
                TextField("Optional name", text: $newName).textInputAutocapitalization(.never).autocorrectionDisabled()
                Button("Add section") {
                    Task {
                        guard OpenWrtModel.validUciName(newType), newName.isEmpty || OpenWrtModel.validUciName(newName) else {
                            return model.show("A section type and name use letters, digits and underscores only.", error: true)
                        }
                        if await model.add(config, type: newType, name: newName, values: JSONObject(), what: "Section \(newName.isEmpty ? newType : newName)") {
                            newType = ""
                            newName = ""
                        }
                    }
                }
            }
        }
        .navigationTitle("All settings")
        .task(id: config) { await loadConfig() }
    }

    private func loadConfig() async {
        if model.snapshot.configs[config] == nil { await model.reloadConfig(config) }
    }
}

struct OpenWrtSectionEditor: View {
    let model: OpenWrtRouterModel
    let config: String
    let name: String
    @Environment(\.dismiss) private var dismiss
    @State private var editing: String?
    @State private var value = ""
    @State private var newOption = ""
    @State private var newValue = ""
    @State private var confirmDelete = false

    var body: some View {
        let section = model.snapshot.section(config, name)
        List {
            if let section {
                Section("\(config).\(section.name) · \(section.type)") {
                    ForEach(section.options.keys.sorted(), id: \.self) { key in
                        Button {
                            value = section.option(key)
                            editing = key
                        } label: {
                            LabeledContent(key, value: OpenWrtModel.isSecretOption(key) ? "••••••" : section.option(key))
                        }
                        .foregroundStyle(.primary)
                        .swipeActions {
                            Button("Delete", role: .destructive) {
                                Task { await model.stage(config, name, UciEdit(removed: [key]), what: "\(config).\(name).\(key)") }
                            }
                        }
                    }
                }
                Section("Add an option") {
                    TextField("option_name", text: $newOption).textInputAutocapitalization(.never).autocorrectionDisabled()
                    TextField("Value (spaces make a list)", text: $newValue).textInputAutocapitalization(.never).autocorrectionDisabled()
                    Button("Set option") {
                        Task {
                            guard OpenWrtModel.validUciName(newOption) else {
                                return model.show("An option name uses letters, digits and underscores only.", error: true)
                            }
                            let list = newValue.contains(" ")
                            await model.stage(config, name, UciEdit(values: [newOption: OpenWrtModel.optionValue(newValue, list: list)]), what: "\(config).\(name).\(newOption)")
                            newOption = ""
                            newValue = ""
                        }
                    }
                }
                Section {
                    Button("Delete section", role: .destructive) { confirmDelete = true }
                }
            } else {
                Text("This section no longer exists.").foregroundStyle(.secondary)
            }
        }
        .navigationTitle(name)
        .alert(editing ?? "", isPresented: Binding(get: { editing != nil }, set: { if !$0 { editing = nil } })) {
            TextField("Value", text: $value).textInputAutocapitalization(.never).autocorrectionDisabled()
            Button("Save") {
                guard let key = editing, let section else { return }
                let isList = section.isList(key)
                Task {
                    let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
                    let edit = trimmed.isEmpty ? UciEdit(removed: [key]) : UciEdit(values: [key: OpenWrtModel.optionValue(trimmed, list: isList)])
                    await model.stage(config, name, edit, what: "\(config).\(name).\(key)")
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Empty removes the option. For a list, separate values with spaces.")
        }
        .confirmationDialog("Delete \(name)?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                Task {
                    await model.delete(config, name, what: "\(config).\(name)")
                    dismiss()
                }
            }
        } message: {
            Text("The whole section is removed once you apply.")
        }
    }
}
