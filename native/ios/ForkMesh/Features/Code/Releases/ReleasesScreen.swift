import FMAccount
import FMApi
import FMBase
import FMCode
import FMCrypto
import SwiftUI
import UIKit

struct RepoReleasesScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoReleasesModel?

    init(repo: RepoRef) {
        if let client = CIServices.client {
            _model = State(initialValue: RepoReleasesModel(client: client, repo: repo))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        Group {
            if let model = model {
                List {
                    if let error = model.error {
                        Text(error).foregroundStyle(fm.danger)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .fmMessageTools(error)
                    }
                    if model.releases.isEmpty && !model.loading {
                        Text("No releases published yet.").foregroundStyle(fm.textSecondary)
                    }
                    ForEach(model.releases, id: \.tag) { (release: RepoRelease) in
                        NavigationLink {
                            ReleaseDetailScreen(model: model, tag: release.tag)
                                .shellPageClearance()
                        } label: {
                            ReleaseRow(release: release)
                        }
                    }
                }
                .listStyle(.insetGrouped)
                .overlay {
                    if model.loading && model.releases.isEmpty {
                        ProgressView()
                    }
                }
                .task { await model.load() }
                .fmRefreshable { await model.load(refresh: true) }
            } else {
                FmEmptyState(icon: "new_releases_outlined", title: "Releases are unavailable")
            }
        }
        .navigationTitle("Releases")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct ReleaseRow: View {
    @Environment(\.fmPalette) private var fm
    let release: RepoRelease

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(release.tag).font(.body.weight(.semibold))
                if !release.channel.isEmpty {
                    FmStatusBadge(label: release.channel, color: fm.accent)
                }
                Spacer()
                Text(release.createdDate).font(.caption).foregroundStyle(fm.textSecondary)
            }
            Text(release.assetSummary).font(.caption).foregroundStyle(fm.textSecondary)
        }
    }
}

struct ReleaseDetailScreen: View {
    @Environment(\.fmPalette) private var fm
    let model: RepoReleasesModel
    let tag: String
    @State private var announcing: Bool = false
    @State private var desktopNotice: String?

    var body: some View {
        List {
            if let release = model.release(tag: tag) {
                Section {
                    FmKeyValueRow(label: "Tag", value: release.tag)
                    if !release.shortCommit.isEmpty {
                        FmKeyValueRow(label: "Commit", value: release.shortCommit)
                    }
                    if !release.createdDate.isEmpty {
                        FmKeyValueRow(label: "Published", value: release.createdDate)
                    }
                    if !release.channel.isEmpty {
                        FmKeyValueRow(label: "Channel", value: release.channel)
                    }
                }
                Section("Artifacts") {
                    if release.assets.isEmpty {
                        Text("No published artifacts yet").foregroundStyle(fm.textSecondary)
                    }
                    ForEach(release.assets, id: \.sha256) { (asset: RepoReleaseAsset) in
                        ReleaseAssetRow(asset: asset)
                    }
                }
                Section {
                    Button {
                        announcing = true
                    } label: {
                        Label("Announce on the Fediverse", systemImage: "megaphone")
                    }
                }
                if DesktopCommandAvailability.shared.available {
                    desktopSection(release)
                }
            } else {
                Text("This release is not in the list any more.").foregroundStyle(fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools("This release is not in the list any more.")
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle(tag)
        .navigationBarTitleDisplayMode(.inline)
        .sheet(isPresented: $announcing) {
            if let release = model.release(tag: tag) {
                ReleaseAnnounceSheet(model: model, release: release)
            }
        }
        .task {
            if let api: ApiService = Services.get(ApiService.self) {
                await DesktopCommandAvailability.shared.refresh(owner: model.repo.owner, name: model.repo.name, probe: api)
            }
        }
    }

    private func desktopSection(_ release: RepoRelease) -> some View {
        Section("On your desktop") {
            ForEach(RepoReleaseAction.all) { (action: RepoReleaseAction) in
                Button(action.label) { requestOnDesktop(action, release) }
            }
            if let desktopNotice = desktopNotice {
                Text(desktopNotice).font(.caption).foregroundStyle(fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(desktopNotice)
            }
        }
    }

    private func requestOnDesktop(_ action: RepoReleaseAction, _ release: RepoRelease) {
        let repo: RepoRef = model.repo
        Task {
            guard await AccountFeature.requireAccount(action: DesktopCommand.guestAction) else {
                return
            }
            desktopNotice = await DesktopCommandRequests.request(
                client: RepoServices.client, signer: RepoServices.signer, owner: repo.owner, name: repo.name,
                command: action.command, target: DesktopCommandContract.releaseTarget(release.tag),
                payload: RepoDesktopCommands.releasePayload(release), successMessage: action.successMessage)
        }
    }
}

struct ReleaseAssetRow: View {
    @Environment(\.fmPalette) private var fm
    let asset: RepoReleaseAsset

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(asset.name.isEmpty ? asset.shortSha : asset.name)
            Text("\(asset.platformLabel) · \(asset.sizeLabel) · \(asset.downloadLabel)")
                .font(.caption)
                .foregroundStyle(fm.textSecondary)
            Text(asset.shortSha).font(.caption2).monospaced().foregroundStyle(fm.textTertiary)
        }
        .contextMenu {
            Button("Copy SHA-256") { UIPasteboard.general.string = asset.sha256 }
        }
    }
}

struct ReleaseAnnounceSheet: View {
    @Environment(\.dismiss) private var dismiss
    let model: RepoReleasesModel
    let release: RepoRelease
    @State private var draft: ReleaseAnnouncementDraft
    @State private var message: String = ""

    init(model: RepoReleasesModel, release: RepoRelease) {
        self.model = model
        self.release = release
        _draft = State(initialValue: ReleaseAnnouncementDraft(release: release, repo: model.repo))
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Title") {
                    TextField("Title", text: $draft.title)
                }
                Section("Text") {
                    TextEditor(text: $draft.body).frame(minHeight: 140)
                }
                if !message.isEmpty {
                    Section {
                        Text(message)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .fmMessageTools(message)
                    }
                }
            }
            .navigationTitle("Announce \(release.tag)")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Post") { post() }
                        .disabled(model.busy || draft.validationError != nil)
                }
            }
        }
    }

    private func post() {
        Task {
            let account: String = Services.get(AuthService.self)?.session?.nodeName ?? ""
            let identity: DeviceIdentity? = Services.get(DeviceIdentity.self)
            let now: Int64 = SystemClock().nowMillis
            message = await model.announce(draft, tag: release.tag, account: account, identity: identity, nowMillis: now)
            if message == "Announced on the Fediverse." {
                dismiss()
            }
        }
    }
}

struct RepoArtifactsScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var releases: RepoReleasesModel?
    @State private var mirrors: RepoMirrorsModel?

    init(repo: RepoRef) {
        if let client = CIServices.client {
            _releases = State(initialValue: RepoReleasesModel(client: client, repo: repo))
            _mirrors = State(initialValue: RepoMirrorsModel(client: client, repo: repo))
        } else {
            _releases = State(initialValue: nil)
            _mirrors = State(initialValue: nil)
        }
    }

    var body: some View {
        List {
            if let releases = releases {
                Section {
                    FmKeyValueRow(label: "Release artifacts", value: "\(releases.artifacts.count)")
                    FmKeyValueRow(label: "Total size", value: ByteCountFormatter.string(fromByteCount: Int64(releases.artifactsTotalBytes), countStyle: .file))
                    if let mirrors = mirrors, mirrors.totalArtifacts > 0 {
                        FmKeyValueRow(label: "Build artifacts on hosts", value: "\(mirrors.totalArtifacts)")
                    }
                } footer: {
                    Text("Build artifacts are kept and pruned by the desktop and hosts that run this repository's actions.")
                }
                ForEach(releases.artifacts) { (row: RepoArtifactRow) in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(row.tag).font(.caption).foregroundStyle(fm.textSecondary)
                        ReleaseAssetRow(asset: row.asset)
                    }
                }
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle("Artifacts")
        .navigationBarTitleDisplayMode(.inline)
        .task {
            await releases?.load()
            await mirrors?.load()
        }
        .fmRefreshable {
            await releases?.load(refresh: true)
            await mirrors?.load()
        }
    }
}

struct ReleaseDestination: View {
    let repo: RepoRef
    let tag: String
    @State private var model: RepoReleasesModel?

    init(repo: RepoRef, tag: String) {
        self.repo = repo
        self.tag = tag
        if let client = CIServices.client {
            _model = State(initialValue: RepoReleasesModel(client: client, repo: repo))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        Group {
            if let model = model {
                ReleaseDetailScreen(model: model, tag: tag)
                    .overlay {
                        if model.loading {
                            ProgressView()
                        }
                    }
                    .task { await model.load() }
            } else {
                FmEmptyState(icon: "new_releases_outlined", title: "Releases are unavailable")
            }
        }
    }
}
