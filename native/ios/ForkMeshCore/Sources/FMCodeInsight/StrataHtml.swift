import Foundation

public struct StrataModel: Sendable, Equatable {
    public var snapshot: InsightSnapshot
    public var report: StrataReport
    public var layout: StrataGraphLayout
    public var order: [Int]
    public var depth: Int

    public init(snapshot: InsightSnapshot, report: StrataReport, layout: StrataGraphLayout, order: [Int], depth: Int) {
        self.snapshot = snapshot
        self.report = report
        self.layout = layout
        self.order = order
        self.depth = depth
    }

    public var summary: String { Strata.summaryLine(report) }
}

extension Strata {
    public static func buildModel(_ snapshot: InsightSnapshot, depth: Int) -> StrataModel {
        let graph = Insight.buildModuleGraph(snapshot, depth: depth)
        let report = buildReport(snapshot, graph)
        return StrataModel(snapshot: snapshot, report: report, layout: moduleLayout(report.graph), order: matrixOrder(report.graph), depth: depth)
    }

    public static let ratingHex = ["#3fb98f", "#8cc265", "#e3b341", "#f0883e", "#e5534b"]
    public static let severityHex = ["#e5534b", "#f0883e", "#e3b341", "#8b949e"]

    public static func gateHex(_ status: StrataGateStatus) -> String {
        status == .fail ? "#e5534b" : status == .warn ? "#e3b341" : "#3fb98f"
    }

    public static func reportHtml(_ snapshot: InsightSnapshot, _ report: StrataReport, title: String, analyzedText: String? = nil) -> String {
        let esc = htmlEscaped
        var html = ""
        html += "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
        html += "<title>Strata report · \(esc(title))</title>"
        html += "<style>"
            + "body{font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif;background:#0d1117;color:#e6edf3;margin:0;padding:32px 40px;}"
            + "h1{font-size:26px;margin:0 0 4px}h2{font-size:18px;margin:36px 0 12px;border-bottom:1px solid #30363d;padding-bottom:6px}"
            + ".muted{color:#8b949e}.row{display:flex;gap:16px;flex-wrap:wrap}"
            + ".tile{background:#161b22;border-radius:10px;padding:14px 18px;min-width:140px}"
            + ".tile b{display:block;font-size:22px;margin-top:4px}"
            + ".rating{width:84px;height:84px;border-radius:16px;font-size:48px;font-weight:700;color:#0d1117;display:flex;align-items:center;justify-content:center}"
            + "table{border-collapse:collapse;width:100%;font-size:13px}td,th{text-align:left;padding:6px 10px;border-bottom:1px solid #21262d;vertical-align:top}"
            + "th{color:#8b949e;font-weight:600}.num{text-align:right;font-variant-numeric:tabular-nums}"
            + ".dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:6px}"
            + "details{background:#161b22;border-radius:8px;margin:6px 0;padding:8px 12px}summary{cursor:pointer}"
            + "a{color:#58a6ff}"
            + "</style></head><body>"
        html += "<h1>Strata · \(esc(title))</h1>"
        var facts: [String] = []
        if !snapshot.branch.isEmpty { facts.append("branch \(esc(snapshot.branch))") }
        if !snapshot.head.isEmpty { facts.append("commit \(esc(String(snapshot.head.prefix(10))))") }
        if snapshot.analyzedAt > 0 {
            let text: String
            if let analyzedText {
                text = analyzedText
            } else {
                let formatter = DateFormatter()
                formatter.dateStyle = .short
                formatter.timeStyle = .short
                text = formatter.string(from: Date(timeIntervalSince1970: TimeInterval(snapshot.analyzedAt)))
            }
            facts.append("analyzed \(text)")
        }
        html += "<div class=\"muted\">\(facts.joined(separator: " · "))</div>"

        html += "<h2 id=\"dashboard\">Dashboard</h2><div class=\"row\">"
        html += "<div class=\"tile\" style=\"display:flex;gap:16px;align-items:center\"><div class=\"rating\" style=\"background:\(ratingHex[max(0, min(4, report.total.rating))])\">\(ratingLetter(report.total.rating))</div>"
            + "<div>Debt ratio<b>\(formatPercent(report.total.ratio))</b><span class=\"muted\">\(formatDebt(report.total.debt)) debt · \(formatDebt(report.total.interest)) interest per year</span></div></div>"
        let m = report.metrics
        let tiles: [(String, String)] = [
            ("Lines of code", grouped(m.codeLines)),
            ("Files", grouped(m.files)),
            ("Modules", grouped(m.modules)),
            ("Types", grouped(m.types)),
            ("Functions", grouped(m.functions)),
            ("Average complexity", localeFixed(m.averageComplexity, 1)),
            ("Max complexity", grouped(m.maxComplexity)),
            ("Comments", "\(localeFixed(m.commentPercent, 1)) %"),
            ("Duplicated", "\(localeFixed(m.duplicatedPercent, 1)) %"),
        ]
        for tile in tiles {
            html += "<div class=\"tile\">\(esc(tile.0))<b>\(esc(tile.1))</b></div>"
        }
        html += "</div>"

        html += "<h2 id=\"gates\">Quality gates</h2><table><tr><th>Gate</th><th>Status</th><th>Measured</th><th>Rule</th></tr>"
        for gate in report.gates {
            html += "<tr><td>\(esc(gate.name))</td><td><span class=\"dot\" style=\"background:\(gateHex(gate.status))\"></span>\(gate.status.name)</td><td>\(esc(gate.measured))</td><td class=\"muted\">\(esc(gate.detail))</td></tr>"
        }
        html += "</table>"

        html += "<h2 id=\"rules\">Rules</h2>"
        let ruleOrder = stableSorted(Array(report.rules.indices)) { report.rules[$0].debt > report.rules[$1].debt }
        for r in ruleOrder {
            let rule = report.rules[r]
            let worst = rule.violations.isEmpty ? rule.severity.rawValue : rule.worst
            let color = rule.violations.isEmpty ? "#3fb98f" : severityHex[worst]
            let noun = rule.violations.count == 1 ? "violation" : "violations"
            html += "<details><summary><span class=\"dot\" style=\"background:\(color)\"></span><b>\(esc(rule.id))</b> \(esc(rule.title)) · \(rule.category.name) · \(rule.violations.count) \(noun) · \(formatDebt(rule.debt))</summary>"
            html += "<p>\(esc(rule.description))</p><p class=\"muted\">How to fix: \(esc(rule.howToFix))</p>"
            if !rule.violations.isEmpty {
                html += "<table><tr><th>Severity</th><th>Where</th><th>Issue</th><th class=\"num\">Debt</th></tr>"
                let shown = min(rule.violations.count, 200)
                for i in 0..<shown {
                    let v = rule.violations[i]
                    var whereText = ""
                    if v.file >= 0 && v.file < snapshot.files.count && !rule.moduleScope {
                        whereText = snapshot.files[v.file].path + (v.line > 0 ? ":\(v.line)" : "")
                    } else if v.module >= 0 && v.module < report.graph.nodes.count {
                        whereText = report.graph.nodes[v.module].path
                    }
                    html += "<tr><td>\(v.severity.name)</td><td>\(esc(whereText))</td><td>\(esc(v.message))</td><td class=\"num\">\(formatDebt(v.debt))</td></tr>"
                }
                if rule.violations.count > shown {
                    html += "<tr><td colspan=\"4\" class=\"muted\">and \(rule.violations.count - shown) more</td></tr>"
                }
                html += "</table>"
            }
            html += "</details>"
        }

        html += "<h2 id=\"modules\">Modules</h2><table><tr><th>Module</th><th>Rating</th><th class=\"num\">Lines of code</th>"
            + "<th class=\"num\">Files</th><th class=\"num\">Ca</th><th class=\"num\">Ce</th><th class=\"num\">I</th>"
            + "<th class=\"num\">A</th><th class=\"num\">D</th><th class=\"num\">Debt</th></tr>"
        let modules = stableSorted(Array(report.graph.nodes.indices)) { report.modules[$0].debt > report.modules[$1].debt }
        for i in modules {
            let node = report.graph.nodes[i]
            let score = report.modules[i]
            let name = esc(node.path.isEmpty ? node.name : node.path)
            let cycle = node.cycle >= 0 ? " <span style=\"color:#e5534b\">(cycle)</span>" : ""
            html += "<tr><td>\(name)\(cycle)</td><td><span class=\"dot\" style=\"background:\(ratingHex[max(0, min(4, score.rating))])\"></span>\(ratingLetter(score.rating))</td><td class=\"num\">\(grouped(node.codeLines))</td>"
                + "<td class=\"num\">\(node.files.count)</td><td class=\"num\">\(node.afferent)</td><td class=\"num\">\(node.efferent)</td><td class=\"num\">\(fixed(node.instability, 2))</td>"
            html += "<td class=\"num\">\(fixed(node.abstractness, 2))</td><td class=\"num\">\(fixed(node.distance, 2))</td><td class=\"num\">\(formatDebt(score.debt))</td></tr>"
        }
        html += "</table>"
        html += "<p class=\"muted\">Debt ratio = estimated remediation time ÷ development effort (18 man-days per 1,000 lines of code). "
            + "Ratings: A ≤ 5 %, B ≤ 10 %, C ≤ 20 %, D ≤ 50 %, E above. Generated by Strata in ForkMesh.</p>"
        html += "</body></html>\n"
        return html
    }
}
