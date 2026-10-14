// The cards of the Settings screen: profile and status,
// this device (with the UDID and serve-as-node rows), the node registry, the
// signed-in sessions, push, appearance, and the account cards. Text and
// states come from FMApi's SettingsLogic and the Settings models.

import FMAccount
import FMAgents
import FMApi
import FMBase
import FMPush
import SwiftUI
import Speech
import UIKit

// MARK: Shared pieces

/// A card's bold first line.
struct SettingsTitle: View {
    let text: String

    var body: some View {
        Text(text)
            .fmText(.bodyMedium, weight: FmType.emphasis)
    }
}

/// The small secondary paragraph most cards carry.
struct SettingsNote: View {
    @Environment(\.fmPalette) var fm
    let text: String
    var color: Color?

    var body: some View {
        Text(text)
            .fmText(.bodySmall, color: color ?? fm.textSecondary, height: 1.35)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// A labelled text field on the page's base colour, saved on change.
struct SettingsTextField: View {
    @Environment(\.fmPalette) var fm
    let label: String
    let text: Binding<String>
    let secure: Bool

    @FocusState var focused: Bool

    init(label: String, text: Binding<String>, secure: Bool = false) {
        self.label = label
        self.text = text
        self.secure = secure
    }

    @ViewBuilder private var field: some View {
        if secure {
            SecureField("", text: text)
        } else {
            TextField("", text: text)
                .autocorrectionDisabled(true)
        }
    }

    var body: some View {
        FmField(label: label) {
            field
                .fmText(.bodyLarge)
                .textInputAutocapitalization(.never)
                .focused($focused)
                .padding(.horizontal, FmSpace.x4)
                .padding(.vertical, FmSpace.x3)
                .background(fm.bgBase)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
                .overlay {
                    FmRoundedShape(radius: FmRadius.md)
                        .strokeBorder(focused ? fm.accent : Color.clear, lineWidth: 1.2)
                }
        }
        .padding(.bottom, FmSpace.x3)
    }
}

struct SettingsEmptyNote: View {
    let title: String
    let text: String

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            SettingsTitle(text: title)
            SettingsNote(text: text)
        }
    }
}

/// A small refresh control, or the spinner while loading.
struct SettingsRefreshControl: View {
    let loading: Bool
    let label: String
    let onRefresh: (() -> Void)?

    var body: some View {
        if loading {
            FmCircularProgressIndicator(size: 16, strokeWidth: 2)
        } else if let onRefresh = onRefresh {
            FmIconButton(icon: "refresh", label: label, size: 18, onPressed: onRefresh)
                .frame(width: 36, height: 36)
        }
    }
}

/// A tinted square around a glyph (device and session rows).
struct SettingsGlyphTile: View {
    let icon: String
    let color: Color

    var body: some View {
        FmIcon(icon, size: 20, color: color)
            .frame(width: 36, height: 36)
            .background(color.fmWithAlpha(28))
            .clipShape(FmRoundedShape(radius: FmRadius.md))
    }
}

// MARK: Profile

struct ProfileAccountCard: View {
    @Environment(\.fmPalette) var fm
    let session: AuthSession?
    let guest: Bool
    let status: UserStatus?

    var body: some View {
        let text: ProfileCardText = profileCardText(session, guest: guest)
        HStack(spacing: FmSpace.x3) {
            FmIcon("person_outline", size: 24, color: fm.dark ? FmColors.darkBgBase : FmColors.bgRaised)
                .frame(width: 42, height: 42)
                .background(fm.textPrimary)
                .clipShape(Circle())
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                HStack(spacing: 0) {
                    Text(text.title)
                        .fmText(.bodyMedium, weight: FmType.emphasis)
                        .lineLimit(1)
                        .truncationMode(.tail)
                    if let status = status, !guest {
                        UserStatusEmoji(status: status)
                    }
                }
                Text(text.subtitle)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            if guest {
                FmStatusBadge(label: "Guest", color: fm.textSecondary)
            } else {
                FmStatusBadge(label: "Active", color: fm.success)
            }
        }
    }
}

/// The Away switch and the "Set a status" row.
struct UserStatusControls: View {
    @Environment(\.fmPalette) var fm
    let service: UserStatusService
    let onOpenSheet: () -> Void
    let onRefused: (String) -> Void

    private var awayBinding: Binding<Bool> {
        Binding<Bool>(
            get: { service.status.away },
            set: { (away: Bool) in
                Task { @MainActor in
                    if let message = await setAwayFromSettings(service, away) {
                        onRefused(message)
                    }
                }
            })
    }

    var body: some View {
        let status: UserStatus = service.status
        VStack(alignment: .leading, spacing: 0) {
            Toggle(isOn: awayBinding) {
                HStack(spacing: FmSpace.x3) {
                    PresenceDot(online: true, away: status.away)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Away").fmText(.bodyLarge)
                        Text("Others see you as away next to your name")
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                }
            }
            .tint(fm.accent)
            .disabled(service.busy)
            .accessibilityIdentifier("settings-away-switch")
            .padding(.vertical, FmSpace.x2)
            Button(action: onOpenSheet) {
                HStack(spacing: FmSpace.x3) {
                    if status.emoji.isEmpty {
                        FmIcon("add_reaction_outlined", size: 24, color: fm.textSecondary)
                    } else {
                        Text(verbatim: status.emoji).fmText(.titleMedium)
                    }
                    VStack(alignment: .leading, spacing: 2) {
                        Text(userStatusSummary(status, now: Date()))
                            .fmText(.bodyLarge)
                            .lineLimit(1)
                            .truncationMode(.tail)
                        if !status.hasCustom {
                            Text("An emoji and a short note next to your name")
                                .fmText(.bodySmall, color: fm.textSecondary)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    FmIcon("chevron_right", size: 24)
                }
                .padding(.vertical, FmSpace.x2)
                .contentShape(Rectangle())
            }
            .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.06), cornerRadius: FmRadius.md))
            .accessibilityIdentifier("settings-set-status")
        }
    }
}

// MARK: Organizations, sponsored, legal

struct SponsoredMessagesRow: View {
    @Environment(\.fmPalette) var fm
    let settings: SettingsService

    private var binding: Binding<Bool> {
        Binding<Bool>(
            get: { settings.showSponsoredMessages },
            set: { (value: Bool) in settings.setShowSponsoredMessages(value) })
    }

    var body: some View {
        Toggle(isOn: binding) {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                SettingsTitle(text: "Show sponsored messages")
                SettingsNote(text: SettingsCopy.sponsored)
            }
        }
        .tint(fm.accent)
        .accessibilityIdentifier("show-sponsored-messages")
    }
}

struct LegalLinkRow: View {
    @Environment(\.fmPalette) var fm
    let label: String
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            HStack {
                SettingsTitle(text: label)
                    .frame(maxWidth: .infinity, alignment: .leading)
                FmIcon("open_in_new", size: 16, color: fm.textTertiary)
            }
            .padding(FmSpace.x4)
            .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.06)))
        .accessibilityLabel(Text(label))
    }
}

// MARK: This device

struct ThisDeviceCardView: View {
    @Environment(\.fmPalette) var fm
    let session: AuthSession?
    let identityKey: String
    let settings: SettingsService

    var body: some View {
        switch thisDeviceCard(session) {
        case .signInToRegister:
            SettingsEmptyNote(title: SettingsCopy.signInToRegisterTitle, text: SettingsCopy.signInToRegisterBody)
        case .notRegistered:
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                SettingsEmptyNote(title: SettingsCopy.notRegisteredTitle, text: SettingsCopy.notRegisteredBody)
                Text(verbatim: "Key: \(identityKey)")
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .font(FmFonts.ui(size: FmType.micro))
            }
        case .mobileNode(let label, let deviceId, let capabilities, let lastSeen, let canServeAsNode):
            VStack(alignment: .leading, spacing: 0) {
                HStack(spacing: FmSpace.x3) {
                    SettingsGlyphTile(icon: "phone_iphone", color: fm.success)
                    VStack(alignment: .leading, spacing: FmSpace.x1) {
                        SettingsTitle(text: label)
                            .lineLimit(1)
                        FmStatusBadge(label: "Mobile node", color: fm.success)
                    }
                }
                VStack(spacing: 0) {
                    FmKeyValueRow(label: "Device", value: deviceId)
                    FmKeyValueRow(label: "Can", value: capabilities)
                    FmKeyValueRow(label: "Last seen", value: lastSeen)
                    FmKeyValueRow(label: "Key", value: identityKey)
                }
                .padding(.top, FmSpace.x3)
                SettingsNote(text: SettingsCopy.thisPhone)
                    .padding(.top, FmSpace.x2)
                if canServeAsNode {
                    ServeAsNodeRow(settings: settings)
                        .padding(.top, FmSpace.x3)
                }
            }
        }
    }
}

/// Opt-in for acting as this account's node. On a relay
/// that does not serve the drain, the row says so instead of a switch that
/// turns on and does nothing.
struct ServeAsNodeRow: View {
    @Environment(\.fmPalette) var fm
    let settings: SettingsService

    private var supported: Bool {
        return settings.supports(.nodeSync)
    }

    private var binding: Binding<Bool> {
        Binding<Bool>(
            get: { settings.serveAsNode && supported },
            set: { (value: Bool) in settings.setServeAsNode(value) })
    }

    var body: some View {
        Toggle(isOn: binding) {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                SettingsTitle(text: "Serve as a node")
                SettingsNote(
                    text: SettingsCopy.serveAsNode(supported: supported, relayLabel: settings.relays.active.label),
                    color: supported ? nil : fm.warning)
            }
        }
        .tint(fm.accent)
        .disabled(!supported)
        .accessibilityIdentifier("serve-as-node")
    }
}

/// iOS gives apps no API for the UDID, so it is read through
/// a one-time profile on the web page and handed back by a forkmesh:// link.
struct IosUdidCard: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.openURL) var openURL
    let settings: SettingsService
    @State private var notice: FmNotice?

    private func read() {
        let nonce: String = settings.beginIosUdidRead()
        guard let url = URL(string: IosUdidLink.pageUri(settings.serverUrl, nonce)) else {
            notice = FmNotice.error("Could not open Safari.")
            return
        }
        openURL(url) { (accepted: Bool) in
            if !accepted {
                notice = FmNotice.error("Could not open Safari.")
            }
        }
    }

    private func copy(_ udid: String) {
        UIPasteboard.general.string = udid
        notice = FmNotice("UDID copied")
    }

    var body: some View {
        let udid: String = settings.iosUdid
        VStack(alignment: .leading, spacing: 0) {
            SettingsNote(text: SettingsCopy.iosUdid)
            VStack(spacing: 0) {
                FmKeyValueRow(label: "UDID", value: udid.isEmpty ? "Not read yet" : udid)
                if !settings.iosUdidProduct.isEmpty {
                    FmKeyValueRow(label: "Model", value: settings.iosUdidProduct)
                }
                if !settings.iosUdidVersion.isEmpty {
                    FmKeyValueRow(label: "iOS build", value: settings.iosUdidVersion)
                }
            }
            .padding(.top, FmSpace.x2)
            HStack(spacing: FmSpace.x3) {
                if udid.isEmpty {
                    FmFilledButton("Find UDID", icon: "open_in_new", onPressed: { read() })
                } else {
                    FmFilledButton("Copy UDID", icon: "copy", onPressed: { copy(udid) })
                    FmOutlinedButton("Read again", icon: "refresh", onPressed: { read() })
                }
            }
            .padding(.top, FmSpace.x3)
            if notice != nil {
                FmNoticeLine($notice)
                    .padding(.top, FmSpace.x2)
            }
        }
    }
}

// MARK: Nodes

/// The account's node registry, falling back to the
/// inventory cached on the login payload while loading or after a failure.
@MainActor
struct AccountNodesCard: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.openURL) var openURL
    @State var model: AccountNodesModel
    let webSettings: URL?

    init(session: AuthSession?, api: ApiService?, webSettings: URL?) {
        var list: (() async throws -> [AccountDevice])? = nil
        if let client = api {
            list = { try await client.listDevices() }
        }
        _model = State(initialValue: AccountNodesModel(session: session, listDevices: list))
        self.webSettings = webSettings
    }

    private var webLink: some View {
        Button(action: {
            if let url = webSettings {
                openURL(url)
            }
        }) {
            HStack(spacing: FmSpace.x2) {
                FmIcon("open_in_new", size: 14, color: fm.textSecondary)
                Text("Manage and revoke nodes from the web app → Settings")
                    .fmText(.bodySmall, color: fm.accent, weight: FmType.emphasis)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(.vertical, FmSpace.x1)
        }
        .buttonStyle(FmPressStyle(highlight: fm.accent.opacity(0.08), cornerRadius: FmRadius.sm))
        .accessibilityLabel(Text("Manage nodes on the web"))
        .padding(.top, FmSpace.x3)
    }

    @ViewBuilder private func registry(_ summary: NodesSummary) -> some View {
        HStack {
            SettingsTitle(text: summary.headline)
                .frame(maxWidth: .infinity, alignment: .leading)
            SettingsRefreshControl(
                loading: model.loading,
                label: "Refresh nodes",
                onRefresh: model.canRefresh ? { Task { await model.load() } } : nil)
        }
        if !model.error.isEmpty {
            Text(model.error)
                .fmText(.bodySmall, color: fm.warning)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(model.error)
                .padding(.top, FmSpace.x2)
        }
        if summary.devices.isEmpty {
            SettingsNote(text: nodesEmptyText(loading: model.loading))
                .padding(.top, FmSpace.x2)
        } else {
            if summary.showsDesktopNote {
                SettingsNote(text: SettingsCopy.desktopNote)
                    .padding(.top, FmSpace.x2)
            }
            VStack(spacing: FmSpace.x2) {
                ForEach(summary.devices) { (device: AccountDevice) in
                    DeviceRow(device: device, isCurrent: model.isCurrent(device))
                }
            }
            .padding(.top, FmSpace.x3)
        }
        webLink
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if let summary = model.summary {
                registry(summary)
            } else {
                SettingsEmptyNote(title: SettingsCopy.signInForNodesTitle, text: SettingsCopy.signInForNodesBody)
            }
        }
        .task { await model.load() }
    }
}

struct DeviceRow: View {
    @Environment(\.fmPalette) var fm
    let device: AccountDevice
    let isCurrent: Bool

    private var tone: Color {
        switch deviceTone(device) {
        case .offline: return fm.offline
        case .success: return fm.success
        case .warning: return fm.warning
        }
    }

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x3) {
            SettingsGlyphTile(icon: device.isDesktopNode ? "desktop_mac_outlined" : "phone_iphone", color: tone)
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                SettingsTitle(text: deviceRowTitle(device, isCurrent: isCurrent))
                    .lineLimit(1)
                FmStatusBadge(label: device.statusLabel, color: tone)
                Text(deviceRowDetail(device))
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .lineLimit(2)
                if let timeline = deviceRowTimeline(device) {
                    Text(timeline)
                        .font(FmFonts.ui(size: FmType.micro))
                        .foregroundStyle(fm.textTertiary)
                }
                if !device.pubkey.isEmpty {
                    Text(verbatim: "Key: \(shortDeviceKey(device.pubkey))")
                        .font(FmFonts.ui(size: FmType.micro))
                        .foregroundStyle(fm.textTertiary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(FmSpace.x3)
        .background(fm.bgBase)
        .clipShape(FmRoundedShape(radius: FmRadius.md))
    }
}

// MARK: Sessions

/// Every live sign-in on the account, with a "Log
/// out" on each row and "Log out all other sessions".
@MainActor
struct AccountSessionsCard: View {
    @Environment(\.fmPalette) var fm
    @State var model: AccountSessionsModel
    @State var confirming: RevokeConfirmation?
    @State private var notice: FmNotice?
    let onCurrentSignedOut: () -> Void

    init(api: ApiService?, onCurrentSignedOut: @escaping () -> Void) {
        var list: (() async throws -> [AccountSession])? = nil
        var revoke: ((String) async throws -> Bool)? = nil
        if let client = api {
            list = { try await client.listSessions() }
            revoke = { (target: String) in try await client.revokeSession(target) }
        }
        _model = State(initialValue: AccountSessionsModel(listSessions: list, revokeSession: revoke))
        self.onCurrentSignedOut = onCurrentSignedOut
    }

    private var confirmShown: Binding<Bool> {
        Binding<Bool>(
            get: { confirming != nil },
            set: { (shown: Bool) in
                if !shown {
                    confirming = nil
                }
            })
    }

    private func revoke(_ target: String) {
        confirming = nil
        notice = nil
        Task { @MainActor in
            let outcome: AccountSessionsModel.RevokeOutcome = await model.revoke(target)
            switch outcome {
            case .currentSignedOut:
                onCurrentSignedOut()
            case .revoked(let message):
                notice = FmNotice(message)
            case .failed, .ignored:
                break
            }
        }
    }

    @ViewBuilder private var rows: some View {
        if model.visible.isEmpty {
            SettingsNote(text: model.emptyText)
        } else {
            VStack(spacing: FmSpace.x2) {
                ForEach(model.visible) { (session: AccountSession) in
                    SessionRow(
                        session: session,
                        busy: model.isBusy(session),
                        onLogOut: model.canRevoke ? { confirming = revokeOneConfirmation(session) } : nil)
                }
            }
        }
    }

    @ViewBuilder private var othersButton: some View {
        if model.othersCount > 0 {
            FmToneButton(
                label: "Log out all other sessions",
                icon: "logout",
                tone: .danger,
                height: 44,
                onPressed: model.canRevoke ? { confirming = revokeOthersConfirmation(count: model.othersCount) } : nil)
                .padding(.top, FmSpace.x3)
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                SettingsTitle(text: model.headline)
                    .frame(maxWidth: .infinity, alignment: .leading)
                SettingsRefreshControl(
                    loading: model.loading,
                    label: "Refresh sessions",
                    onRefresh: model.apiAvailable ? { Task { await model.load() } } : nil)
            }
            SettingsNote(text: SettingsCopy.sessions)
                .padding(.top, FmSpace.x2)
            if !model.error.isEmpty {
                Text(model.error)
                    .fmText(.bodySmall, color: fm.warning)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(model.error)
                    .padding(.top, FmSpace.x2)
            }
            rows
                .padding(.top, FmSpace.x3)
            if notice != nil {
                FmNoticeLine($notice)
                    .padding(.top, FmSpace.x2)
            }
            othersButton
        }
        .task { await model.load() }
        .fmDialog(isPresented: confirmShown) {
            if let confirmation = confirming {
                FmConfirmDialog(
                    title: confirmation.title,
                    message: confirmation.body,
                    confirmLabel: confirmation.action,
                    destructive: true,
                    onCancel: { confirming = nil },
                    onConfirm: { revoke(confirmation.target) })
            }
        }
    }
}

struct SessionRow: View {
    @Environment(\.fmPalette) var fm
    let session: AccountSession
    let busy: Bool
    let onLogOut: (() -> Void)?

    var body: some View {
        let tone: Color = session.current ? fm.success : fm.textSecondary
        let meta: String = sessionRowMeta(session)
        HStack(spacing: FmSpace.x3) {
            SettingsGlyphTile(icon: sessionRowIcon(session), color: tone)
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                SettingsTitle(text: sessionRowTitle(session))
                    .lineLimit(1)
                if !meta.isEmpty {
                    Text(meta)
                        .font(FmFonts.ui(size: FmType.micro))
                        .foregroundStyle(fm.textTertiary)
                        .lineLimit(1)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            if busy {
                FmCircularProgressIndicator(size: 16, strokeWidth: 2)
            } else {
                FmTextButton("Log out", color: fm.danger, onPressed: onLogOut)
            }
        }
        .padding(FmSpace.x3)
        .background(fm.bgBase)
        .clipShape(FmRoundedShape(radius: FmRadius.md))
    }
}

// MARK: Push

/// Where the APNs chain stands for this phone, and
/// a real test alert. Every link fails silently on its own, so each is named.
@MainActor
struct PushNotificationsCard: View {
    @Environment(\.fmPalette) var fm
    @State var model: PushCardModel
    @State private var notice: FmNotice?

    init(push: PushNotificationService) {
        _model = State(initialValue: PushCardModel(push: push))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            SettingsNote(text: SettingsCopy.push)
            VStack(spacing: 0) {
                FmKeyValueRow(label: "Permission", value: model.permissionLabel)
                FmKeyValueRow(label: "Token", value: model.tokenLabel)
                FmKeyValueRow(label: "Relay", value: model.relayLabel)
            }
            .padding(.top, FmSpace.x2)
            if !model.push.lastError.isEmpty {
                SettingsNote(text: model.push.lastError, color: fm.danger)
                    .fmMessageTools(model.push.lastError)
                    .padding(.top, FmSpace.x1)
            }
            if model.denied {
                SettingsNote(text: SettingsCopy.pushDenied)
                    .padding(.top, FmSpace.x1)
            }
            HStack(spacing: FmSpace.x3) {
                if model.canEnable {
                    FmFilledButton(
                        "Enable notifications",
                        icon: "notifications_active_outlined",
                        onPressed: { Task { await model.enable() } })
                }
                FmOutlinedButton(
                    model.sendLabel,
                    icon: "send_outlined",
                    onPressed: model.canSendTest
                        ? {
                            notice = nil
                            Task { @MainActor in
                                notice = FmNotice(await model.sendTest())
                            }
                        } : nil)
            }
            .padding(.top, FmSpace.x3)
            if notice != nil {
                FmNoticeLine($notice)
                    .padding(.top, FmSpace.x2)
            }
        }
    }
}

struct NotificationContentCard: View {
    @AppStorage(PushPreviewMode.preferenceKey) private var stored: String = PushPreviewMode.nameAndMessage.rawValue

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Picker("Notification content", selection: Binding<String>(
                get: { PushPreviewMode.parse(stored).rawValue },
                set: { (value: String) in
                    stored = value
                    PushCredentialStore.setPreview(value)
                })
            ) {
                ForEach(PushPreviewMode.allCases, id: \.rawValue) { (mode: PushPreviewMode) in
                    Text(mode.title).tag(mode.rawValue)
                }
            }
            .pickerStyle(.navigationLink)
            .accessibilityIdentifier("settings-notification-content")
            SettingsNote(text: "What a chat notification shows. The server never sees the message: this phone fetches the still-encrypted message and opens it itself, as Signal does. Name only shows who wrote; No name or message shows only that something arrived.")
        }
    }
}

// MARK: Appearance

/// Theme mode and accent colour, applied the moment they
/// are tapped.
struct AppearanceCard: View {
    @Environment(\.fmPalette) var fm
    let settings: SettingsService

    private static let modes: [(mode: ThemeMode, icon: String, label: String)] = [
        (mode: ThemeMode.system, icon: "brightness_auto_outlined", label: "System"),
        (mode: ThemeMode.light, icon: "light_mode_outlined", label: "Light"),
        (mode: ThemeMode.dark, icon: "dark_mode_outlined", label: "Dark"),
    ]

    private var modeTrack: some View {
        HStack(spacing: 0) {
            ForEach(AppearanceCard.modes, id: \.label) { (entry: (mode: ThemeMode, icon: String, label: String)) in
                ThemeModeSegment(
                    icon: entry.icon,
                    label: entry.label,
                    selected: settings.themeMode == entry.mode,
                    onTap: { settings.setThemeMode(entry.mode) })
                    .accessibilityIdentifier("appearance-theme-\(entry.mode.rawValue)")
            }
        }
        .padding(3)
        .background(fm.bgOverlay)
        .clipShape(Capsule())
    }

    private var swatches: some View {
        HStack(spacing: FmSpace.x5) {
            ForEach(FmAccent.allCases, id: \.self) { (accent: FmAccent) in
                AccentSwatch(
                    accent: accent,
                    selected: settings.accent.rawValue == accent.rawValue,
                    onTap: { settings.setAccent(AccentChoice.fromName(accent.rawValue)) })
                    .accessibilityIdentifier("appearance-accent-\(accent.rawValue)")
            }
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("Theme").fmText(.labelMedium, color: fm.textSecondary)
            modeTrack
                .padding(.top, FmSpace.x2)
            Text("Accent colour").fmText(.labelMedium, color: fm.textSecondary)
                .padding(.top, FmSpace.x4)
            swatches
                .padding(.top, FmSpace.x2)
        }
    }
}

private struct ThemeModeSegment: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion
    let icon: String
    let label: String
    let selected: Bool
    let onTap: () -> Void

    private var fill: Color {
        if !selected {
            return Color.clear
        }
        // In dark mode the raised surface is darker than the overlay track,
        // so the selected segment is lifted with a light wash instead.
        return fm.dark ? Color.white.opacity(0.12) : fm.bgRaised
    }

    var body: some View {
        let color: Color = selected ? fm.textPrimary : fm.textSecondary
        HStack(spacing: FmSpace.x1) {
            FmIcon(icon, size: 16, color: color)
            Text(label)
                .fmText(.labelMedium, color: color, weight: selected ? FmType.emphasis : FmType.medium)
                .lineLimit(1)
                .truncationMode(.tail)
        }
        .frame(maxWidth: .infinity, minHeight: 38)
        .background(fill)
        .clipShape(Capsule())
        .shadow(color: selected && !fm.dark ? Color(fmARGB: 0x14000000) : Color.clear, radius: 3, x: 0, y: 1)
        .contentShape(Capsule())
        .onTapGesture {
            if selected {
                return
            }
            FmHaptics.selection()
            withAnimation(FmMotion.easeOutCubic(FmMotion.normal, reduceMotion: reduceMotion)) {
                onTap()
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(verbatim: "\(label) theme"))
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : [.isButton])
    }
}

private struct AccentSwatch: View {
    @Environment(\.fmPalette) var fm
    let accent: FmAccent
    let selected: Bool
    let onTap: () -> Void

    var body: some View {
        // The variant the app will actually draw in the current mode.
        let color: Color = fm.dark ? accent.onDark : accent.light
        VStack(spacing: FmSpace.x1) {
            Circle()
                .fill(color)
                .overlay {
                    if selected {
                        FmIcon("check_rounded", size: 20, color: Color.white)
                    }
                }
                .padding(3)
                .frame(width: 44, height: 44)
                .overlay {
                    Circle().strokeBorder(selected ? color : Color.clear, lineWidth: 2)
                }
            Text(accent.label)
                .fmText(
                    .bodySmall,
                    color: selected ? fm.textPrimary : fm.textSecondary,
                    weight: selected ? FmType.emphasis : FmType.regular)
        }
        .contentShape(Rectangle())
        .onTapGesture {
            if selected {
                return
            }
            FmHaptics.selection()
            onTap()
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(verbatim: "\(accent.label) accent"))
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : [.isButton])
    }
}

// MARK: Account

struct SignOutCard: View {
    @Environment(\.fmPalette) var fm
    let signedInAs: String
    let onSignOut: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            SettingsTitle(text: "Signed in as \(signedInAs)")
                .lineLimit(1)
                .truncationMode(.tail)
            SettingsNote(text: SettingsCopy.signOutCard)
                .padding(.top, FmSpace.x2)
            FmToneButton(label: "Sign out", icon: "logout", tone: .danger, height: 44, onPressed: onSignOut)
                .padding(.top, FmSpace.x3)
        }
    }
}

/// The ways out of guest mode.
struct GuestAccountCard: View {
    @Environment(\.fmPalette) var fm
    let onLogIn: () -> Void
    let onSignUp: () -> Void
    let onLeave: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            SettingsTitle(text: "You are exploring as a guest")
            SettingsNote(text: SettingsCopy.guestAccount(channel: guestChatChannel))
                .padding(.top, FmSpace.x2)
            ViewThatFits(in: .horizontal) {
                HStack(spacing: FmSpace.x3) {
                    guestButtons
                }
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    guestButtons
                }
            }
            .padding(.top, FmSpace.x3)
        }
        .accessibilityIdentifier("settings-guest-account")
    }

    @ViewBuilder private var guestButtons: some View {
        FmFilledButton("Log in", icon: "login", onPressed: onLogIn)
            .accessibilityIdentifier("settings-guest-log-in")
        FmOutlinedButton("Create an account", onPressed: onSignUp)
        FmTextButton("Leave guest mode", color: fm.textSecondary, onPressed: onLeave)
    }
}

struct DeleteAccountCard: View {
    @Environment(\.fmPalette) var fm
    let onDelete: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            SettingsTitle(text: "Delete account")
            SettingsNote(text: SettingsCopy.deleteCard)
                .padding(.top, FmSpace.x2)
            FmOutlinedButton(
                "Delete account",
                icon: "delete_forever_outlined",
                foreground: fm.danger,
                minHeight: 44,
                onPressed: onDelete)
                .accessibilityIdentifier("settings-delete-account")
                .padding(.top, FmSpace.x3)
        }
    }
}

/// The delete-account dialog: the current password
/// and DELETE typed out.
struct DeleteAccountDialog: View {
    @Environment(\.fmPalette) var fm
    @Bindable var model: DeleteAccountModel
    let onCancel: () -> Void
    let onDeleted: () -> Void

    private func submit() {
        Task { @MainActor in
            if await model.submit() {
                onDeleted()
            }
        }
    }

    var body: some View {
        FmDialog(
            title: SettingsCopy.deleteTitle,
            message: SettingsCopy.deleteBody,
            content: {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    FmTextField(text: $model.password, hint: "Current password", secure: true)
                        .submitLabel(.next)
                    FmTextField(text: $model.confirm, hint: "Type DELETE to confirm", onSubmit: { submit() })
                        .textInputAutocapitalization(.characters)
                        .autocorrectionDisabled(true)
                        .submitLabel(.done)
                    if let error = model.error {
                        Text(error).fmText(.bodyMedium, color: fm.danger)
                            .fmMessageTools(error)
                    }
                }
            },
            actions: {
                FmDialogActions(
                    onCancel: onCancel,
                    onConfirm: { submit() },
                    confirmLabel: "Delete account",
                    destructive: true,
                    busy: model.busy)
            })
    }
}

struct QuickSetupCard: View {
    @Environment(\.fmPalette) var fm
    let input: QuickSetupInput
    let onStep: (QuickSetupStepId) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(QuickSetup.steps(input).enumerated()), id: \.element.id) { (index: Int, step: QuickSetupStep) in
                if index > 0 { FmHairline() }
                FmListRow(
                    title: step.title,
                    subtitle: step.detail,
                    leading: FmIcon(step.done ? "check_circle" : "radio_button_unchecked", size: 22,
                                    color: step.done ? fm.success : fm.textTertiary).fmAny,
                    onTap: step.done ? nil : { onStep(step.id) },
                    showChevron: !step.done)
            }
        }
    }
}

struct DevicePreferencesCard: View {
    @Environment(\.fmPalette) var fm
    let settings: SettingsService

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Toggle(isOn: Binding<Bool>(get: { settings.hapticsEnabled }, set: { (on: Bool) in settings.setHapticsEnabled(on) })) {
                Text("Haptics").fmText(.bodyMedium)
            }
            .tint(fm.accent)
            Toggle(isOn: Binding<Bool>(get: { settings.soundsEnabled }, set: { (on: Bool) in settings.setSoundsEnabled(on) })) {
                Text("Sounds").fmText(.bodyMedium)
            }
            .tint(fm.accent)
            SettingsNote(text: "Soft notes when you tap, switch tabs, send, finish or delete. On when the app opens, even in silent mode; switch them off here.")
            SettingsNote(text: "Start-up, the window footer and repository sync timers are desktop settings; change them on the desktop.")
        }
    }
}

struct GenieSpiderCard: View {
    @Environment(\.fmPalette) var fm
    @State private var hub: GenieHub = GenieHub.shared

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Toggle(isOn: Binding<Bool>(get: { !hub.spiderHidden }, set: { (on: Bool) in hub.setSpiderHidden(!on) })) {
                Text("Show the Genie spider").fmText(.bodyMedium)
            }
            .tint(fm.accent)
            .accessibilityIdentifier("settings-genie-spider")
            SettingsNote(text: "The spider that crawls over the app. Off hides it completely: it stops wandering, listening and hand play. Touch and hold the spider and choose Hide the spider to hide it from there.")
        }
    }
}

struct SuggestionModeCard: View {
    @Environment(\.fmPalette) var fm
    @State private var suggestions: ShellSuggestions = ShellSuggestions.shared

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Toggle(isOn: Binding<Bool>(get: { suggestions.on }, set: { (on: Bool) in suggestions.setOn(on) })) {
                Text("Suggestion mode").fmText(.bodyMedium)
            }
            .tint(fm.accent)
            .accessibilityIdentifier("settings-suggestion-mode")
            SettingsNote(text: "Shows the next best new CRM contact and the marketing channel that needs attention at the top, next to Glasses Alert. Tap the contact and the Genie writes to them; tap the megaphone to open the channels. Touch and hold the contact for Not now.")
            Button {
                ShellPocket.shared.lock()
            } label: {
                Label("Pocket mode", systemImage: "lock.iphone").fmText(.bodyMedium, color: fm.accent)
            }
            .accessibilityIdentifier("settings-pocket-mode")
            SettingsNote(text: "A lock screen with Glasses Alert, your pings and the walking figure shown big. Slide to unlock. Your avatar's menu at the top opens it too; it shows a walking figure there while you walk.")
        }
    }
}

struct LocalRepoDefaultsCard: View {
    @Environment(\.fmPalette) var fm
    let settings: SettingsService
    let session: AuthSession?
    @State var branch: String = ""
    @State var branchError: String = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            SettingsNote(text: "Used when this phone commits or creates a repository. Empty fields use your account.")
                .padding(.bottom, FmSpace.x3)
            SettingsTextField(
                label: "Author name (\(session?.nodeName ?? "account name"))",
                text: Binding<String>(get: { settings.localAuthorName }, set: { (v: String) in settings.setLocalAuthorName(v) }))
            SettingsTextField(
                label: "Author email",
                text: Binding<String>(get: { settings.localAuthorEmail }, set: { (v: String) in settings.setLocalAuthorEmail(v) }))
            if !settings.localAuthorEmail.isEmpty && !SettingsService.isPlausibleEmail(settings.localAuthorEmail) {
                SettingsNote(text: "That does not look like an email address.", color: fm.warning)
                    .fmMessageTools("That does not look like an email address.")
                    .padding(.bottom, FmSpace.x3)
            }
            SettingsTextField(label: "Default branch for new repositories", text: Binding<String>(
                get: { branch },
                set: { (v: String) in
                    branch = v
                    branchError = settings.setLocalDefaultBranch(v) ? "" : "Git does not allow that branch name."
                }))
            if !branchError.isEmpty {
                SettingsNote(text: branchError, color: fm.danger)
                    .fmMessageTools(branchError)
            }
        }
        .onAppear { branch = settings.localDefaultBranch == SettingsService.fallbackBranch ? "" : settings.localDefaultBranch }
    }
}

struct AgentPreferencesCard: View {
    @Environment(\.fmPalette) var fm
    @State var credentials: AgentCredentials?
    @State var showSetup: Bool = false

    var body: some View {
        VStack(spacing: 0) {
            FmListRow(
                title: "This phone's agents",
                subtitle: credentials.map { "\($0.kind.label) · \($0.model.isEmpty ? "default model" : $0.model)" } ?? "Not set up",
                leading: FmIcon("smart_toy_outlined", size: 22, color: fm.textSecondary).fmAny,
                onTap: { showSetup = true })
            FmHairline()
            SettingsNote(text: "Your desktop's providers and default are listed under Desktops. Which models the composer offers is chosen in the composer.")
                .padding(FmSpace.x4)
        }
        .onAppear { credentials = Services.get(AgentCredentialStore.self)?.read() }
        .sheet(isPresented: $showSetup, onDismiss: { credentials = Services.get(AgentCredentialStore.self)?.read() }) {
            AgentSetupSheet(initial: credentials) { (saved: AgentCredentials?) in
                credentials = saved
            }
            .fmSheetStyle()
        }
    }
}

struct DictationLanguageCard: View {
    @Environment(\.fmPalette) var fm
    let settings: SettingsService
    @State private var model: PromptSpeechModel?
    @State private var installing: Bool = false

    private var locales: [String] {
        SFSpeechRecognizer.supportedLocales().map { $0.identifier }.sorted { name($0) < name($1) }
    }

    private func name(_ identifier: String) -> String {
        Locale.current.localizedString(forIdentifier: identifier) ?? identifier
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Toggle(isOn: Binding<Bool>(get: { settings.dictatesOnOpen }, set: { (on: Bool) in settings.setDictatesOnOpen(on) })) {
                Text("Listen when the prompt opens").fmText(.bodyMedium)
            }
            .tint(fm.accent)
            SettingsNote(text: "The prompt starts dictation as soon as it opens. Typing, sending, closing the prompt or tapping Listening stops it; /dictate starts it again. Touch and hold Listening to turn this off.")
            Picker("Dictation language", selection: Binding<String>(
                get: { settings.dictationLocale },
                set: { (v: String) in settings.setDictationLocale(v) })
            ) {
                Text("Same as the phone").tag("")
                ForEach(locales, id: \.self) { (identifier: String) in
                    Text(name(identifier)).tag(identifier)
                }
            }
            .pickerStyle(.navigationLink)
            speechModel
            SettingsNote(text: "Dictation runs on this phone with Apple's speech recognition. The desktop's whisper.cpp and Parakeet engines stay on the desktop.")
        }
        .task(id: settings.dictationLocale) {
            model = nil
            model = await PromptDictation.modelStatus()
        }
    }

    @ViewBuilder private var speechModel: some View {
        if installing {
            HStack(spacing: FmSpace.x2) {
                ProgressView()
                SettingsNote(text: "Installing Apple's on-device speech model…")
            }
        } else if let model = model {
            switch model {
            case .olderSystem:
                SettingsNote(text: "Apple's newer on-device speech model needs iOS 26. Until then dictation uses Apple's speech recognizer, on the device when it can.")
            case .unsupported:
                SettingsNote(text: "Apple's on-device speech model has no model for this language on this phone, so dictation uses Apple's speech recognizer.")
            case .installed:
                SettingsNote(text: "Apple's on-device speech model is installed. Dictation runs on this phone, without a network.")
            case .missing:
                SettingsNote(text: "Apple's on-device speech model for this language is not installed yet. Until it is, dictation uses Apple's speech recognizer.")
                FmOutlinedButton("Install speech model", icon: "download_outlined", onPressed: { install() })
            case .failed(let message):
                SettingsNote(text: "Apple's speech model could not be installed: \(message)", color: fm.danger)
                    .fmMessageTools("Apple's speech model could not be installed: \(message)")
                FmOutlinedButton("Try again", icon: "refresh", onPressed: { install() })
            }
        } else {
            SettingsNote(text: "Checking Apple's on-device speech model…")
        }
    }

    private func install() {
        guard !installing else {
            return
        }
        installing = true
        Task { @MainActor in
            model = await PromptDictation.installModel()
            installing = false
        }
    }
}

struct RecentAccountsCard: View {
    @Environment(\.fmPalette) var fm
    let accounts: [RecentAccount]
    let onSwitch: (RecentAccount) -> Void
    let onForget: (RecentAccount) -> Void
    let onAdd: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            ForEach(Array(accounts.enumerated()), id: \.element.id) { (index: Int, account: RecentAccount) in
                if index > 0 { FmHairline() }
                FmListRow(
                    title: account.name,
                    subtitle: account.subtitle,
                    leading: FmAvatar(name: account.name, size: 32).fmAny,
                    trailing: FmIconButton(icon: "close", label: "Forget \(account.name)", size: 18, color: fm.textTertiary,
                                           onPressed: { onForget(account) }).fmAny,
                    onTap: { onSwitch(account) },
                    showChevron: false)
            }
            if !accounts.isEmpty { FmHairline() }
            FmListRow(
                title: "Use another account",
                subtitle: "Sign out and sign in as someone else",
                leading: FmIcon("login", size: 22, color: fm.textSecondary).fmAny,
                onTap: onAdd)
        }
    }
}

struct IdentityBackupNag: View {
    @Environment(\.fmPalette) var fm

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x3) {
            FmIcon("warning_amber_outlined", size: 22, color: fm.warning)
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                SettingsTitle(text: "Back up your identity key")
                SettingsNote(text: "Save an encrypted recovery kit so you can restore this key if the phone is lost.")
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            FmIcon("chevron_right", size: 20, color: fm.textDisabled)
        }
        .padding(FmSpace.x4)
        .contentShape(Rectangle())
    }
}
