import FMCodeInsight
import SwiftUI
import UIKit

struct StrataRulesView: View {
    let model: StrataModel
    let query: String
    @Binding var severity: StrataSeverity?
    @Binding var expanded: Int?
    let open: (StrataFocus) -> Void
    let openFile: (String, Int) -> Void

    @Environment(\.fmPalette) private var fm
    @State private var openRules: Set<Int> = []

    var body: some View {
        let groups = Strata.ruleGroups(model, text: query, severity: severity)
        VStack(spacing: 0) {
            filters
            if groups.isEmpty {
                FmEmptyState(icon: "rule", title: "No rules match", message: query.isEmpty ? "No violation has this severity." : "Nothing matches \"\(query)\".")
            } else {
                ScrollViewReader { (reader: ScrollViewProxy) in
                    List {
                        ForEach(groups) { (group: StrataRuleGroup) in
                            Section {
                                ForEach(group.rows) { (row: StrataRuleRow) in
                                    ruleRow(row).id(row.rule)
                                }
                            } header: {
                                HStack {
                                    Text(group.category.name)
                                    Spacer()
                                    Text("\(Strata.grouped(group.count)) · \(Strata.formatDebt(group.debt))")
                                }
                                .accessibilityElement(children: .combine)
                            }
                        }
                    }
                    .listStyle(.insetGrouped)
                    .scrollContentBackground(.hidden)
                    .background(fm.canvas)
                    .onAppear {
                        if let target = expanded {
                            openRules.insert(target)
                            expanded = nil
                            DispatchQueue.main.async { reader.scrollTo(target, anchor: .top) }
                        }
                    }
                }
            }
        }
        .accessibilityIdentifier("strata-rules")
    }

    private var filters: some View {
        FmEdgeFadeScroll {
            HStack(spacing: FmSpace.x2) {
                StrataChip(label: "All \(Strata.grouped(model.report.total.violations))", active: severity == nil) { severity = nil }
                ForEach(StrataSeverity.allCases, id: \.self) { (level: StrataSeverity) in
                    StrataChip(label: "\(level.name) \(Strata.grouped(model.report.bySeverity[level.rawValue]))", active: severity == level) {
                        severity = severity == level ? nil : level
                    }
                    .accessibilityIdentifier("strata-severity-\(level.name.lowercased())")
                }
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x2)
        }
    }

    private func binding(_ rule: Int) -> Binding<Bool> {
        Binding(
            get: { openRules.contains(rule) },
            set: { (value: Bool) in
                FmHaptics.selection()
                if value { openRules.insert(rule) } else { openRules.remove(rule) }
            })
    }

    private func ruleRow(_ row: StrataRuleRow) -> some View {
        let rule = model.report.rules[row.rule]
        let tone = rule.violations.isEmpty ? StrataPalette.healthy.color : StrataPalette.severity(StrataSeverity.at(rule.worst)).color
        return DisclosureGroup(isExpanded: binding(row.rule)) {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                Text("\(rule.id) · \(rule.category.name) · default severity \(rule.severity.name) · \(rule.violations.count) violations · \(Strata.formatDebt(rule.debt))")
                    .fmText(.labelSmall, color: fm.textTertiary)
                Text(rule.description).fmText(.bodySmall)
                (Text("How to fix. ").bold() + Text(rule.howToFix)).fmText(.bodySmall, color: fm.textSecondary)
            }
            .padding(.vertical, FmSpace.x1)
            if openRules.contains(row.rule) {
                let shown = Strata.ruleViolations(model, rule: row.rule, text: query, severity: severity, all: row.all)
                ForEach(shown.indexes, id: \.self) { (index: Int) in
                    violationRow(rule: row.rule, index: index)
                }
                if shown.hidden > 0 {
                    Text("and \(Strata.grouped(shown.hidden)) more — export the report for the full list")
                        .fmText(.bodySmall, color: fm.textTertiary)
                }
            }
        } label: {
            HStack(spacing: FmSpace.x2) {
                StrataDot(color: tone, size: 10)
                VStack(alignment: .leading, spacing: 2) {
                    Text(rule.title).fmText(.bodyMedium).lineLimit(2)
                    Text(rule.id).fmText(.labelSmall, color: fm.textTertiary)
                }
                Spacer(minLength: FmSpace.x2)
                VStack(alignment: .trailing, spacing: 2) {
                    Text(Strata.grouped(row.matching)).fmText(.labelLarge, tabular: true)
                    Text(Strata.formatDebt(row.debt)).fmText(.labelSmall, color: fm.textSecondary, tabular: true)
                }
            }
            .accessibilityElement(children: .ignore)
            .accessibilityLabel("\(rule.title), \(rule.violations.isEmpty ? "no violations" : StrataSeverity.at(rule.worst).name), \(row.matching) violations, \(Strata.formatDebt(row.debt)) debt")
        }
        .listRowBackground(fm.bgRaised)
        .accessibilityIdentifier("strata-rule-\(rule.id)")
    }

    private func violationRow(rule: Int, index: Int) -> some View {
        let violation = model.report.rules[rule].violations[index]
        let place = Strata.violationWhere(model, rule: rule, violation)
        let target = Strata.violationTarget(model, rule: rule, violation)
        return Button {
            FmHaptics.tap()
            if let target, !model.report.rules[rule].moduleScope {
                openFile(target.path, target.line)
            } else if violation.module >= 0 {
                open(.module(violation.module))
            } else if let target {
                openFile(target.path, target.line)
            }
        } label: {
            HStack(alignment: .top, spacing: FmSpace.x2) {
                StrataDot(color: StrataPalette.severity(violation.severity).color, size: 8)
                    .padding(.top, 5)
                VStack(alignment: .leading, spacing: 2) {
                    Text(place).fmMono(size: FmType.meta, color: fm.accent).lineLimit(1).truncationMode(.middle)
                    Text(violation.message).fmText(.bodySmall).fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: FmSpace.x1)
                Text(Strata.formatDebt(violation.debt)).fmText(.labelSmall, color: fm.textSecondary, tabular: true)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .contextMenu {
            if let target {
                Button {
                    openFile(target.path, target.line)
                } label: {
                    Label("Open the file", systemImage: "doc.text")
                }
            }
            if violation.file >= 0 {
                Button {
                    open(.file(violation.file))
                } label: {
                    Label("File details", systemImage: "info.circle")
                }
            }
            if violation.module >= 0 {
                Button {
                    open(.module(violation.module))
                } label: {
                    Label("Module details", systemImage: "square.stack.3d.up")
                }
            }
            Button {
                UIPasteboard.general.string = "\(place) — \(violation.message)"
            } label: {
                Label("Copy", systemImage: "doc.on.doc")
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(violation.severity.name): \(violation.message), \(place), \(Strata.formatDebt(violation.debt)) debt")
        .accessibilityHint(target != nil && !model.report.rules[rule].moduleScope ? "Opens the file at the line" : "Shows the module")
        .accessibilityAddTraits(.isButton)
    }
}
