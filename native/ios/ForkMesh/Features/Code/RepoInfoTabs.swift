import FMApi
import FMBase
import FMCode
import SwiftUI

struct RepoAboutTab: View {
    @Environment(\.fmPalette) private var fm

    let repo: Repository

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                RepoHeaderCard(repo: repo)
                RepoOwnerBountyWalletPanel(repo: repo)
                RepoInfoCard(title: "Repository details") {
                    VStack(alignment: .leading, spacing: 0) {
                        FmKeyValueRow(label: "Default branch", value: repo.defaultBranch)
                        FmKeyValueRow(label: "Language", value: repo.language.isEmpty ? "-" : repo.language)
                        FmKeyValueRow(label: "Stars", value: String(repo.stars))
                        FmKeyValueRow(label: "Forks", value: String(repo.forks))
                        FmKeyValueRow(label: "Mirrors", value: String(repo.mirrors))
                        FmKeyValueRow(label: "Visibility", value: repo.isPrivate ? "Private" : "Public")
                    }
                }
            }
            .padding(16)
        }
    }
}

struct RepoHeaderCard: View {
    @Environment(\.fmPalette) private var fm

    let repo: Repository

    private var chips: [(icon: String, label: String)] {
        var chips: [(icon: String, label: String)] = [("account_tree_outlined", repo.defaultBranch)]
        if !repo.language.isEmpty {
            chips.append(("circle", repo.language))
        }
        chips.append(("star_border", String(repo.stars)))
        chips.append(("call_split", String(repo.forks)))
        chips.append(("dns_outlined", "\(repo.mirrors) mirrors"))
        chips.append((repo.isPrivate ? "lock_outline" : "public", repo.isPrivate ? "Private" : "Public"))
        return chips
    }

    var body: some View {
        RepoInfoCard(title: repo.fullName) {
            VStack(alignment: .leading, spacing: 14) {
                Text(repo.description.isEmpty ? "No description provided." : repo.description)
                    .fmText(.bodyMedium, color: fm.textSecondary, height: 1.35)
                RepoChipWrap(chips)
            }
        }
    }
}

/// The retired custodial wallet, status only.
struct RepoOwnerBountyWalletPanel: View {
    @Environment(\.fmPalette) private var fm

    let repo: Repository

    private var gateCopy: String {
        return "Legacy Worker-custodied bounty wallets are frozen for an offline, balance-reconciled migration. Do not send funds to a historical deposit address. New bounties require an external self-custodial wallet, program, or multisig."
    }

    var body: some View {
        RepoInfoCard(title: "Legacy bounty wallet") {
            VStack(alignment: .leading, spacing: 0) {
                Text("Custodial wallet disabled · migration status only")
                    .fmText(.bodyMedium, color: fm.textPrimary, weight: FmType.emphasis)
                Text("ForkMesh no longer creates or uses Worker-held bounty keys. Historical public status may be shown for reconciliation, but this app never prepares or funds a custodial address.")
                    .fmText(.bodyMedium, color: fm.textSecondary, height: 1.35)
                    .padding(.top, 6)
                FmFilledButton("Custodial wallet disabled", icon: "account_balance_wallet_outlined", onPressed: nil)
                    .accessibilityIdentifier("repo-owner-bounty-wallet-prepare")
                    .padding(.top, 12)
                Text(gateCopy)
                    .fmText(.bodySmall, color: fm.textTertiary, height: 1.35)
                    .padding(.top, 10)
                Text("\(repo.owner) can review historical public signatures here, but no wallet or payment URI can be created from this client.")
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .padding(.top, 10)
            }
        }
    }
}

struct RepoMirrorsTab: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoMirrorsTabModel

    init(repo: Repository) {
        _model = State(initialValue: RepoMirrorsTabModel(client: RepoServices.client, repo: repo))
    }

    private func mirrorRow(_ mirror: RepoMirror) -> some View {
        RepoMobileCard(
            icon: RepoMirrorsTabModel.mirrorIcon(mirror),
            iconColor: mirror.online ? fm.success : fm.textTertiary,
            title: mirror.label,
            subtitle: RepoMirrorsTabModel.mirrorSubtitle(mirror),
            badge: RepoMirrorsTabModel.mirrorBadge(mirror))
    }

    @ViewBuilder private var content: some View {
        if model.mirrorSet.loading {
            RepoLoadingCard(label: "Loading…")
                .padding(16)
        } else if let error = model.mirrorSet.error {
            RepoEmptyCard(message: error)
                .padding(16)
        } else {
            VStack(spacing: 0) {
                if let card = model.syncCard {
                    RepoSyncStatusCardView(card: card)
                    FmRowDivider()
                }
                if model.mirrors.isEmpty {
                    RepoEmptyCard(message: RepoMirrorsTabModel.emptyMessage)
                        .padding(16)
                }
                RepoDividedRows(model.mirrors) { (mirror: RepoMirror) in
                    mirrorRow(mirror)
                }
            }
            .padding(.bottom, FmSpace.x4)
        }
    }

    var body: some View {
        ScrollView {
            content
        }
        .background(fm.listSurface)
        .task {
            await model.start()
        }
    }
}

/// The repository's standing in forkmesh-sync-status-v1.
struct RepoSyncStatusCardView: View {
    @Environment(\.fmPalette) private var fm

    let card: RepoSyncStatusCard

    var body: some View {
        RepoMobileCard(
            icon: card.icon,
            iconColor: fm.repoTone(card.tone),
            title: card.label,
            subtitle: card.summary,
            badge: card.badge)
            .accessibilityAddTraits(.updatesFrequently)
    }
}

struct RepoReleasesTab: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoReleasesTabModel
    @State private var notice: FmNotice?
    @State private var noticeTag: String?

    init(repo: Repository) {
        _model = State(initialValue: RepoReleasesTabModel(
            client: RepoServices.client, repo: repo, signer: RepoServices.signer))
    }

    private func request(_ action: RepoReleaseAction, _ release: RepoRelease) {
        Task {
            guard await AccountFeature.requireAccount(action: "request desktop control") else {
                return
            }
            let text: String = await model.request(action, for: release)
            noticeTag = release.tag
            notice = FmNotice(text, isError: text != action.successMessage)
        }
    }

    private func actionButton(_ action: RepoReleaseAction, _ release: RepoRelease) -> some View {
        Group {
            if action.filled {
                FmFilledButton(action.label, icon: action.icon, onPressed: { request(action, release) })
            } else {
                FmOutlinedButton(action.label, icon: action.icon, onPressed: { request(action, release) })
            }
        }
    }

    private func chips(_ release: RepoRelease) -> [(icon: String, label: String)] {
        var chips: [(icon: String, label: String)] = []
        if !release.channel.isEmpty {
            chips.append(("local_offer_outlined", release.channel))
        }
        if !release.shortCommit.isEmpty {
            chips.append(("commit_outlined", release.shortCommit))
        }
        if !release.createdDate.isEmpty {
            chips.append(("calendar_today_outlined", release.createdDate))
        }
        chips.append(("inventory_2_outlined", release.assetSummary))
        return chips
    }

    private func releaseCard(_ release: RepoRelease) -> some View {
        RepoInfoCard(title: release.tag) {
            VStack(alignment: .leading, spacing: 12) {
                RepoChipWrap(chips(release))
                Text("Release artifacts are content-addressed and verified by sha256 integrity pins. Mobile can request desktop release work; the paired Qt node builds, signs, mirrors, and publishes locally.")
                    .fmText(.bodyMedium, color: fm.textSecondary)
                if release.assets.isEmpty {
                    Text(RepoReleasesTabModel.noArtifacts)
                        .fmText(.bodyMedium)
                } else {
                    VStack(spacing: 8) {
                        ForEach(Array(release.assets.enumerated()), id: \.offset) { (entry: (offset: Int, element: RepoReleaseAsset)) in
                            RepoReleaseAssetRow(asset: entry.element)
                        }
                    }
                }
                if DesktopCommandAvailability.shared.available {
                    RepoWrap(spacing: 8, runSpacing: 8) {
                        ForEach(RepoReleaseAction.all) { (action: RepoReleaseAction) in
                            actionButton(action, release)
                        }
                    }
                }
                if noticeTag == release.tag {
                    FmNoticeLine($notice)
                }
            }
        }
    }

    @ViewBuilder private var content: some View {
        if model.releases.loading {
            RepoLoadingCard(label: "Loading releases...")
        } else if let error = model.releases.error {
            RepoEmptyCard(message: error)
        } else if let releases = model.releases.data, !releases.isEmpty {
            ForEach(Array(releases.enumerated()), id: \.offset) { (entry: (offset: Int, element: RepoRelease)) in
                releaseCard(entry.element)
            }
        } else {
            RepoEmptyCard(message: RepoReleasesTabModel.emptyMessage)
        }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                content
            }
            .padding(FmSpace.x4)
        }
        .task {
            await model.start()
        }
        .task {
            if let api: ApiService = Services.get(ApiService.self) {
                await DesktopCommandAvailability.shared.refresh(owner: model.repo.owner, name: model.repo.name, probe: api)
            }
        }
    }
}

struct RepoReleaseAssetRow: View {
    @Environment(\.fmPalette) private var fm

    let asset: RepoReleaseAsset

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(RepoReleasesTabModel.assetTitle(asset))
                .fmText(.bodyMedium, weight: FmType.emphasis)
            RepoChipWrap([
                ("devices_outlined", asset.platformLabel),
                ("storage_outlined", asset.sizeLabel),
                ("download_outlined", asset.downloadLabel),
            ])
            .padding(.top, 6)
            if !asset.sha256.isEmpty {
                Text("sha256: \(asset.sha256)")
                    .fmMono(size: FmType.meta, color: fm.textSecondary)
                    .textSelection(.enabled)
                    .padding(.top, 8)
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(fm.bgBase)
        .clipShape(FmRoundedShape(radius: FmRadius.lg))
        .overlay {
            FmRoundedShape(radius: FmRadius.lg).strokeBorder(fm.border, lineWidth: 1)
        }
    }
}

struct RepoWorktreesTab: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoWorktreesTabModel
    @State private var opened: RepoBranch?

    init(repo: Repository) {
        _model = State(initialValue: RepoWorktreesTabModel(client: RepoServices.client, repo: repo))
    }

    @ViewBuilder private var content: some View {
        if model.worktrees.loading {
            RepoLoadingCard(label: "Loading…")
                .padding(16)
        } else if let error = model.worktrees.error {
            RepoEmptyCard(message: error)
                .padding(16)
        } else if let branches = model.worktrees.data, !branches.isEmpty {
            RepoDividedRows(branches) { (branch: RepoBranch) in
                RepoMobileCard(
                    icon: "folder_special_outlined",
                    iconColor: fm.accent,
                    title: branch.name,
                    subtitle: branch.worktreePath,
                    chips: RepoWorktreesTabModel.chips(branch),
                    onTap: { opened = branch })
            }
            .padding(.bottom, FmSpace.x4)
        } else {
            RepoEmptyCard(message: RepoWorktreesTabModel.emptyMessage)
                .padding(16)
        }
    }

    var body: some View {
        ScrollView {
            content
        }
        .background(fm.listSurface)
        .task {
            await model.start()
        }
        .navigationDestination(item: $opened) { (branch: RepoBranch) in
            RepoWorktreeDetailScreen(repo: model.repo, branch: branch)
                .shellPageClearance()
        }
    }
}

struct RepoWorktreeDetailScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var notice: FmNotice?

    let repo: Repository
    let branch: RepoBranch

    private var chips: [(icon: String, label: String)] {
        var chips: [(icon: String, label: String)] = [("account_tree_outlined", branch.name)]
        if branch.isDefault {
            chips.append(("flag_outlined", "default branch"))
        }
        if !branch.sha.isEmpty {
            chips.append(("commit_outlined", RepoDisplay.shortHash(branch.sha)))
        }
        chips.append(("folder_outlined", repo.fullName))
        return chips
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                RepoInfoCard(title: branch.name) {
                    VStack(alignment: .leading, spacing: 0) {
                        RepoChipWrap(chips)
                            .padding(.bottom, 16)
                        FmKeyValueRow(label: "Branch", value: branch.name)
                        FmKeyValueRow(label: "Worktree path", value: branch.worktreePath)
                        if !branch.sha.isEmpty {
                            FmKeyValueRow(label: "Commit", value: branch.sha)
                        }
                        Text("This worktree is managed by the desktop node. Mobile can view worktree state; operations like merge, update, and delete are desktop-controlled.")
                            .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                            .padding(.top, 16)
                    }
                }
                RepoInfoCard(title: "Worktree actions") {
                    VStack(alignment: .leading, spacing: 12) {
                        Text("Worktree operations are controlled from the ForkMesh desktop app. Use it to merge, update from main, or delete this worktree.")
                            .fmText(.bodyMedium)
                        FmOutlinedButton("Copy path", icon: "copy_all_outlined", onPressed: {
                            notice = repoCopy("Worktree path", branch.worktreePath)
                        })
                        FmNoticeLine($notice)
                    }
                }
            }
            .padding(20)
        }
        .background(fm.canvas)
        .navigationTitle("Worktree: \(branch.name)")
        .navigationBarTitleDisplayMode(.inline)
    }
}
