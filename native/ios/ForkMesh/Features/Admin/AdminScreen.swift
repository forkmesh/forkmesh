import FMApi
import FMInsights
import SwiftUI

struct AdminDestination: View {
    let section: String?

    var body: some View {
        if !InsightsFeature.isAdmin {
            AdminRequiredView()
        } else if let api = InsightsFeature.api {
            AdminRouter(model: AdminScreenModel(transport: api, section: AdminSection(route: section)))
        } else {
            InsightsUnavailableView(title: "Admin is unavailable", systemImage: "lock", message: "Sign in first.")
        }
    }
}

struct AdminRequiredView: View {
    var body: some View {
        ContentUnavailableView(
            "Administrators only", systemImage: "lock.shield",
            description: Text("This page is for the server's administrators. Your account is not one."))
            .navigationTitle("Admin")
            .navigationBarTitleDisplayMode(.inline)
    }
}

struct AdminRouter: View {
    @State var model: AdminScreenModel

    var body: some View {
        Group {
            if model.denied {
                AdminRequiredView()
            } else {
                switch model.section {
                case .overview: AdminOverviewScreen(model: model)
                case .users: AdminUsersScreen(model: model)
                case .errors: AdminErrorsScreen(model: model)
                case .database: AdminDatabaseScreen(model: model)
                case .usage: AdminUsageScreen(model: model)
                case .repos: AdminRepoPurgeScreen(model: model)
                }
            }
        }
        .task { await model.reload() }
    }
}

struct AdminOverviewScreen: View {
    @Bindable var model: AdminScreenModel

    var body: some View {
        List {
            switch model.overview {
            case .idle, .loading:
                InsightsLoadingRows(rows: 5)
            case .failed(let error):
                InsightsBanner(text: error.message, systemImage: "exclamationmark.triangle")
                Button("Retry") { Task { await model.reload() } }
            case .loaded(let overview):
                Section("Signed in as \(overview.actor)") {
                    count("Users", overview.users)
                    count("Quarantined", overview.quarantinedUsers)
                    count("Awaiting email confirmation", overview.pendingVerifications)
                    count("Errors, 24 h", overview.errors24h)
                    count("Relays awaiting approval", overview.relaysAwaitingApproval)
                }
            }
            Section {
                ForEach(AdminSection.allCases.filter { $0 != .overview }, id: \.self) { section in
                    NavigationLink(value: Route.admin(section: section.rawValue)) {
                        Label(section.title, systemImage: section.symbol)
                    }
                }
                NavigationLink {
                    SentryIssuesScreen()
                        .shellPageClearance()
                } label: {
                    Label("Sentry (from your desktop)", systemImage: "ladybug")
                }
            }
            Section {
                Text("Writes to the database stay on the desktop. Deleting or quarantining an account asks for Face ID.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle("Admin")
        .navigationBarTitleDisplayMode(.large)
        .fmRefreshable { await model.reload() }
    }

    private func count(_ label: String, _ value: Int?) -> some View {
        LabeledContent(label, value: value.map { StatsFormat.compactNumber(Double($0)) } ?? "–")
    }
}

struct AdminErrorsScreen: View {
    @Bindable var model: AdminScreenModel
    @State private var source = ""

    var body: some View {
        List {
            switch model.errors {
            case .idle, .loading:
                InsightsLoadingRows(rows: 6)
            case .failed(let error):
                InsightsBanner(text: error.message, systemImage: "exclamationmark.triangle")
            case .loaded(let report):
                Section {
                    LabeledContent("Errors, 24 h", value: "\(report.total)")
                    LabeledContent("Groups", value: "\(report.groupCount)")
                    LabeledContent("Accounts affected", value: "\(report.usersHit)")
                    if report.truncated {
                        InsightsBanner(text: "Only the newest 5,000 errors were read.", systemImage: "info.circle", tint: .secondary)
                    }
                    Picker("Source", selection: $source) {
                        Text("All").tag("")
                        Text("Server").tag("server")
                        Text("Browser").tag("javascript")
                        Text("Desktop").tag("desktop")
                    }
                    .pickerStyle(.segmented)
                }
                Section("Groups, most frequent first") {
                    let groups = report.filtered(source: source)
                    if groups.isEmpty {
                        Text("No errors.").foregroundStyle(.secondary)
                    }
                    ForEach(groups) { group in
                        NavigationLink {
                            AdminErrorDetail(group: group)
                                .shellPageClearance()
                        } label: {
                            VStack(alignment: .leading, spacing: 4) {
                                HStack {
                                    Text(group.status).font(.caption.monospaced().weight(.bold))
                                        .foregroundStyle(group.status.hasPrefix("5") ? Color.red : Color.orange)
                                    Text(group.title).lineLimit(2)
                                    Spacer()
                                    Text("\(group.count)").font(.headline.monospacedDigit())
                                }
                                Text(group.requestLine).font(.caption.monospaced()).foregroundStyle(.secondary).lineLimit(1)
                                Text(AdminErrorsScreen.meta(group))
                                    .font(.caption2).foregroundStyle(.secondary)
                            }
                            .accessibilityElement(children: .combine)
                        }
                    }
                }
            }
        }
        .navigationTitle("Errors")
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await model.reload() }
    }

    static func meta(_ group: ErrorGroup) -> String {
        let base: String = "\(group.sourceLabel) · last \(InsightsTime.relative(group.lastSeen))"
        return group.affected.isEmpty ? base : "\(base) · \(group.affected)"
    }
}

struct AdminErrorDetail: View {
    let group: ErrorGroup

    var body: some View {
        List {
            Section {
                LabeledContent("Status", value: group.status)
                LabeledContent("Request", value: group.requestLine)
                LabeledContent("Source", value: group.sourceLabel)
                LabeledContent("Occurrences, 24 h", value: "\(group.count)")
                LabeledContent("First seen", value: InsightsTime.relative(group.firstSeen))
                LabeledContent("Last seen", value: InsightsTime.relative(group.lastSeen))
            }
            Section("Message") {
                Text(group.message.isEmpty ? "(no message)" : group.message).font(.callout.monospaced()).textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(group.message)
            }
            if !group.users.isEmpty || group.anonymous > 0 {
                Section("Accounts affected") {
                    ForEach(group.users) { user in
                        NavigationLink(value: Route.profile(name: user.name)) {
                            LabeledContent(user.name, value: "\(user.count)")
                        }
                    }
                    if group.userCount > group.users.count {
                        Text("and \(group.userCount - group.users.count) more").foregroundStyle(.secondary)
                    }
                    if group.anonymous > 0 { LabeledContent("Without a session", value: "\(group.anonymous)") }
                }
            }
            Section {
                NavigationLink(value: Route.compose(ComposeTarget(text: group.agentPrompt(host: InsightsFeature.hostLabel)))) {
                    Label("Send to agent", systemImage: "sparkles")
                }
                ShareLink(item: group.agentPrompt(host: InsightsFeature.hostLabel)) {
                    Label("Share", systemImage: "square.and.arrow.up")
                }
            }
        }
        .navigationTitle("Error group")
        .navigationBarTitleDisplayMode(.inline)
    }
}
