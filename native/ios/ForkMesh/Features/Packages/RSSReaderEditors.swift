import FMPackages
import SwiftUI

struct RSSSmartFeedEditor: View {
    let model: RSSReaderModel
    @Environment(\.dismiss) private var dismiss
    @State private var draft: RSSSmartFeed
    @State private var error = ""

    static let ages: [Int] = [0, 1, 3, 7, 14, 30, 90, 365]

    init(model: RSSReaderModel, draft: RSSSmartFeed) {
        self.model = model
        _draft = State(initialValue: draft)
    }

    private var invalidRegex: Bool {
        draft.regex && !draft.words.isEmpty && !RSSMatcher.isValidRegex(draft.words)
    }

    private var ages: [Int] {
        Self.ages.contains(draft.maxAgeDays) ? Self.ages : (Self.ages + [draft.maxAgeDays]).sorted()
    }

    private var feeds: [RSSFeed] {
        model.feeds.sorted { $0.displayTitle.localizedCaseInsensitiveCompare($1.displayTitle) == .orderedAscending }
    }

    private func ageTitle(_ days: Int) -> String {
        switch days {
        case 0: return "Any Age"
        case 1: return "Last Day"
        default: return "Last \(days) Days"
        }
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Name", text: $draft.name)
                        .accessibilityIdentifier("rss-smart-name")
                }
                Section {
                    TextField("Words", text: $draft.words)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    Toggle("Regular Expression", isOn: $draft.regex)
                    if invalidRegex {
                        Text("Invalid regular expression")
                            .foregroundStyle(.red)
                    }
                } header: {
                    Text("Title or Text Contains")
                } footer: {
                    Text("Leave empty to match every article.")
                }
                Section {
                    Picker("Feeds", selection: $draft.scope) {
                        Text("All Feeds").tag("")
                        ForEach(model.folders) { (folder: RSSFolder) in
                            Label(folder.name, systemImage: "folder").tag("folder:\(folder.id)")
                        }
                        ForEach(feeds) { (feed: RSSFeed) in
                            Text(feed.displayTitle).tag("feed:\(feed.id)")
                        }
                    }
                    Toggle("Unread Only", isOn: $draft.unreadOnly)
                    Toggle("Starred Only", isOn: $draft.starredOnly)
                    Picker("Age", selection: $draft.maxAgeDays) {
                        ForEach(ages, id: \.self) { (days: Int) in
                            Text(ageTitle(days)).tag(days)
                        }
                    }
                }
                if !error.isEmpty {
                    Section {
                        Label(error, systemImage: "exclamationmark.triangle")
                            .foregroundStyle(.red)
                    }
                }
            }
            .navigationTitle(draft.id.isEmpty ? "New Smart Feed" : "Edit Smart Feed")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { save() }
                        .disabled(draft.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || invalidRegex)
                }
            }
        }
    }

    private func save() {
        do {
            try model.saveSmartFeed(draft)
            dismiss()
        } catch {
            self.error = RSSReaderErrors.message(error)
        }
    }
}

struct RSSFilterDraft: Identifiable {
    var filter: RSSFilter

    var id: String { filter.id.isEmpty ? "new" : filter.id }
}

struct RSSFiltersScreen: View {
    let model: RSSReaderModel
    @State private var editing: RSSFilterDraft?

    private func feedName(_ filter: RSSFilter) -> String {
        filter.feedId.isEmpty ? "All Feeds" : (model.feed(filter.feedId)?.displayTitle ?? "All Feeds")
    }

    var body: some View {
        List {
            Section {
                Toggle(
                    "Mark Duplicate Links as Read",
                    isOn: Binding(
                        get: { model.settings.foldDuplicates },
                        set: { (value: Bool) in model.updateSettings { $0.foldDuplicates = value } }))
            } footer: {
                Text("A new article whose link is already stored in another feed arrives as read.")
            }
            Section {
                let filters: [RSSFilter] = model.filters
                if filters.isEmpty {
                    Text("No filters yet.")
                        .foregroundStyle(.secondary)
                }
                ForEach(filters) { (filter: RSSFilter) in
                    Button {
                        editing = RSSFilterDraft(filter: filter)
                    } label: {
                        row(filter)
                    }
                    .swipeActions(edge: .trailing, allowsFullSwipe: true) {
                        Button(role: .destructive) {
                            model.deleteFilter(filter.id)
                        } label: {
                            Label("Delete", systemImage: "trash")
                        }
                    }
                }
            } footer: {
                Text("Hidden articles stay on this iPhone and come back when the filter is removed. Mark as Read applies to new articles as they arrive. Starred articles are never filtered.")
            }
        }
        .navigationTitle("Filters")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button {
                    editing = RSSFilterDraft(filter: RSSFilter(id: ""))
                } label: {
                    Image(systemName: "plus")
                }
                .disabled(model.filters.count >= RSSReaderLimits.maxFilters)
                .accessibilityLabel("Add Filter")
                .accessibilityIdentifier("rss-add-filter")
            }
        }
        .sheet(item: $editing) { (draft: RSSFilterDraft) in
            RSSFilterEditor(model: model, draft: draft.filter)
                .rssPalette(model.settings.palette)
        }
    }

    private func row(_ filter: RSSFilter) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(filter.pattern)
                .foregroundStyle(Color.primary)
                .lineLimit(1)
            Text("\(filter.field.title) \u{00B7} \(filter.action.title) \u{00B7} \(feedName(filter))")
                .font(.caption)
                .foregroundStyle(.secondary)
            if filter.regex && !RSSMatcher.isValidRegex(filter.pattern) {
                Text("Invalid regular expression")
                    .font(.caption)
                    .foregroundStyle(.red)
            }
        }
        .padding(.vertical, 2)
    }
}

struct RSSFilterEditor: View {
    let model: RSSReaderModel
    @Environment(\.dismiss) private var dismiss
    @State private var draft: RSSFilter
    @State private var error = ""

    init(model: RSSReaderModel, draft: RSSFilter) {
        self.model = model
        _draft = State(initialValue: draft)
    }

    private var invalidRegex: Bool {
        draft.regex && !draft.pattern.isEmpty && !RSSMatcher.isValidRegex(draft.pattern)
    }

    private var feeds: [RSSFeed] {
        model.feeds.sorted { $0.displayTitle.localizedCaseInsensitiveCompare($1.displayTitle) == .orderedAscending }
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Text to Match", text: $draft.pattern)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .accessibilityIdentifier("rss-filter-pattern")
                    Toggle("Regular Expression", isOn: $draft.regex)
                    if invalidRegex {
                        Text("Invalid regular expression")
                            .foregroundStyle(.red)
                    }
                }
                Section {
                    Picker("Match", selection: $draft.field) {
                        ForEach(RSSFilterField.allCases, id: \.self) { (field: RSSFilterField) in
                            Text(field.title).tag(field)
                        }
                    }
                    Picker("Action", selection: $draft.action) {
                        ForEach(RSSFilterAction.allCases, id: \.self) { (action: RSSFilterAction) in
                            Text(action.title).tag(action)
                        }
                    }
                    Picker("Feed", selection: $draft.feedId) {
                        Text("All Feeds").tag("")
                        ForEach(feeds) { (feed: RSSFeed) in
                            Text(feed.displayTitle).tag(feed.id)
                        }
                    }
                } footer: {
                    Text("Matching ignores case. Mark as Read applies to new articles as they arrive.")
                }
                if !draft.id.isEmpty {
                    Section {
                        Button("Delete Filter", role: .destructive) {
                            model.deleteFilter(draft.id)
                            dismiss()
                        }
                    }
                }
                if !error.isEmpty {
                    Section {
                        Label(error, systemImage: "exclamationmark.triangle")
                            .foregroundStyle(.red)
                    }
                }
            }
            .navigationTitle(draft.id.isEmpty ? "New Filter" : "Edit Filter")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { save() }
                        .disabled(draft.pattern.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || invalidRegex)
                }
            }
        }
    }

    private func save() {
        do {
            try model.saveFilter(draft)
            dismiss()
        } catch {
            self.error = RSSReaderErrors.message(error)
        }
    }
}
