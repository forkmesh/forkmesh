// The quick project switcher (every project with
// its host and who you are there, one tap to move the app to it) and `ProjectChip`, the compact
// "which instance am I on?" button that opens it.
//
// `ProjectSwitcherButton` is self-contained: it finds everything it needs in `Services`, so the
// shell (the top bar, or the bottom bar that replaces it), the guest bar and the sign-in screens can
// place it anywhere without passing anything in. The switch itself runs through the
// RelaySwitchController, whose overlay covers the app while the project underneath is replaced.

import FMAccount
import FMApi
import FMBase
import FMChat
import FMNet
import Observation
import SwiftUI
import UIKit

/// The active project's name as a chip (or, `compact`, just the icon) that opens the switcher.
struct ProjectSwitcherButton: View {
    @Environment(\.fmPalette) private var fm
    @State private var settings: SettingsService? = Services.get(SettingsService.self)
    @State private var showing: Bool = false

    /// The project's icon only, for a crowded bar.
    let compact: Bool
    /// Drawn over dark artwork (the welcome screen).
    let onDark: Bool

    init(compact: Bool = false, onDark: Bool = false) {
        self.compact = compact
        self.onDark = onDark
    }

    var body: some View {
        if let settings = settings {
            let project: RelayProfile = settings.project
            Button {
                FmHaptics.tap()
                showing = true
            } label: {
                label(project)
            }
            .buttonStyle(.plain)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel("Project \(project.label), \(project.displayOrigin). Switch project")
            .accessibilityAddTraits(.isButton)
            .projectDrawer(isPresented: $showing)
        }
    }

    private var tint: Color { onDark ? Color.white : fm.textPrimary }

    @ViewBuilder private func label(_ project: RelayProfile) -> some View {
        if compact {
            ProjectIcon(project: project, size: 30)
                .frame(width: 44, height: 44)
                .contentShape(Rectangle())
        } else {
            HStack(spacing: FmSpace.x1) {
                FmIcon("layers_outlined", size: 16, color: tint)
                Text(project.label)
                    .fmText(.labelLarge, color: tint)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: 180, alignment: .leading)
                    .fixedSize(horizontal: true, vertical: false)
                FmIcon("expand_more", size: 16, color: tint)
            }
            .padding(.leading, FmSpace.x3)
            .padding(.trailing, FmSpace.x2)
            .frame(minHeight: 36)
            .overlay {
                Capsule().strokeBorder(onDark ? Color.white.opacity(0.5) : fm.border, lineWidth: 1)
            }
            .contentShape(Capsule())
            .frame(minHeight: 44)
        }
    }
}

struct ProjectIcon: View {
    @Environment(\.fmPalette) private var fm
    @State private var images: ProjectImages = ProjectImages.shared
    let project: RelayProfile
    var size: CGFloat = 32

    private var shape: RoundedRectangle {
        RoundedRectangle(cornerRadius: size * 0.25, style: .continuous)
    }

    var body: some View {
        Group {
            if let image = images.image(for: project) {
                Image(uiImage: image)
                    .resizable()
                    .interpolation(.high)
                    .scaledToFit()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(fm.bgRaised)
            } else if AppEnv.isProdAlias(project.id), let image = UIImage(named: "Logo") {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFit()
                    .padding(size * 0.11)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(Color.black)
            } else {
                Text(FmAvatar.initial(project.label))
                    .font(FmFonts.ui(size: size * 0.5, weight: FmType.emphasis))
                    .foregroundStyle(Color.white)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(FmColors.senderColor(project.label))
            }
        }
        .frame(width: size, height: size)
        .clipShape(shape)
        .accessibilityHidden(true)
        .task(id: project.logo + "|" + project.id) {
            await images.load(project)
        }
    }
}

extension View {
    func projectDrawer(isPresented: Binding<Bool>) -> some View {
        fullScreenCover(isPresented: isPresented) {
            ProjectDrawer()
                .presentationBackground(.clear)
        }
        .transaction(value: isPresented.wrappedValue) { (transaction: inout Transaction) in
            transaction.disablesAnimations = true
        }
    }
}

struct ProjectDrawer: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var shown: Bool = false

    static let widthFraction: CGFloat = 0.75

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            let width: CGFloat = proxy.size.width * ProjectDrawer.widthFraction
            ZStack(alignment: .leading) {
                Color.black.opacity(shown ? 0.45 : 0)
                    .ignoresSafeArea()
                    .onTapGesture { close(then: nil) }
                    .accessibilityLabel("Close projects")
                    .accessibilityAddTraits(.isButton)
                ProjectSwitcher(close: close(then:))
                    .frame(width: width)
                    .frame(maxHeight: .infinity, alignment: .top)
                    .background(fm.bgRaised.ignoresSafeArea())
                    .shadow(color: Color.black.opacity(0.3), radius: 18, x: 6)
                    .offset(x: shown ? 0 : -(width + 24))
            }
        }
        .accessibilityAction(.escape) { close(then: nil) }
        .onAppear {
            withAnimation(.easeOut(duration: 0.25)) { shown = true }
        }
    }

    private func close(then: (() -> Void)?) {
        withAnimation(.easeIn(duration: 0.18)) {
            shown = false
        } completion: {
            var transaction: Transaction = Transaction()
            transaction.disablesAnimations = true
            withTransaction(transaction) { dismiss() }
            then?()
        }
    }
}

/// The switcher drawer: every project, which one is active, who you are on each; add a project
/// or open the full list to manage them.
struct ProjectSwitcher: View {
    @Environment(\.fmPalette) private var fm
    let close: (_ then: (() -> Void)?) -> Void
    @State private var model: RelaySettingsModel? = RelaySettingsSection.makeModel()
    @State private var adding: Bool = false
    @State private var managing: Bool = false
    @State private var inviting: Bool = false
    @State private var choosingInstance: RelayProfile?

    init(close: @escaping (_ then: (() -> Void)?) -> Void) {
        self.close = close
    }

    static let explanation: String = "Each project has its own sign-in, keys and data on this phone, and can be attached to a ForkMesh instance."

    var body: some View {
        Group {
            if let model = model {
                content(model)
            } else {
                Text("Projects are not available right now.")
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .padding(FmSpace.x4)
            }
        }
        .sheet(isPresented: $adding) {
            AddProjectSheet(onAdded: { (profile: RelayProfile) in
                adding = false
                switchTo(profile)
            })
        }
        .sheet(isPresented: $managing) {
            ProjectsManageSheet()
        }
        .sheet(isPresented: $inviting) {
            if let active = model?.rows.first(where: { $0.active })?.relay {
                ProjectInviteSheet(project: active)
            }
        }
        .sheet(isPresented: choosingInstanceShown) {
            if let project = choosingInstance {
                ChooseInstanceView(project: project, onClose: { choosingInstance = nil })
            }
        }
    }

    private var choosingInstanceShown: Binding<Bool> {
        Binding<Bool>(
            get: { choosingInstance != nil },
            set: { (shown: Bool) in
                if !shown {
                    choosingInstance = nil
                }
            })
    }

    private func content(_ model: RelaySettingsModel) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                FmPanelHeader(title: "Projects")
                Text(ProjectSwitcher.explanation)
                    .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                    .padding(.top, FmSpace.x2)
                FmCard(padding: EdgeInsets(), radius: FmRadius.lg) {
                    VStack(spacing: 0) {
                        ForEach(Array(model.rows.enumerated()), id: \.element.id) {
                            (entry: (offset: Int, element: RelayRowInfo)) in
                            if entry.offset > 0 {
                                FmRowDivider()
                            }
                            ProjectSwitcherRow(
                                info: entry.element,
                                online: entry.element.active && model.connection == .connected,
                                onTap: entry.element.active || model.canSwitch
                                    ? { switchTo(entry.element.relay) } : nil,
                                onChooseInstance: { choosingInstance = entry.element.relay })
                        }
                    }
                }
                .padding(.top, FmSpace.x3)
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    FmFilledButton("Add project", icon: "add", onPressed: model.canSwitch ? { adding = true } : nil)
                    if let active = model.rows.first(where: { $0.active })?.relay {
                        FmOutlinedButton("Invite people to \(active.label)", icon: "group_outlined", onPressed: {
                            inviting = true
                        })
                        .accessibilityIdentifier("project-invite")
                    }
                    FmOutlinedButton("Manage projects", icon: "tune", onPressed: { managing = true })
                }
                .padding(.top, FmSpace.x3)
            }
            .padding(FmSpace.x4)
        }
        .task { await model.refreshStatuses() }
    }

    /// The switch replaces the whole app underneath this drawer, so the drawer closes first.
    private func switchTo(_ project: RelayProfile) {
        FmHaptics.tap()
        let activeId: String = Services.get(SettingsService.self)?.relays.activeId ?? ""
        guard project.id != activeId, let switcher = Services.get(RelaySwitchController.self) else {
            close { ProjectHome.shared.request() }
            return
        }
        close {
            ProjectHome.shared.requestOnArrival()
            Task { _ = await switcher.switchTo(project.id) }
        }
    }
}

@MainActor
@Observable
final class ProjectHome {
    static let shared: ProjectHome = ProjectHome()

    private(set) var requests: Int = 0
    @ObservationIgnored private var onArrival: Bool = false

    func request() {
        requests += 1
    }

    func requestOnArrival() {
        onArrival = true
    }

    func takeArrival() -> Bool {
        defer { onArrival = false }
        return onArrival
    }
}

/// One project: name, host, and who you are there.
struct ProjectSwitcherRow: View {
    @Environment(\.fmPalette) private var fm
    let info: RelayRowInfo
    var online: Bool = false
    let onTap: (() -> Void)?
    var onChooseInstance: (() -> Void)? = nil

    var body: some View {
        HStack(spacing: 0) {
            FmListRow(
                title: info.relay.label,
                subtitleView: AnyView(subtitle),
                leading: AnyView(ProjectIcon(project: info.relay, size: 32)),
                trailing: info.active
                    ? AnyView(Text("Active").fmText(.labelSmall, color: fm.accent))
                    : nil,
                onTap: onTap,
                showChevron: false)
                .accessibilityLabel(accessibility)
            if !info.relay.hasInstance, let onChooseInstance = onChooseInstance {
                chooseInstanceButton(onChooseInstance)
            }
        }
    }

    private func chooseInstanceButton(_ action: @escaping () -> Void) -> some View {
        Button {
            FmHaptics.tap()
            action()
        } label: {
            Text("Choose instance")
                .fmText(.labelSmall, color: fm.accent)
                .lineLimit(1)
                .fixedSize()
                .padding(.horizontal, FmSpace.x2)
                .frame(minHeight: 26)
                .overlay {
                    Capsule().strokeBorder(fm.border, lineWidth: 1)
                }
                .frame(minHeight: 44)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .padding(.trailing, FmSpace.x3)
        .accessibilityLabel("Choose an instance for \(info.relay.label)")
        .accessibilityIdentifier("project-choose-instance")
    }

    @ViewBuilder private var subtitle: some View {
        if info.signedIn {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                Text(info.relay.displayOrigin)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.tail)
                UserStatusBuilder(status: info.userStatus) { (visible: UserStatus) in
                    account(visible)
                }
            }
        } else {
            (Text(info.relay.displayOrigin).foregroundColor(fm.textSecondary)
                + Text(" · ").foregroundColor(fm.textTertiary)
                + Text(info.who).foregroundColor(fm.textTertiary))
                .fmText(.bodySmall)
                .lineLimit(1)
                .truncationMode(.tail)
        }
    }

    private func account(_ visible: UserStatus) -> some View {
        HStack(spacing: FmSpace.x2) {
            FmAvatar(name: info.account, avatarPng: info.avatarPng, size: 22, fontSize: 11)
                .overlay(alignment: .bottomTrailing) {
                    if online || visible.away {
                        PresenceDot(online: true, away: visible.away, size: 8)
                            .background(Circle().fill(fm.bgRaised).padding(-1.5))
                            .offset(x: 2, y: 2)
                    }
                }
            (Text(info.account).foregroundColor(fm.success)
                + Text(ProjectSwitcherRow.statusLine(visible)).foregroundColor(fm.textSecondary))
                .fmText(.bodySmall)
                .lineLimit(1)
                .truncationMode(.tail)
        }
    }

    static func statusLine(_ visible: UserStatus) -> String {
        var parts: [String] = []
        if visible.hasCustom {
            parts.append([visible.emoji, visible.text].filter { !$0.isEmpty }.joined(separator: " "))
        }
        if visible.away { parts.append("Away") }
        return parts.isEmpty ? "" : " · " + parts.joined(separator: " · ")
    }

    private var accessibility: String {
        let state: String = info.active ? "Active. " : ""
        let status: String = info.signedIn ? info.userStatus.describe(Date()) : ""
        let tail: String = status.isEmpty ? "" : ". Status: \(status)"
        return "\(info.relay.label), \(info.relay.displayOrigin). \(state)\(info.who)\(tail)"
    }
}

/// "Manage projects": the full list with test, rename and remove (the Projects card of Settings).
private struct ProjectsManageSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ScrollView {
                RelaySettingsSection()
                    .padding(FmSpace.x4)
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle("Projects")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}
