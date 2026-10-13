import FMApi
import FMBase
import FMCode
import SwiftUI

struct IssueBoardScreen: View {
    let repo: RepoRef
    @State private var board: IssueBoardSession
    @State private var page: String = ""
    @State private var notice: CollabNotice?
    @State private var editingColumns = false
    @Environment(\.horizontalSizeClass) private var sizeClass

    init(repo: RepoRef) {
        self.repo = repo
        let api = ReviewEnv.api
        _board = State(initialValue: IssueBoardSession(
            owner: repo.owner, name: repo.name, store: UserDefaultsStore(),
            writer: ReviewEnv.writer(owner: repo.owner, name: repo.name),
            load: { try await api.publishedIssues(repo.owner, repo.name) }))
    }

    var body: some View {
        content
            .navigationTitle("Board")
            .navigationBarTitleDisplayMode(.inline)
            .searchable(text: $board.query, prompt: "Filter cards")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Button {
                            editingColumns = true
                        } label: {
                            Label("Edit columns", systemImage: "rectangle.split.3x1")
                        }
                        NavigationLink(value: Route.newIssue(repo)) {
                            Label("New issue", systemImage: "plus")
                        }
                    } label: {
                        Image(systemName: "ellipsis.circle")
                    }
                    .accessibilityLabel("More")
                }
            }
            .sheet(isPresented: $editingColumns) {
                BoardColumnsSheet(board: board)
            }
            .task {
                if board.issues.isEmpty { await board.refresh() }
                if page.isEmpty { page = board.columns.first ?? "" }
            }
            .fmRefreshable { await board.refresh() }
    }

    @ViewBuilder private var content: some View {
        if let error = board.loadError, board.issues.isEmpty {
            ContentUnavailableView {
                Label("Could not load issues", systemImage: "exclamationmark.triangle")
            } description: {
                Text(error)
            } actions: {
                Button("Try again") { Task { await board.refresh() } }
                FmMessageTools(error)
            }
        } else if board.loading && board.issues.isEmpty {
            ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if sizeClass == .regular {
            VStack(alignment: .leading, spacing: 0) {
                if notice != nil {
                    noticeLine
                }
                ScrollView(.horizontal) {
                    HStack(alignment: .top, spacing: 12) {
                        ForEach(board.layout) { (column: IssueBoardColumn) in
                            columnView(column)
                                .frame(width: 300)
                        }
                    }
                    .padding(16)
                }
            }
        } else {
            VStack(spacing: 0) {
                Picker("Column", selection: $page) {
                    ForEach(board.layout) { (column: IssueBoardColumn) in
                        Text("\(column.name) \(column.issues.count)").tag(column.name)
                    }
                }
                .pickerStyle(.segmented)
                .padding(.horizontal, 16)
                .padding(.vertical, 8)
                if notice != nil {
                    noticeLine
                }
                TabView(selection: $page) {
                    ForEach(board.layout) { (column: IssueBoardColumn) in
                        ScrollView {
                            columnView(column).padding(16)
                        }
                        .tag(column.name)
                    }
                }
                .tabViewStyle(.page(indexDisplayMode: .never))
            }
        }
    }

    private var noticeLine: some View {
        FmNoticeLine(notice?.message, isError: notice?.isError ?? false, onDismiss: { notice = nil })
            .padding(.horizontal, 16)
    }

    private func columnView(_ column: IssueBoardColumn) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(column.name).font(.headline)
                Text("\(column.issues.count)").font(.caption).foregroundStyle(.secondary)
                Spacer()
            }
            if column.issues.isEmpty {
                Text("No cards").font(.footnote).foregroundStyle(.secondary).padding(.vertical, 12)
            }
            ForEach(column.issues) { (issue: Issue) in
                card(issue, in: column)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func card(_ issue: Issue, in column: IssueBoardColumn) -> some View {
        NavigationLink(value: Route.issue(repo, number: issue.number)) {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text("#\(issue.number)").font(.caption.monospaced()).foregroundStyle(.secondary)
                    if issue.priority > 0 {
                        Text(IssueFields.priorityLabel(issue.priority))
                            .font(.caption2.weight(.semibold))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Color.orange.opacity(0.15), in: Capsule())
                    }
                    Spacer()
                    if board.movingNumber == issue.number { ProgressView().controlSize(.small) }
                }
                Text(issue.title).font(.subheadline).foregroundStyle(.primary).multilineTextAlignment(.leading)
                let labels = issue.labels.filter { !IssueBoard.isStatusLabel($0) }
                if !labels.isEmpty {
                    Text(labels.joined(separator: " · ")).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                }
            }
            .padding(10)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.secondary.opacity(0.08), in: RoundedRectangle(cornerRadius: 10))
        }
        .buttonStyle(.plain)
        .contextMenu {
            Menu("Move to…") {
                ForEach(board.columns.filter { $0 != column.name }, id: \.self) { (target: String) in
                    Button(target) {
                        Task {
                            notice = await board.move(issue, to: target)
                            if notice?.isError == false { FmHaptics.commit() }
                        }
                    }
                }
            }
        }
        .draggable("\(issue.number)")
        .dropDestination(for: String.self) { items, _ in
            guard let first = items.first, let number = Int(first),
                  let moving = board.issues.first(where: { $0.number == number }) else { return false }
            Task { notice = await board.move(moving, to: column.name) }
            return true
        }
    }
}

struct BoardColumnsSheet: View {
    @Bindable var board: IssueBoardSession
    @Environment(\.dismiss) private var dismiss
    @State private var names: [String] = []
    @State private var error: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    ForEach(names.indices, id: \.self) { (index: Int) in
                        TextField("Column", text: $names[index])
                    }
                    .onDelete { names.remove(atOffsets: $0) }
                    .onMove { names.move(fromOffsets: $0, toOffset: $1) }
                    Button("Add column") { names.append("") }
                } footer: {
                    Text("Left to right. The last column is the done column and maps to closed issues. Columns are kept on this device.")
                }
                if let error {
                    Text(error).foregroundStyle(.red)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                }
            }
            .environment(\.editMode, .constant(.active))
            .navigationTitle("Board columns")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        if let problem = board.saveColumns(names) { error = problem } else { dismiss() }
                    }
                }
            }
            .onAppear { names = board.columns }
        }
    }
}
