import FMBase
import FMPackages
import SwiftUI

struct ReticulumRowsView: View {
    @Environment(\.fmPalette) private var fm
    let rows: [ReticulumRow]

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            ForEach(rows) { row in
                HStack(alignment: .firstTextBaseline, spacing: FmSpace.x3) {
                    Text(row.label)
                        .fmText(.labelMedium, color: fm.textTertiary)
                        .frame(width: 112, alignment: .leading)
                    Group {
                        if row.mono {
                            Text(row.value).fmMono(color: fm.textPrimary)
                        } else {
                            Text(row.value).fmText(.bodySmall)
                        }
                    }
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
    }
}

struct ReticulumNetworkTab: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var hub: ReticulumHub

    var body: some View {
        let records = hub.destinationList
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            FmTextField(text: $hub.mapFilter, hint: "Highlight by name or hash")
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .accessibilityIdentifier("reticulum-map-filter")
            FmCard(padding: EdgeInsets.fmAll(FmSpace.x2)) {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    ReticulumNetworkMap(
                        spokes: hub.spokes, destinations: records, selected: hub.selected, filter: hub.mapFilter,
                        selfOnline: hub.canAnnounce, selfLabel: "This \(hub.deviceNoun)", pulses: hub.activePulses(at: hub.now),
                        now: hub.now, onSelect: { hub.select($0) })
                    ReticulumLegend(destinations: records)
                }
            }
            FmCard { ReticulumTrafficChart(samples: hub.history) }
            ReticulumSelectedCard(hub: hub)
        }
        .padding(.top, FmSpace.x2)
    }
}

struct ReticulumSelectedCard: View {
    @Environment(\.fmPalette) private var fm
    let hub: ReticulumHub

    var body: some View {
        let record = hub.selectedRecord
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text("Selected destination").fmText(.labelLarge, color: fm.textSecondary)
                HStack(spacing: FmSpace.x2) {
                    if let record { ReticulumMarkerIcon(group: record.group, size: 14) }
                    Text(hub.detailTitle).fmText(.titleSmall).textSelection(.enabled)
                }
                if record == nil {
                    Text("Tap a point on the map or a row in Destinations.")
                        .fmText(.bodySmall, color: fm.textSecondary)
                } else {
                    ReticulumRowsView(rows: hub.detailRows)
                    HStack(spacing: FmSpace.x2) {
                        if record?.aspect == ReticulumLxmf.deliveryAspect {
                            FmFilledButton("Message", onPressed: { hub.openConversation(hub.selected) })
                                .accessibilityIdentifier("reticulum-message-peer")
                        }
                        FmOutlinedButton("Find path", onPressed: hub.canAnnounce ? { hub.requestPath(hub.selected) } : nil)
                        FmOutlinedButton("Copy hash", onPressed: { ReticulumCopy.copy(ReticulumWire.hex(hub.selected), "Destination hash", hub: hub) })
                        FmTextButton("Forget", color: fm.danger, onPressed: { hub.forget(hub.selected) })
                    }
                }
            }
        }
        .accessibilityIdentifier("reticulum-selected")
    }
}

struct ReticulumInterfacesTab: View {
    @Environment(\.fmPalette) private var fm
    let hub: ReticulumHub
    let onAdd: () -> Void
    let onImport: () -> Void
    let onEdit: (ReticulumInterfaceConfig) -> Void
    let onRemove: (ReticulumInterfaceConfig) -> Void
    let onUseDiscovered: (ReticulumInterfaceConfig) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: FmSpace.x2) {
                    FmFilledButton("Add interface", icon: "add", onPressed: onAdd)
                        .accessibilityIdentifier("reticulum-add-interface")
                    Menu {
                        ForEach(ReticulumInterfaceConfig.publicEntryPoints) { entry in
                            Button("\(entry.name) (\(entry.endpoint))") { hub.addPublicEntryPoint(entry) }
                        }
                    } label: {
                        Text("Add public entry point")
                            .fmText(.labelLarge, color: fm.textPrimary)
                            .padding(.horizontal, FmSpace.x4)
                            .frame(minHeight: 40)
                            .overlay(Capsule().strokeBorder(fm.border, lineWidth: 1))
                    }
                    FmOutlinedButton("Import Reticulum config…", onPressed: onImport)
                }
            }
            let rows = hub.interfaceRows
            if rows.isEmpty {
                Text("No interfaces yet. Add a TCP connection to a Reticulum node, or a public entry point.")
                    .fmText(.bodySmall, color: fm.textSecondary)
            }
            ForEach(rows) { row in
                ReticulumInterfaceCard(hub: hub, row: row, onEdit: onEdit, onRemove: onRemove)
            }
            FmSectionHeader(title: "Entry points announced on the network").padding(.top, FmSpace.x2)
            let discovered = hub.discoveredRows
            if discovered.isEmpty {
                Text("None heard yet. Transport nodes that publish their interfaces appear here with their proof of work.")
                    .fmText(.bodySmall, color: fm.textSecondary)
            }
            ForEach(discovered) { row in
                ReticulumDiscoveredCard(row: row) {
                    if let draft = hub.discoveredDraft(row.hash) { onUseDiscovered(draft) }
                }
            }
        }
        .padding(.top, FmSpace.x2)
    }
}

struct ReticulumStateDot: View {
    @Environment(\.fmPalette) private var fm
    let state: ReticulumLinkState

    var body: some View {
        let color: Color = {
            switch state {
            case .online: return fm.success
            case .connecting, .waiting: return fm.warning
            case .blocked, .failed: return fm.danger
            case .disabled: return fm.textTertiary
            }
        }()
        Circle().fill(color).frame(width: 9, height: 9).accessibilityHidden(true)
    }
}

struct ReticulumInterfaceCard: View {
    @Environment(\.fmPalette) private var fm
    let hub: ReticulumHub
    let row: ReticulumInterfaceRow
    let onEdit: (ReticulumInterfaceConfig) -> Void
    let onRemove: (ReticulumInterfaceConfig) -> Void

    private var expanded: Bool { hub.selectedInterface == row.id }

    var body: some View {
        let status = row.status
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Button {
                    FmHaptics.tap()
                    hub.selectedInterface = expanded ? "" : row.id
                } label: {
                    summary(status)
                }
                .buttonStyle(.plain)
                .accessibilityHint(Text(expanded ? "Hides the details" : "Shows the details and actions"))
                if expanded {
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(hub.interfaceLines(row), id: \.self) { line in
                            Text(line).fmText(.bodySmall, color: fm.textSecondary).textSelection(.enabled)
                        }
                    }
                    ReticulumTrafficChart(samples: status.history, title: "This interface, last 3 minutes")
                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(spacing: FmSpace.x2) {
                            FmOutlinedButton("Reconnect", onPressed: hub.running && row.config.enabled ? { hub.reconnect(row.id) } : nil)
                            FmOutlinedButton(row.config.enabled ? "Turn off" : "Turn on", onPressed: { hub.setInterfaceEnabled(row.id, !row.config.enabled) })
                            FmOutlinedButton("Edit", onPressed: { onEdit(row.config) })
                            FmTextButton("Remove", color: fm.danger, onPressed: { onRemove(row.config) })
                        }
                    }
                }
            }
        }
        .contextMenu {
            Button("Reconnect", systemImage: "arrow.clockwise") { hub.reconnect(row.id) }.disabled(!hub.running || !row.config.enabled)
            Button(row.config.enabled ? "Turn off" : "Turn on", systemImage: "power") { hub.setInterfaceEnabled(row.id, !row.config.enabled) }
            Button("Edit", systemImage: "pencil") { onEdit(row.config) }
            Button("Remove", systemImage: "trash", role: .destructive) { onRemove(row.config) }
        }
        .accessibilityIdentifier("reticulum-interface-\(row.id)")
    }

    private func summary(_ status: ReticulumInterfaceStatus) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                ReticulumStateDot(state: status.state)
                VStack(alignment: .leading, spacing: 2) {
                    Text(row.config.name).fmText(.titleSmall).lineLimit(1)
                    Text("\(row.config.kindLabel) · \(row.config.endpoint)")
                        .fmText(.labelSmall, color: fm.textSecondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                Spacer(minLength: 0)
                Text(status.state.label).fmText(.labelMedium, color: fm.textSecondary)
            }
            HStack(spacing: FmSpace.x3) {
                stat("Online", row.onlineFor)
                stat("↓", ReticulumFormat.bytes(Double(status.rxBytes)))
                stat("↑", ReticulumFormat.bytes(Double(status.txBytes)))
                stat("Packets", "\(status.rxPackets) / \(status.txPackets)")
                stat("Announces", "\(status.announces)")
            }
        }
        .contentShape(Rectangle())
    }

    private func stat(_ label: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(label).fmText(.labelSmall, color: fm.textTertiary)
            Text(value).fmText(.labelMedium, tabular: true).lineLimit(1)
        }
    }
}

struct ReticulumDiscoveredCard: View {
    @Environment(\.fmPalette) private var fm
    let row: ReticulumDiscoveredRow
    let onConnect: () -> Void

    var body: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                HStack {
                    Text(row.name).fmText(.titleSmall).lineLimit(1)
                    Spacer(minLength: 0)
                    FmOutlinedButton("Connect", onPressed: row.usable ? onConnect : nil)
                }
                Text("\(row.found.type) · \(row.address)").fmText(.labelSmall, color: fm.textSecondary)
                Text("Transport \(row.found.transport ? "Yes" : "No") · Proof of work \(row.proof) · \(row.location) · \(row.heard)")
                    .fmText(.labelSmall, color: fm.textTertiary)
                if !row.found.networkName.isEmpty {
                    Text("Needs the network name \(row.found.networkName), which this connector does not support yet")
                        .fmText(.labelSmall, color: fm.warning)
                }
            }
        }
        .opacity(row.usable ? 1 : 0.6)
    }
}

struct ReticulumDestinationsTab: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var hub: ReticulumHub

    var body: some View {
        let records = hub.filteredDestinations
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            Text("Find a path to").fmText(.labelLarge, color: fm.textSecondary)
            HStack(spacing: FmSpace.x2) {
                FmTextField(text: $hub.lookupText, hint: "Destination hash, 32 hex characters", mono: true, onSubmit: { hub.lookup() })
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .accessibilityIdentifier("reticulum-lookup")
                FmFilledButton("Find path", onPressed: { hub.lookup() })
            }
            if !hub.lookupResult.isEmpty {
                Text(hub.lookupResult).fmText(.bodySmall, color: fm.textSecondary).accessibilityIdentifier("reticulum-lookup-result")
            }
            FmTextField(text: $hub.destinationFilter, hint: "Filter by name, application or hash")
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            HStack(spacing: FmSpace.x2) {
                Menu {
                    Button("Every kind") { hub.kindFilter = nil }
                    ForEach(ReticulumMapGroup.allCases, id: \.self) { group in
                        Button(group.label) { hub.kindFilter = group }
                    }
                } label: {
                    Label(hub.kindFilter?.label ?? "Every kind", systemImage: "line.3.horizontal.decrease.circle")
                        .fmText(.labelMedium, color: fm.accent)
                }
                Menu {
                    ForEach(ReticulumSort.allCases, id: \.self) { sort in
                        Button(sort.title) { hub.sort = sort }
                    }
                } label: {
                    Label(hub.sort.title, systemImage: "arrow.up.arrow.down")
                        .fmText(.labelMedium, color: fm.accent)
                }
                Spacer(minLength: 0)
                Text(hub.destinationCount(shown: records.count)).fmText(.labelSmall, color: fm.textTertiary)
                FmTextButton("Clear list", onPressed: { hub.clearDestinations() })
            }
            LazyVStack(alignment: .leading, spacing: 0) {
                ForEach(records) { record in
                    ReticulumDestinationRow(hub: hub, record: record)
                    FmRowDivider(indent: 28)
                }
            }
            .background(fm.bgRaised)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
        }
        .padding(.top, FmSpace.x2)
    }
}

struct ReticulumDestinationRow: View {
    @Environment(\.fmPalette) private var fm
    let hub: ReticulumHub
    let record: ReticulumDestination

    var body: some View {
        Button {
            FmHaptics.tap()
            hub.select(record.hash, showMap: true)
        } label: {
            HStack(alignment: .top, spacing: FmSpace.x2) {
                ReticulumMarkerIcon(group: record.group, size: 14).padding(.top, 3)
                VStack(alignment: .leading, spacing: 2) {
                    Text(record.displayName.isEmpty ? "—" : record.displayName).fmText(.titleSmall).lineLimit(1)
                    Text("\(record.label) · \(ReticulumFormat.hops(record.hops)) · \(hub.interfaceName(record.interfaceId)) · \(ReticulumFormat.age(record.lastHeard, now: hub.now))")
                        .fmText(.labelSmall, color: fm.textSecondary)
                        .lineLimit(2)
                    Text(ReticulumWire.hex(record.hash)).fmMono(color: fm.textTertiary).lineLimit(1)
                }
                Spacer(minLength: 0)
                Text("\(record.announces)")
                    .fmText(.labelMedium, color: fm.textTertiary, tabular: true)
                    .accessibilityLabel(Text("\(record.announces) announces"))
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x2)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .contextMenu {
            if record.aspect == ReticulumLxmf.deliveryAspect {
                Button("Message", systemImage: "bubble.left") { hub.openConversation(record.hash) }
            }
            Button("Show on map", systemImage: "scope") { hub.select(record.hash, showMap: true) }
            Button("Find path", systemImage: "point.topleft.down.to.point.bottomright.curvepath") { hub.requestPath(record.hash) }
            Button("Copy destination hash", systemImage: "doc.on.doc") { ReticulumCopy.copy(ReticulumWire.hex(record.hash), "Destination hash", hub: hub) }
            Button("Copy identity hash", systemImage: "person.text.rectangle") { ReticulumCopy.copy(ReticulumWire.hex(record.identityHash), "Identity hash", hub: hub) }
            Button("Forget", systemImage: "trash", role: .destructive) { hub.forget(record.hash) }
        }
    }
}

struct ReticulumActivityTab: View {
    @Environment(\.fmPalette) private var fm
    let hub: ReticulumHub

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            FmCard {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    Text("Packets since you connected").fmText(.labelLarge, color: fm.textSecondary)
                    ForEach(hub.counterRows) { row in
                        HStack {
                            Text(row.label).fmText(.bodySmall, color: fm.textSecondary)
                            Spacer()
                            Text(row.value).fmText(.bodySmall, tabular: true)
                        }
                    }
                }
            }
            let events = hub.events
            if events.isEmpty {
                Text("Announces, path answers and interfaces coming and going show up here.")
                    .fmText(.bodySmall, color: fm.textSecondary)
            }
            LazyVStack(alignment: .leading, spacing: 0) {
                ForEach(events) { event in
                    Button {
                        if !event.destination.isEmpty, hub.node.destinations[event.destination] != nil {
                            hub.select(event.destination, showMap: true)
                        }
                    } label: {
                        HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                            Text(ReticulumFormat.clock(event.at)).fmMono(color: fm.textTertiary)
                            Text(event.text).fmText(.bodySmall, color: event.kind == "rejected" ? fm.warning : fm.textPrimary)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        .padding(.horizontal, FmSpace.x3)
                        .padding(.vertical, 6)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
            }
            .background(fm.bgRaised)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
        }
        .padding(.top, FmSpace.x2)
    }
}

struct ReticulumIdentityTab: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var hub: ReticulumHub
    let onRenew: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            Text("This \(hub.deviceNoun) on Reticulum").fmText(.titleSmall)
            Text("ForkMesh keeps a Reticulum identity for this \(hub.deviceNoun): an X25519 key for encryption and an Ed25519 key for signing, stored in the Keychain. Announcing publishes its public key and the name below under the forkmesh.node application, so others can find a path to it. Nothing is announced until you choose to.")
                .fmText(.bodySmall, color: fm.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
            FmCard {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    hashRow("Destination", hub.destinationHashText, what: "Destination hash")
                    hashRow("Identity", hub.identityHashText, what: "Identity hash")
                    hashRow("Messaging address", hub.messagingAddressText, what: "Messaging address")
                    VStack(alignment: .leading, spacing: FmSpace.x1) {
                        Text("Name in announces").fmText(.labelMedium, color: fm.textTertiary)
                        FmTextField(text: $hub.announceName, hint: "Name shown to others, for example your handle", onSubmit: { hub.commitAnnounceName() })
                            .accessibilityIdentifier("reticulum-announce-name")
                            .onChange(of: hub.announceName) { _, value in
                                if value.unicodeScalars.count > 64 { hub.announceName = String(String.UnicodeScalarView(value.unicodeScalars.prefix(64))) }
                            }
                    }
                    HStack {
                        Text("Announce").fmText(.labelMedium, color: fm.textTertiary)
                        Spacer()
                        Menu {
                            ForEach(ReticulumAnnounceInterval.choices) { choice in
                                Button(choice.label) { hub.setAnnounceMinutes(choice.minutes) }
                            }
                        } label: {
                            Text(ReticulumAnnounceInterval.choices.first { $0.minutes == hub.announceMinutes }?.label ?? "")
                                .fmText(.labelLarge, color: fm.accent)
                        }
                        .accessibilityIdentifier("reticulum-announce-interval")
                    }
                    Toggle(isOn: Binding(get: { hub.answerPathRequests }, set: { hub.setAnswerPathRequests($0) })) {
                        Text("Answer path requests for this \(hub.deviceNoun)").fmText(.bodyMedium)
                    }
                    .tint(fm.accent)
                    HStack {
                        Text("Last announce").fmText(.labelMedium, color: fm.textTertiary)
                        Spacer()
                        Text(hub.lastAnnounceText).fmText(.bodySmall, color: fm.textSecondary)
                    }
                }
            }
            HStack(spacing: FmSpace.x2) {
                FmFilledButton("Announce now", onPressed: { hub.announceNow() })
                    .accessibilityIdentifier("reticulum-announce-now")
                FmTextButton("Make a new identity", color: fm.danger, onPressed: onRenew)
            }
        }
        .padding(.top, FmSpace.x2)
        .onDisappear { hub.commitAnnounceName() }
    }

    private func hashRow(_ label: String, _ value: String, what: String) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            Text(label).fmText(.labelMedium, color: fm.textTertiary)
            HStack(spacing: FmSpace.x2) {
                Text(value).fmMono(color: fm.textPrimary).textSelection(.enabled).lineLimit(1).minimumScaleFactor(0.7)
                Spacer(minLength: 0)
                FmIconButton(icon: "content_copy", label: "Copy \(what.lowercased())", size: 18, onPressed: {
                    ReticulumCopy.copy(value.trimmingCharacters(in: CharacterSet(charactersIn: "<>")), what, hub: hub)
                })
            }
        }
    }
}
