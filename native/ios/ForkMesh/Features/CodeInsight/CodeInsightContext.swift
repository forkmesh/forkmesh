import FMCodeInsight
import Foundation

struct CodeInsightContext {
    let snapshot: InsightSnapshot
    let repoTitle: String
    let branch: String
    let sourceLabel: String
    let openFile: (String, Int) -> Void
    let loadTrend: (String) async -> [InsightTrendPoint]
    let reanalyze: () -> Void
    let setSummary: (String) -> Void
}
