import FMAccount
import FMApi
import FMBase
import FMPackages
import FMTools
import SwiftUI
import UniformTypeIdentifiers

struct PluginMenuActions {
    let name: String
    let open: () -> Void
}

private struct PluginMenuActionsKey: EnvironmentKey {
    static let defaultValue: PluginMenuActions? = nil
}

extension EnvironmentValues {
    var pluginMenu: PluginMenuActions? {
        get { self[PluginMenuActionsKey.self] }
        set { self[PluginMenuActionsKey.self] = newValue }
    }
}

struct PluginMenuItems: View {
    @Environment(\.pluginMenu) private var actions

    var body: some View {
        if let actions {
            Divider()
            Button {
                actions.open()
            } label: {
                Label(PluginMenu.optionsLabel, systemImage: "star.bubble")
            }
            .accessibilityIdentifier("plugin-more-options")
        }
    }
}

struct PluginMoreMenu: View {
    @Environment(\.pluginMenu) private var actions

    var body: some View {
        if let actions {
            PluginMoreButton(actions: actions)
        }
    }
}

struct PluginMoreButton: View {
    let actions: PluginMenuActions

    var body: some View {
        Button {
            actions.open()
        } label: {
            Label("More", systemImage: "ellipsis.circle")
        }
        .accessibilityIdentifier(PluginMenu.buttonIdentifier)
    }
}

enum PluginQuickExit {
    case sent(String)
    case agent(String)
    case uninstall(String)
}

struct PluginQuickSheet: View {
    let name: String
    let packageId: String
    let screen: String
    let reviews: PackageReviewsModel
    let uninstall: MarketUninstall?
    let onExit: (PluginQuickExit) -> Void
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var feedback = PluginFeedback()
    @State private var report: ReportBugModel
    @State private var busy = false
    @State private var problem = ""
    @State private var fieldIssues: [String: String] = [:]

    init(
        name: String, packageId: String, screen: String, reviews: PackageReviewsModel, uninstall: MarketUninstall?,
        onExit: @escaping (PluginQuickExit) -> Void
    ) {
        self.name = name
        self.packageId = packageId
        self.screen = screen
        self.reviews = reviews
        self.uninstall = uninstall
        self.onExit = onExit
        let build: AppBuild = Services.get(AppConfig.self)?.build ?? AppBuild(infoDictionary: Bundle.main.infoDictionary)
        _report = State(initialValue: ReportBugModel(
            screen: screen, package: packageId, packageName: name, build: build,
            signedIn: Services.get(AuthService.self)?.isAuthenticated ?? false, api: Services.get(ApiService.self)))
    }

    var body: some View {
        ScrollView {
            content
                .padding(FmSpace.x4)
        }
        .scrollDismissesKeyboard(.interactively)
        .fmPreGlassBackground(fm.wallpaper ? fm.canvas : fm.bgRaised)
        .presentationDetents([.medium, .large])
        .presentationDragIndicator(.visible)
        .interactiveDismissDisabled(busy)
        .accessibilityIdentifier("plugin-quick-sheet")
        .task { await reviews.refresh() }
        .onChange(of: feedback) { (_: PluginFeedback, _: PluginFeedback) in
            problem = ""
            fieldIssues = [:]
        }
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            header
            stars
            FmTextField(text: $feedback.text, hint: PluginFeedback.hint, multiline: true)
                .frame(minHeight: 72, alignment: .top)
                .disabled(busy)
                .accessibilityIdentifier("plugin-quick-text")
            issue("problem")
            if feedback.asksImprovement(reviews.rules) {
                FmTextField(text: $feedback.improvement, hint: PluginFeedback.improvementHint, multiline: true)
                    .frame(minHeight: 48, alignment: .top)
                    .disabled(busy)
                    .accessibilityIdentifier("plugin-quick-improvement")
                issue("improvement")
            }
            if !problem.isEmpty {
                Text(problem)
                    .fmText(.bodySmall, color: fm.danger)
                    .fmMessageTools(problem)
                    .accessibilityIdentifier("plugin-quick-problem")
            }
            actions
            if let uninstall {
                Divider()
                uninstallRows(uninstall)
            }
        }
    }

    private var header: some View {
        HStack(alignment: .center) {
            Text(name)
                .fmText(.titleLarge)
                .frame(maxWidth: .infinity, alignment: .leading)
            FmIconButton(icon: "close", label: "Close", onPressed: busy ? nil : { dismiss() })
        }
    }

    @ViewBuilder private var stars: some View {
        let page = reviews.page
        let canRate = page?.viewer.canRate ?? false
        let shown = feedback.rates ? feedback.stars : (page?.viewer.review?.stars ?? 0)
        VStack(alignment: .leading, spacing: 2) {
            Text(PluginMenu.rateLabel(name, rated: page?.viewer.review != nil))
                .fmText(.bodySmall, color: fm.textTertiary)
            HStack(spacing: 2) {
                ForEach(1...5, id: \.self) { (star: Int) in
                    Button {
                        feedback.stars = star
                    } label: {
                        Image(systemName: star <= shown ? "star.fill" : "star")
                            .font(.title2)
                            .foregroundStyle(.orange)
                            .frame(minWidth: 40, minHeight: 40)
                    }
                    .buttonStyle(.plain)
                    .disabled(busy || !canRate)
                    .accessibilityLabel("Rate \(star) \(star == 1 ? "star" : "stars")")
                    .accessibilityIdentifier("plugin-quick-star-\(star)")
                }
                if reviews.loading { ProgressView().padding(.leading, FmSpace.x2) }
            }
            if let page, !page.viewer.canRate, !page.viewer.cannotRateReason.isEmpty {
                Text(page.viewer.cannotRateReason)
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
        }
    }

    @ViewBuilder private func issue(_ key: String) -> some View {
        if let code = fieldIssues[key] {
            Text(ReviewDraft.fieldMessage(code))
                .fmText(.bodySmall, color: fm.danger)
        }
    }

    private var actions: some View {
        HStack(spacing: FmSpace.x2) {
            FmToneButton(
                label: PluginFeedback.agentLabel, icon: "auto_awesome_outlined", tone: .accent, height: 44, busy: false,
                onPressed: busy || !feedback.canAskAgent ? nil : askAgent)
                .accessibilityIdentifier("plugin-quick-agent")
            FmFilledButton(
                busy ? PluginFeedback.sendingLabel : PluginFeedback.sendLabel, icon: busy ? nil : "send_rounded",
                background: fm.accent, foreground: Color.white, minHeight: 44, radius: FmRadius.lg, fillWidth: true,
                role: .titleSmall, busy: busy, onPressed: busy || !feedback.canSend ? nil : send)
                .accessibilityIdentifier("plugin-quick-send")
        }
    }

    @ViewBuilder private func uninstallRows(_ uninstall: MarketUninstall) -> some View {
        if case .organization(let organizations) = uninstall {
            ForEach(organizations, id: \.self) { (organization: String) in
                uninstallRow("Uninstall from \(organization)", organization: organization)
            }
        } else {
            uninstallRow(uninstall == .account ? "Uninstall everywhere" : "Uninstall", organization: "")
        }
    }

    private func uninstallRow(_ label: String, organization: String) -> some View {
        Button(role: .destructive) {
            onExit(.uninstall(organization))
            dismiss()
        } label: {
            Label(label, systemImage: "trash")
                .fmText(.bodyMedium, color: fm.danger)
                .frame(maxWidth: .infinity, minHeight: 36, alignment: .leading)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(busy)
        .accessibilityIdentifier(organization.isEmpty ? "plugin-quick-uninstall" : "plugin-quick-uninstall-\(organization)")
    }

    private func askAgent() {
        let device = UIDevice.current.userInterfaceIdiom == .pad ? "iPad" : "iPhone"
        onExit(.agent(feedback.agentPrompt(name: name, packageId: packageId, screen: screen, device: device)))
        dismiss()
    }

    private func send() {
        guard feedback.canSend else {
            problem = PluginFeedback.nothingToSend
            return
        }
        let review = feedback.review(reviews.draft, rules: reviews.rules)
        if let review {
            let issues = review.issues(reviews.rules)
            if !issues.isEmpty {
                fieldIssues = issues
                return
            }
        }
        busy = true
        Task {
            defer { busy = false }
            if let review {
                reviews.draft = review
                guard await reviews.submit() else {
                    problem = reviews.errorText.isEmpty ? reviews.page?.viewer.cannotRateReason ?? "" : reviews.errorText
                    fieldIssues = reviews.fieldIssues
                    return
                }
            }
            if feedback.reports {
                report.message = feedback.text
                guard await report.submit() else {
                    problem = report.problem
                    return
                }
            }
            FmHaptics.commit()
            onExit(.sent(PluginFeedback.thanks(name, rated: review != nil, reported: feedback.reports)))
            dismiss()
        }
    }
}

struct PluginMenuHost: ViewModifier {
    let route: Route
    let packageId: String
    @State private var market: MarketModel
    @State private var reviews: PackageReviewsModel
    @State private var showing = false
    @State private var pendingExit: PluginQuickExit?
    @State private var thanks: FmNotice?
    @State private var uninstallRequest: PackageUninstallRequest?
    @State private var exportURL: URL?
    @State private var exporting = false
    @Environment(\.dismiss) private var dismiss

    init(client: PackagesClient, route: Route, packageId: String) {
        self.route = route
        self.packageId = packageId
        _market = State(initialValue: MarketModel(client: client, account: PackagesFeature.accountPackages, ledger: .shared))
        _reviews = State(initialValue: PackageReviewsModel(client: client, packageId: packageId))
    }

    private var package: MarketPackage? { market.package(packageId) }

    private var name: String { PluginMenu.name(catalog: package?.name, route: route) }

    private var uninstall: MarketUninstall? {
        package.flatMap { (package: MarketPackage) in market.uninstallAction(package, signedIn: PackagesFeature.signedIn) }
    }

    private var actions: PluginMenuActions {
        PluginMenuActions(name: name, open: { showing = true })
    }

    func body(content: Content) -> some View {
        content
            .toolbar {
                if PluginMenu.addsButton(for: route) {
                    ToolbarItem(placement: .topBarTrailing) {
                        PluginMoreButton(actions: actions)
                    }
                }
            }
            .environment(\.pluginMenu, actions)
            .task(id: packageId) {
                await market.refreshInstalls(organizations: await PackagesFeature.organizationNames())
            }
            .packageUninstall(
                $uninstallRequest, market: market,
                onRemoved: { (removal: PackageRemoval) in
                    exportURL = PackageUninstallRequest.exportFile(removal)
                    exporting = exportURL != nil
                },
                onUninstalled: {
                    if !exporting { dismiss() }
                })
            .fileExporter(
                isPresented: $exporting, document: market.removed?.export.map(PackageExportDocument.init(data:)), contentType: .json,
                defaultFilename: exportURL?.lastPathComponent
            ) { _ in
                dismiss()
            }
            .sheet(isPresented: $showing, onDismiss: finish) {
                PluginQuickSheet(
                    name: name, packageId: packageId, screen: route.title, reviews: reviews, uninstall: uninstall,
                    onExit: { (chosen: PluginQuickExit) in pendingExit = chosen })
            }
            .pluginThanks($thanks)
    }

    private func finish() {
        guard let chosen = pendingExit else { return }
        pendingExit = nil
        switch chosen {
        case let .sent(message):
            thanks = FmNotice(message)
        case let .agent(prompt):
            SurfaceRouter.open(.compose(ComposeTarget(text: prompt)))
        case let .uninstall(organization):
            guard let package, let uninstall else { return }
            uninstallRequest = PackageUninstallRequest(package, uninstall, organization: organization, account: market.account)
        }
    }
}

struct PluginFeedbackSheet: ViewModifier {
    @Binding var isPresented: Bool
    let packageId: String
    let name: String
    let screen: String
    @State private var thanks: FmNotice?

    func body(content: Content) -> some View {
        content
            .sheet(isPresented: $isPresented) {
                ReportBugSheet(
                    screen: screen, package: packageId, packageName: name,
                    onFinished: { (filed: Bool) in
                        if filed {
                            FmHaptics.commit()
                            thanks = FmNotice(ReportBugModel.feedbackThanks(name))
                        }
                    })
            }
            .pluginThanks($thanks)
    }
}

struct PluginThanks: ViewModifier {
    @Binding var thanks: FmNotice?

    func body(content: Content) -> some View {
        content
            .overlay(alignment: .top) {
                if thanks != nil {
                    FmNoticeLine($thanks)
                        .padding(.horizontal, FmSpace.x3)
                        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: FmRadius.md))
                        .padding(.horizontal, FmSpace.x4)
                        .padding(.top, FmSpace.x2)
                        .task(id: thanks?.id) {
                            try? await Task.sleep(for: .seconds(4))
                            thanks = nil
                        }
                        .accessibilityIdentifier("plugin-feedback-thanks")
                }
            }
    }
}

extension View {
    @ViewBuilder
    func pluginMenu(for route: Route) -> some View {
        if let packageId = PluginMenu.packageId(for: route), let client = PackagesFeature.client {
            modifier(PluginMenuHost(client: client, route: route, packageId: packageId))
        } else {
            self
        }
    }

    func pluginFeedbackSheet(isPresented: Binding<Bool>, packageId: String, name: String, screen: String) -> some View {
        modifier(PluginFeedbackSheet(isPresented: isPresented, packageId: packageId, name: name, screen: screen))
    }

    func pluginThanks(_ thanks: Binding<FmNotice?>) -> some View {
        modifier(PluginThanks(thanks: thanks))
    }
}
