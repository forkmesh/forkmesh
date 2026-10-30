import FMAccount
import FMBase
import SwiftUI

@MainActor
struct AccountDataSubjectScreen: View {
    @Environment(\.fmPalette) var fm
    let browser: AccountDataBrowser
    let subjectId: String

    private var subject: AccountDataSubject? {
        browser.inventory?.subjects.first { $0.id == subjectId }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                if let subject = subject {
                    SettingsNote(text: subject.subtitle)
                        .padding(.bottom, FmSpace.x4)
                }
                let sections = browser.inventory?.groups(for: subjectId) ?? []
                if sections.isEmpty {
                    SettingsEmptyNote(title: "Nothing here", text: "ForkMesh keeps nothing in this section any more.")
                }
                ForEach(Array(sections.enumerated()), id: \.element.id) { (index: Int, section: AccountDataSection) in
                    SettingsSection(title: section.category, last: index == sections.count - 1) {
                        SettingsCardBox(padded: false) {
                            VStack(spacing: 0) {
                                ForEach(Array(section.groups.enumerated()), id: \.element.id) { (row: Int, group: AccountDataGroup) in
                                    if row > 0 { FmHairline() }
                                    NavigationLink {
                                        AccountDataGroupScreen(browser: browser, group: group)
                                            .shellPageClearance()
                                    } label: {
                                        SettingsLinkRow(icon: groupIcon(group), title: group.label, subtitle: groupSubtitle(group))
                                    }
                                    .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                                }
                            }
                        }
                    }
                }
            }
            .padding(FmSpace.x4)
        }
        .fmRefreshable { await browser.load(refresh: true) }
        .background(fm.canvas.ignoresSafeArea())
        .navigationTitle(subject?.title ?? "Your data")
        .navigationBarTitleDisplayMode(.inline)
    }

    private func groupIcon(_ group: AccountDataGroup) -> String {
        if group.withheld { return "lock_outline" }
        if !group.readable { return "visibility_off_outlined" }
        return "storage_outlined"
    }

    private func groupSubtitle(_ group: AccountDataGroup) -> String {
        let count = group.rows == 1 && !group.truncated ? "1 item" : "\(group.countText) items"
        let note = group.withheld ? "credentials, not shown" : !group.readable ? "organization only" : group.deletable ? "removable" : ""
        return note.isEmpty ? count : count + " · " + note
    }
}

@MainActor
struct AccountDataGroupScreen: View {
    @Environment(\.fmPalette) var fm
    let browser: AccountDataBrowser
    let group: AccountDataGroup
    @State var expanded: Set<String> = []
    @State var confirmDelete: AccountDataRow?
    @State var notice: FmNotice?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                if !group.note.isEmpty {
                    SettingsNote(text: group.note)
                }
                content
                FmNoticeLine($notice)
            }
            .padding(FmSpace.x4)
        }
        .fmRefreshable { await browser.loadRows(group) }
        .task { if browser.page(group) == nil { await browser.loadRows(group) } }
        .background(fm.canvas.ignoresSafeArea())
        .navigationTitle(group.label)
        .navigationBarTitleDisplayMode(.inline)
        .fmDialog(isPresented: deleteShown) {
            if let row = confirmDelete {
                FmConfirmDialog(
                    title: "Remove \(row.title)?",
                    message: "ForkMesh deletes this item from its database. This cannot be undone.",
                    confirmLabel: "Remove",
                    destructive: true,
                    onCancel: { confirmDelete = nil },
                    onConfirm: {
                        confirmDelete = nil
                        Task { @MainActor in
                            if await browser.delete(row, in: group) { notice = FmNotice("Removed") }
                        }
                    })
            }
        }
    }

    private var deleteShown: Binding<Bool> {
        Binding<Bool>(get: { confirmDelete != nil }, set: { (shown: Bool) in if !shown { confirmDelete = nil } })
    }

    @ViewBuilder private var content: some View {
        if let page = browser.page(group) {
            if !page.readable {
                SettingsCardBox { SettingsNote(text: page.reasonText) }
            } else if page.rows.isEmpty {
                SettingsEmptyNote(title: "Nothing here", text: "ForkMesh keeps nothing here any more.")
            } else {
                ForEach(page.rows) { (row: AccountDataRow) in
                    rowCard(row)
                }
                if page.next > 0 {
                    FmOutlinedButton(browser.loadingGroup == group.id ? "Loading…" : "Show more", icon: "expand_more",
                                     onPressed: browser.loadingGroup == group.id ? nil : {
                        Task { @MainActor in await browser.loadRows(group, more: true) }
                    })
                }
            }
        } else if browser.loadingGroup == group.id {
            FmSkeletonList(rows: 4, hasLeading: false, label: "Loading items")
        }
        if !browser.rowsError.isEmpty {
            SettingsNote(text: browser.rowsError, color: fm.danger)
                .fmMessageTools(browser.rowsError)
        }
    }

    private func rowCard(_ row: AccountDataRow) -> some View {
        let open = expanded.contains(row.id)
        return SettingsCardBox(padded: false) {
            VStack(alignment: .leading, spacing: 0) {
                FmListRow(
                    title: row.title,
                    subtitle: row.subtitle.isEmpty ? nil : row.subtitle,
                    leading: FmIcon(open ? "expand_less" : "expand_more", size: 22, color: fm.textSecondary).fmAny,
                    trailing: row.deletable
                        ? FmIconButton(icon: "delete_outline", label: "Remove \(row.title)", size: 20, color: fm.textTertiary,
                                       onPressed: { confirmDelete = row }).fmAny
                        : nil,
                    onTap: {
                        if open { expanded.remove(row.id) } else { expanded.insert(row.id) }
                    },
                    showChevron: false)
                if open {
                    FmHairline()
                    VStack(alignment: .leading, spacing: FmSpace.x2) {
                        ForEach(row.fields) { (field: AccountDataField) in
                            fieldView(field)
                        }
                    }
                    .padding(FmSpace.x4)
                }
            }
        }
    }

    @ViewBuilder private func fieldView(_ field: AccountDataField) -> some View {
        if field.isStructured || field.text.count > 60 {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                Text(field.label).fmText(.labelMedium, color: fm.textTertiary)
                Text(field.text)
                    .fmText(.bodySmall)
                    .monospaced(field.isStructured)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        } else {
            FmKeyValueRow(label: field.label, value: field.text)
        }
    }
}
