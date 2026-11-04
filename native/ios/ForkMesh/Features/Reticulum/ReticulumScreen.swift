import FMBase
import FMPackages
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct ReticulumEditing: Identifiable {
    let id = UUID()
    let config: ReticulumInterfaceConfig
    let creating: Bool
}

struct ReticulumScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    @State private var hub = ReticulumAppState.hub
    @State private var editing: ReticulumEditing?
    @State private var importing = false
    @State private var removing: ReticulumInterfaceConfig?
    @State private var renewing = false
    @State private var composing = false

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: FmSpace.x3, pinnedViews: [.sectionHeaders]) {
                Text(hub.summary)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .accessibilityIdentifier("reticulum-summary")
                if let notice = hub.notice {
                    FmNoticeLine(notice.text, isError: notice.isError, onDismiss: { hub.notice = nil })
                }
                if hub.showsOnboarding { onboarding }
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: FmSpace.x2) {
                        ForEach(hub.tiles) { tile in ReticulumStatTile(tile: tile) }
                    }
                }
                Section {
                    content
                } header: {
                    ScrollView(.horizontal, showsIndicators: false) {
                        FmSegmentTabBar(
                            tabs: ReticulumTab.allCases.map { tab in
                                FmSegmentTab(label: tab.title, count: tab == .messages && hub.unreadCount > 0 ? hub.unreadCount : nil)
                            },
                            index: hub.tab.rawValue,
                            onChanged: { index in
                                FmHaptics.tap()
                                hub.tab = ReticulumTab(rawValue: index) ?? .network
                            })
                    }
                    .background(fm.canvas)
                }
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.bottom, FmSpace.x6)
        }
        .background(fm.canvas)
        .navigationTitle(ReticulumHub.title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Button { hub.announceNow() } label: {
                    Label("Announce", systemImage: "dot.radiowaves.left.and.right")
                }
                .disabled(!hub.canAnnounce)
                .accessibilityIdentifier("reticulum-announce")
                Menu {
                    Button(hub.running ? "Disconnect" : "Connect", systemImage: hub.running ? "bolt.slash" : "bolt") { hub.toggleNetwork() }
                    Button("Add interface", systemImage: "plus") { addInterface() }
                    Menu("Add public entry point") {
                        ForEach(ReticulumInterfaceConfig.publicEntryPoints) { entry in
                            Button("\(entry.name) (\(entry.endpoint))") { hub.addPublicEntryPoint(entry) }
                        }
                    }
                    Button("Import Reticulum config…", systemImage: "doc.badge.plus") { importing = true }
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                .accessibilityIdentifier("reticulum-menu")
            }
        }
        .onAppear {
            hub.visible = true
            hub.open()
        }
        .onDisappear { hub.visible = false }
        .onChange(of: scenePhase) { _, phase in
            if phase == .active { hub.open() }
            if phase == .background { hub.saveNow() }
        }
        .navigationDestination(isPresented: Binding(get: { !hub.currentPeer.isEmpty }, set: { if !$0 { hub.closeConversation() } })) {
            ReticulumConversationScreen(hub: hub)
        }
        .sheet(isPresented: $composing) {
            ReticulumNewMessageSheet(hub: hub)
        }
        .sheet(item: $editing) { target in
            ReticulumInterfaceEditor(hub: hub, config: target.config, creating: target.creating)
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.plainText, .data, .item]) { result in
            importConfig(result)
        }
        .fmDialog(isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } })) {
            FmConfirmDialog(
                title: "Remove interface",
                message: "Remove \(removing?.name ?? "")? Destinations heard through it stay in the list.",
                confirmLabel: "Remove",
                destructive: true,
                onCancel: { removing = nil },
                onConfirm: {
                    if let config = removing { hub.removeInterface(config.id) }
                    removing = nil
                })
        }
        .fmDialog(isPresented: $renewing) {
            FmConfirmDialog(
                title: "Make a new identity",
                message: "This \(hub.deviceNoun) gets a new destination hash. Anyone who knew the old one can no longer reach this \(hub.deviceNoun) through it. Continue?",
                confirmLabel: "Make a new identity",
                destructive: true,
                onCancel: { renewing = false },
                onConfirm: {
                    renewing = false
                    hub.newIdentity()
                })
        }
    }

    private var onboarding: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text("Reticulum is an encrypted mesh network that runs over the internet, LoRa radios, packet radio and local networks. Join a public entry point to see it, or import the interfaces from a Reticulum config file.")
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                HStack(spacing: FmSpace.x2) {
                    FmFilledButton("Join the public network", onPressed: { hub.joinPublicNetwork() })
                        .accessibilityIdentifier("reticulum-join")
                    FmOutlinedButton("Import a config", onPressed: { importing = true })
                }
            }
        }
    }

    @ViewBuilder private var content: some View {
        switch hub.tab {
        case .network:
            ReticulumNetworkTab(hub: hub)
        case .messages:
            ReticulumMessagesTab(hub: hub, onNew: { composing = true })
        case .interfaces:
            ReticulumInterfacesTab(hub: hub, onAdd: addInterface, onImport: { importing = true },
                                   onEdit: { editing = ReticulumEditing(config: $0, creating: false) },
                                   onRemove: { removing = $0 },
                                   onUseDiscovered: { config in editing = ReticulumEditing(config: config, creating: true) })
        case .destinations:
            ReticulumDestinationsTab(hub: hub)
        case .activity:
            ReticulumActivityTab(hub: hub)
        case .identity:
            ReticulumIdentityTab(hub: hub, onRenew: { renewing = true })
        }
    }

    private func addInterface() {
        editing = ReticulumEditing(config: ReticulumInterfaceConfig(id: ReticulumInterfaceConfig.newId(), name: "New interface"), creating: true)
    }

    private func importConfig(_ result: Result<URL, Error>) {
        guard case .success(let url) = result else { return }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
        guard size <= 1024 * 1024, let data = try? Data(contentsOf: url) else {
            hub.show("Could not read \(url.lastPathComponent).", error: true)
            return
        }
        hub.importConfig(String(decoding: data, as: UTF8.self), from: url.lastPathComponent)
    }
}

enum ReticulumCopy {
    @MainActor
    static func copy(_ text: String, _ what: String, hub: ReticulumHub) {
        guard !text.isEmpty else { return }
        UIPasteboard.general.string = text
        FmHaptics.tap()
        hub.show("\(what) copied.")
    }
}

struct ReticulumInterfaceEditor: View {
    @Environment(\.dismiss) private var dismiss
    let hub: ReticulumHub
    let creating: Bool
    @State private var config: ReticulumInterfaceConfig
    @State private var port: String
    @State private var problem = ""

    init(hub: ReticulumHub, config: ReticulumInterfaceConfig, creating: Bool) {
        self.hub = hub
        self.creating = creating
        _config = State(initialValue: config)
        _port = State(initialValue: String(config.port))
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Name", text: $config.name)
                        .accessibilityIdentifier("reticulum-editor-name")
                    TextField("rmap.world", text: $config.host)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .accessibilityIdentifier("reticulum-editor-host")
                    TextField("Port", text: $port)
                        .keyboardType(.numberPad)
                        .accessibilityIdentifier("reticulum-editor-port")
                    Toggle("KISS framing (for a TNC or modem reached over TCP)", isOn: $config.kiss)
                } header: {
                    Text("TCP connection to a Reticulum node")
                } footer: {
                    Text("A node's TCPServerInterface or BackboneInterface, such as rmap.world port 4242. There is no Reticulum daemon on \(hub.deviceNoun), and UDP on the local network is not offered here.")
                }
                Section {
                    TextField("Network name", text: $config.networkName, prompt: Text("Leave empty for an open network"))
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .accessibilityIdentifier("reticulum-editor-network")
                    SecureField("Passphrase", text: $config.passphrase, prompt: Text("Leave empty for an open network"))
                        .accessibilityIdentifier("reticulum-editor-passphrase")
                    Stepper("Access code size: \(config.accessCodeSize) bytes", value: $config.accessCodeSize, in: 1...64)
                } header: {
                    Text("Interface access code")
                } footer: {
                    if !problem.isEmpty {
                        Text(problem).foregroundStyle(Color.red)
                    } else {
                        Text("A virtual network on a shared medium (IFAC): only nodes with the same name and passphrase see each other's packets. The passphrase is kept in the Keychain.")
                    }
                }
            }
            .navigationTitle(creating ? "Add a Reticulum interface" : "Edit interface")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { save() }
                        .accessibilityIdentifier("reticulum-editor-save")
                }
            }
        }
        .presentationDetents([.medium, .large])
    }

    private func save() {
        var result = config
        result.name = result.name.trimmingCharacters(in: .whitespacesAndNewlines)
        result.host = result.host.trimmingCharacters(in: .whitespacesAndNewlines)
        result.networkName = result.networkName.trimmingCharacters(in: .whitespacesAndNewlines)
        result.port = Int(port.trimmingCharacters(in: .whitespaces)) ?? 0
        if let error = hub.saveInterface(result, creating: creating) {
            problem = error
            return
        }
        dismiss()
    }
}
