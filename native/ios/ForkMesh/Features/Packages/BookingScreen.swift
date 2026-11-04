import FMApi
import FMBase
import FMPackages
import SwiftUI
import UIKit

struct BookingDestination: View {
    let booking: String?

    var body: some View {
        PackagesGate(feature: "Booking") { client in
            BookingScreen(client: client, focus: booking)
        }
    }
}

enum BookingTab: String, CaseIterable, Identifiable {
    case upcoming, past

    var id: String { rawValue }

    var title: String { self == .upcoming ? "Upcoming" : "Past & cancelled" }
}

struct BookingShare: Identifiable {
    let id = UUID()
    let items: [Any]
}

struct BookingScreen: View {
    @State private var model: BookingModel
    @State private var focus: String?
    @State private var tab: BookingTab = .upcoming
    @State private var notice: FmNotice?
    @State private var cancelling: Booking?
    @State private var showSettings = false
    @State private var share: BookingShare?
    @State private var confirmUninstall = false
    @State private var slug = ""
    @Environment(\.openURL) private var openURL

    init(client: PackagesClient, focus: String?) {
        _model = State(initialValue: BookingModel(client: client))
        _focus = State(initialValue: focus)
    }

    private var now: Int64 { Int64(Date().timeIntervalSince1970 * 1000) }

    var body: some View {
        ScrollViewReader { proxy in
            List {
                Section {
                    PackagesErrorBanner(text: model.errorText) { await model.refresh() }
                    if notice != nil { FmNoticeLine($notice) }
                }
                if let data = model.overview, data.installed {
                    linkSection(data)
                    statsSection(data.stats)
                    PageActivitySection(activity: data.activity, now: now)
                    Section {
                        Picker("Show", selection: $tab) {
                            ForEach(BookingTab.allCases) { Text($0.title).tag($0) }
                        }
                        .pickerStyle(.segmented)
                        .accessibilityIdentifier("booking-tabs")
                    }
                    .listRowBackground(Color.clear)
                    .listRowInsets(EdgeInsets(top: 0, leading: 0, bottom: 0, trailing: 0))
                    bookingSections(data, tab: tab)
                } else if model.loaded && !model.loading, let data = model.overview {
                    installSection(data)
                }
            }
            .navigationTitle("Booking")
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Button("Booking settings", systemImage: "gearshape") { showSettings = true }
                        .disabled(!model.installed)
                        .accessibilityIdentifier("booking-settings")
                }
                ToolbarItem(placement: .secondaryAction) {
                    Button("Export my data", systemImage: "square.and.arrow.down") { Task { await exportData() } }
                        .disabled(!model.installed || model.working)
                }
                ToolbarItem(placement: .secondaryAction) {
                    Button("Uninstall…", systemImage: "trash", role: .destructive) { confirmUninstall = true }
                        .disabled(!model.installed || model.working)
                }
            }
            .overlay {
                if model.loading && model.overview == nil { ProgressView() }
            }
            .fmRefreshable { await model.refresh() }
            .task {
                await model.refresh()
                if slug.isEmpty { slug = model.overview?.suggestedSlug ?? "" }
                await openFocus(proxy)
            }
            .sheet(item: $cancelling) { booking in
                NavigationStack {
                    BookingCancelSheet(model: model, booking: booking, timeFormat: model.overview?.settings.timeFormat ?? "12h")
                }
            }
            .sheet(isPresented: $showSettings) {
                if let data = model.overview {
                    NavigationStack {
                        BookingSettingsScreen(model: model, overview: data)
                    }
                }
            }
            .sheet(item: $share) { item in
                ActivitySheet(items: item.items)
            }
            .confirmationDialog(BookingText.uninstallConfirm, isPresented: $confirmUninstall, titleVisibility: .visible) {
                Button("Uninstall", role: .destructive) { Task { await uninstall() } }
                Button("Keep Booking", role: .cancel) {}
            }
            .onChange(of: model.notice) { _, text in
                guard !text.isEmpty else { return }
                notice = FmNotice(text)
                model.notice = ""
            }
        }
    }

    private func openFocus(_ proxy: ScrollViewProxy) async {
        guard let id = focus, let data = model.overview, data.installed else { return }
        if data.past.contains(where: { $0.id == id }) { tab = .past }
        guard data.find(id) != nil else {
            focus = nil
            return
        }
        try? await Task.sleep(for: .milliseconds(250))
        withAnimation { proxy.scrollTo(id, anchor: .center) }
    }

    @ViewBuilder private func installSection(_ data: BookingOverview) -> some View {
        Section {
            ContentUnavailableView {
                Label(BookingText.installTitle, systemImage: "calendar.badge.clock")
            } description: {
                Text(BookingText.installMessage)
            } actions: {
                VStack(spacing: 12) {
                    HStack(spacing: 0) {
                        Text(data.linkPrefix).foregroundStyle(.secondary)
                        TextField("your-name", text: $slug)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .keyboardType(.URL)
                            .accessibilityIdentifier("booking-install-slug")
                    }
                    .font(.callout)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 10)
                    .background(.quaternary, in: RoundedRectangle(cornerRadius: 10))
                    Button {
                        Task {
                            if await model.install(slug: slug) { FmHaptics.commit() } else { FmHaptics.failure() }
                        }
                    } label: {
                        if model.working { ProgressView() } else { Text("Create booking page") }
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.green)
                    .disabled(model.working || !BookingText.isSlug(slug.trimmingCharacters(in: .whitespaces).lowercased()))
                    .accessibilityIdentifier("booking-install")
                }
            }
        }
    }

    @ViewBuilder private func linkSection(_ data: BookingOverview) -> some View {
        Section {
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 8) {
                    Text("Your booking link").font(.caption).foregroundStyle(.secondary)
                    BookingBadge(text: data.enabled ? "Live" : "Paused", tint: data.enabled ? .green : .orange)
                }
                Text(data.displayLink)
                    .font(.headline)
                    .textSelection(.enabled)
                    .lineLimit(2)
                    .accessibilityIdentifier("booking-link")
                HStack(spacing: 8) {
                    Button("Copy", systemImage: "doc.on.doc") { copy(data.link) }
                    if let url = URL(string: data.link) {
                        ShareLink(item: url) { Label("Share", systemImage: "square.and.arrow.up") }
                        Button("Open", systemImage: "safari") { openURL(url) }
                    }
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .labelStyle(.titleAndIcon)
                .padding(.top, 4)
                Button {
                    Task {
                        if await model.setEnabled(!data.enabled) { FmHaptics.commit() } else { FmHaptics.failure() }
                    }
                } label: {
                    Label(data.enabled ? "Pause page" : "Turn page on", systemImage: data.enabled ? "pause.circle" : "play.circle")
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .disabled(model.working)
                .accessibilityIdentifier("booking-toggle")
            }
            .padding(.vertical, 4)
            ForEach(data.settings.types) { type in
                BookingTypeLinkRow(type: type, link: data.link(for: type)) { copy($0) }
            }
        }
    }

    @ViewBuilder private func statsSection(_ stats: BookingStats) -> some View {
        Section {
            LazyVGrid(columns: [GridItem(.flexible(), spacing: 8), GridItem(.flexible(), spacing: 8)], spacing: 8) {
                InvoiceSummaryTile(value: "\(stats.upcoming)", label: "Upcoming", detail: "", icon: "calendar", tint: .blue)
                InvoiceSummaryTile(
                    value: "\(stats.next7Days)", label: "Next 7 days", detail: stats.hoursBooked, icon: "calendar.badge.clock",
                    tint: .teal)
                InvoiceSummaryTile(
                    value: "\(stats.booked30Days)", label: "Booked, last 30 days", detail: "", icon: "checkmark.seal", tint: .green)
                InvoiceSummaryTile(
                    value: "\(stats.cancelled30Days)", label: "Cancelled, last 30 days", detail: "", icon: "xmark.circle",
                    tint: stats.cancelled30Days > 0 ? .red : .secondary)
            }
            .listRowInsets(EdgeInsets(top: 4, leading: 0, bottom: 4, trailing: 0))
            .listRowBackground(Color.clear)
        }
    }

    @ViewBuilder private func bookingSections(_ data: BookingOverview, tab: BookingTab) -> some View {
        let list = tab == .upcoming ? data.upcoming : data.past
        let days = BookingText.days(list, timeZone: .current)
        if days.isEmpty {
            Section {
                ContentUnavailableView {
                    Label(
                        tab == .upcoming ? BookingText.upcomingEmptyTitle : BookingText.pastEmptyTitle,
                        systemImage: tab == .upcoming ? "calendar.badge.checkmark" : "clock.arrow.circlepath")
                } description: {
                    Text(tab == .upcoming ? BookingText.upcomingEmptyMessage : BookingText.pastEmptyMessage)
                }
            }
        }
        ForEach(days) { day in
            Section(day.title) {
                ForEach(day.bookings) { booking in
                    BookingRow(booking: booking, timeFormat: data.settings.timeFormat, ownerZone: data.settings.timezone, now: now)
                        .id(booking.id)
                        .listRowBackground(focus == booking.id ? Color.accentColor.opacity(0.12) : nil)
                        .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                            if booking.canCancel(now: now) {
                                Button("Cancel", role: .destructive) { cancelling = booking }
                            }
                        }
                        .contextMenu {
                            if !booking.guest.email.isEmpty {
                                Button("Copy email", systemImage: "doc.on.doc") { copy(booking.guest.email) }
                            }
                            if booking.canCancel(now: now) {
                                Button("Cancel", systemImage: "xmark.circle", role: .destructive) { cancelling = booking }
                            }
                        }
                }
            }
        }
    }

    private func copy(_ text: String) {
        guard !text.isEmpty else { return }
        UIPasteboard.general.string = text
        FmHaptics.commit()
        notice = FmNotice("Link copied.")
    }

    private func exportFile() async -> URL? {
        guard let export = await model.export() else { return nil }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(export.filename)
        do {
            try Data(export.json.utf8).write(to: url, options: .atomic)
            return url
        } catch {
            model.errorText = "Could not export: " + error.localizedDescription
            return nil
        }
    }

    private func exportData() async {
        if let url = await exportFile() { share = BookingShare(items: [url]) }
    }

    private func uninstall() async {
        guard let url = await exportFile() else { return }
        if await model.uninstall() {
            FmHaptics.commit()
            slug = model.overview?.suggestedSlug ?? ""
            share = BookingShare(items: [url])
        } else {
            FmHaptics.failure()
        }
    }
}

struct PageActivitySection: View {
    let activity: PageActivity
    let now: Int64

    var body: some View {
        Section("Visits") {
            Text(activity.summary(now: now))
                .font(.subheadline)
                .foregroundStyle(activity.isEmpty ? .secondary : .primary)
                .accessibilityIdentifier("page-activity")
            ForEach(Array(activity.newest.enumerated()), id: \.offset) { _, visit in
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Text(visit.label).font(.subheadline).lineLimit(2)
                    Spacer(minLength: 8)
                    Text(visit.when(now: now)).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                }
            }
        }
    }
}

struct BookingBadge: View {
    let text: String
    let tint: Color

    var body: some View {
        Text(text)
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 7)
            .padding(.vertical, 2)
            .foregroundStyle(tint)
            .background(tint.opacity(0.14), in: Capsule())
    }
}

struct BookingTypeLinkRow: View {
    let type: BookingType
    let link: String
    let onCopy: (String) -> Void

    var body: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(type.title).font(.subheadline.weight(.medium))
                Text(BookingText.minutesLabel(type.minutes) + (type.enabled ? "" : " · hidden"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer(minLength: 8)
            if !link.isEmpty {
                Button {
                    onCopy(link)
                } label: {
                    Image(systemName: "doc.on.doc")
                }
                .buttonStyle(.borderless)
                .accessibilityLabel("Copy the link for \(type.title)")
                if let url = URL(string: link) {
                    ShareLink(item: url) { Image(systemName: "square.and.arrow.up") }
                        .buttonStyle(.borderless)
                        .accessibilityLabel("Share the link for \(type.title)")
                }
            }
        }
    }
}

struct BookingRow: View {
    let booking: Booking
    let timeFormat: String
    let ownerZone: String
    let now: Int64

    private func tint(_ badge: String) -> Color {
        if badge.hasPrefix("Cancelled") { return .red }
        if badge == "Moved" { return .blue }
        return .secondary
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline, spacing: 6) {
                Text(BookingText.timeRange(booking, timeFormat: timeFormat, timeZone: .current))
                    .font(.subheadline.weight(.semibold).monospacedDigit())
                Text(BookingText.minutesLabel(booking.minutes)).font(.caption).foregroundStyle(.secondary)
                Spacer(minLength: 4)
                ForEach(booking.badges(now: now), id: \.self) { BookingBadge(text: $0, tint: tint($0)) }
            }
            HStack(alignment: .firstTextBaseline, spacing: 6) {
                Text(booking.guest.name.isEmpty ? "Guest" : booking.guest.name).font(.body.weight(.semibold))
                Text(booking.typeTitle).font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
            }
            if !booking.guest.email.isEmpty, let mail = URL(string: "mailto:" + booking.guest.email) {
                Link(booking.guest.email, destination: mail).font(.footnote)
            }
            if !booking.guest.phone.isEmpty, booking.location.kind != "phone" || !booking.location.detail.isEmpty {
                Label(booking.guest.phone, systemImage: "phone").font(.footnote).foregroundStyle(.secondary)
            }
            if !booking.whereText.isEmpty {
                Label(booking.whereText, systemImage: placeSymbol).font(.footnote).foregroundStyle(.secondary).lineLimit(2)
            }
            if let join = booking.joinURL {
                Link(destination: join) { Label("Join video call", systemImage: "video.fill") }.font(.footnote.weight(.semibold))
            }
            if !booking.guest.timezone.isEmpty, booking.guest.timezone != ownerZone {
                Text("Their time zone: " + booking.guest.timezone.replacingOccurrences(of: "_", with: " "))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            if !booking.guest.notes.isEmpty {
                Text(booking.guest.notes).font(.footnote).fixedSize(horizontal: false, vertical: true)
            }
            if booking.isCancelled, !booking.cancelReason.isEmpty {
                Text("“\(booking.cancelReason)”").font(.footnote).italic().foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 2)
        .accessibilityElement(children: .combine)
    }

    private var placeSymbol: String {
        switch booking.location.kind {
        case "video": return "video"
        case "phone": return "phone"
        case "place": return "mappin.and.ellipse"
        default: return "text.alignleft"
        }
    }
}

struct BookingCancelSheet: View {
    let model: BookingModel
    let booking: Booking
    let timeFormat: String
    @State private var reason = ""
    @State private var notifyGuest = true
    @State private var errorText = ""
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        Form {
            Section {
                Text(
                    "\(booking.typeTitle) with \(booking.guest.name.isEmpty ? "your guest" : booking.guest.name), "
                        + BookingText.dayTitle(booking.startAt, timeZone: .current) + " at "
                        + BookingText.clock(booking.startAt, timeFormat: timeFormat, timeZone: .current) + ".")
            }
            Section("Message to your guest") {
                TextField("Sorry, something came up.", text: $reason, axis: .vertical).lineLimit(2...5)
                Toggle("Email the guest that it is cancelled", isOn: $notifyGuest)
            }
            if !errorText.isEmpty {
                Section { PackagesErrorBanner(text: errorText) }
            }
        }
        .disabled(model.working)
        .navigationTitle("Cancel this meeting?")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Keep it") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Cancel meeting", role: .destructive) {
                    Task {
                        if await model.cancel(booking, reason: reason, notifyGuest: notifyGuest) {
                            FmHaptics.commit()
                            dismiss()
                        } else {
                            FmHaptics.failure()
                            errorText = model.errorText
                        }
                    }
                }
                .tint(.red)
                .disabled(model.working)
                .accessibilityIdentifier("booking-cancel-confirm")
            }
        }
    }
}
