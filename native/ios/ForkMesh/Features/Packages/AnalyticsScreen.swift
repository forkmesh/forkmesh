import Charts
import FMApi
import FMPackages
import SwiftUI

struct AnalyticsDestination: View {
    let section: String?

    var body: some View {
        PackagesGate(feature: "Analytics") { client in
            AnalyticsScreen(client: client)
        }
    }
}

struct AnalyticsScreen: View {
    private let client: PackagesClient
    @State private var model: OrgPackageModel<[AnalyticsProject]>
    @State private var projectId = ""
    @State private var range: AnalyticsRange = .week
    @State private var overview: AnalyticsOverview?
    @State private var loadingOverview = false
    @State private var overviewError = ""

    init(client: PackagesClient) {
        self.client = client
        _model = State(
            initialValue: OrgPackageModel<[AnalyticsProject]>(
                organizations: { try await client.analyticsOrganizations() },
                load: { try await client.analyticsProjects($0) }))
    }

    var body: some View {
        List {
            Section {
                OrganizationPicker(organizations: model.organizations, selection: $model.organization) { await reload() }
                if let projects = model.payload, projects.count > 1 {
                    Picker("Project", selection: $projectId) {
                        ForEach(projects) { Text($0.name).tag($0.id) }
                    }
                    .onChange(of: projectId) { _, _ in Task { await loadOverview() } }
                }
                Picker("Range", selection: $range) {
                    ForEach(AnalyticsRange.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)
                .onChange(of: range) { _, _ in Task { await loadOverview() } }
                PackagesErrorBanner(text: model.errorText) { await reload() }
                PackagesErrorBanner(text: overviewError) { await loadOverview() }
            }
            if let overview {
                totals(overview)
                if let series = overview.series.first, !series.data.isEmpty {
                    Section("Visitors") {
                        Chart {
                            ForEach(Array(series.data.enumerated()), id: \.offset) { index, value in
                                BarMark(x: .value("Bucket", index), y: .value("Visitors", value))
                            }
                        }
                        .frame(height: 160)
                        .accessibilityLabel("Visitors over the selected range")
                    }
                }
                top("Top pages", overview.pages)
                top("Referrers", overview.referrers)
                top("Countries", overview.countries)
                top("Devices", overview.devices)
                top("Events", overview.events)
                if overview.truncated {
                    Text("Only the most recent events were counted.").font(.caption).foregroundStyle(.secondary)
                }
            } else if model.payload?.isEmpty == true {
                ContentUnavailableView(
                    "No analytics project", systemImage: "chart.bar",
                    description: Text("Create a project and add its snippet to your site on the web."))
            } else if !model.loading && model.installedOrganizations.isEmpty {
                PackageNotInstalledView(package: "Analytics", organizations: model.organizations)
            }
        }
        .navigationTitle("Analytics")
        .overlay {
            if (model.loading || loadingOverview) && overview == nil { ProgressView() }
        }
        .fmRefreshable { await reload() }
        .task { await reload() }
    }

    private func reload() async {
        await model.refresh()
        if let projects = model.payload, !projects.contains(where: { $0.id == projectId }) {
            projectId = projects.first(where: \.siteDefault)?.id ?? projects.first?.id ?? ""
        }
        await loadOverview()
    }

    private func loadOverview() async {
        guard PackagesText.isHexId(projectId), PackagesText.isOrgName(model.organization) else {
            overview = nil
            return
        }
        loadingOverview = true
        defer { loadingOverview = false }
        do {
            overview = try await client.analyticsOverview(
                model.organization, projectId: projectId, range: range,
                nowMs: Int64(Date().timeIntervalSince1970 * 1000))
            overviewError = ""
        } catch {
            overviewError = PackagesError.describe(error)
        }
    }

    @ViewBuilder private func totals(_ overview: AnalyticsOverview) -> some View {
        Section {
            metric("Visitors", overview.totals.visitors, overview.previous.visitors)
            metric("Pageviews", overview.totals.pageviews, overview.previous.pageviews)
            metric("Sessions", overview.totals.sessions, overview.previous.sessions)
            LabeledContent("Bounce rate", value: overview.totals.bounceRate.formatted(.percent.precision(.fractionLength(0))))
            LabeledContent(
                "Average session",
                value: Duration.milliseconds(overview.totals.averageSessionMs).formatted(.units(width: .abbreviated)))
        }
    }

    private func metric(_ title: String, _ current: Int64, _ previous: Int64) -> some View {
        LabeledContent(title) {
            HStack(spacing: 6) {
                Text(PackagesFormat.count(current)).monospacedDigit()
                if let change = AnalyticsTotals.change(current, previous) {
                    Text((change >= 0 ? "+" : "") + change.formatted(.percent.precision(.fractionLength(0))))
                        .font(.caption)
                        .foregroundStyle(change >= 0 ? .green : .red)
                }
            }
        }
    }

    @ViewBuilder private func top(_ title: String, _ rows: [AnalyticsTopRow]) -> some View {
        if !rows.isEmpty {
            Section(title) {
                ForEach(rows) { row in
                    LabeledContent(row.value, value: PackagesFormat.count(row.count))
                }
            }
        }
    }
}
