import FMApi
import FMBase
import FMPlanning
import SwiftUI

@MainActor
enum PlanningFormat {
    static func time(_ ms: Int64, zone: TimeZone) -> String {
        let formatter: DateFormatter = DateFormatter()
        formatter.timeZone = zone
        formatter.dateStyle = .none
        formatter.timeStyle = .short
        return formatter.string(from: Date(epochMillis: ms))
    }

    static func day(_ ms: Int64, zone: TimeZone, style: DateFormatter.Style = .full) -> String {
        let formatter: DateFormatter = DateFormatter()
        formatter.timeZone = zone
        formatter.dateStyle = style
        formatter.timeStyle = .none
        return formatter.string(from: Date(epochMillis: ms))
    }

    static func monthTitle(_ ms: Int64, zone: TimeZone) -> String {
        let formatter: DateFormatter = DateFormatter()
        formatter.timeZone = zone
        formatter.setLocalizedDateFormatFromTemplate("MMMM yyyy")
        return formatter.string(from: Date(epochMillis: ms))
    }

    static func dayNumber(_ ms: Int64, zone: TimeZone) -> String {
        "\(CalendarMath.calendar(zone).component(.day, from: Date(epochMillis: ms)))"
    }

    static func span(_ occurrence: CalendarOccurrence, zone: TimeZone) -> String {
        if occurrence.event.allDay {
            return "All day"
        }
        return "\(time(occurrence.startAt, zone: zone)) – \(time(occurrence.endAt, zone: zone))"
    }
}

struct CalendarScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: CalendarModel?
    @State private var editing: CalendarEditTarget?

    init(date: String?, mode: CalendarMode) {
        if let api = PlanningServices.api {
            _model = State(initialValue: CalendarModel(client: api, mode: mode, anchorDay: date))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        Group {
            if let model = model {
                CalendarContent(model: model)
                    .task { await model.load() }
                    .fmRefreshable { await model.load() }
                    .sheet(item: $editing) { (target: CalendarEditTarget) in
                        CalendarEventEditor(model: model, eventId: target.eventId, draft: target.draft)
                    }
                    .toolbar {
                        ToolbarItem(placement: .primaryAction) {
                            Button {
                                editing = CalendarEditTarget(eventId: nil, draft: model.newDraft())
                            } label: {
                                Image(systemName: "plus")
                            }
                            .accessibilityLabel("New event")
                        }
                        ToolbarItem(placement: .primaryAction) {
                            Button("Today") { Task { await model.goToToday() } }
                        }
                    }
            } else {
                FmEmptyState(icon: "calendar_today_outlined", title: "The calendar is unavailable")
            }
        }
        .background(fm.canvas)
        .navigationTitle("Calendar")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct CalendarEditTarget: Identifiable {
    let eventId: String?
    let draft: CalendarEventDraft
    var id: String { eventId ?? "new-\(draft.startAt)" }
}

struct CalendarContent: View {
    @Environment(\.fmPalette) private var fm
    let model: CalendarModel

    private var modeBinding: Binding<CalendarMode> {
        Binding(get: { model.mode }, set: { (mode: CalendarMode) in Task { await model.setMode(mode) } })
    }

    var body: some View {
        List {
            Section {
                Picker("View", selection: modeBinding) {
                    ForEach(CalendarMode.allCases, id: \.self) { (mode: CalendarMode) in
                        Text(mode.label).tag(mode)
                    }
                }
                .pickerStyle(.segmented)
                header
                if let error = model.error {
                    Text(error).font(.caption).foregroundStyle(fm.danger)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                }
            }
            switch model.mode {
            case .month:
                Section {
                    CalendarMonthGrid(model: model)
                }
                daySection(model.anchor)
            case .week:
                ForEach(model.days, id: \.self) { (day: Int64) in
                    daySection(day)
                }
            case .day:
                daySection(model.anchor)
            case .agenda:
                agendaSections
            }
            if model.calendars.count > 1 {
                calendarsSection
            }
        }
        .listStyle(.insetGrouped)
        .overlay {
            if model.loading && model.events.isEmpty {
                ProgressView()
            }
        }
    }

    private var header: some View {
        HStack {
            Button {
                Task { await model.step(-1) }
            } label: {
                Image(systemName: "chevron.left")
            }
            .buttonStyle(.borderless)
            .accessibilityLabel("Previous")
            Spacer()
            Text(model.mode == .month ? PlanningFormat.monthTitle(model.anchor, zone: model.timeZone)
                 : PlanningFormat.day(model.anchor, zone: model.timeZone, style: .medium))
                .font(.headline)
            Spacer()
            Button {
                Task { await model.step(1) }
            } label: {
                Image(systemName: "chevron.right")
            }
            .buttonStyle(.borderless)
            .accessibilityLabel("Next")
        }
    }

    private func daySection(_ day: Int64) -> some View {
        let items: [CalendarOccurrence] = model.occurrences(onDay: day)
        return Section(PlanningFormat.day(day, zone: model.timeZone)) {
            if items.isEmpty {
                Text("No events").foregroundStyle(fm.textSecondary)
            }
            ForEach(items) { (occurrence: CalendarOccurrence) in
                occurrenceRow(occurrence)
            }
        }
    }

    private var agendaSections: some View {
        ForEach(model.agenda) { (group: CalendarAgendaDay) in
            Section(group.day) {
                ForEach(group.items) { (occurrence: CalendarOccurrence) in
                    occurrenceRow(occurrence)
                }
            }
        }
    }

    private func occurrenceRow(_ occurrence: CalendarOccurrence) -> some View {
        ShellPaneLink {
            CalendarEventScreen(model: model, eventId: occurrence.event.id)
                .shellPageClearance()
        } label: {
            HStack(alignment: .top, spacing: 10) {
                RoundedRectangle(cornerRadius: 2)
                    .fill(occurrence.event.scope == "organization" ? fm.warning : fm.accent)
                    .frame(width: 4)
                VStack(alignment: .leading, spacing: 2) {
                    Text(occurrence.event.title)
                    Text(PlanningFormat.span(occurrence, zone: model.timeZone))
                        .font(.caption)
                        .foregroundStyle(fm.textSecondary)
                    if !occurrence.event.location.isEmpty {
                        Text(occurrence.event.location).font(.caption).foregroundStyle(fm.textTertiary)
                    }
                }
            }
        }
    }

    private var calendarsSection: some View {
        Section("Calendars") {
            ForEach(model.calendars) { (info: CalendarInfo) in
                Toggle(isOn: Binding(
                    get: { !model.hiddenCalendars.contains(info.id) },
                    set: { (on: Bool) in
                        if on {
                            model.hiddenCalendars.remove(info.id)
                        } else {
                            model.hiddenCalendars.insert(info.id)
                        }
                    }
                )) {
                    Text(info.id == "personal" ? "Personal" : info.name)
                }
            }
        }
    }
}

struct CalendarMonthGrid: View {
    @Environment(\.fmPalette) private var fm
    let model: CalendarModel

    private let columns: [GridItem] = Array(repeating: GridItem(.flexible(), spacing: 2), count: 7)

    var body: some View {
        let today: Int64 = model.today
        let selected: Int64 = CalendarMath.startOfDay(model.anchor, timeZone: model.timeZone)
        LazyVGrid(columns: columns, spacing: 4) {
            ForEach(model.days, id: \.self) { (day: Int64) in
                let count: Int = model.occurrences(onDay: day).count
                Button {
                    model.select(day: day)
                } label: {
                    VStack(spacing: 2) {
                        Text(PlanningFormat.dayNumber(day, zone: model.timeZone))
                            .font(.callout.weight(day == today ? .bold : .regular))
                            .foregroundStyle(model.isInAnchorMonth(day) ? fm.textPrimary : fm.textTertiary)
                        Circle()
                            .fill(count > 0 ? fm.accent : Color.clear)
                            .frame(width: 5, height: 5)
                    }
                    .frame(maxWidth: .infinity, minHeight: 40)
                    .background(day == selected ? fm.accentSubtle : Color.clear, in: RoundedRectangle(cornerRadius: 8))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("\(PlanningFormat.day(day, zone: model.timeZone)), \(count) events")
            }
        }
        .padding(.vertical, 4)
    }
}

struct CalendarEventScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let model: CalendarModel
    let eventId: String
    @State private var editing: CalendarEditTarget?
    @State private var confirmDelete: Bool = false

    private var event: CalendarEvent? { model.event(eventId) }

    var body: some View {
        List {
            if let event = event {
                Section {
                    Text(event.title).font(.title3.weight(.semibold))
                    FmKeyValueRow(label: "Starts", value: event.allDay
                                  ? PlanningFormat.day(event.startAt, zone: model.timeZone)
                                  : "\(PlanningFormat.day(event.startAt, zone: model.timeZone, style: .medium)) \(PlanningFormat.time(event.startAt, zone: model.timeZone))")
                    FmKeyValueRow(label: "Ends", value: "\(PlanningFormat.day(event.endAt, zone: model.timeZone, style: .medium)) \(PlanningFormat.time(event.endAt, zone: model.timeZone))")
                    if event.recurrence != .none {
                        FmKeyValueRow(label: "Repeats", value: event.recurrence.label)
                    }
                    FmKeyValueRow(label: "Calendar", value: event.calendar == "personal" ? "Personal" : event.calendar)
                    if !event.location.isEmpty {
                        FmKeyValueRow(label: "Location", value: event.location)
                    }
                    if let link = URL(string: event.meetingUrl), !event.meetingUrl.isEmpty {
                        Link("Join meeting", destination: link)
                    }
                }
                if !event.description.isEmpty {
                    Section("Notes") {
                        Text(event.description).textSelection(.enabled)
                    }
                }
                if !event.attendees.isEmpty {
                    Section("People") {
                        ForEach(event.attendees, id: \.name) { (attendee: CalendarAttendee) in
                            HStack {
                                Text("@\(attendee.name)")
                                Spacer()
                                Text(attendee.status.capitalized).font(.caption).foregroundStyle(fm.textSecondary)
                            }
                        }
                    }
                }
                if event.attendeeStatus(model.actor) != nil {
                    Section("Your answer") {
                        HStack {
                            ForEach(["accepted", "tentative", "declined"], id: \.self) { (status: String) in
                                Button(status == "accepted" ? "Going" : status == "tentative" ? "Maybe" : "No") {
                                    Task { await model.rsvp(eventId, status: status) }
                                }
                                .buttonStyle(.bordered)
                                .tint(event.attendeeStatus(model.actor) == status ? fm.accent : fm.textSecondary)
                            }
                        }
                    }
                }
                if model.canEdit(event) {
                    Section {
                        Button("Edit") {
                            editing = CalendarEditTarget(eventId: eventId, draft: CalendarEventDraft(event: event))
                        }
                        Button("Delete", role: .destructive) { confirmDelete = true }
                    }
                }
            } else {
                Text("This event is not in the loaded range.").foregroundStyle(fm.textSecondary)
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle("Event")
        .navigationBarTitleDisplayMode(.inline)
        .sheet(item: $editing) { (target: CalendarEditTarget) in
            CalendarEventEditor(model: model, eventId: target.eventId, draft: target.draft)
        }
        .confirmationDialog("Delete this event?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                Task {
                    if await model.delete(eventId) {
                        dismiss()
                    }
                }
            }
        }
    }
}

struct CalendarEventEditor: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let model: CalendarModel
    let eventId: String?
    @State private var draft: CalendarEventDraft
    @State private var attendees: String
    @State private var tried: Bool = false
    @State private var problemShown: Int = 0

    init(model: CalendarModel, eventId: String?, draft: CalendarEventDraft) {
        self.model = model
        self.eventId = eventId
        _draft = State(initialValue: draft)
        _attendees = State(initialValue: draft.attendees.joined(separator: ", "))
    }

    private func dateBinding(_ keyPath: WritableKeyPath<CalendarEventDraft, Int64>) -> Binding<Date> {
        Binding(
            get: { Date(epochMillis: draft[keyPath: keyPath]) },
            set: { (date: Date) in
                let old: Int64 = draft[keyPath: keyPath]
                draft[keyPath: keyPath] = date.epochMillis
                if keyPath == \CalendarEventDraft.startAt {
                    draft.endAt += date.epochMillis - old
                }
            })
    }

    var body: some View {
        NavigationStack {
            ScrollViewReader { (proxy: ScrollViewProxy) in
                Form {
                    if let problem = tried ? draft.validationError ?? model.error : nil {
                        Section {
                            Text(problem).foregroundStyle(fm.danger)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .fmMessageTools(problem)
                                .id("calendar-editor-problem")
                        }
                    }
                    Section {
                        TextField("Title", text: $draft.title)
                        Toggle("All day", isOn: $draft.allDay)
                        DatePicker("Starts", selection: dateBinding(\.startAt),
                                   displayedComponents: draft.allDay ? [.date] : [.date, .hourAndMinute])
                        DatePicker("Ends", selection: dateBinding(\.endAt),
                                   displayedComponents: draft.allDay ? [.date] : [.date, .hourAndMinute])
                        Picker("Repeat", selection: $draft.recurrence) {
                            ForEach(CalendarRecurrence.allCases, id: \.self) { (value: CalendarRecurrence) in
                                Text(value.label).tag(value)
                            }
                        }
                    }
                    Section {
                        Picker("Calendar", selection: $draft.calendar) {
                            ForEach(model.calendars) { (info: CalendarInfo) in
                                Text(info.id == "personal" ? "Personal" : info.name).tag(info.id == "personal" ? "personal" : info.name)
                            }
                        }
                        TextField("Location", text: $draft.location)
                        TextField("Meeting link", text: $draft.meetingUrl)
                            .keyboardType(.URL)
                            .textInputAutocapitalization(.never)
                        TextField("Invite (names, comma separated)", text: $attendees)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                    }
                    Section("Reminders") {
                        ForEach(CalendarEventDraft.allowedReminders, id: \.self) { (minutes: Int) in
                            Toggle(CalendarEventDraft.reminderLabel(minutes), isOn: Binding(
                                get: { draft.reminders.contains(minutes) },
                                set: { (on: Bool) in
                                    if on {
                                        draft.reminders.append(minutes)
                                    } else {
                                        draft.reminders.removeAll { $0 == minutes }
                                    }
                                }
                            ))
                        }
                    }
                    Section("Notes") {
                        TextEditor(text: $draft.description).frame(minHeight: 100)
                    }
                }
                .onChange(of: problemShown) { (_: Int, _: Int) in
                    withAnimation { proxy.scrollTo("calendar-editor-problem", anchor: .top) }
                }
            }
            .navigationTitle(eventId == nil ? "New event" : "Edit event")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    if model.busy {
                        ProgressView()
                    } else {
                        Button("Save") { save() }
                            .disabled(draft.title.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    }
                }
            }
        }
    }

    private func save() {
        draft.attendees = attendees.split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
        model.clearError()
        tried = true
        guard draft.validationError == nil else {
            FmHaptics.warning()
            problemShown += 1
            return
        }
        let pending: CalendarEventDraft = draft
        Task {
            let saved: CalendarEvent?
            if let id = eventId {
                saved = await model.update(id, pending)
            } else {
                saved = await model.create(pending)
            }
            if saved != nil {
                FmHaptics.commit()
                dismiss()
            } else {
                FmHaptics.warning()
                problemShown += 1
            }
        }
    }
}

struct CalendarEventDestination: View {
    let eventId: String
    @State private var model: CalendarModel?

    init(eventId: String) {
        self.eventId = eventId
        if let api = PlanningServices.api {
            _model = State(initialValue: CalendarModel(client: api, mode: .agenda))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        if let model = model {
            CalendarEventScreen(model: model, eventId: eventId)
                .task { await model.loadEvent(eventId) }
        } else {
            FmEmptyState(icon: "calendar_today_outlined", title: "The calendar is unavailable")
        }
    }
}
