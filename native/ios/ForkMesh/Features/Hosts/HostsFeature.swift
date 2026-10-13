// WS7: the owner's desktop from anywhere. Builds the services the Hosts area and the unified
// Agents list share, once:
//
//   DesktopControlHub   the LAN channel (FMChat's LocalControlService) or, away from home, the
//                       same channel through the relay (a fresh account event socket per dial)
//   AgentControlCenter  desktop agents (LAN or relay) and node runs (relay queue) in one list
//   HostsStore          the desktop's Hosts page, read and changed with fresh confirmations
//
// `register()` is what `HostsInstaller` (AppServices+Hosts.swift) runs at startup once the
// installHosts() hook exists; until then the first screen that needs these calls it itself, so
// the feature works without touching files other workstreams own.

import FMAccount
import FMApi
import FMBase
import FMChat
import FMControl
import FMCrypto
import FMHosts
import FMNet
import Foundation
import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

@MainActor
enum HostsFeature {
    private static var topicsWired = false

    static func register() {
        if Services.get(DesktopControlHub.self) != nil { return }
        let local: LocalControlService? = Services.get(LocalControlService.self)
        let hub = DesktopControlHub(
            local: local, openRelay: HostsFeature.relayOpener(), accountDesktops: HostsFeature.accountDesktops())
        Services.put(hub)
        Services.put(hub, as: ShellLocalDesktopBridge.self)
        let center = AgentControlCenter(hub: hub, cloud: Services.get(ApiService.self))
        Services.put(center)
        Services.put(HostsStore(channel: hub))
        wireAccountTopics(center)
        if let lifecycle = Services.get(AppLifecycle.self) {
            lifecycle.observe { (state: AppLifecycleState) in
                hub.keepWarm = state == .resumed
            }
            hub.keepWarm = lifecycle.isForeground
        }
    }

    static var hub: DesktopControlHub? {
        register()
        return Services.get(DesktopControlHub.self)
    }

    static var agents: AgentControlCenter? {
        register()
        return Services.get(AgentControlCenter.self)
    }

    static var hosts: HostsStore? {
        register()
        return Services.get(HostsStore.self)
    }

    /// Dials the paired desktop through the relay: a socket of its own on the account's object,
    /// where the desktop holds its node socket, then the control handshake inside it.
    private static func relayOpener() -> RelayControlOpener {
        return { (target: PairedDesktop) async throws -> RelayControlConnection in
            guard let api = Services.get(ApiService.self),
                let connector = Services.get(WebSocketConnector.self),
                let auth = Services.get(AuthService.self),
                let identity = Services.get(DeviceIdentity.self),
                !(auth.session?.sessionToken ?? "").isEmpty
            else {
                throw LocalControlException("not_connected")
            }
            if target.name.isEmpty { throw LocalControlException("not_paired") }
            let channel = AccountEventChannel(
                api: api, sessionToken: { auth.session?.sessionToken ?? "" }, connector: connector)
            return try await RelayControlConnection.open(
                channel: channel, identity: identity, desktopKey: target.key, node: target.name,
                deviceName: HostsFeature.deviceName, user: auth.session?.nodeName ?? "",
                platform: AppBuild.currentPlatform)
        }
    }

    private static func accountDesktops() -> AccountDesktopsLoader {
        return { () async throws -> [PairedDesktop] in
            guard let api = Services.get(ApiService.self), api.hasSession else { return [] }
            let listed = try await api.listAccountDesktops()
            return listed.desktops.map {
                PairedDesktop(key: $0.key, name: $0.node, user: listed.account, pairedAtMs: 0)
            }
        }
    }

    private static var deviceName: String {
        #if canImport(UIKit)
        return UIDevice.current.name
        #else
        return "Phone"
        #endif
    }

    /// Node runs change on the relay: its "cloud-agents" (and a desktop's "agents") topic rings
    /// the account event socket. Chained onto whatever AppServices.wire() installed.
    private static func wireAccountTopics(_ center: AgentControlCenter) {
        if topicsWired { return }
        guard let events = Services.get(AccountEventChannel.self) else { return }
        topicsWired = true
        let previous = events.onTopic
        events.onTopic = { (topic: String) in
            previous?(topic)
            if topic == "cloud-agents" || topic == "agents" {
                center.cloudChanged()
            }
        }
    }
}

/// A transport badge, the same everywhere a run or a host channel is shown.
struct TransportBadge: View {
    var text: String
    var systemImage: String
    var tint: Color

    init(_ badge: AgentTransportBadge) {
        text = badge.label
        switch badge {
        case .desktopLan:
            systemImage = "wifi"
            tint = .green
        case .desktopRelay:
            systemImage = "lock.icloud"
            tint = .blue
        case .node:
            systemImage = "server.rack"
            tint = .purple
        }
    }

    init(row: UnifiedAgentRow) {
        self.init(row.badge)
        if row.isRunner {
            text = row.badgeLabel
            systemImage = "cpu"
        }
    }

    init(runner: String) {
        self.init(.node)
        text = RunnerAgentText.badge(runner)
        systemImage = "cpu"
    }

    init(transport: ControlTransport) {
        text = transport.label
        systemImage = transport == .lan ? "wifi" : "lock.icloud"
        tint = transport == .lan ? .green : .blue
    }

    var body: some View {
        Label(text, systemImage: systemImage)
            .font(.caption2.weight(.semibold))
            .labelStyle(.titleAndIcon)
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .foregroundStyle(tint)
            .background(tint.opacity(0.12), in: Capsule())
            .accessibilityLabel("Via \(text)")
    }
}

/// The desktop channel's state, as one line at the top of the Hosts and Agents screens.
struct DesktopReachLine: View {
    let reach: DesktopReach

    var body: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(color)
                .frame(width: 8, height: 8)
            Text(reach.label)
                .font(.footnote)
                .foregroundStyle(.secondary)
                .lineLimit(2)
            FmMessageTools(message)
        }
        .accessibilityElement(children: .contain)
    }

    private var message: String {
        if case .connected = reach { return "" }
        return reach.label
    }

    private var color: Color {
        switch reach {
        case .connected: return .green
        case .connecting: return .orange
        case .offline: return .red
        case .notPaired: return .gray
        }
    }
}
