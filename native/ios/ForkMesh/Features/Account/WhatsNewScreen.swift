import FMBase
import SwiftUI

struct WhatsNewScreen: View {
    @Environment(\.fmPalette) private var fm
    let changelog: AppChangelog

    static let bundled: AppChangelog = AppChangelog.bundled()

    init(changelog: AppChangelog = WhatsNewScreen.bundled) {
        self.changelog = changelog
    }

    var body: some View {
        List {
            ForEach(Array(changelog.entries.enumerated()), id: \.element.id) { (pair: (offset: Int, element: AppChangelog.Entry)) in
                let index: Int = pair.offset
                let entry: AppChangelog.Entry = pair.element
                Section {
                    if entry.sections.isEmpty {
                        Text(index == 0 ? "No notes were written for this build." : "No notes for this build.")
                            .fmText(.bodyMedium, color: fm.textSecondary)
                            .padding(.vertical, FmSpace.x1)
                    }
                    ForEach(entry.sections) { (section: AppChangelog.Section) in
                        sectionRow(section)
                    }
                } header: {
                    header(entry, current: index == 0)
                }
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle("What's new")
        .navigationBarTitleDisplayMode(.inline)
        .accessibilityIdentifier("whats-new")
    }

    private func header(_ entry: AppChangelog.Entry, current: Bool) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: FmSpace.x2) {
                Text(verbatim: entry.heading)
                    .fmText(.titleSmall, color: fm.textPrimary)
                if current {
                    Text("Installed")
                        .fmText(.labelSmall, color: fm.accent)
                        .padding(.horizontal, FmSpace.x2)
                        .padding(.vertical, 2)
                        .background(Capsule().fill(fm.accentSubtle))
                }
            }
            if !entry.date.isEmpty {
                Text(verbatim: entry.formattedDate())
                    .fmText(.bodySmall, color: fm.textSecondary)
            }
        }
        .textCase(nil)
        .padding(.top, FmSpace.x2)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isHeader)
    }

    private func sectionRow(_ section: AppChangelog.Section) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text(verbatim: section.title)
                .fmText(.labelLarge, color: fm.accent)
            ForEach(Array(section.items.enumerated()), id: \.offset) { (pair: (offset: Int, element: String)) in
                HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                    Text(verbatim: "•")
                        .fmText(.bodyMedium, color: fm.textTertiary)
                        .accessibilityHidden(true)
                    Text(verbatim: pair.element)
                        .fmText(.bodyMedium, color: fm.textPrimary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
        .padding(.vertical, FmSpace.x1)
        .accessibilityElement(children: .combine)
    }
}
