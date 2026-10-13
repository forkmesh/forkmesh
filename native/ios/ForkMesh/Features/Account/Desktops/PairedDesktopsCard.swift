import FMAccount
import FMApi
import FMBase
import FMChat
import FMControl
import SwiftUI

@MainActor
struct PairedDesktopsCard: View {
    @Environment(\.fmPalette) var fm
    @State var renaming: PairedDesktop?
    @State var renameText: String = ""
    @State var forgetting: PairedDesktop?
    @State var summary: DesktopStatusSummary?
    @State var revision: Int = 0
    @State var notice: FmNotice?

    private let local: LocalControlService?
    private let nicknames: DesktopNicknames?

    init() {
        local = Services.get(LocalControlService.self)
        nicknames = Services.get(SettingsService.self).map { DesktopNicknames(prefs: $0.prefs) }
    }

    var body: some View {
        let paired: [PairedDesktop] = local?.paired ?? []
        VStack(spacing: 0) {
            if paired.isEmpty {
                SettingsEmptyNote(
                    title: "No desktops paired",
                    text: "Sign the desktop in to this account, then open Desktops here on the same network. A desktop on another account asks you to compare a code.")
                    .padding(FmSpace.x4)
            } else {
                ForEach(Array(paired.enumerated()), id: \.element.key) { (index: Int, desktop: PairedDesktop) in
                    if index > 0 { FmHairline() }
                    row(desktop)
                }
            }
            if notice != nil {
                FmHairline()
                FmNoticeLine($notice)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.vertical, FmSpace.x3)
            }
            FmHairline()
            NavigationLink(value: Route.desktops) {
                SettingsLinkRow(icon: "desktop_mac_outlined", title: "Open Desktops", subtitle: "Files, logs, health and agents on your desktop")
            }
            .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
        }
        .id(revision)
        .task { await loadSummary() }
        .fmDialog(isPresented: renameShown) {
            if let desktop = renaming {
                FmPromptDialog(
                    title: "Rename desktop",
                    message: "The name is kept on this phone only.",
                    text: $renameText,
                    hint: desktop.name.isEmpty ? "Desktop" : desktop.name,
                    confirmLabel: "Save",
                    onCancel: { renaming = nil },
                    onConfirm: {
                        nicknames?.rename(desktop.key, to: renameText)
                        renaming = nil
                        revision += 1
                    })
            }
        }
        .fmDialog(isPresented: forgetShown) {
            if let desktop = forgetting {
                FmConfirmDialog(
                    title: "Forget \(displayName(desktop))?",
                    message: "This phone stops trusting the desktop. The desktop asks to approve this phone again next time.",
                    confirmLabel: "Forget",
                    destructive: true,
                    onCancel: { forgetting = nil },
                    onConfirm: {
                        forgetting = nil
                        Task { @MainActor in
                            await local?.forget(desktop.key)
                            nicknames?.forget(desktop.key)
                            revision += 1
                            notice = FmNotice("Desktop forgotten")
                        }
                    })
            }
        }
    }

    private var renameShown: Binding<Bool> {
        Binding<Bool>(get: { renaming != nil }, set: { (shown: Bool) in if !shown { renaming = nil } })
    }

    private var forgetShown: Binding<Bool> {
        Binding<Bool>(get: { forgetting != nil }, set: { (shown: Bool) in if !shown { forgetting = nil } })
    }

    private func displayName(_ desktop: PairedDesktop) -> String {
        nicknames?.name(for: desktop.key, fallback: desktop.name) ?? (desktop.name.isEmpty ? "Desktop" : desktop.name)
    }

    private func subtitle(_ desktop: PairedDesktop) -> String {
        let connected: Bool = local?.connected == true && local?.desktopKey == desktop.key
        if connected, let summary = summary {
            return summary.line
        }
        let owner: String = desktop.user.isEmpty ? "" : "@\(desktop.user) · "
        let when: String = desktop.pairedAtMs > 0
            ? "paired \(Format.relativeTime(desktop.pairedAtMs, nowMillis: SystemClock().nowMillis))"
            : "paired"
        return owner + when
    }

    private func row(_ desktop: PairedDesktop) -> some View {
        HStack(spacing: FmSpace.x3) {
            FmIcon("desktop_mac_outlined", size: 22, color: fm.textSecondary)
            VStack(alignment: .leading, spacing: 2) {
                Text(displayName(desktop)).fmText(.titleSmall).lineLimit(1)
                Text(subtitle(desktop)).fmText(.bodySmall, color: fm.textTertiary).lineLimit(2)
                if local?.desktopKey == desktop.key, let summary = summary {
                    Text(summary.agentsLine).fmText(.bodySmall, color: fm.textTertiary).lineLimit(2)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            Menu {
                Button("Rename") {
                    renameText = nicknames?.all[desktop.key] ?? ""
                    renaming = desktop
                }
                NavigationLink(value: Route.desktop(id: desktop.name.lowercased(), section: nil)) {
                    Text("Open")
                }
                Button("Forget", role: .destructive) { forgetting = desktop }
            } label: {
                FmIcon("more_vert", size: 20, color: fm.textTertiary)
                    .frame(width: 44, height: 44)
            }
            .accessibilityLabel(Text("Desktop actions"))
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x2)
        .frame(minHeight: 60)
    }

    private func loadSummary() async {
        guard let local = local, local.connected else { return }
        guard let status = try? await local.call("status") else { return }
        let agents: JSONObject = (try? await local.call("agents.list")) ?? [:]
        summary = DesktopStatusSummary.parse(status: status, agents: agents)
    }
}
