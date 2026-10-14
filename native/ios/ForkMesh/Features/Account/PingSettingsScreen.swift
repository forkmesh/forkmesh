// The Pings settings page (issue #347).
// Code pushes, the pause, quiet hours, every ping kind with its ping and email switch, email
// pacing, this phone's push chain, and (administrators only) the /status monitor alerts. Every
// switch saves at once and re-renders from what the Worker stored. Logic: FMApi's
// PingSettingsModel and PingSettings.

import FMAccount
import FMApi
import FMBase
import SwiftUI

@MainActor
struct PingSettingsScreen: View {
    @Environment(\.fmPalette) var fm

    @State var model: PingSettingsModel?
    @State var customPause: Bool = false
    @State var customPauseDate: Date = Date().addingTimeInterval(24 * 60 * 60)
    @State var editingZone: Bool = false
    @State var zoneDraft: String = ""

    private let push: PushNotificationService?

    /// Width of each trailing switch column.
    private static let switchColumn: CGFloat = 64

    init() {
        push = Services.get(PushNotificationService.self)
        if let api = Services.get(ApiService.self) {
            let auth: AuthService? = Services.get(AuthService.self)
            _model = State(initialValue: PingSettingsModel(api: api, node: {
                auth?.session?.nodeName ?? ""
            }))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        Group {
            if let model = model {
                content(model)
            } else {
                FmEmptyState(
                    icon: "notifications_none_rounded", title: "Ping settings are not available offline.")
            }
        }
        .background(fm.canvas.ignoresSafeArea())
        .navigationTitle("Ping settings")
        .navigationBarTitleDisplayMode(.inline)
    }

    @ViewBuilder
    private func content(_ model: PingSettingsModel) -> some View {
        Group {
            if let settings = model.settings {
                ScrollView {
                    VStack(alignment: .leading, spacing: 0) {
                        if model.saveError != nil {
                            FmNoticeLine(model.saveError, isError: true, onDismiss: { model.saveError = nil })
                                .padding(.bottom, FmSpace.x3)
                                .accessibilityIdentifier("ping-settings-save-error")
                        }
                        pauseSection(model, settings)
                        quietSection(model, settings)
                        kindsSection(model, settings)
                        emailSection(model, settings)
                        pushSection()
                        if let monitors = settings.statusMonitorAlerts {
                            adminSection(model, monitors)
                        }
                    }
                    .padding(FmSpace.x4)
                }
            } else if !model.error.isEmpty && !model.loading {
                errorCard(model)
            } else {
                FmSkeletonList(rows: 6, label: "Loading ping settings")
            }
        }
        .task { await model.start() }
        .sheet(isPresented: $customPause) {
            customPauseSheet(model)
        }
        .alert("Quiet hours time zone", isPresented: $editingZone) {
            TextField("Europe/Berlin", text: $zoneDraft)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            Button("Cancel", role: .cancel) {}
            Button("Save") {
                let zone: String = zoneDraft
                Task { await model.setQuietTimezone(zone) }
            }
        }
    }

    private func errorCard(_ model: PingSettingsModel) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            SettingsCardBox {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    SettingsTitle(text: "Could not load ping settings")
                    SettingsNote(text: model.error)
                        .fmMessageTools(model.error)
                    FmOutlinedButton("Retry", icon: "refresh", onPressed: {
                        Task { await model.load() }
                    })
                }
            }
            Spacer()
        }
        .padding(FmSpace.x4)
    }

    // MARK: Pieces

    /// A title, a muted line under it, and a switch on the trailing edge.
    private func switchRow(
        _ title: String, subtitle: String? = nil, value: Bool, enabled: Bool = true, id: String,
        onChange: @escaping (Bool) -> Void
    ) -> some View {
        Toggle(isOn: Binding(get: { value }, set: { (next: Bool) in onChange(next) })) {
            VStack(alignment: .leading, spacing: 2) {
                Text(title).fmText(.bodyMedium, weight: FmType.emphasis)
                if let subtitle = subtitle, !subtitle.isEmpty {
                    Text(subtitle).fmText(.bodySmall, color: fm.textSecondary)
                }
            }
        }
        .tint(fm.accent)
        .disabled(!enabled)
        .accessibilityIdentifier(id)
    }

    /// A trailing switch in one of the table columns.
    private func columnSwitch(value: Bool, enabled: Bool = true, id: String, onChange: @escaping (Bool) -> Void)
        -> some View
    {
        Toggle("", isOn: Binding(get: { value }, set: { (next: Bool) in onChange(next) }))
            .labelsHidden()
            .tint(fm.accent)
            .disabled(!enabled)
            .frame(width: PingSettingsScreen.switchColumn)
            .accessibilityIdentifier(id)
    }

    /// The group's name over its switch columns.
    private func columnHeader(_ title: String, columns: [String]) -> some View {
        HStack(spacing: 0) {
            Text(title)
                .fmText(.titleSmall)
                .frame(maxWidth: .infinity, alignment: .leading)
            ForEach(columns, id: \.self) { (column: String) in
                Text(column)
                    .fmText(.labelSmall, color: fm.textTertiary)
                    .frame(width: PingSettingsScreen.switchColumn)
            }
        }
    }

    private func chip(_ label: String, id: String, selected: Bool = false, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(label)
                .fmText(.labelMedium, color: selected ? fm.accent : fm.textPrimary)
                .padding(.horizontal, FmSpace.x3)
                .padding(.vertical, FmSpace.x2)
                .background(selected ? fm.accentSubtle : fm.bgBase)
                .clipShape(Capsule())
                .overlay(Capsule().stroke(selected ? fm.accent : fm.border, lineWidth: 1))
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier(id)
    }

    // MARK: Pause

    private func pauseSection(_ model: PingSettingsModel, _ settings: PingSettings) -> some View {
        let paused: Bool = settings.isPaused()
        return SettingsSection(title: "Pause") {
            SettingsCardBox {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    HStack(spacing: FmSpace.x2) {
                        FmIcon(
                            "notifications_active_outlined", size: 20,
                            color: paused ? fm.warning : fm.success)
                        Text(model.pauseLabel)
                            .fmText(.titleSmall)
                            .accessibilityIdentifier("ping-pause-status")
                    }
                    SettingsNote(text: "Pause every banner and email for a while. Pings keep landing in your inbox.")
                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(spacing: FmSpace.x2) {
                            chip("1 hour", id: "ping-pause-1h") {
                                Task { await model.pause(for: 60 * 60) }
                            }
                            chip("1 day", id: "ping-pause-1d") {
                                Task { await model.pause(for: 24 * 60 * 60) }
                            }
                            chip("1 week", id: "ping-pause-1w") {
                                Task { await model.pause(for: 7 * 24 * 60 * 60) }
                            }
                            chip("Custom…", id: "ping-pause-custom") {
                                customPauseDate = Date().addingTimeInterval(24 * 60 * 60)
                                customPause = true
                            }
                            if paused {
                                chip("Resume", id: "ping-pause-resume", selected: true) {
                                    Task { await model.resume() }
                                }
                            }
                        }
                    }
                    .padding(.top, FmSpace.x1)
                }
            }
        }
    }

    private func customPauseSheet(_ model: PingSettingsModel) -> some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                DatePicker(
                    "Pause pings until",
                    selection: $customPauseDate,
                    in: Date()...Date().addingTimeInterval(PingSettingsModel.maxPause),
                    displayedComponents: [.date, .hourAndMinute])
                    .datePickerStyle(.graphical)
                    .tint(fm.accent)
                Spacer()
            }
            .padding(FmSpace.x4)
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle("Pause pings until")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { customPause = false }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Pause") {
                        let until: Date = customPauseDate
                        customPause = false
                        Task { await model.pause(until: until) }
                    }
                }
            }
        }
    }

    // MARK: Quiet hours

    /// Minutes after midnight as a Date on a fixed day in UTC, so the clock picker shows the
    /// minutes as they are, whatever zone the phone is in (the window's own zone is a field).
    private static var utc: Calendar {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC") ?? TimeZone.current
        return calendar
    }

    private static func date(minutes: Int) -> Date {
        Date(timeIntervalSince1970: Double(minutes * 60))
    }

    private static func minutes(of date: Date) -> Int {
        let parts = utc.dateComponents([.hour, .minute], from: date)
        return (parts.hour ?? 0) * 60 + (parts.minute ?? 0)
    }

    private func timeButton(_ label: String, minutes: Int, id: String, onPicked: @escaping (Int) -> Void)
        -> some View
    {
        HStack(spacing: FmSpace.x2) {
            Text(label).fmText(.bodyMedium, color: fm.textSecondary)
            Spacer(minLength: 0)
            DatePicker(
                label,
                selection: Binding(
                    get: { PingSettingsScreen.date(minutes: minutes) },
                    set: { (next: Date) in
                        let picked: Int = PingSettingsScreen.minutes(of: next)
                        if picked != minutes { onPicked(picked) }
                    }),
                displayedComponents: [.hourAndMinute])
                .labelsHidden()
                .environment(\.timeZone, TimeZone(identifier: "UTC") ?? TimeZone.current)
                .accessibilityIdentifier(id)
        }
        .frame(maxWidth: .infinity)
    }

    private func quietSection(_ model: PingSettingsModel, _ settings: PingSettings) -> some View {
        let quiet: PingQuietHours = settings.quietHours
        let days: Set<Int> = Set(quiet.days)
        return SettingsSection(title: "Quiet hours") {
            SettingsCardBox {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    switchRow(
                        "Quiet hours", subtitle: model.quietLabel, value: quiet.enabled, id: "ping-quiet-enabled"
                    ) { (on: Bool) in
                        Task { await model.setQuietEnabled(on) }
                    }
                    HStack(spacing: FmSpace.x3) {
                        timeButton("From", minutes: quiet.start, id: "ping-quiet-start") { (minutes: Int) in
                            Task { await model.setQuietStart(minutes) }
                        }
                        timeButton("Until", minutes: quiet.end, id: "ping-quiet-end") { (minutes: Int) in
                            Task { await model.setQuietEnd(minutes) }
                        }
                    }
                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(spacing: FmSpace.x1) {
                            ForEach(0..<7, id: \.self) { (day: Int) in
                                chip(
                                    PingSettingsModel.weekdays[day], id: "ping-quiet-day-\(day)",
                                    selected: days.contains(day)
                                ) {
                                    let on: Bool = !days.contains(day)
                                    Task { await model.setQuietDay(day, on) }
                                }
                            }
                        }
                    }
                    SettingsNote(text: "Days a quiet window starts on. A window that ends before it starts runs past midnight.")
                    Button {
                        zoneDraft = quiet.timezone
                        editingZone = true
                    } label: {
                        HStack(spacing: FmSpace.x3) {
                            FmIcon("public", size: 20, color: fm.textTertiary)
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Time zone").fmText(.bodyMedium)
                                Text(quiet.timezone).fmText(.bodySmall, color: fm.textSecondary)
                            }
                            Spacer(minLength: 0)
                        }
                        .frame(minHeight: 44)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .accessibilityIdentifier("ping-quiet-timezone")
                }
            }
        }
    }

    // MARK: Ping types

    private func kindsSection(_ model: PingSettingsModel, _ settings: PingSettings) -> some View {
        let emailOn: Bool = settings.emailNotifications
        return SettingsSection(title: "Ping types") {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                // Its own switch rather than a catalog row: it is the one kind that fires on every
                // push, so it is the one people most want to turn off (or on) without hunting
                // through the repository group.
                SettingsCardBox {
                    switchRow(
                        "Code pushes",
                        subtitle: "A ping on this phone whenever code is pushed to a repository you own or follow.",
                        value: settings.codePushes,
                        id: "ping-code-pushes"
                    ) { (on: Bool) in
                        Task { await model.setCodePushes(on) }
                    }
                }
                ForEach(settings.catalog, id: \.key) { (group: PingKindGroup) in
                    SettingsCardBox {
                        VStack(alignment: .leading, spacing: FmSpace.x1) {
                            columnHeader(group.label, columns: ["Ping", "Email"])
                            ForEach(group.kinds, id: \.kind) { (option: PingKindOption) in
                                kindRow(model, settings, option, emailOn: emailOn)
                            }
                        }
                    }
                }
            }
        }
    }

    private func kindRow(
        _ model: PingSettingsModel, _ settings: PingSettings, _ option: PingKindOption, emailOn: Bool
    ) -> some View {
        let pingOn: Bool = settings.pingEnabled(option.kind)
        return HStack(spacing: 0) {
            Text(option.label)
                .fmText(.bodyMedium, color: pingOn ? fm.textPrimary : fm.textTertiary)
                .frame(maxWidth: .infinity, alignment: .leading)
            columnSwitch(value: pingOn, id: "ping-kind-\(option.kind)") { (on: Bool) in
                Task { await model.setPingEnabled(option.kind, on) }
            }
            if option.email {
                columnSwitch(
                    value: pingOn && emailOn && settings.emailEnabled(option.kind),
                    enabled: pingOn && emailOn,
                    id: "ping-email-\(option.kind)"
                ) { (on: Bool) in
                    Task { await model.setEmailEnabled(option.kind, on) }
                }
            } else {
                Color.clear.frame(width: PingSettingsScreen.switchColumn, height: 1)
            }
        }
        .frame(minHeight: 44)
    }

    // MARK: Email

    private func emailSection(_ model: PingSettingsModel, _ settings: PingSettings) -> some View {
        let emailOn: Bool = settings.emailNotifications
        return SettingsSection(title: "Email") {
            SettingsCardBox {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    switchRow(
                        "Email me about pings", subtitle: "Sent to your verified address", value: emailOn,
                        id: "ping-email-master"
                    ) { (on: Bool) in
                        Task { await model.setEmailNotifications(on) }
                    }
                    Text("Delivery").fmText(.titleSmall)
                    Picker(
                        "Delivery",
                        selection: Binding(
                            get: { settings.delivery },
                            set: { (next: String) in Task { await model.setDelivery(next) } })
                    ) {
                        Text("Digest").tag("digest")
                        Text("Instant").tag("instant")
                    }
                    .pickerStyle(.segmented)
                    .disabled(!emailOn)
                    .accessibilityIdentifier("ping-email-delivery")
                    SettingsNote(text: model.deliveryNote)
                    switchRow(
                        "#general chat digest", subtitle: "A roundup of the community chat",
                        value: emailOn && settings.emailEnabled("general_chat"), enabled: emailOn,
                        id: "ping-email-general_chat"
                    ) { (on: Bool) in
                        Task { await model.setEmailEnabled("general_chat", on) }
                    }
                }
            }
        }
    }

    // MARK: Push (iPhone)

    @ViewBuilder
    private func pushSection() -> some View {
        let relaySupportsPush: Bool = Services.get(SettingsService.self)?.supports(.push) ?? true
        if let push = push, push.supported, relaySupportsPush {
            SettingsSection(title: "Phone banners") {
                SettingsCardBox {
                    PushNotificationsCard(push: push)
                }
            }
        }
    }

    // MARK: Administrator alerts

    private func adminSection(_ model: PingSettingsModel, _ monitors: [StatusMonitorAlert]) -> some View {
        SettingsSection(title: "Administrator alerts", last: true) {
            SettingsCardBox {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    SettingsNote(
                        text: "Platform-wide /status monitors. Continual repeats a still-down alert every 5 minutes.")
                    columnHeader("Monitor", columns: ["Ping", "Email", "Repeat"])
                    if monitors.isEmpty {
                        SettingsNote(text: "No monitors yet.")
                            .padding(.vertical, FmSpace.x2)
                    }
                    ForEach(monitors) { (monitor: StatusMonitorAlert) in
                        HStack(spacing: 0) {
                            Text(monitor.label)
                                .fmText(.bodyMedium)
                                .frame(maxWidth: .infinity, alignment: .leading)
                            columnSwitch(value: monitor.pings, id: "ping-monitor-\(monitor.id)-pings") { (on: Bool) in
                                Task { await model.setMonitor(monitor.id, "pings", on) }
                            }
                            columnSwitch(value: monitor.emails, id: "ping-monitor-\(monitor.id)-emails") {
                                (on: Bool) in
                                Task { await model.setMonitor(monitor.id, "emails", on) }
                            }
                            columnSwitch(value: monitor.continual, id: "ping-monitor-\(monitor.id)-continual") {
                                (on: Bool) in
                                Task { await model.setMonitor(monitor.id, "continual", on) }
                            }
                        }
                        .frame(minHeight: 44)
                    }
                }
            }
        }
    }
}
