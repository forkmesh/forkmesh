import FMApi
import FMBase
import SwiftUI

@MainActor
struct CommandPaletteSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var query: String = ""
    @State private var searchPresented: Bool = false
    @State private var search: GlobalSearchModel

    let context: PaletteContext
    let onCommand: (PaletteCommand) -> Void

    init(context: PaletteContext, onCommand: @escaping (PaletteCommand) -> Void) {
        self.context = context
        self.onCommand = onCommand
        let sources: [GlobalSearchSource] = GlobalSearchSources.standard().filter { $0.kind != .destinations }
        _search = State(initialValue: GlobalSearchModel(sources: sources, saved: SavedSearchStore(store: nil)))
    }

    private var sections: [PaletteSection] {
        CommandPalette.sections(query: query, context: context)
    }

    var body: some View {
        NavigationStack {
            List {
                ForEach(sections) { (section: PaletteSection) in
                    Section(section.title) {
                        ForEach(section.entries) { (entry: PaletteEntry) in
                            row(title: entry.title, subtitle: entry.subtitle, symbol: entry.symbol) {
                                run(entry.command)
                            }
                            .accessibilityIdentifier("palette-\(entry.id)")
                        }
                    }
                }
                ForEach(search.buckets) { (bucket: SearchBucket) in
                    Section(bucket.kind.title) {
                        ForEach(bucket.hits) { (hit: SearchHit) in
                            row(title: hit.title, subtitle: hit.subtitle, symbol: hit.symbol) {
                                run(.open(hit.route))
                            }
                        }
                    }
                }
            }
            .listStyle(.insetGrouped)
            .searchable(
                text: $query, isPresented: $searchPresented, placement: .navigationBarDrawer(displayMode: .always),
                prompt: Text("Go to or run…"))
            .onChange(of: query) { (_: String, next: String) in
                search.query = next
                search.queryChanged()
            }
            .onSubmit(of: .search) {
                if let first = sections.first?.entries.first {
                    run(first.command)
                }
            }
            .navigationTitle("Commands")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                        .keyboardShortcut(.cancelAction)
                }
            }
            .onAppear { searchPresented = true }
        }
        .presentationDetents([.medium, .large])
        .presentationDragIndicator(.visible)
        .accessibilityIdentifier("command-palette")
    }

    private func row(title: String, subtitle: String, symbol: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label {
                VStack(alignment: .leading, spacing: 2) {
                    Text(title)
                        .fmText(.bodyMedium)
                        .lineLimit(1)
                    if !subtitle.isEmpty {
                        Text(subtitle)
                            .fmText(.bodySmall, color: fm.textSecondary)
                            .lineLimit(1)
                    }
                }
            } icon: {
                Image(systemName: symbol)
                    .foregroundStyle(fm.accent)
            }
            .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isButton)
    }

    private func run(_ command: PaletteCommand) {
        FmHaptics.selection()
        dismiss()
        onCommand(command)
    }
}
