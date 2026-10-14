// The Issues tab with its filter bar, status chips and filter pills. The
// filters live in FMCode's RepoIssuesModel (and IssueFilterMemory, so they
// survive switching tabs); the "New issue" action is RepoDetailScreen's.

import FMApi
import FMBase
import FMCode
import SwiftUI

struct RepoIssuesTab: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoIssuesModel

    init(repo: Repository) {
        _model = State(initialValue: RepoIssuesModel(repo: repo, api: CollabEnv.api, account: { CollabEnv.account() }))
    }

    var body: some View {
        VStack(spacing: 0) {
            CollabNoticeLine(horizontalPadding: FmSpace.x4)
            content
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(fm.listSurface)
        .task {
            await model.start()
        }
    }

    @ViewBuilder private var content: some View {
        switch model.state {
        case .loading:
            CollabStateScroll {
                CollabLoadingCard()
            }
        case .failed(let message):
            CollabStateScroll {
                CollabEmptyCard(message: message)
            }
        case .ready(let issues):
            if issues.isEmpty {
                CollabStateScroll {
                    CollabEmptyCard(message: RepoIssuesModel.emptyList)
                }
            } else {
                VStack(spacing: 0) {
                    IssueFilterBar(model: model)
                    list
                }
            }
        }
    }

    @ViewBuilder private var list: some View {
        let visible: [Issue] = model.visible
        if visible.isEmpty {
            CollabStateScroll {
                CollabEmptyCard(message: RepoIssuesModel.noMatch)
            }
        } else {
            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(visible) { (issue: Issue) in
                        issueRow(issue)
                    }
                }
                .padding(.vertical, FmSpace.x3)
            }
        }
    }

    private func issueRow(_ issue: Issue) -> some View {
        NavigationLink {
            IssueDetailScreen(repo: model.repo, issue: issue)
                .shellPageClearance()
        } label: {
            CollabRow(
                icon: IssueCard.icon(issue),
                tone: IssueCard.tone(issue),
                title: issue.title,
                subtitle: IssueCard.subtitle(issue),
                badge: issue.status)
        }
        .buttonStyle(.plain)
    }
}

/// The open/closed/all switch, the count, the clear
/// button and the four metadata pills.
private struct IssueFilterBar: View {
    @Environment(\.fmPalette) private var fm

    let model: RepoIssuesModel

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            statusRow
            // Wrapped rather than scrolled sideways: four pills overflow a
            // narrow phone by a hair, and a pill parked off the right edge is
            // a filter nobody finds.
            CollabWrap(spacing: FmSpace.x2, runSpacing: FmSpace.x2) {
                ForEach(IssueFilterField.allCases, id: \.self) { (field: IssueFilterField) in
                    IssueFilterPill(model: model, field: field)
                }
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x3)
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var statusRow: some View {
        HStack(spacing: FmSpace.x2) {
            ForEach(IssueStatusFilter.allCases, id: \.self) { (option: IssueStatusFilter) in
                IssueStatusChip(option: option, selected: option == model.status) {
                    model.setStatus(option)
                }
            }
            Text(model.countLabel)
                .fmText(.labelMedium, color: fm.textTertiary, weight: FmType.emphasis)
                .lineLimit(1)
                .frame(maxWidth: .infinity, alignment: .trailing)
            if model.hasMetadataFilter {
                FmIconButton(label: RepoIssuesModel.clearLabel, onPressed: { model.clearMetadata() }) {
                    FmIcon("filter_alt_off_outlined", size: 18, color: fm.textSecondary)
                }
                .frame(width: 36, height: 36)
                .accessibilityIdentifier("issue-filter-clear")
            }
        }
    }
}

private struct IssueStatusChip: View {
    @Environment(\.fmPalette) private var fm

    let option: IssueStatusFilter
    let selected: Bool
    let onSelected: () -> Void

    var body: some View {
        Button(action: {
            FmHaptics.selection()
            onSelected()
        }) {
            Text(option.label)
                .fmText(.labelMedium, color: selected ? fm.accent : fm.textSecondary, weight: FmType.emphasis)
                .padding(.horizontal, FmSpace.x3)
                .frame(height: 32)
                .background(selected ? fm.accentSubtle : Color.clear)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
                .shellGlass(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
                .overlay {
                    FmRoundedShape(radius: FmRadius.md).strokeBorder(selected ? fm.accent : Color.clear, lineWidth: 1)
                }
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("issue-status-\(option.rawValue)")
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}

/// A menu of the values the field carries.
private struct IssueFilterPill: View {
    @Environment(\.fmPalette) private var fm

    let model: RepoIssuesModel
    let field: IssueFilterField

    private var active: Bool { model.isActive(field) }

    private var tint: Color { active ? fm.accent : fm.textSecondary }

    var body: some View {
        let options: [IssueFilterOption] = model.options(field)
        let selected: String = model.selection(field).value
        Menu {
            ForEach(options, id: \.value) { (option: IssueFilterOption) in
                Button(action: { model.select(field, option.value) }) {
                    Label(option.label, systemImage: option.value == selected ? "checkmark.circle.fill" : "circle")
                }
            }
        } label: {
            face
        }
        .disabled(!model.pillEnabled(field))
        .accessibilityLabel(Text("Filter by \(field.title.lowercased())"))
        .accessibilityIdentifier("issue-filter-\(field.rawValue)")
    }

    private var face: some View {
        HStack(spacing: FmSpace.x1) {
            Text(model.pillLabel(field))
                .fmText(.labelMedium, color: tint, weight: FmType.emphasis)
                .lineLimit(1)
                .truncationMode(.tail)
            FmIcon("expand_more", size: 16, color: active ? fm.accent : fm.textTertiary)
        }
        .padding(.horizontal, FmSpace.x3)
        .frame(height: 34)
        .frame(maxWidth: 190)
        .background(active ? fm.accentSubtle : Color.clear)
        .clipShape(Capsule())
        .shellGlass(Capsule())
        .overlay {
            Capsule().strokeBorder(active ? fm.accent : Color.clear, lineWidth: 1)
        }
    }
}
