import FMTools
import SwiftUI
import UIKit

struct PhoneSecurityScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var report: PhoneSecurityReport?
    @State private var running: Bool = false
    @State private var checkedAt: Date?
    @State private var filter: PhoneSecurityFilter = .all
    @State private var category: PhoneSecurityCategory?
    @State private var query: String = ""
    @State private var picked: PhoneSecurityFinding?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                summaryCard
                if let report {
                    filterBar(report)
                    Text(report.filteredSummary(filter: filter, category: category, query: query))
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .accessibilityIdentifier("phone-security-summary")
                    let rows: [PhoneSecurityFinding] = report.visible(filter: filter, category: category, query: query)
                    if rows.isEmpty {
                        FmEmptyState(icon: "search", title: "No checks to show",
                                     message: report.filteredSummary(filter: filter, category: category, query: query))
                    } else {
                        VStack(spacing: 0) {
                            ForEach(rows) { (finding: PhoneSecurityFinding) in
                                row(finding)
                                if finding.id != rows.last?.id { FmRowDivider() }
                            }
                        }
                        .background(fm.bgRaised)
                        .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                    }
                    Text("The checkup reads this iPhone's settings and ForkMesh's own process. Nothing is uploaded, and nothing changes unless you follow a fix into Settings.")
                        .fmText(.bodySmall, color: fm.textTertiary)
                        .padding(.top, FmSpace.x2)
                }
            }
            .padding(FmSpace.x4)
        }
        .background(fm.canvas)
        .navigationTitle("Phone Security")
        .navigationBarTitleDisplayMode(.inline)
        .searchable(text: $query, prompt: "Search checks")
        .refreshable { await run() }
        .task { if report == nil { await run() } }
        .sheet(item: $picked) { (finding: PhoneSecurityFinding) in
            PhoneSecurityDetailSheet(finding: finding)
        }
    }

    private var summaryCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                HStack(spacing: FmSpace.x2) {
                    FmIcon(report.map { PhoneSecurityStyle.icon($0.worst ?? .pass) } ?? "info_outline", size: 28,
                           color: report.map { PhoneSecurityStyle.color($0.worst ?? .pass, fm) } ?? fm.textSecondary)
                    Text(report?.headline ?? "Checking this iPhone…")
                        .fmText(.titleMedium)
                        .accessibilityIdentifier("phone-security-headline")
                    Spacer(minLength: 0)
                }
                if let report {
                    Text(report.summary).fmText(.bodySmall, color: fm.textSecondary)
                }
                if let checkedAt {
                    Text("Checked \(checkedAt.formatted(date: .omitted, time: .shortened))")
                        .fmText(.bodySmall, color: fm.textTertiary)
                }
                FmTextButton(running ? "Checking…" : "Run again", icon: "refresh") {
                    Task { await run() }
                }
                .disabled(running)
                .accessibilityIdentifier("phone-security-run")
            }
        }
    }

    private func filterBar(_ report: PhoneSecurityReport) -> some View {
        FmEdgeFadeScroll {
            HStack(spacing: FmSpace.x2) {
                ForEach(PhoneSecurityFilter.allCases, id: \.self) { (option: PhoneSecurityFilter) in
                    chip(option.label, count: report.count(option), active: filter == option) { filter = option }
                        .accessibilityIdentifier("phone-security-filter-\(option.rawValue)")
                }
                Menu {
                    Button("All categories") { category = nil }
                    ForEach(report.categories, id: \.self) { (option: PhoneSecurityCategory) in
                        Button(option.label) { category = option }
                    }
                } label: {
                    HStack(spacing: FmSpace.x1) {
                        FmIcon("tune", size: 16, color: category == nil ? fm.textSecondary : fm.accent)
                        Text(category?.label ?? "Category")
                            .fmText(.labelMedium, color: category == nil ? fm.textSecondary : fm.accent)
                    }
                    .padding(.horizontal, FmSpace.x3)
                    .frame(height: 32)
                    .background(fm.bgRaised)
                    .clipShape(Capsule())
                }
                .accessibilityIdentifier("phone-security-category")
            }
        }
    }

    private func chip(_ label: String, count: Int, active: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: FmSpace.x1) {
                Text(label).fmText(.labelMedium, color: active ? fm.accent : fm.textSecondary)
                FmCountPill(count: count, active: active)
            }
            .padding(.horizontal, FmSpace.x3)
            .frame(height: 32)
            .background(active ? fm.accentSubtle : fm.bgRaised)
            .clipShape(Capsule())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(active ? .isSelected : [])
    }

    private func row(_ finding: PhoneSecurityFinding) -> some View {
        FmListRow(
            title: finding.check.title,
            subtitle: finding.detail,
            leading: AnyView(FmIcon(PhoneSecurityStyle.icon(finding.status), color: PhoneSecurityStyle.color(finding.status, fm))),
            trailing: AnyView(FmStatusBadge(label: finding.status.label, color: PhoneSecurityStyle.color(finding.status, fm))),
            onTap: { picked = finding })
            .accessibilityIdentifier("phone-security-row-\(finding.check.id)")
    }

    private func run() async {
        guard !running else { return }
        running = true
        let facts: PhoneSecurityFacts = await PhoneSecurityProbe.facts()
        report = PhoneSecurityReport(facts: facts)
        checkedAt = Date()
        running = false
    }
}

enum PhoneSecurityStyle {
    static func icon(_ status: PhoneSecurityStatus) -> String {
        switch status {
        case .critical: return "error"
        case .warning: return "warning_amber"
        case .review: return "info_outline"
        case .unavailable: return "help_outline"
        case .pass: return "check_circle"
        }
    }

    static func color(_ status: PhoneSecurityStatus, _ fm: FmPalette) -> Color {
        switch status {
        case .critical: return fm.danger
        case .warning: return fm.warning
        case .review: return fm.accent
        case .unavailable: return fm.textTertiary
        case .pass: return fm.success
        }
    }

    @MainActor
    static func open(_ page: PhoneSettingsPage) {
        let link: String
        switch page {
        case .app: link = UIApplication.openSettingsURLString
        case .appNotifications: link = UIApplication.openNotificationSettingsURLString
        }
        guard let url = URL(string: link) else { return }
        UIApplication.shared.open(url)
    }
}

struct PhoneSecurityDetailSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let finding: PhoneSecurityFinding

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    HStack(spacing: FmSpace.x2) {
                        FmStatusBadge(label: finding.status.label, color: PhoneSecurityStyle.color(finding.status, fm))
                        Text(finding.check.category.label).fmText(.bodySmall, color: fm.textSecondary)
                    }
                    Text(finding.detail).fmText(.titleSmall)
                    if let usage = finding.usage {
                        FmSectionHeader(title: "Do you need it?")
                        Text(usage).fmText(.bodyMedium)
                    }
                    FmSectionHeader(title: "What this is")
                    Text(finding.check.explanation).fmText(.bodyMedium, color: fm.textSecondary)
                    if let fix = finding.fix {
                        FmSectionHeader(title: "Fix")
                        Text(fix.description).fmText(.bodyMedium)
                        if case .open(let page) = fix.action {
                            FmFilledButton(fix.label, icon: "open_in_new", fillWidth: true) {
                                PhoneSecurityStyle.open(page)
                            }
                            .accessibilityIdentifier("phone-security-fix")
                        }
                    }
                    Text(finding.check.id).fmText(.bodySmall, color: fm.textTertiary)
                        .textSelection(.enabled)
                }
                .padding(FmSpace.x4)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .background(fm.canvas)
            .navigationTitle(finding.check.title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .presentationDetents([.medium, .large])
    }
}
