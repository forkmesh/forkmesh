// One organization's overview, members, teams and linked repos (with the add /
// role / permission / unlink actions for owners and admins), one team's roster
// (`TeamDetailScreen`) and the prompt sheets. Logic: FMApi's OrgDetailModel,
// TeamDetailModel and OrgsLogic.

import FMAccount
import FMApi
import FMBase
import SwiftUI

@MainActor
struct OrgDetailScreen: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.dismiss) var dismiss
    @State var model: OrgDetailModel?
    @State var confirmDelete: Bool = false
    @State var prompt: OrgPrompt?
    @State var openTeam: OrgTeam?

    let orgName: String

    init(orgName: String) {
        self.orgName = orgName
        if let api = Services.get(ApiService.self) {
            let session: AuthSession? = Services.get(AuthService.self)?.session
            _model = State(initialValue: OrgDetailModel(client: api, orgName: orgName, session: session))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        Group {
            if let model = model {
                content(model)
            } else {
                FmEmptyState(icon: "cloud_off_outlined", title: "Could not load organization")
            }
        }
        .background(fm.bgBase.ignoresSafeArea())
        .navigationBarTitleDisplayMode(.inline)
    }

    private func content(_ model: OrgDetailModel) -> some View {
        OrgDetailBody(
            model: model,
            onAddMember: { prompt = .member },
            onAddTeam: { prompt = .team },
            onLinkRepo: { prompt = .repo(defaultNode: model.defaultNode) },
            onOpenTeam: { (team: OrgTeam) in openTeam = team })
            .toolbar {
                ToolbarItem(placement: .principal) {
                    VStack(spacing: 2) {
                        Text(model.title).fmText(.titleMedium).lineLimit(1)
                        Text(model.subtitle).fmText(.bodySmall).lineLimit(1)
                    }
                }
                if model.isOwner {
                    ToolbarItem(placement: .topBarTrailing) {
                        FmIconButton(
                            icon: "delete_outline", label: "Delete organization", color: fm.danger,
                            onPressed: { confirmDelete = true })
                            .accessibilityIdentifier("org-delete-button")
                    }
                }
            }
            .task { await model.load() }
            .sheet(item: $prompt) { (kind: OrgPrompt) in
                OrgPromptSheet(
                    kind: kind,
                    onMember: { (input: OrgMemberInput) in
                        prompt = nil
                        Task { await model.addMember(input) }
                    },
                    onTeam: { (input: OrgTeamInput) in
                        prompt = nil
                        Task { await model.addTeam(input) }
                    },
                    onRepo: { (input: OrgRepoInput) in
                        prompt = nil
                        Task { await model.linkRepo(input) }
                    })
                    .fmSheetStyle(fitsContent: true, background: fm.bgRaised)
            }
            .navigationDestination(item: $openTeam) { (team: OrgTeam) in
                TeamDetailScreen(orgName: orgName, team: team, orgMembers: model.members, canManage: model.canManage)
                    .shellPageClearance()
                    .onDisappear { Task { await model.load() } }
            }
            .fmDialog(isPresented: $confirmDelete) {
                FmConfirmDialog(
                    title: orgDeleteTitle(orgName),
                    message: OrgCopy.deleteBody,
                    confirmLabel: "Delete",
                    destructive: true,
                    onCancel: { confirmDelete = false },
                    onConfirm: {
                        confirmDelete = false
                        Task { @MainActor in
                            if await model.deleteOrg() {
                                dismiss()
                            }
                        }
                    })
            }
    }
}

/// Which prompt sheet is up.
enum OrgPrompt: Identifiable, Hashable {
    case member
    case team
    case repo(defaultNode: String)

    var id: String {
        switch self {
        case .member: return "member"
        case .team: return "team"
        case .repo: return "repo"
        }
    }
}

private struct OrgDetailBody: View {
    @Environment(\.fmPalette) var fm
    let model: OrgDetailModel
    let onAddMember: () -> Void
    let onAddTeam: () -> Void
    let onLinkRepo: () -> Void
    let onOpenTeam: (OrgTeam) -> Void

    var body: some View {
        if model.loading && model.profile == nil {
            FmSkeletonList(rows: 5, label: "Loading organization")
        } else if let profile = model.profile {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x5) {
                    if model.actionError != nil {
                        FmNoticeLine(model.actionError, isError: true, onDismiss: { model.actionError = nil })
                            .accessibilityIdentifier("org-action-error")
                    }
                    OrgOverviewCard(profile: profile)
                    membersSection
                    teamsSection
                    reposSection
                }
                .padding(FmSpace.x4)
            }
            .fmRefreshable { await model.load() }
        } else {
            OrgStateMessage(
                icon: "cloud_off_outlined", title: "Could not load organization", message: model.error,
                action: FmTextButton("Retry", icon: "refresh", onPressed: { Task { await model.load() } }).fmAny)
        }
    }

    private var membersSection: some View {
        OrgSection(title: "Members", count: model.members.count, emptyLabel: nil, onAdd: model.canManage ? onAddMember : nil) {
            ForEach(model.members, id: \.name) { (member: OrgMember) in
                OrgMemberRow(
                    member: member,
                    canManage: model.canManage,
                    onChoose: { (value: String) in Task { await model.memberMenu(member, choose: value) } })
            }
        }
    }

    private var teamsSection: some View {
        OrgSection(title: "Teams", count: model.teams.count, emptyLabel: "No teams yet", onAdd: model.canManage ? onAddTeam : nil) {
            ForEach(model.teams, id: \.team) { (team: OrgTeam) in
                OrgTeamRow(
                    team: team,
                    canManage: model.canManage,
                    onOpen: { onOpenTeam(team) },
                    onChoose: { (value: String) in Task { await model.teamMenu(team, choose: value) } })
            }
        }
    }

    private var reposSection: some View {
        OrgSection(
            title: "Linked repos", count: model.repos.count, emptyLabel: "No repos linked",
            onAdd: model.canManage ? onLinkRepo : nil
        ) {
            ForEach(model.repos, id: \.repo) { (repo: OrgRepo) in
                OrgRepoRow(
                    repo: repo, orgName: model.orgName, canManage: model.canManage,
                    onUnlink: { Task { await model.unlink(repo) } })
            }
        }
    }
}

private struct OrgOverviewCard: View {
    @Environment(\.fmPalette) var fm
    let profile: OrgProfile

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: 0) {
                HStack {
                    Text(profile.title)
                        .fmText(.titleMedium)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    if !profile.viewerRole.isEmpty {
                        OrgRoleBadge(role: profile.viewerRole)
                    }
                }
                if !profile.description.isEmpty {
                    Text(profile.description)
                        .fmText(.bodyMedium, color: fm.textSecondary, height: 1.35)
                        .padding(.top, FmSpace.x2)
                }
                HStack(spacing: 0) {
                    ForEach(orgStats(profile), id: \.label) { (stat: OrgStat) in
                        VStack(alignment: .leading, spacing: 0) {
                            Text(verbatim: "\(stat.value)").fmText(.titleLarge, tabular: true)
                            Text(stat.label).fmText(.bodySmall, color: fm.textTertiary)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .padding(.top, FmSpace.x3)
            }
        }
    }
}

/// A header with a count and an add control, then the rows or an
/// empty note.
private struct OrgSection<Rows: View>: View {
    @Environment(\.fmPalette) var fm
    let title: String
    let count: Int
    let emptyLabel: String?
    let onAdd: (() -> Void)?
    let rows: Rows

    init(title: String, count: Int, emptyLabel: String?, onAdd: (() -> Void)?, @ViewBuilder rows: () -> Rows) {
        self.title = title
        self.count = count
        self.emptyLabel = emptyLabel
        self.onAdd = onAdd
        self.rows = rows()
    }

    private var addButton: AnyView? {
        guard let onAdd = onAdd else {
            return nil
        }
        return FmIconButton(icon: "add_circle_outline", label: "Add", size: 20, color: fm.accent, onPressed: onAdd)
            .frame(width: 32, height: 32)
            .fmAny
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: title, trailing: addButton, count: addButton == nil ? count : nil)
            if count == 0 {
                FmCard(radius: FmRadius.lg) {
                    Text(emptyLabel ?? "Nothing here yet")
                        .fmText(.bodyMedium, color: fm.textTertiary)
                }
            } else {
                VStack(spacing: FmSpace.x2) {
                    rows
                }
            }
        }
    }
}

private struct OrgMemberRow: View {
    @Environment(\.fmPalette) var fm
    let member: OrgMember
    let canManage: Bool
    let onChoose: (String) -> Void

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            HStack(spacing: FmSpace.x2) {
                Text(member.name)
                    .fmText(.bodyMedium, weight: FmType.emphasis)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: .infinity, alignment: .leading)
                OrgRoleBadge(role: member.role)
                if canManage {
                    Menu {
                        ForEach(memberMenuRoles(member)) { (item: OrgMenuItem) in
                            Button(item.label) { onChoose(item.value) }
                                .disabled(!item.enabled)
                        }
                        Divider()
                        Button(memberMenuRemove.label, role: .destructive) { onChoose(memberMenuRemove.value) }
                    } label: {
                        FmIcon("more_vert", size: 24, color: fm.textTertiary, label: "Member actions")
                            .frame(width: 36, height: 36)
                    }
                }
            }
        }
    }
}

private struct OrgTeamRow: View {
    @Environment(\.fmPalette) var fm
    let team: OrgTeam
    let canManage: Bool
    let onOpen: () -> Void
    let onChoose: (String) -> Void

    /// The team's name, count and permission open the roster; the menu stays
    /// its own control (a Menu inside a Button would never open).
    private var opener: some View {
        Button(action: {
            FmHaptics.tap()
            onOpen()
        }) {
            HStack(spacing: FmSpace.x2) {
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    Text(team.team)
                        .fmText(.bodyMedium, weight: FmType.emphasis)
                        .lineLimit(1)
                        .truncationMode(.tail)
                    Text(teamMemberCountLabel(team.members))
                        .fmText(.bodySmall, color: fm.textTertiary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                FmStatusBadge(label: team.permission, color: fm.textSecondary)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.06), cornerRadius: FmRadius.md))
    }

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            HStack(spacing: FmSpace.x2) {
                opener
                if canManage {
                    Menu {
                        ForEach(teamMenuPermissions(team)) { (item: OrgMenuItem) in
                            Button(item.label) { onChoose(item.value) }
                                .disabled(!item.enabled)
                        }
                        Divider()
                        Button(teamMenuDelete.label, role: .destructive) { onChoose(teamMenuDelete.value) }
                    } label: {
                        FmIcon("more_vert", size: 24, color: fm.textTertiary, label: "Team actions")
                            .frame(width: 36, height: 36)
                    }
                }
            }
        }
    }
}

private struct OrgRepoRow: View {
    @Environment(\.fmPalette) var fm
    let repo: OrgRepo
    let orgName: String
    let canManage: Bool
    let onUnlink: () -> Void

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            HStack(spacing: FmSpace.x2) {
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    Text(linkedRepoTitle(orgName, repo))
                        .fmText(.bodyMedium, weight: FmType.emphasis)
                        .lineLimit(1)
                        .truncationMode(.tail)
                    if let serves = linkedRepoServes(repo) {
                        Text(serves)
                            .fmText(.bodySmall, color: fm.textTertiary)
                            .lineLimit(1)
                            .truncationMode(.tail)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                if canManage {
                    FmIconButton(icon: "link_off", label: "Unlink repo", size: 20, color: fm.textTertiary, onPressed: onUnlink)
                        .frame(width: 40, height: 40)
                }
            }
        }
    }
}

// MARK: Team

/// Which existing org members belong to one team. Team
/// membership only ever raises a member's permission (the Worker rejects
/// non-members), so the add picker lists current org members only.
@MainActor
struct TeamDetailScreen: View {
    @Environment(\.fmPalette) var fm
    @State var model: TeamDetailModel?
    @State var notice: FmNotice?
    @State var picking: Bool = false

    init(orgName: String, team: OrgTeam, orgMembers: [OrgMember], canManage: Bool) {
        if let api = Services.get(ApiService.self) {
            _model = State(
                initialValue: TeamDetailModel(
                    client: api, orgName: orgName, team: team, orgMembers: orgMembers, canManage: canManage))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        Group {
            if let model = model {
                content(model)
            }
        }
        .background(fm.bgBase.ignoresSafeArea())
        .navigationBarTitleDisplayMode(.inline)
    }

    private func addToTeam(_ model: TeamDetailModel) {
        if model.candidates.isEmpty {
            notice = FmNotice(teamAlreadyFull)
            return
        }
        notice = nil
        picking = true
    }

    private func content(_ model: TeamDetailModel) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            if model.actionError != nil {
                FmNoticeLine(model.actionError, isError: true, onDismiss: { model.actionError = nil })
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.top, FmSpace.x3)
                    .accessibilityIdentifier("team-action-error")
            }
            if notice != nil {
                FmNoticeLine($notice)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.top, FmSpace.x3)
            }
            TeamRoster(model: model)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .toolbar {
            ToolbarItem(placement: .principal) {
                VStack(spacing: 2) {
                    Text(model.team.team).fmText(.titleMedium).lineLimit(1)
                    Text(model.subtitle).fmText(.bodySmall).lineLimit(1)
                }
            }
            if model.canManage {
                ToolbarItem(placement: .topBarTrailing) {
                    FmIconButton(icon: "add", label: "Add member to team", onPressed: { addToTeam(model) })
                }
            }
        }
        .task { await model.load() }
        .sheet(isPresented: $picking) {
            OrgPickMemberSheet(candidates: model.candidates, onPick: { (name: String) in
                picking = false
                Task { await model.add(name) }
            })
            .fmSheetStyle(fitsContent: true, background: fm.bgRaised)
        }
    }
}

private struct TeamRoster: View {
    @Environment(\.fmPalette) var fm
    let model: TeamDetailModel

    var body: some View {
        if model.loading {
            FmSkeletonList(rows: 4, label: "Loading members")
        } else if model.members.isEmpty {
            FmEmptyState(icon: "group_outlined", title: "No members on this team", message: model.emptyMessage)
        } else {
            ScrollView {
                VStack(spacing: FmSpace.x2) {
                    ForEach(model.members, id: \.name) { (member: OrgMember) in
                        FmCard(radius: FmRadius.lg) {
                            HStack {
                                Text(member.name)
                                    .fmText(.bodyMedium, weight: FmType.emphasis)
                                    .frame(maxWidth: .infinity, alignment: .leading)
                                if model.canManage {
                                    FmIconButton(
                                        icon: "close", label: "Remove from team", size: 20, color: fm.textTertiary,
                                        onPressed: { Task { await model.remove(member) } })
                                        .frame(width: 40, height: 40)
                                }
                            }
                        }
                    }
                }
                .padding(FmSpace.x4)
            }
        }
    }
}

// MARK: Sheets

private struct OrgPickMemberSheet: View {
    @Environment(\.fmPalette) var fm
    let candidates: [OrgMember]
    let onPick: (String) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("Add member to team")
                .fmText(.titleMedium)
                .padding(FmSpace.x4)
            ForEach(candidates, id: \.name) { (member: OrgMember) in
                FmListRow(
                    title: member.name,
                    trailing: OrgRoleBadge(role: member.role).fmAny,
                    onTap: { onPick(member.name) },
                    showChevron: false)
            }
        }
        .padding(.bottom, FmSpace.x3)
    }
}

/// The member, team and repository prompt sheets in one layout.
private struct OrgPromptSheet: View {
    @Environment(\.fmPalette) var fm
    let kind: OrgPrompt
    let onMember: (OrgMemberInput) -> Void
    let onTeam: (OrgTeamInput) -> Void
    let onRepo: (OrgRepoInput) -> Void

    @State var name: String = ""
    @State var choice: String = ""
    @State var node: String = ""

    init(
        kind: OrgPrompt,
        onMember: @escaping (OrgMemberInput) -> Void,
        onTeam: @escaping (OrgTeamInput) -> Void,
        onRepo: @escaping (OrgRepoInput) -> Void
    ) {
        self.kind = kind
        self.onMember = onMember
        self.onTeam = onTeam
        self.onRepo = onRepo
        switch kind {
        case .member:
            _choice = State(initialValue: "member")
        case .team:
            _choice = State(initialValue: "read")
        case .repo(let defaultNode):
            _node = State(initialValue: defaultNode)
        }
    }

    private var title: String {
        switch kind {
        case .member: return OrgCopy.memberPromptTitle
        case .team: return OrgCopy.teamPromptTitle
        case .repo: return OrgCopy.repoPromptTitle
        }
    }

    private var helper: String {
        switch kind {
        case .member: return OrgCopy.memberPromptHelper
        case .team: return OrgCopy.teamPromptHelper
        case .repo: return OrgCopy.repoPromptHelper
        }
    }

    @ViewBuilder private var fields: some View {
        switch kind {
        case .member:
            OrgTextField(label: "Account name", text: $name, autofocus: true)
                .accessibilityIdentifier("org-member-field")
            OrgChoiceField(label: "Role", options: orgRoles, value: $choice)
        case .team:
            OrgTextField(label: "Team name (lowercase, dashes)", text: $name, autofocus: true)
                .accessibilityIdentifier("org-team-field")
            OrgChoiceField(label: "Permission", options: orgTeamPermissions, value: $choice)
        case .repo:
            OrgTextField(label: "Repo name", text: $name, autofocus: true)
                .accessibilityIdentifier("org-repo-field")
            OrgTextField(label: "Your node (defaults to your account)", text: $node)
        }
    }

    /// Save does nothing for a blank name.
    private func save() {
        switch kind {
        case .member:
            if let input = orgMemberInput(name, choice) {
                onMember(input)
            }
        case .team:
            if let input = orgTeamInput(name, choice) {
                onTeam(input)
            }
        case .repo:
            if let input = orgRepoInput(name, node) {
                onRepo(input)
            }
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(title).fmText(.titleLarge)
            Text(helper)
                .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                .padding(.top, FmSpace.x2)
            VStack(alignment: .leading, spacing: 0) {
                fields
            }
            .padding(.top, FmSpace.x4)
            FmFilledButton("Save", fillWidth: true, onPressed: { save() })
                .padding(.top, FmSpace.x2)
                .accessibilityIdentifier("org-prompt-submit")
        }
        .padding(FmSpace.x4)
    }
}

private struct OrgChoiceField: View {
    @Environment(\.fmPalette) var fm
    let label: String
    let options: [String]
    @Binding var value: String

    var body: some View {
        FmField(label: label) {
            Menu {
                ForEach(options, id: \.self) { (option: String) in
                    Button(option) { value = option }
                }
            } label: {
                HStack {
                    Text(value).fmText(.bodyLarge)
                    Spacer(minLength: 0)
                    FmIcon("arrow_drop_down", size: 24)
                }
                .padding(.horizontal, FmSpace.x4)
                .padding(.vertical, FmSpace.x3)
                .background(fm.bgBase)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
            }
        }
        .padding(.bottom, FmSpace.x3)
    }
}
