import FMAccount
import FMBase
import FMControl
import FMDesktopRemote
import Foundation
import LocalAuthentication
import SwiftUI

@MainActor
enum DesktopRemoteFeature {
    static func register() {
        guard let hub = HostsFeature.hub else { return }
        if Services.get(FleetStore.self) == nil { Services.put(FleetStore(channel: hub)) }
        if Services.get(FilesBrowserStore.self) == nil { Services.put(FilesBrowserStore(channel: hub)) }
        if Services.get(DiagnosticsStore.self) == nil { Services.put(DiagnosticsStore(channel: hub)) }
        if Services.get(DesktopUpdateStore.self) == nil { Services.put(DesktopUpdateStore(channel: hub)) }
        if Services.get(DesktopVitals.self) == nil { wireVitals(hub) }
    }

    private static func wireVitals(_ hub: DesktopControlHub) {
        let vitals = DesktopVitals(channel: hub)
        Services.put(vitals)
        hub.addLinkListener { [weak vitals, weak hub] in
            guard let vitals, let hub else { return }
            if hub.currentLink == nil {
                vitals.clear()
            } else if AppLifecycle.shared.isForeground {
                Task { await vitals.watch() }
            }
        }
        AppLifecycle.shared.observe { [weak vitals, weak hub] (state: AppLifecycleState) in
            guard let vitals, let hub else { return }
            if state == .paused {
                Task { await vitals.unwatch() }
            } else if state == .resumed, hub.currentLink != nil, !vitals.watching {
                Task { await vitals.watch() }
            }
        }
        if hub.currentLink != nil { Task { await vitals.watch() } }
    }

    static var vitals: DesktopVitals? {
        register()
        return Services.get(DesktopVitals.self)
    }

    static var hub: DesktopControlHub? { HostsFeature.hub }

    static var fleet: FleetStore? {
        register()
        return Services.get(FleetStore.self)
    }

    static var files: FilesBrowserStore? {
        register()
        return Services.get(FilesBrowserStore.self)
    }

    static var diagnostics: DiagnosticsStore? {
        register()
        return Services.get(DiagnosticsStore.self)
    }

    static var update: DesktopUpdateStore? {
        register()
        return Services.get(DesktopUpdateStore.self)
    }

    static func authenticateOwner(reason: String) async -> Bool {
        let context = LAContext()
        var error: NSError?
        guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error) else { return false }
        do {
            return try await context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: reason)
        } catch {
            return false
        }
    }
}

struct DesktopUnavailableView: View {
    var title = "The desktop is not available"

    var body: some View {
        ContentUnavailableView(
            title, systemImage: "desktopcomputer",
            description: Text("Sign in, then pair your desktop on the same network."))
    }
}

struct DesktopOutdatedRow: View {
    var body: some View {
        Label(
            "This desktop's ForkMesh does not answer this yet. Update it from the desktop's Settings.",
            systemImage: "arrow.down.circle"
        )
        .font(.footnote)
        .foregroundStyle(.orange)
        .fmMessageTools("This desktop's ForkMesh does not answer this yet. Update it from the desktop's Settings.")
    }
}

struct RemoteOutcomeRow: View {
    let outcome: RemoteActionOutcome

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: outcome.ok ? "checkmark.circle.fill" : "xmark.octagon.fill")
                .foregroundStyle(outcome.ok ? .green : .red)
            VStack(alignment: .leading, spacing: 2) {
                Text(outcome.subject.isEmpty ? outcome.action : "\(outcome.action) · \(outcome.subject)")
                    .font(.subheadline.weight(.semibold))
                if !outcome.message.isEmpty {
                    Text(outcome.message)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(6)
                        .fmMessageTools(outcome.message)
                }
            }
        }
        .accessibilityElement(children: .combine)
    }
}

struct ShareBar: View {
    let share: Double
    var tint: Color = .accentColor

    var body: some View {
        GeometryReader { proxy in
            ZStack(alignment: .leading) {
                Capsule().fill(Color.secondary.opacity(0.15))
                Capsule()
                    .fill(tint)
                    .frame(width: max(2, proxy.size.width * min(1, max(0, share))))
            }
        }
        .frame(height: 6)
        .accessibilityHidden(true)
    }
}
