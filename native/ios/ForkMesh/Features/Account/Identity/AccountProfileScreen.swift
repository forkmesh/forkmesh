import FMAccount
import FMApi
import FMBase
import SwiftUI

@MainActor
struct AccountProfileScreen: View {
    @Environment(\.fmPalette) var fm
    @State var model: AccountProfileModel?

    private let viewer: String

    init(name: String) {
        let api: ApiService? = Services.get(ApiService.self)
        viewer = Services.get(AuthService.self)?.session?.nodeName ?? ""
        _model = State(initialValue: api.map { AccountProfileModel(name: name, client: $0) })
    }

    var body: some View {
        Group {
            if let model = model {
                content(model)
            } else {
                FmEmptyState(icon: "person_outline", title: "Profile unavailable")
            }
        }
        .background(fm.bgBase.ignoresSafeArea())
        .navigationTitle("@\(model?.name ?? "")")
        .navigationBarTitleDisplayMode(.inline)
    }

    @ViewBuilder
    private func content(_ model: AccountProfileModel) -> some View {
        if let profile = model.profile, !profile.exists {
            OrgDetailScreen(orgName: model.name)
        } else if let profile = model.profile, profile.isPrivate {
            FmEmptyState(icon: "lock_outline", title: "@\(profile.name)", message: "This profile is private.")
        } else if let profile = model.profile {
            page(profile, model: model)
        } else if !model.error.isEmpty {
            OrgStateMessage(
                icon: "cloud_off_outlined", title: "Profile unavailable", message: model.error,
                action: FmTextButton("Retry", icon: "refresh", onPressed: { Task { await model.load() } }).fmAny)
        } else {
            FmSkeletonList(rows: 4, label: "Loading profile")
                .task { await model.load() }
        }
    }

    private func page(_ profile: AccountProfile, model: AccountProfileModel) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                SettingsSection(title: profile.kindLabel) {
                    SettingsCardBox {
                        VStack(alignment: .leading, spacing: FmSpace.x2) {
                            HStack(spacing: FmSpace.x3) {
                                FmAvatar(name: profile.name, size: 48)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("@\(profile.name)").fmText(.titleMedium).lineLimit(1)
                                    Text(profile.accountStatus).fmText(.bodySmall, color: fm.textTertiary)
                                }
                            }
                            if !profile.bio.isEmpty {
                                SettingsNote(text: profile.bio)
                            }
                            if !profile.location.isEmpty {
                                FmKeyValueRow(label: "Location", value: profile.location)
                            }
                            if !profile.isNode {
                                FmKeyValueRow(label: "Followers", value: "\(profile.followers)")
                                FmKeyValueRow(label: "Following", value: "\(profile.following)")
                            }
                            FmKeyValueRow(label: "Mirrors served", value: "\(profile.mirrorCount)")
                            if profile.createdAt > 0 {
                                FmKeyValueRow(label: "Joined", value: Format.relativeTime(profile.createdAt, nowMillis: SystemClock().nowMillis))
                            }
                        }
                    }
                }
                SettingsSection(title: profile.isNode ? "Owner" : "Nodes") {
                    SettingsCardBox(padded: false) { nodes(profile) }
                }
                if !profile.pubkey.isEmpty {
                    SettingsSection(title: "Keys", last: true) {
                        SettingsCardBox {
                            VStack(alignment: .leading, spacing: FmSpace.x3) {
                                if !profile.pubkey.isEmpty {
                                    CopyableValueRow(label: "Public key", value: profile.pubkey)
                                }
                            }
                        }
                    }
                }
            }
            .padding(FmSpace.x4)
        }
        .fmRefreshable { await model.load() }
    }

    @ViewBuilder
    private func nodes(_ profile: AccountProfile) -> some View {
        VStack(spacing: 0) {
            if profile.isNode {
                if profile.owner.isEmpty {
                    SettingsEmptyNote(title: profile.linkedNodesLine, text: "Link it from the desktop's node profile.")
                        .padding(FmSpace.x4)
                } else {
                    NavigationLink(value: Route.profile(name: profile.owner)) {
                        SettingsLinkRow(icon: "person_outline", title: "@\(profile.owner)", subtitle: "The user this node belongs to")
                    }
                    .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                }
            } else if profile.nodes.isEmpty {
                SettingsEmptyNote(title: profile.linkedNodesLine, text: "Nodes linked to this account appear here.")
                    .padding(FmSpace.x4)
            } else {
                ForEach(Array(profile.nodes.enumerated()), id: \.element) { (index: Int, node: String) in
                    if index > 0 { FmHairline() }
                    NavigationLink(value: Route.profile(name: node)) {
                        SettingsLinkRow(icon: "dns_outlined", title: node, subtitle: "Node")
                    }
                    .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                }
            }
            if profile.canLinkNode(viewer: viewer) {
                FmHairline()
                NavigationLink {
                    IdentityScreen()
                        .shellPageClearance()
                } label: {
                    SettingsLinkRow(icon: "link", title: "Link a node", subtitle: "Scan the code on your desktop")
                }
                .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
            }
        }
    }
}
