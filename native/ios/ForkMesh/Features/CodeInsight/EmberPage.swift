import FMCodeInsight
import SwiftUI

struct EmberPage: View {
    let context: CodeInsightContext
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var model = EmberPageModel()
    @State private var mode: EmberMode = .map
    @State private var showFilters = false
    @State private var showHelp = false

    private var snapshotKey: String {
        let s = context.snapshot
        return "\(s.source)|\(s.head)|\(s.analyzedAt)|\(s.files.count)|\(s.commitsScanned)|\(s.error)"
    }

    private var unusable: String? {
        let s = context.snapshot
        if !s.error.isEmpty { return s.error }
        if s.files.isEmpty { return "No source files found here. Pick another repository or folder." }
        return nil
    }

    private var search: Binding<String> {
        Binding(get: { model.filters.search },
                set: { (text: String) in
                    model.filters.search = text
                    model.filtersChanged(debounce: true)
                })
    }

    private var cardShown: Binding<Bool> {
        Binding(get: { model.selected >= 0 },
                set: { (shown: Bool) in if !shown { model.select(-1, zoom: false) } })
    }

    var body: some View {
        Group {
            if let unusable {
                FmEmptyState(icon: "search", title: "Nothing to map", message: unusable)
            } else if !model.ready {
                VStack(spacing: FmSpace.x3) {
                    ProgressView()
                    Text("Laying out the map…").fmText(.bodySmall, color: fm.textSecondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                VStack(spacing: 0) {
                    EmberTabBar(model: model)
                    controls
                    if mode == .map {
                        GeometryReader { proxy in
                            VStack(spacing: 0) {
                                EmberCrumbBar(model: model)
                                EmberMapView(model: model, onHelp: { showHelp = true })
                                    .frame(height: max(220, proxy.size.height * 0.62))
                                ScrollView {
                                    EmberPanel(model: model)
                                        .padding(FmSpace.x3)
                                }
                            }
                        }
                    } else {
                        EmberListMode(model: model)
                    }
                }
            }
        }
        .background(fm.canvas)
        .searchable(text: search, placement: .navigationBarDrawer(displayMode: .automatic), prompt: "Search by filename")
        .searchSuggestions {
            ForEach(Ember.searchHits(model.snapshot, query: model.filters.search, limit: 8)) { (hit: EmberSearchHit) in
                Button {
                    model.select(hit.file, zoom: true)
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(hit.title).fmText(.bodyMedium)
                        Text(hit.detail).fmText(.bodySmall, color: fm.textSecondary).lineLimit(1)
                    }
                }
            }
        }
        .onSubmit(of: .search) { model.selectBestMatch() }
        .sheet(isPresented: cardShown) { EmberFileSheet(model: model, context: context) }
        .alert("How Ember scores files", isPresented: $showHelp) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(Ember.scoringHelp)
        }
        .fmFeedback(.selection, trigger: model.selected)
        .task(id: snapshotKey) {
            guard unusable == nil else {
                context.setSummary("")
                return
            }
            await model.load(context.snapshot, key: snapshotKey)
            context.setSummary(model.summary.text)
        }
        .onChange(of: reduceMotion, initial: true) { (_: Bool, value: Bool) in model.reduceMotion = value }
        .onAppear { model.visible = true }
        .onDisappear { model.visible = false }
    }

    private var controls: some View {
        HStack(spacing: FmSpace.x2) {
            Picker("View", selection: $mode) {
                ForEach(EmberMode.allCases) { (option: EmberMode) in
                    Text(option.rawValue).tag(option)
                }
            }
            .pickerStyle(.segmented)
            .frame(width: 132)
            .accessibilityHint("List ranks every file by the tab's measure")
            Spacer(minLength: FmSpace.x1)
            if model.filters.active {
                Text("\(Ember.number(model.coloring.matching)) of \(Ember.number(model.snapshot.files.count))")
                    .fmText(.bodySmall, color: fm.textSecondary, tabular: true)
                    .lineLimit(1)
            }
            Button { showFilters = true } label: {
                HStack(spacing: FmSpace.x1) {
                    FmIcon("tune", size: 16, color: model.filters.active ? fm.accent : fm.textSecondary)
                    Text("Filters").fmText(.labelMedium, color: model.filters.active ? fm.accent : fm.textSecondary)
                }
                .padding(.horizontal, FmSpace.x3)
                .frame(height: 32)
                .background(model.filters.active ? fm.accentSubtle : fm.bgRaised)
                .clipShape(Capsule())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(model.filters.active ? "Filters, active" : "Filters")
            .sheet(isPresented: $showFilters) { EmberFilterSheet(model: model) }
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.bottom, FmSpace.x2)
    }
}

struct EmberTabBar: View {
    let model: EmberPageModel

    var body: some View {
        FmEdgeFadeScroll {
            HStack(spacing: FmSpace.x2) {
                ForEach(EmberTab.allCases) { (tab: EmberTab) in
                    EmberChip(label: tab.title, active: model.tab == tab, muted: tab.needsHistory && !model.history) {
                        model.setTab(tab)
                    }
                    .accessibilityHint(tab.needsHistory && !model.history ? "Needs git history" : "")
                    .accessibilityIdentifier("ember-tab-\(tab.rawValue)")
                }
            }
            .padding(.horizontal, FmSpace.x3)
        }
        .padding(.vertical, FmSpace.x2)
    }
}

struct EmberCrumbBar: View {
    let model: EmberPageModel
    @Environment(\.fmPalette) private var fm

    var body: some View {
        let crumbs = model.crumbs()
        ScrollViewReader { (reader: ScrollViewProxy) in
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: FmSpace.x1) {
                    ForEach(Array(crumbs.enumerated()), id: \.offset) { (item: (offset: Int, element: EmberCrumb)) in
                        if item.offset > 0 {
                            Text("/").fmText(.bodySmall, color: fm.textTertiary).accessibilityHidden(true)
                        }
                        crumb(item.element).id(item.offset)
                    }
                }
                .padding(.horizontal, FmSpace.x3)
                .frame(height: 30)
            }
            .onChange(of: crumbs.count) { (_: Int, count: Int) in
                if count > 0 { reader.scrollTo(count - 1, anchor: .trailing) }
            }
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Breadcrumb")
    }

    @ViewBuilder private func crumb(_ crumb: EmberCrumb) -> some View {
        if crumb.current {
            Text(crumb.name).fmText(.bodySmall, weight: FmType.emphasis).lineLimit(1)
        } else if crumb.node < 0 {
            Text(crumb.name).fmText(.bodySmall, color: fm.textTertiary)
        } else {
            Button(crumb.name) { model.focusNode(crumb.node) }
                .buttonStyle(.plain)
                .fmText(.bodySmall, color: fm.accent)
                .lineLimit(1)
                .accessibilityHint("Zooms the map to this folder")
        }
    }
}

struct EmberPanel: View {
    let model: EmberPageModel
    @Environment(\.fmPalette) private var fm

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            if model.tab == .refactoringTargets && model.history {
                EmberTargetsSection(model: model)
            } else {
                EmberOverview(model: model)
            }
            Text("Tap a file to see its metrics. Tap a folder to zoom in, tap outside it to zoom out. Pinch to zoom, drag to pan, double-tap to show the whole repository.")
                .fmText(.bodySmall, color: fm.textTertiary)
                .padding(.top, FmSpace.x2)
        }
    }
}

struct EmberTargetsSection: View {
    let model: EmberPageModel
    @Environment(\.fmPalette) private var fm

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text("Refactoring targets").fmText(.titleSmall)
            Text(Ember.targetsIntro).fmText(.bodySmall, color: fm.textSecondary)
            let rows = Ember.targetRows(model.snapshot, model.coloring.targets)
            if rows.isEmpty {
                Text("No refactoring targets.").fmText(.bodyMedium, color: fm.textSecondary)
            }
            ForEach(rows) { (row: EmberRow) in
                EmberRowView(row: row, selected: model.selected == row.id) { model.select(row.id, zoom: true) }
            }
        }
    }
}

struct EmberOverview: View {
    let model: EmberPageModel
    @Environment(\.fmPalette) private var fm

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text("Code health").fmText(.titleSmall)
            ForEach(Ember.bandRows(model.summary)) { (row: EmberRow) in
                let band = HealthBand.allCases[row.id]
                let range = Ember.bandRange(band)
                let active = model.filters.minHealth == range.0 && model.filters.maxHealth == range.1
                EmberRowView(row: row, selected: active) { model.toggleBand(band) }
                    .accessibilityHint(active ? "Shows every file again" : "Shows only \(band.name.lowercased()) files")
            }
            FmSectionHeader(title: "Top hotspots")
                .padding(.top, FmSpace.x2)
            let hot = Ember.hotspotRows(model.snapshot)
            if hot.isEmpty {
                Text(Ember.hotspotsEmptyText(history: model.history)).fmText(.bodySmall, color: fm.textSecondary)
            }
            ForEach(hot) { (row: EmberRow) in
                EmberRowView(row: row, selected: model.selected == row.id) { model.select(row.id, zoom: true) }
            }
        }
    }
}

struct EmberListMode: View {
    let model: EmberPageModel
    @Environment(\.fmPalette) private var fm

    var body: some View {
        let rows = Ember.rankedFiles(model.snapshot, coloring: model.coloring, coupling: model.coupling)
        List {
            if !model.coloring.note.isEmpty {
                Section {
                    Text(model.coloring.note).fmText(.bodySmall, color: fm.textSecondary)
                }
            }
            if model.coloring.tab == .refactoringTargets && model.history {
                Section {
                    Text(Ember.targetsIntro).fmText(.bodySmall, color: fm.textSecondary)
                }
            }
            Section {
                ForEach(rows.prefix(1000)) { (row: EmberRow) in
                    EmberRowView(row: row, selected: model.selected == row.id) { model.select(row.id, zoom: false) }
                        .listRowInsets(EdgeInsets(top: 0, leading: FmSpace.x1, bottom: 0, trailing: FmSpace.x1))
                        .listRowBackground(Color.clear)
                }
            } header: {
                Text("\(model.coloring.tab.title) · \(Ember.number(rows.count)) files")
            } footer: {
                if rows.count > 1000 { Text("Showing the first 1,000. Use search or filters to narrow the list.") }
            }
            Section {
                EmberOverview(model: model)
                    .listRowBackground(Color.clear)
            }
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
    }
}
