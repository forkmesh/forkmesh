// The Settings screen: profile and status, appearance, organizations, this device, push, the node registry, signed-in
// sessions, the relay and its room, import tokens, the account (sign out,
// guest ways out, delete), sponsored messages, on-device AI and the legal
// links. A section whose relay does not serve it is not drawn. The cards are
// in SettingsCards.swift.

import FMAccount
import FMApi
import FMBase
import FMChat
import FMCrypto
import FMInsights
import FMNet
import FMPackages
import FMTools
import StoreKit
import SwiftUI

@MainActor
struct SettingsScreen: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.openURL) var openURL

    @State var form: SettingsFormModel?
    @State var accountNotice: FmNotice?
    @State var setupNotice: FmNotice?
    @State var profileNotice: FmNotice?
    @State var confirmSignOut: Bool = false
    @State var deleteModel: DeleteAccountModel?
    @State var showStatusSheet: Bool = false
    @State var query: String = ""
    @State var switchTarget: RecentAccount?
    @State var recentRevision: Int = 0
    @State var openIdentity: Bool = false
    @State var diagnosticsFleet: FleetLightsModel? = InsightsFeature.api.map {
        FleetLightsModel(
            transport: $0, prober: HTTPStatusProbe(http: Services.get(HTTPClient.self) ?? URLSessionHTTPClient()))
    }
    @State var diagnosticsPackages: AccountPackagesStore? = PackagesFeature.accountPackages
    @State var diagnosticsDesktopLoad: DesktopLoadLightModel? = InsightsFeature.desktopLoadLight
    @State var diagnosticsBuildMac: BuildMacLightModel? = InsightsFeature.buildMacLight
    @State var diagnosticsUpdate: UpdateRequiredCheck = UpdateRequiredCheck.shared

    private let settings: SettingsService?
    private let auth: AuthService?
    private let api: ApiService?
    private let push: PushNotificationService?
    private let userStatus: UserStatusService?
    private let chatRetention: ChatRetentionService?
    private let identity: DeviceIdentity?
    private let focus: SettingsSectionId?

    init(section: String? = nil) {
        self.focus = SettingsCatalog.section(named: section)
        let settings: SettingsService? = Services.get(SettingsService.self)
        self.settings = settings
        self.auth = Services.get(AuthService.self)
        self.api = Services.get(ApiService.self)
        self.push = Services.get(PushNotificationService.self)
        self.userStatus = Services.get(UserStatusService.self)
        self.chatRetention = Services.get(ChatRetentionService.self)
        self.identity = Services.get(DeviceIdentity.self)
        _form = State(initialValue: settings.map { SettingsFormModel(settings: $0) })
    }

    var body: some View {
        Group {
            if let settings = settings, let form = form {
                page(settings: settings, form: form)
            } else {
                FmEmptyState(icon: "settings_outlined", title: "Settings")
            }
        }
        .background(fm.canvas.ignoresSafeArea())
        .navigationTitle("Settings")
        .sheet(isPresented: $showStatusSheet) {
            UserStatusSheet()
        }
        .fmDialog(isPresented: $confirmSignOut) {
            FmConfirmDialog(
                title: SettingsCopy.signOutTitle,
                message: SettingsCopy.signOutBody,
                confirmLabel: "Sign out",
                onCancel: { confirmSignOut = false },
                onConfirm: {
                    confirmSignOut = false
                    signOut()
                })
        }
        .fmDialog(isPresented: switchShown) {
            if let target = switchTarget {
                FmConfirmDialog(
                    title: "Switch to \(target.name)?",
                    message: "You are signed out of this account on this phone, then asked to sign in as \(target.name).",
                    confirmLabel: "Switch",
                    onCancel: { switchTarget = nil },
                    onConfirm: {
                        switchTarget = nil
                        switchAccount()
                    })
            }
        }
        .fmDialog(isPresented: deleteShown, barrierDismissible: false) {
            if let model = deleteModel {
                DeleteAccountDialog(
                    model: model,
                    onCancel: { deleteModel = nil },
                    onDeleted: {
                        deleteModel = nil
                        accountNotice = FmNotice(SettingsCopy.accountDeleted)
                    })
            }
        }
        .task { await refreshDiagnostics() }
        .onChange(of: settings?.showTelemetryStrip ?? true) { (_: Bool, enabled: Bool) in
            if enabled {
                Task { await refreshDiagnostics() }
            }
        }
    }

    private var deleteShown: Binding<Bool> {
        Binding<Bool>(
            get: { deleteModel != nil },
            set: { (shown: Bool) in
                if !shown {
                    deleteModel = nil
                }
            })
    }

    private var switchShown: Binding<Bool> {
        Binding<Bool>(
            get: { switchTarget != nil },
            set: { (shown: Bool) in
                if !shown {
                    switchTarget = nil
                }
            })
    }

    private func switchAccount() {
        guard let auth = auth else { return }
        Task { @MainActor in
            await auth.logout()
            AccountFeature.openSignIn(.login)
        }
    }


    private func signOut() {
        guard let auth = auth else { return }
        Task { @MainActor in
            await auth.logout()
            accountNotice = FmNotice(SettingsCopy.signedOut)
        }
    }

    /// The sessions card revoked this app's own session server-side: drop it
    /// locally too.
    private func remoteSignedOut() {
        guard let auth = auth else { return }
        Task { @MainActor in
            await auth.logout()
            accountNotice = FmNotice(SettingsCopy.remoteSignedOut)
        }
    }

    // MARK: Page

    private func available(_ settings: SettingsService, session: AuthSession?, guest: Bool) -> Set<SettingsSectionId> {
        var ids: Set<SettingsSectionId> = [
            .quickSetup, .profile, .general, .repos, .agents, .voice, .identity, .device, .desktops,
            .relay, .data, .security, .account, .onDeviceAI, .legal,
        ]
        if Services.get(ChatPrivacy.self) != nil { ids.insert(.chatPrivacy) }
        let signedIn: Bool = hasRealSession(session) && !guest
        if signedIn {
            ids.formUnion([.sshKeys, .mcp, .sessions, .apps])
        }
        if settings.supports(.orgs) { ids.insert(.organizations) }
        if settings.supports(.devices) { ids.insert(.nodes) }
        if settings.supports(.imports) { ids.insert(.imports) }
        if settings.supports(.ads) { ids.insert(.sponsored) }
        if showsPingSettings(session: session, guest: guest) || showsPushSection(settings: settings, guest: guest) {
            ids.insert(.notifications)
        }
        return ids
    }

    private func quickSetupInput(settings: SettingsService, session: AuthSession?, guest: Bool) -> QuickSetupInput {
        let key: String = identity?.publicKeyB64url ?? ""
        return QuickSetupInput(
            signedIn: hasRealSession(session),
            guest: guest,
            projectName: settings.project.label.isEmpty ? settings.project.id : settings.project.label,
            desktopsPaired: Services.get(LocalControlService.self)?.paired.count ?? 0,
            identityBackedUp: !IdentityBackupStore(prefs: settings.prefs).state.needsBackup(currentKey: key),
            pushEnabled: push.map { $0.registered && $0.permission != "denied" } ?? false)
    }

    private func page(settings: SettingsService, form: SettingsFormModel) -> some View {
        let session: AuthSession? = auth?.session
        let guest: Bool = auth?.isGuest ?? false
        let shown: Set<SettingsSectionId> = Set(
            SettingsCatalog.visible(query, available: available(settings, session: session, guest: guest)).map(\.id))
        let setup: QuickSetupInput = quickSetupInput(settings: settings, session: session, guest: guest)
        return ScrollViewReader { (proxy: ScrollViewProxy) in
            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    topSections(settings: settings, form: form, session: session, guest: guest, shown: shown, setup: setup)
                    middleSections(settings: settings, session: session, shown: shown)
                    bottomSections(settings: settings, form: form, session: session, guest: guest, shown: shown)
                }
                .padding(FmSpace.x4)
            }
            .scrollDismissesKeyboard(.interactively)
            .searchable(text: $query, placement: .navigationBarDrawer(displayMode: .automatic), prompt: "Search settings")
            .onAppear {
                form.adoptAccountName(session)
                rememberAccount(session)
                if let focus = focus {
                    DispatchQueue.main.async { proxy.scrollTo(focus, anchor: .top) }
                }
            }
            .onChange(of: session?.nodeName ?? "") { (_: String, _: String) in
                form.adoptAccountName(auth?.session)
                rememberAccount(auth?.session)
            }
            .navigationDestination(isPresented: $openIdentity) {
                IdentityScreen()
                    .shellPageClearance()
            }
        }
    }

    @ViewBuilder
    private func topSections(
        settings: SettingsService, form: SettingsFormModel, session: AuthSession?, guest: Bool,
        shown: Set<SettingsSectionId>, setup: QuickSetupInput
    ) -> some View {
        if shown.isEmpty {
            FmEmptyState(icon: "search_off", title: "No settings match", message: "Try another word.")
        }
        if shown.contains(.quickSetup) && (QuickSetup.remaining(setup) > 0 || !query.isEmpty) {
            SettingsSection(title: "Quick setup") {
                SettingsCardBox(padded: false) {
                    VStack(alignment: .leading, spacing: 0) {
                        QuickSetupCard(input: setup, onStep: { (step: QuickSetupStepId) in quickSetupStep(step) })
                        if setupNotice != nil {
                            FmHairline()
                            FmNoticeLine($setupNotice)
                                .padding(.horizontal, FmSpace.x4)
                                .padding(.vertical, FmSpace.x3)
                        }
                    }
                }
            }
            .id(SettingsSectionId.quickSetup)
        }
        if shown.contains(.profile) {
            profileSection(settings: settings, form: form, session: session, guest: guest)
                .id(SettingsSectionId.profile)
        }
        if shown.contains(.general) {
            SettingsSection(title: "General") {
                VStack(spacing: FmSpace.x3) {
                    SettingsCardBox { AppearanceCard(settings: settings) }
                    SettingsCardBox(padded: false) { HomeScreenSettingsCard() }
                    SettingsCardBox(padded: false) { BottomBarSettingsCard() }
                    SettingsCardBox { DevicePreferencesCard(settings: settings) }
                    SettingsCardBox { GenieSpiderCard() }
                    SettingsCardBox { SuggestionModeCard() }
                }
            }
            .id(SettingsSectionId.general)
        }
    }

    @ViewBuilder
    private func middleSections(settings: SettingsService, session: AuthSession?, shown: Set<SettingsSectionId>) -> some View {
        if shown.contains(.identity) {
            identitySection(settings: settings).id(SettingsSectionId.identity)
        }
        if shown.contains(.sshKeys) || shown.contains(.mcp) {
            keysSection(shown: shown).id(SettingsSectionId.sshKeys)
        }
        if shown.contains(.organizations) {
            organizationsSection.id(SettingsSectionId.organizations)
        }
        if shown.contains(.apps) {
            appsSection.id(SettingsSectionId.apps)
        }
        if shown.contains(.agents) {
            SettingsSection(title: "Agents") {
                SettingsCardBox(padded: false) { AgentPreferencesCard() }
            }
            .id(SettingsSectionId.agents)
        }
        if shown.contains(.voice) {
            SettingsSection(title: "Voice") {
                SettingsCardBox { DictationLanguageCard(settings: settings) }
            }
            .id(SettingsSectionId.voice)
        }
        if shown.contains(.repos) {
            SettingsSection(title: "Repositories on this phone") {
                SettingsCardBox { LocalRepoDefaultsCard(settings: settings, session: session) }
            }
            .id(SettingsSectionId.repos)
        }
    }

    @ViewBuilder
    private func bottomSections(
        settings: SettingsService, form: SettingsFormModel, session: AuthSession?, guest: Bool, shown: Set<SettingsSectionId>
    ) -> some View {
        Group {
            if shown.contains(.device) {
                deviceSection(settings: settings, session: session).id(SettingsSectionId.device)
            }
            if shown.contains(.desktops) {
                SettingsSection(title: "Desktops") {
                    SettingsCardBox(padded: false) { PairedDesktopsCard() }
                }
                .id(SettingsSectionId.desktops)
            }
            if shown.contains(.notifications) {
                notificationsSection(settings: settings, session: session, guest: guest)
                    .id(SettingsSectionId.notifications)
            }
            if shown.contains(.chatPrivacy), let privacy = Services.get(ChatPrivacy.self) {
                SettingsSection(title: "Chat privacy") {
                    SettingsCardBox { ChatPrivacyCard(privacy: privacy) }
                }
                .id(SettingsSectionId.chatPrivacy)
            }
            if shown.contains(.nodes) {
                SettingsSection(title: "Nodes on this account") {
                    SettingsCardBox {
                        AccountNodesCard(
                            session: session, api: api,
                            webSettings: URL(string: webSettingsURL(settings.serverUrl)))
                            .id("nodes-\(session?.sessionToken ?? "")")
                    }
                }
                .id(SettingsSectionId.nodes)
            }
            if shown.contains(.sessions) {
                SettingsSection(title: "Where you're signed in") {
                    SettingsCardBox {
                        AccountSessionsCard(api: api, onCurrentSignedOut: remoteSignedOut)
                            .id("sessions-\(session?.sessionToken ?? "")")
                    }
                }
                .id(SettingsSectionId.sessions)
            }
        }
        if shown.contains(.relay) {
            relaySection(form: form).id(SettingsSectionId.relay)
        }
        if shown.contains(.imports) {
            importSection(form: form).id(SettingsSectionId.imports)
        }
        if shown.contains(.data) || shown.contains(.security) {
            dataSection(shown: shown).id(SettingsSectionId.data)
        }
        if shown.contains(.account) {
            accountSection(session: session, guest: guest).id(SettingsSectionId.account)
        }
        if shown.contains(.sponsored) {
            SettingsSection(title: "Sponsored messages") {
                SettingsCardBox { SponsoredMessagesRow(settings: settings) }
            }
            .id(SettingsSectionId.sponsored)
        }
        if shown.contains(.onDeviceAI) {
            SettingsSection(title: "On-device AI") {
                SettingsCardBox(padded: false) { onDeviceAIRow }
            }
            .id(SettingsSectionId.onDeviceAI)
        }
        if shown.contains(.legal) {
            legalSection.id(SettingsSectionId.legal)
        }
    }

    private func rememberAccount(_ session: AuthSession?) {
        guard let settings = settings, hasRealSession(session), let session = session, !(auth?.isGuest ?? false) else { return }
        RecentAccountsStore(prefs: settings.prefs).record(name: session.nodeName, email: session.email, at: SystemClock().nowMillis)
        recentRevision += 1
    }

    private func quickSetupStep(_ step: QuickSetupStepId) {
        switch step {
        case .signIn:
            AccountFeature.openSignIn(.login)
        case .project:
            setupNotice = FmNotice("Pick a project from the project switcher at the top of the You tab.")
        case .desktop:
            setupNotice = FmNotice("Sign the desktop in to this account, then open Desktops on the same network. A desktop on another account asks you to compare a code.")
        case .backup:
            openIdentity = true
        case .notifications:
            setupNotice = FmNotice("Turn on notifications under Notifications below.")
        }
    }

    @ViewBuilder
    private func identitySection(settings: SettingsService) -> some View {
        let key: String = identity?.publicKeyB64url ?? ""
        let backup: IdentityBackupState = IdentityBackupStore(prefs: settings.prefs).state
        SettingsSection(title: "Identity and recovery") {
            SettingsCardBox(padded: false) {
                VStack(spacing: 0) {
                    if backup.showsNag(currentKey: key, nowMillis: SystemClock().nowMillis) {
                        NavigationLink {
                            IdentityScreen()
                                .shellPageClearance()
                        } label: {
                            IdentityBackupNag()
                        }
                        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                        FmHairline()
                    }
                    NavigationLink {
                        IdentityScreen()
                            .shellPageClearance()
                    } label: {
                        SettingsLinkRow(
                            icon: "fingerprint",
                            title: "Identity key, wallet and node links",
                            subtitle: "Recovery kit, QR codes, link a node, trust lists")
                    }
                    .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                    .accessibilityIdentifier("settings-identity-row")
                }
            }
        }
    }

    private func keysSection(shown: Set<SettingsSectionId>) -> some View {
        SettingsSection(title: "Keys and tokens") {
            SettingsCardBox(padded: false) {
                VStack(spacing: 0) {
                    if shown.contains(.sshKeys) {
                        NavigationLink {
                            SshKeysScreen()
                                .shellPageClearance()
                        } label: {
                            SettingsLinkRow(icon: "key_outlined", title: "SSH keys", subtitle: "Public keys for Git over SSH")
                        }
                        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                        .accessibilityIdentifier("settings-ssh-keys-row")
                    }
                    if shown.contains(.sshKeys) && shown.contains(.mcp) {
                        FmHairline()
                    }
                    if shown.contains(.mcp) {
                        NavigationLink {
                            McpConnectorScreen()
                                .shellPageClearance()
                        } label: {
                            SettingsLinkRow(icon: "hub_outlined", title: "MCP connector and API tokens", subtitle: "Remote MCP URL, mint or revoke tokens")
                        }
                        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                        .accessibilityIdentifier("settings-mcp-row")
                    }
                    FmHairline()
                    NavigationLink(value: Route.vault(section: "secrets")) {
                        SettingsLinkRow(icon: "lock_outline", title: "Secrets", subtitle: "Actions variables and secrets, the vault")
                    }
                    .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                }
            }
        }
    }

    private func dataSection(shown: Set<SettingsSectionId>) -> some View {
        SettingsSection(title: "Data and security") {
            SettingsCardBox(padded: false) {
                VStack(spacing: 0) {
                    if shown.contains(.data) {
                        DataFolderSettingsRow()
                        FmHairline()
                        NavigationLink {
                            AccountDataScreen()
                                .shellPageClearance()
                        } label: {
                            SettingsLinkRow(icon: "storage_outlined", title: "Data", subtitle: "Save to ForkMesh, see and download everything about you")
                        }
                        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                    }
                    if shown.contains(.data) && shown.contains(.security) {
                        FmHairline()
                    }
                    if shown.contains(.security) {
                        NavigationLink {
                            SecurityReportScreen()
                                .shellPageClearance()
                        } label: {
                            SettingsLinkRow(icon: "verified_user_outlined", title: "Report a vulnerability", subtitle: "Privately, to the security team")
                        }
                        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                    }
                }
            }
        }
    }

    // MARK: Sections

    @ViewBuilder
    private func profileSection(
        settings: SettingsService, form: SettingsFormModel, session: AuthSession?, guest: Bool
    ) -> some View {
        let statusShown: Bool = (userStatus?.signedIn ?? false) && showsUserStatus(session, guest: guest)
        SettingsSection(title: "Profile") {
            SettingsCardBox {
                VStack(alignment: .leading, spacing: 0) {
                    ProfileAccountCard(session: session, guest: guest, status: userStatus?.status)
                    if statusShown, let service = userStatus {
                        UserStatusControls(service: service, onOpenSheet: { showStatusSheet = true }, onRefused: { (message: String) in profileNotice = FmNotice.error(message) })
                            .padding(.top, FmSpace.x3)
                    }
                    if !guest, let retention = chatRetention, retention.signedIn {
                        ChatRetentionSettingsRow(service: retention, onRefused: { (message: String) in profileNotice = FmNotice.error(message) })
                            .padding(.top, FmSpace.x2)
                    }
                    if profileNotice != nil {
                        FmNoticeLine($profileNotice)
                            .padding(.top, FmSpace.x2)
                            .accessibilityIdentifier("settings-profile-notice")
                    }
                    SettingsTextField(label: "Display name", text: binding(form, .displayName))
                        .padding(.top, FmSpace.x4)
                    Text(verbatim: "Node id: \(identity?.publicKeyB64url ?? "")")
                        .fmText(.bodySmall, color: fm.textTertiary)
                        .lineLimit(1)
                        .truncationMode(.tail)
                    if settings.showTelemetryStrip {
                        FmHairline()
                            .padding(.top, FmSpace.x4)
                        ShellVersionFooter(
                            version: AppServices.current?.config.build.version ?? "", update: diagnosticsUpdate.available,
                            desktop: Services.get(ShellLocalDesktopBridge.self)?.localDesktop ?? .hidden,
                            fleet: diagnosticsStatusPageOn ? diagnosticsFleet?.shownLights ?? [] : [],
                            systems: diagnosticsStatusPageOn ? diagnosticsFleet?.shownSystems ?? [] : [],
                            desktopLoad: diagnosticsDesktopLoad?.load, buildMac: diagnosticsBuildMac?.light,
                            runnerAgents: ShellCounts.shared.runnerAgents,
                            onOpenDesktop: { AppNavigator.shared?.open(.desktops) },
                            onOpenStatus: { AppNavigator.shared?.open(.status) })
                            .padding(.top, FmSpace.x3)
                            .accessibilityIdentifier("settings-profile-status-footer")
                    }
                }
            }
        }
    }

    private var diagnosticsStatusPageOn: Bool {
        diagnosticsPackages?.statusPageShown ?? true
    }

    private var diagnosticsStatusWatch: StatusWatch {
        guard let account = diagnosticsPackages else { return StatusWatch() }
        let filter: StatusMonitorFilter = account.monitorRules.map { (rules: PackageMonitors) -> StatusMonitorFilter in
            StatusMonitorFilter(systems: rules.systems, prefixes: rules.prefixes)
        } ?? .all
        let custom: [MonitorTarget] = account.monitors.compactMap { (monitor: StatusMonitor) -> MonitorTarget? in
            monitor.address.map { (url: URL) -> MonitorTarget in MonitorTarget(id: monitor.id, name: monitor.name, url: url) }
        }
        return StatusWatch(
            filter: filter, onlyProblems: account.onlyProblems, healthyMinutes: account.refreshMinutes, custom: custom)
    }

    private func refreshDiagnostics() async {
        if let packages = diagnosticsPackages {
            await packages.refresh()
        }
        guard settings?.showTelemetryStrip ?? true else { return }
        await diagnosticsUpdate.check()
        await diagnosticsUpdate.checkStore()
        if let fleet = diagnosticsFleet, diagnosticsStatusPageOn {
            fleet.watch = diagnosticsStatusWatch
            await fleet.refresh()
        }
        if let desktopLoad = diagnosticsDesktopLoad {
            await desktopLoad.refresh()
        }
        if let buildMac = diagnosticsBuildMac {
            await buildMac.refresh()
        }
    }

    private var organizationsSection: some View {
        SettingsSection(title: "Organizations") {
            SettingsCardBox(padded: false) {
                NavigationLink {
                    OrgsScreen()
                        .shellPageClearance()
                } label: {
                    SettingsLinkRow(
                        icon: "apartment_outlined",
                        title: "Manage organizations",
                        subtitle: "Orgs, members, teams, and linked repos")
                }
                .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                .accessibilityIdentifier("settings-organizations-row")
            }
        }
    }

    private var appsSection: some View {
        SettingsSection(title: "Installed apps") {
            SettingsCardBox(padded: false) {
                NavigationLink {
                    InstalledAppsScreen()
                        .shellPageClearance()
                } label: {
                    SettingsLinkRow(
                        icon: "extension_outlined",
                        title: "Manage installed apps",
                        subtitle: "Scopes, last used, revoke, organization requests")
                }
                .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                .accessibilityIdentifier("settings-installed-apps-row")
            }
        }
    }

    private func deviceSection(settings: SettingsService, session: AuthSession?) -> some View {
        SettingsSection(title: "This device") {
            VStack(spacing: FmSpace.x3) {
                SettingsCardBox {
                    ThisDeviceCardView(session: session, identityKey: identity?.shortKey ?? "", settings: settings)
                }
                // iOS only: nothing else can read this phone's UDID.
                if settings.supports(.iosUdid) {
                    SettingsCardBox { IosUdidCard(settings: settings) }
                }
            }
        }
    }

    private func showsPushSection(settings: SettingsService, guest: Bool) -> Bool {
        guard let push = push else { return false }
        return showsPushCard(supported: push.supported, guest: guest) && settings.supports(.push)
    }

    private func notificationsSection(settings: SettingsService, session: AuthSession?, guest: Bool) -> some View {
        SettingsSection(title: "Notifications") {
            VStack(spacing: FmSpace.x3) {
                if showsPingSettings(session: session, guest: guest) {
                    SettingsCardBox(padded: false) {
                        NavigationLink {
                            PingSettingsScreen()
                                .shellPageClearance()
                        } label: {
                            SettingsLinkRow(
                                icon: "tune_rounded",
                                title: "Ping settings",
                                subtitle: "Choose which pings you get, pause them, or set quiet hours")
                        }
                        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                        .accessibilityIdentifier("settings-pings-row")
                    }
                }
                if let push = push, showsPushSection(settings: settings, guest: guest) {
                    SettingsCardBox {
                        PushNotificationsCard(push: push)
                    }
                    SettingsCardBox {
                        NotificationContentCard()
                    }
                }
            }
        }
    }

    private func relaySection(form: SettingsFormModel) -> some View {
        SettingsSection(title: "Relay") {
            VStack(spacing: FmSpace.x3) {
                SettingsCardBox { RelaySettingsSection() }
                SettingsCardBox {
                    // Room and passphrase belong to the chat room on whichever
                    // relay is active, so they sit under the relay list.
                    VStack(alignment: .leading, spacing: 0) {
                        SettingsTextField(label: "Room", text: binding(form, .room))
                        SettingsTextField(label: "Passphrase", text: binding(form, .passphrase), secure: true)
                        SettingsNote(text: SettingsCopy.room)
                    }
                }
            }
        }
    }

    private func importSection(form: SettingsFormModel) -> some View {
        SettingsSection(title: "Import tokens") {
            SettingsCardBox {
                VStack(alignment: .leading, spacing: 0) {
                    SettingsNote(text: SettingsCopy.importTokens)
                        .padding(.bottom, FmSpace.x3)
                    SettingsTextField(label: "GitHub token", text: binding(form, .github), secure: true)
                    SettingsTextField(label: "GitLab token", text: binding(form, .gitlab), secure: true)
                }
            }
        }
    }

    private func accountSection(session: AuthSession?, guest: Bool) -> some View {
        SettingsSection(title: "Account") {
            VStack(spacing: FmSpace.x3) {
                if accountNotice != nil {
                    FmNoticeLine($accountNotice)
                        .padding(.horizontal, FmSpace.x4)
                        .accessibilityIdentifier("settings-account-notice")
                }
                if !guest, let settings = settings {
                    let store: RecentAccountsStore = RecentAccountsStore(prefs: settings.prefs)
                    SettingsCardBox(padded: false) {
                        RecentAccountsCard(
                            accounts: store.others(than: session?.nodeName ?? ""),
                            onSwitch: { (account: RecentAccount) in
                                if hasRealSession(session) {
                                    switchTarget = account
                                } else {
                                    AccountFeature.openSignIn(.login)
                                }
                            },
                            onForget: { (account: RecentAccount) in
                                store.forget(account.name)
                                recentRevision += 1
                            },
                            onAdd: {
                                if hasRealSession(session) {
                                    switchTarget = RecentAccount(name: "another account", email: "", lastUsedAt: 0)
                                } else {
                                    AccountFeature.openSignIn(.login)
                                }
                            })
                            .id("recent-\(recentRevision)")
                    }
                }
                SettingsCardBox {
                    switch accountCard(guest: guest) {
                    case .guest:
                        GuestAccountCard(
                            onLogIn: { AccountFeature.openSignIn(.login) },
                            onSignUp: { AccountFeature.openSignIn(.signup) },
                            onLeave: { auth?.exitGuestMode() })
                    case .signOut:
                        SignOutCard(signedInAs: signedInAsName(session), onSignOut: { confirmSignOut = true })
                    }
                }
                if showsDeleteAccount(session), let auth = auth {
                    SettingsCardBox {
                        DeleteAccountCard(onDelete: { deleteModel = DeleteAccountModel(auth: auth) })
                    }
                }
            }
        }
    }

    private var onDeviceAIRow: some View {
        NavigationLink {
            LocalAISettingsView()
                .shellPageClearance()
        } label: {
            SettingsLinkRow(
                icon: "memory_outlined",
                title: "On-device AI",
                subtitle: "Gemma 4 on this phone's GPU")
        }
        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
        .accessibilityIdentifier("settings-on-device-ai-row")
    }

    private var legalSection: some View {
        SettingsSection(title: "Legal", last: true) {
            SettingsCardBox(padded: false) {
                VStack(spacing: 0) {
                    LegalLinkRow(label: "Terms of Service", onTap: { openURL(LegalLinks.terms) })
                    FmHairline()
                    LegalLinkRow(label: "Privacy Policy", onTap: { openURL(LegalLinks.privacy) })
                    FmHairline()
                    NavigationLink {
                        AppleHealthGuideScreen()
                            .shellPageClearance()
                    } label: {
                        SettingsLinkRow(
                            icon: "monitor_heart",
                            title: AppleHealthGuide.title,
                            subtitle: AppleHealthGuide.settingsSubtitle)
                    }
                    .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                    .accessibilityIdentifier("settings-apple-health-row")
                }
            }
        }
    }

    private func binding(_ form: SettingsFormModel, _ field: SettingsFormModel.Field) -> Binding<String> {
        return Binding<String>(
            get: { form.text(field) },
            set: { (value: String) in form.setText(field, value) })
    }
}

/// An FmSectionHeader over its cards, with the page's section spacing.
struct SettingsSection<Content: View>: View {
    let title: String
    let last: Bool
    let content: Content

    init(title: String, last: Bool = false, @ViewBuilder content: () -> Content) {
        self.title = title
        self.last = last
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: title)
            content
        }
        .padding(.bottom, last ? 0 : FmSpace.x5)
    }
}

/// `FmCard(radius: FmRadius.lg, ...)` as every Settings section draws it.
struct SettingsCardBox<Content: View>: View {
    let padded: Bool
    let content: Content

    init(padded: Bool = true, @ViewBuilder content: () -> Content) {
        self.padded = padded
        self.content = content()
    }

    var body: some View {
        FmCard(padding: padded ? nil : EdgeInsets(), radius: FmRadius.lg) {
            content
        }
    }
}

/// A navigating row (FmListRow's look, for a NavigationLink label).
struct SettingsLinkRow: View {
    @Environment(\.fmPalette) var fm
    let icon: String
    let title: String
    let subtitle: String

    var body: some View {
        HStack(spacing: FmSpace.x3) {
            FmIcon(icon, size: 24, color: fm.textSecondary)
            VStack(alignment: .leading, spacing: 3) {
                Text(title)
                    .fmText(.titleSmall)
                    .lineLimit(1)
                Text(subtitle)
                    .fmText(.bodySmall, color: fm.textTertiary, height: FmType.leadingTight)
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            FmIcon("chevron_right", size: 20, color: fm.textDisabled)
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x2)
        .frame(minHeight: 60)
        .contentShape(Rectangle())
    }
}
