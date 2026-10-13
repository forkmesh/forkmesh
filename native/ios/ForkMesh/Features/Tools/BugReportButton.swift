import SwiftUI

struct BugReportButton: View {
    @Environment(\.fmPalette) private var fm

    let onReport: () -> Void

    var body: some View {
        Button(action: {
            FmHaptics.tap()
            onReport()
        }) {
            FmGlassSurface(cornerRadius: FmRadius.full) {
                FmIcon("bug_report_outlined", size: 21, color: fm.textSecondary)
                    .frame(width: 48, height: 48)
            }
            .contentShape(Circle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.08), cornerRadius: FmRadius.full))
        .accessibilityLabel(Text("Report a bug"))
        .accessibilityHint(Text("Opens a form to describe a problem"))
        .accessibilityIdentifier("bug-report-button")
        .genieKeepsClear()
    }
}
