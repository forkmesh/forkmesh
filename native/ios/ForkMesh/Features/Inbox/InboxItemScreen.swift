import FMApi
import FMBase
import FMCode
import FMControl
import SwiftUI
import UIKit

struct InboxItemScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let model: InboxModel
    let item: InboxItem
    @State private var composing: InboxComposeBox?
    @State private var notice: FmNotice?
    @State private var confirmDelete: Bool = false
    @State private var quickFixes: PingQuickFixes = PingQuickFixes()
    @State private var checkingFixes: Bool = false
    @State private var fixing: String?

    private var webURL: URL? {
        Services.get(ApiService.self)?.webLink(item.href)
    }

    private var linkText: String {
        webURL?.absoluteString ?? item.href.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    var body: some View {
        List {
            Section {
                header
            }
            if !item.repo.isEmpty || !item.actor.isEmpty || !linkText.isEmpty {
                Section("Details") {
                    if !item.repo.isEmpty {
                        LabeledContent("Repository", value: item.repo)
                    }
                    if !item.actor.isEmpty {
                        LabeledContent("From", value: "@\(item.actor)")
                            .opensProfile(item.actor)
                    }
                    if !linkText.isEmpty {
                        LabeledContent("Link") {
                            Text(linkText).textSelection(.enabled).lineLimit(2)
                        }
                    }
                }
            }
            Section("Actions") {
                if notice != nil {
                    FmNoticeLine($notice)
                        .accessibilityIdentifier("ping-detail-notice")
                }
                actions
            }
        }
        .listStyle(.insetGrouped)
        .background(fm.canvas)
        .navigationTitle(item.kindLabel)
        .navigationBarTitleDisplayMode(.inline)
        .shellKeepsNavigationBar()
        .task {
            await model.markRead(item)
            await loadQuickFixes()
        }
        .inboxComposeSheet($composing)
        .alert(item.source == .ping ? "Delete this ping?" : "Remove this from the inbox?", isPresented: $confirmDelete) {
            Button("Delete", role: .destructive) {
                dismiss()
                Task { await model.delete(item) }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            if item.source == .ping {
                Text("This removes it from your ForkMesh inbox on every device. This cannot be undone.")
            }
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 10) {
            FmTextButton("Inbox", icon: "chevron_left", minHeight: 36, onPressed: { dismiss() })
                .padding(.leading, -FmSpace.x3)
                .accessibilityIdentifier("ping-detail-back")
            HStack(spacing: 12) {
                FmIconTile(icon: item.icon, tint: fm.accent, size: 40, iconSize: 20)
                VStack(alignment: .leading, spacing: 2) {
                    Text(item.kindLabel).font(.caption.weight(.semibold)).foregroundStyle(fm.textTertiary)
                    let when: String = pingDetailWhen(item.ts)
                    if !when.isEmpty {
                        Text(when).font(.caption).foregroundStyle(fm.textSecondary)
                            .accessibilityIdentifier("ping-detail-time")
                    }
                }
            }
            Text(item.title)
                .font(.title3.weight(.semibold))
                .foregroundStyle(fm.textPrimary)
                .textSelection(.enabled)
            if !item.body.isEmpty {
                LinkedText(item.body, repo: referenceRepo(href: "", slug: item.repo), commits: false)
                    .font(.body)
                    .foregroundStyle(fm.textSecondary)
                    .textSelection(.enabled)
            }
            if item.sync == .pending || item.sync == .failed {
                Label(item.sync == .pending ? "Waiting to sync" : "Could not sync", systemImage: "arrow.triangle.2.circlepath")
                    .font(.caption)
                    .foregroundStyle(item.sync == .failed ? fm.danger : fm.textTertiary)
                    .fmMessageTools(item.sync == .pending ? "Waiting to sync" : "Could not sync")
            }
        }
        .padding(.vertical, 4)
    }

    private var fixClient: DesktopPingFixes? {
        guard item.fromDesktop, let hub = HostsFeature.hub else { return nil }
        return DesktopPingFixes(channel: hub)
    }

    private func loadQuickFixes() async {
        guard let client = fixClient else { return }
        checkingFixes = true
        defer { checkingFixes = false }
        quickFixes = (try? await client.fixes(for: item)) ?? PingQuickFixes()
    }

    private func apply(_ fix: PingQuickFix) async {
        guard let client = fixClient, fixing == nil else { return }
        fixing = fix.id
        defer { fixing = nil }
        do {
            let result = try await client.apply(fix, to: item)
            quickFixes = result
            notice = FmNotice(result.message.isEmpty ? "Done" : result.message)
        } catch {
            notice = FmNotice.error(DesktopPingFixes.explain(error))
        }
    }

    @ViewBuilder
    private var quickFixRows: some View {
        if checkingFixes && quickFixes.fixes.isEmpty {
            Label("Looking for a quick fix\u{2026}", systemImage: "wrench.and.screwdriver")
                .foregroundStyle(fm.textTertiary)
        }
        ForEach(quickFixes.fixes) { (fix: PingQuickFix) in
            Button {
                Task { await apply(fix) }
            } label: {
                HStack(spacing: 8) {
                    Label {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(fix.label).fontWeight(fix.primary ? .semibold : .regular)
                            if !fix.detail.isEmpty {
                                Text(fix.detail).font(.caption).foregroundStyle(fm.textSecondary)
                            }
                        }
                    } icon: {
                        Image(systemName: fix.systemImage)
                    }
                    Spacer(minLength: 0)
                    if fixing == fix.id {
                        ProgressView()
                    } else if fix.trusted {
                        Text("Auto").font(.caption2.weight(.semibold)).foregroundStyle(fm.accent)
                    }
                }
                .foregroundStyle(fix.primary ? fm.accent : fm.textPrimary)
            }
            .disabled(fixing != nil)
            .accessibilityIdentifier(fix.primary ? "ping-detail-fix" : "ping-detail-fix-\(fix.id)")
        }
    }

    @ViewBuilder
    private var actions: some View {
        quickFixRows
        if let route = item.route {
            NavigationLink {
                RouteDestinationView(route: route)
                    .shellPageClearance()
            } label: {
                Label("Open", systemImage: "arrow.up.forward.square")
            }
            .accessibilityIdentifier("ping-detail-open")
        } else if let url = webURL {
            Button {
                NotificationActions.onOpenLink?(url)
            } label: {
                Label("Open in browser", systemImage: "safari")
            }
            .accessibilityIdentifier("ping-detail-open")
        }
        Button {
            InboxComposeBox.send(model.prompt(for: item), fallback: $composing)
        } label: {
            Label("Send to agent", systemImage: "paperplane")
        }
        .accessibilityIdentifier("ping-detail-agent")
        Button {
            UIPasteboard.general.string = model.prompt(for: item).text ?? item.title
            notice = FmNotice("Ping copied")
        } label: {
            Label("Copy ping", systemImage: "doc.on.doc")
        }
        if !linkText.isEmpty {
            Button {
                UIPasteboard.general.string = linkText
                notice = FmNotice("Link copied")
            } label: {
                Label("Copy link", systemImage: "link")
            }
        }
        if item.source == .ping {
            NavigationLink {
                RouteDestinationView(route: .pingSettings)
                    .shellPageClearance()
            } label: {
                Label("Ping settings", systemImage: "gearshape")
            }
        }
        Button(role: .destructive) {
            confirmDelete = true
        } label: {
            Label("Delete", systemImage: "trash")
        }
        .accessibilityIdentifier("ping-detail-delete")
    }
}

extension InboxComposeBox {
    @MainActor
    static func send(_ target: ComposeTarget, fallback: Binding<InboxComposeBox?>) {
        if let compose = NotificationActions.onCompose {
            compose(target)
        } else {
            fallback.wrappedValue = InboxComposeBox(target: target)
        }
    }
}

extension View {
    func inboxComposeSheet(_ composing: Binding<InboxComposeBox?>) -> some View {
        sheet(item: composing) { (box: InboxComposeBox) in
            NavigationStack {
                RouteDestinationView(route: .compose(box.target))
                    .toolbar {
                        ToolbarItem(placement: .cancellationAction) {
                            Button("Close") { composing.wrappedValue = nil }
                        }
                    }
            }
        }
    }
}
