import FMApi
import FMBase
import FMPackages
import SwiftUI

struct BookingSettingsScreen: View {
    let model: BookingModel
    let overview: BookingOverview
    @State private var draft: BookingDraft
    @State private var slugCheck: BookingSlugCheck?
    @State private var errorText = ""
    @Environment(\.dismiss) private var dismiss

    init(model: BookingModel, overview: BookingOverview) {
        self.model = model
        self.overview = overview
        _draft = State(initialValue: BookingDraft(overview: overview))
    }

    private var options: BookingOptions { overview.options }

    private var zones: [String] {
        var list = TimeZone.knownTimeZoneIdentifiers
        for extra in [draft.timezone, TimeZone.current.identifier, "UTC"] where !list.contains(extra) { list.append(extra) }
        return list.sorted()
    }

    var body: some View {
        Form {
            pageSection
            hoursSection
            weekSection
            typesSection
            emailSection
            if !errorText.isEmpty {
                Section { PackagesErrorBanner(text: errorText) }
            }
        }
        .disabled(model.working)
        .navigationTitle("Booking settings")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") { Task { await save() } }
                    .disabled(model.working || draft.types.isEmpty)
                    .accessibilityIdentifier("booking-settings-save")
            }
        }
        .task(id: draft.slug) { await checkSlug() }
    }

    private var pageSection: some View {
        Section {
            LabeledContent("Link") {
                HStack(spacing: 0) {
                    Text(overview.linkPrefix).foregroundStyle(.secondary).lineLimit(1)
                    TextField("your-name", text: $draft.slug)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .keyboardType(.URL)
                }
            }
            if let slugCheck, slugCheck.slug == draft.slug.trimmingCharacters(in: .whitespaces).lowercased() {
                Text(slugCheck.message).font(.caption).foregroundStyle(slugCheck.available ? Color.green : Color.red)
            }
            TextField("Name shown", text: $draft.displayName)
            TextField("Headline", text: $draft.headline)
            TextField("Welcome message", text: $draft.welcome, axis: .vertical).lineLimit(2...6)
            LabeledContent("Colour") {
                HStack(spacing: 10) {
                    ForEach(options.accents) { accent in
                        Button {
                            draft.accent = accent.id
                        } label: {
                            Circle()
                                .fill(swatch(accent))
                                .frame(width: 24, height: 24)
                                .overlay {
                                    if draft.accent == accent.id {
                                        Circle().strokeBorder(Color.accentColor, lineWidth: 2).padding(-4)
                                    }
                                }
                        }
                        .buttonStyle(.borderless)
                        .accessibilityLabel(BookingText.accentName(accent.id))
                        .accessibilityAddTraits(draft.accent == accent.id ? .isSelected : [])
                    }
                }
            }
            Picker("Clock", selection: $draft.timeFormat) {
                Text("12-hour (2:30 PM)").tag("12h")
                Text("24-hour (14:30)").tag("24h")
            }
        } header: {
            Text("Page")
        }
    }

    private var hoursSection: some View {
        Section("Hours") {
            Picker("Time zone", selection: $draft.timezone) {
                ForEach(zones, id: \.self) { Text($0.replacingOccurrences(of: "_", with: " ")).tag($0) }
            }
            .pickerStyle(.navigationLink)
            Picker("Shortest notice", selection: $draft.minNoticeMinutes) {
                ForEach(options.notices, id: \.self) { Text(BookingText.noticeLabel($0)).tag($0) }
            }
            Picker("Book up to", selection: $draft.maxDaysAhead) {
                ForEach(options.horizons, id: \.self) { Text(BookingText.horizonLabel($0)).tag($0) }
            }
            Picker("Gap between meetings", selection: $draft.bufferMinutes) {
                ForEach(options.buffers, id: \.self) { Text(BookingText.bufferLabel($0)).tag($0) }
            }
            Picker("Start times every", selection: $draft.slotStepMinutes) {
                ForEach(options.steps, id: \.self) { Text(BookingText.stepLabel($0)).tag($0) }
            }
            Stepper(value: $draft.dailyLimit, in: 0...50) {
                LabeledContent("Most meetings a day", value: BookingText.dailyLimitLabel(draft.dailyLimit))
            }
        }
    }

    private var weekSection: some View {
        Section {
            ForEach(BookingText.weekdays) { day in
                Toggle(day.name, isOn: Binding(get: { draft.isOn(day.id) }, set: { draft.setDay(day.id, on: $0) }))
                    .accessibilityIdentifier("booking-day-\(day.id)")
                if draft.isOn(day.id) {
                    ForEach(Array(draft.windows(day.id).enumerated()), id: \.offset) { index, window in
                        HStack(spacing: 8) {
                            DatePicker("From", selection: timeBinding(day.id, index, start: true, value: window.start), displayedComponents: .hourAndMinute)
                                .labelsHidden()
                            Text("–").foregroundStyle(.secondary)
                            DatePicker("Until", selection: timeBinding(day.id, index, start: false, value: window.end), displayedComponents: .hourAndMinute)
                                .labelsHidden()
                            Spacer()
                            Button(role: .destructive) {
                                draft.removeWindow(day.id, at: index)
                            } label: {
                                Image(systemName: "minus.circle.fill")
                            }
                            .buttonStyle(.borderless)
                            .accessibilityLabel("Remove these hours")
                        }
                        .environment(\.timeZone, TimeZone(identifier: "UTC") ?? .current)
                        .padding(.leading, 16)
                    }
                    Button("Add hours", systemImage: "plus") { draft.addWindow(day.id) }
                        .buttonStyle(.borderless)
                        .padding(.leading, 16)
                }
            }
            Toggle("Never offer times that clash with my ForkMesh calendar", isOn: $draft.checkCalendar)
        } header: {
            Text("Weekly hours")
        }
    }

    private var typesSection: some View {
        Section {
            ForEach(Array(draft.types.enumerated()), id: \.offset) { index, type in
                NavigationLink {
                    BookingTypeEditor(
                        initial: type, options: options,
                        onChange: { updated in
                            if draft.types.indices.contains(index) { draft.types[index] = updated }
                        },
                        onRemove: { draft.removeType(at: index) })
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(type.title.isEmpty ? "Untitled" : type.title)
                        Text(
                            BookingText.minutesLabel(type.minutes) + " · " + BookingText.locationLabel(type.location.kind)
                                + (type.enabled ? "" : " · hidden")
                        )
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    }
                }
            }
            .onDelete { draft.types.remove(atOffsets: $0) }
            Button("Add meeting type", systemImage: "plus") { draft.addType() }
        } header: {
            Text("Meeting types")
        } footer: {
            if draft.types.isEmpty { Text(BookingText.errorSentence("meeting_type_required")) }
        }
    }

    private var emailSection: some View {
        Section {
            TextField("Guests reply to", text: $draft.replyTo, prompt: Text("you@example.com"))
                .keyboardType(.emailAddress).textInputAutocapitalization(.never).autocorrectionDisabled()
            TextField("Send my booking emails to", text: $draft.notifyEmail, prompt: Text("you@example.com"))
                .keyboardType(.emailAddress).textInputAutocapitalization(.never).autocorrectionDisabled()
            Toggle("Email me when someone books, moves or cancels", isOn: $draft.emailOwner)
            Toggle("Ping me when someone opens my page or starts picking a time", isOn: $draft.notifyVisits)
            Toggle("Add booked meetings to my ForkMesh calendar", isOn: $draft.addToCalendar)
        } header: {
            Text("Email and calendar")
        } footer: {
            Text(
                "Confirmations come from ForkMesh; replies go to the reply address. You always get a ForkMesh notification. Guests always get a confirmation, a reminder the day before, and an email when the meeting moves or is cancelled."
            )
        }
    }

    private func swatch(_ accent: BookingAccent) -> Color {
        guard let rgb = accent.rgb else { return .secondary }
        return Color(red: rgb.red, green: rgb.green, blue: rgb.blue)
    }

    private func timeBinding(_ day: String, _ index: Int, start: Bool, value: String) -> Binding<Date> {
        Binding(
            get: { Date(timeIntervalSince1970: Double((BookingDraft.minutes(value) ?? 0) % 1440) * 60) },
            set: { date in
                let minutes = (Int(date.timeIntervalSince1970 / 60) % 1440 + 1440) % 1440
                let clock = BookingDraft.clock(minutes)
                if start { draft.setWindow(day, at: index, start: clock) } else { draft.setWindow(day, at: index, end: clock) }
            })
    }

    private func checkSlug() async {
        let slug = draft.slug.trimmingCharacters(in: .whitespaces).lowercased()
        guard slug != overview.slug, !slug.isEmpty else {
            slugCheck = nil
            return
        }
        try? await Task.sleep(for: .milliseconds(350))
        guard !Task.isCancelled else { return }
        slugCheck = try? await model.client.checkBookingSlug(slug)
    }

    private func save() async {
        if await model.save(draft.body, notice: "Booking settings saved.") {
            FmHaptics.commit()
            dismiss()
        } else {
            FmHaptics.failure()
            errorText = model.errorText
        }
    }
}

struct BookingTypeEditor: View {
    let options: BookingOptions
    let onChange: (BookingType) -> Void
    let onRemove: () -> Void
    @State private var type: BookingType
    @Environment(\.dismiss) private var dismiss

    init(initial: BookingType, options: BookingOptions, onChange: @escaping (BookingType) -> Void, onRemove: @escaping () -> Void) {
        self.options = options
        self.onChange = onChange
        self.onRemove = onRemove
        _type = State(initialValue: initial)
    }

    private var durations: [Int] {
        options.durations.contains(type.minutes) ? options.durations : (options.durations + [type.minutes]).sorted()
    }

    var body: some View {
        Form {
            Section {
                TextField("Name", text: $type.title)
                Picker("Length", selection: $type.minutes) {
                    ForEach(durations, id: \.self) { Text(BookingText.minutesLabel($0)).tag($0) }
                }
                TextField("Description", text: $type.description, axis: .vertical).lineLimit(2...5)
            }
            Section {
                Picker("Where", selection: $type.location.kind) {
                    ForEach(options.locations, id: \.self) { Text(BookingText.locationLabel($0)).tag($0) }
                }
                if type.location.kind != "none" {
                    TextField("Details", text: $type.location.detail, prompt: Text(BookingText.locationHint(type.location.kind)))
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .keyboardType(type.location.kind == "video" ? .URL : .default)
                }
                Toggle("Show on my page", isOn: $type.enabled)
            }
            Section {
                Button("Remove", role: .destructive) {
                    dismiss()
                    onRemove()
                }
            }
        }
        .navigationTitle(type.title.isEmpty ? "Meeting type" : type.title)
        .navigationBarTitleDisplayMode(.inline)
        .onChange(of: type) { _, updated in onChange(updated) }
    }
}
