import FMApi
import FMBase
import FMCode
import SwiftUI
import UIKit

struct RepoSettingsScreen: View {
    let repo: RepoRef
    let section: RepoSettingsSection
    @State private var original: RepoAbout?
    @State private var about = RepoAbout()
    @State private var error: String?
    @State private var saving = false
    @State private var saved: String?

    init(repo: RepoRef, section: String?) {
        self.repo = repo
        self.section = RepoSettingsSection(route: section)
    }

    private var canEdit: Bool {
        RepoSettingsRules.canEdit(repoOwner: repo.owner, account: ReviewEnv.accountName, organizationOwner: false)
    }

    private var saveDisabled: Bool {
        if !canEdit || saving { return true }
        guard let original else { return true }
        if original == about { return true }
        return RepoSettingsRules.validateWebsite(about.website) != nil
    }

    var body: some View {
        List {
            Section {
                ForEach(RepoSettingsSection.allCases, id: \.self) { (item: RepoSettingsSection) in
                    if item != section {
                        NavigationLink(value: Route.repoSettings(repo, section: item.rawValue)) {
                            Label(item.title, systemImage: item.icon)
                        }
                    }
                }
            }
            switch section {
            case .general: generalSection
            case .agents: agentsSection
            case .automation: automationSection
            case .mirror: ProviderMirrorSections(repo: repo, canEdit: canEdit)
            case .coves:
                Section {
                    NavigationLink(value: Route.coves(repo)) {
                        Label("Open this repository's coves", systemImage: "lock.shield")
                    }
                }
            case .permissions: permissionsSection
            case .danger: dangerSection
            }
        }
        .navigationTitle(section.title)
        .navigationBarTitleDisplayMode(.inline)
        .task { if section == .general && original == nil { await load() } }
    }

    @ViewBuilder private var generalSection: some View {
        Section("About") {
            if original == nil && error == nil {
                ProgressView()
            }
            TextField("Description", text: $about.description, axis: .vertical)
                .lineLimit(2...5)
                .disabled(!canEdit)
            TextField("Website", text: $about.website)
                .keyboardType(.URL)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .disabled(!canEdit)
            if let problem = RepoSettingsRules.validateWebsite(about.website) {
                Text(problem).font(.footnote).foregroundStyle(.red)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(problem)
            }
        }
        Section {
            Toggle("Publish to the fediverse", isOn: $about.federate)
            Toggle("Post issues, pulls and releases", isOn: $about.broadcastEvents)
                .disabled(!about.federate)
            Toggle("Accept remote replies", isOn: $about.acceptComments)
                .disabled(!about.federate)
        } header: {
            Text("Fediverse")
        } footer: {
            if !about.fediverseHandle.isEmpty {
                Text("\(about.fediverseHandle) · \(about.followers) follower\(about.followers == 1 ? "" : "s")")
            }
        }
        .disabled(!canEdit)
        Section {
            if let error { Text(error).foregroundStyle(.red).font(.footnote).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error) }
            if let saved { Text(saved).foregroundStyle(.secondary).font(.footnote).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(saved) }
            Button(saving ? "Saving…" : "Save") {
                Task { await save() }
            }
            .disabled(saveDisabled)
        } footer: {
            if !canEdit { Text("Only the repository owner can change these settings.") }
        }
    }

    @ViewBuilder private var agentsSection: some View {
        Section {
            NavigationLink(value: Route.repo(repo, section: .agents)) {
                Label("Agent runs for this repository", systemImage: "cpu")
            }
        } footer: {
            Text("Agent providers, models and guardrails are chosen on the desktop that runs them.")
        }
    }

    @ViewBuilder private var automationSection: some View {
        Section {
            NavigationLink {
                MergeQueueScreen(repo: repo)
                    .shellPageClearance()
            } label: {
                Label("Merge queue on the desktop", systemImage: "arrow.triangle.merge")
            }
            NavigationLink(value: Route.repo(repo, section: .actions)) {
                Label("Actions", systemImage: "play.rectangle")
            }
            NavigationLink {
                CIRepoSections.webhooks(repo: repo)
                    .shellPageClearance()
            } label: {
                Label("Webhooks", systemImage: "arrow.up.forward.app")
            }
        } footer: {
            Text("The merge queue runs on your desktop; this phone turns it on or off, pauses it and reorders it.")
        }
    }

    @ViewBuilder private var permissionsSection: some View {
        Section {
            Text("Who may serve, mirror and run actions for this repository is set on the desktop that owns it (Control › Repository permissions). Organization teams are managed from the organization page.")
                .foregroundStyle(.secondary)
            NavigationLink(value: Route.orgs) {
                Label("Organizations", systemImage: "person.3")
            }
        }
    }

    @ViewBuilder private var dangerSection: some View {
        Section {
            Text("Renaming, archiving, changing visibility and deleting a repository rewrite its history on the owning node, so they are done on the desktop. The relay has no route that does this from a phone.")
                .foregroundStyle(.secondary)
        }
    }

    private func load() async {
        do {
            let loaded = try await ReviewEnv.settings(repo).about()
            original = loaded
            about = loaded
            error = nil
        } catch {
            self.error = dartErrorText(error)
        }
    }

    private func save() async {
        guard let original else { return }
        saving = true
        defer { saving = false }
        do {
            if try await ReviewEnv.settings(repo).saveAbout(from: original, to: about) {
                self.original = about
                saved = "Saved."
                FmHaptics.commit()
            }
            error = nil
        } catch {
            self.error = dartErrorText(error)
        }
    }
}

struct RepoActionsMenu: View {
    let repo: RepoRef
    let ref: String
    @State private var forking = false
    @State private var confirmFork = false
    @State private var message: String?
    @State private var archiving = false
    @State private var archive: URL?
    @State private var progress = ""

    var body: some View {
        Menu {
            Button {
                confirmFork = true
            } label: {
                Label("Fork", systemImage: "tuningfork")
            }
            Button {
                Task { await buildZip() }
            } label: {
                Label("Download ZIP", systemImage: "arrow.down.doc")
            }
            NavigationLink(value: Route.codeMap(repo, ref: ref)) {
                Label("Code map", systemImage: "square.grid.3x3")
            }
            ForEach(CodeInsightPlugin.allCases) { (plugin: CodeInsightPlugin) in
                if PackageGate.shows(plugin.route(repo)) {
                    NavigationLink(value: plugin.route(repo)) {
                        Label(plugin.title, systemImage: plugin.systemImage)
                    }
                }
            }
            NavigationLink(value: Route.board(repo)) {
                Label("Issue board", systemImage: "rectangle.split.3x1")
            }
            NavigationLink(value: Route.repo(repo, section: .branches)) {
                Label("Branches", systemImage: "arrow.triangle.branch")
            }
            if RepoSettingsRules.canEdit(repoOwner: repo.owner, account: ReviewEnv.accountName, organizationOwner: false) {
                NavigationLink(value: Route.repoSettings(repo, section: RepoSettingsSection.mirror.rawValue)) {
                    Label("Mirror to GitHub or GitLab", systemImage: "arrow.left.arrow.right")
                }
            }
            NavigationLink(value: Route.repoSettings(repo, section: nil)) {
                Label("Settings", systemImage: "gearshape")
            }
        } label: {
            if archiving {
                ProgressView()
            } else {
                Image(systemName: "ellipsis.circle")
            }
        }
        .accessibilityLabel("Repository actions")
        .confirmationDialog("Fork \(repo.slug)?", isPresented: $confirmFork, titleVisibility: .visible) {
            Button(forking ? "Forking…" : "Fork to my managed mirror") { Task { await fork() } }
        } message: {
            Text("The fork is recorded on your linked managed mirror, which creates it the next time it is online.")
        }
        .alert("Repository", isPresented: Binding(get: { message != nil }, set: { if !$0 { message = nil } })) {
            Button("Copy") { UIPasteboard.general.string = message ?? "" }
            Button("Send to agent") { FmMessageHandoff.send(message ?? "") }
            Button("OK", role: .cancel) {}
        } message: {
            Text(message ?? "")
        }
        .sheet(isPresented: Binding(get: { archive != nil }, set: { if !$0 { archive = nil } })) {
            if let archive {
                VStack(spacing: 16) {
                    Label(archive.lastPathComponent, systemImage: "doc.zipper").font(.headline)
                    Text(progress).font(.footnote).foregroundStyle(.secondary)
                    ShareLink(item: archive) {
                        Label("Share or save", systemImage: "square.and.arrow.up")
                    }
                    .buttonStyle(.borderedProminent)
                }
                .padding(24)
                .presentationDetents([.medium])
            }
        }
    }

    private func fork() async {
        forking = true
        defer { forking = false }
        do {
            message = try await ReviewEnv.settings(repo).fork().message
        } catch {
            message = dartErrorText(error)
        }
    }

    private func buildZip() async {
        archiving = true
        defer { archiving = false }
        do {
            let result = try await RepoArchiveBuilder(reader: ReviewEnv.api).build(owner: repo.owner, name: repo.name, ref: ref)
            let url = FileManager.default.temporaryDirectory.appendingPathComponent(result.fileName)
            try? FileManager.default.removeItem(at: url)
            try result.data.write(to: url, options: [.atomic, .completeFileProtection])
            progress = "\(result.files) files" + (result.skipped.isEmpty ? "" : " · \(result.skipped.count) skipped (too large or unreadable)")
            archive = url
        } catch {
            message = dartErrorText(error)
        }
    }
}
