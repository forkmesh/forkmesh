// What serving as a node did to the repositories on this phone ("N committed
// here", "N left for a desktop", the push reminder and the top three reasons
// items went back). Shown under the local repositories; it renders nothing
// until the applier has handled an item.

import FMChat
import SwiftUI

struct LocalNodeSyncSummary: View {
    @Environment(\.fmPalette) private var fm
    let applier: LocalNodeSyncApplier

    var body: some View {
        if applier.appliedTotal > 0 || applier.leftTotal > 0 {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                HStack(spacing: FmSpace.x2) {
                    if applier.appliedTotal > 0 {
                        FmStatusBadge(label: "\(applier.appliedTotal) committed here", color: fm.success)
                    }
                    if applier.leftTotal > 0 {
                        FmStatusBadge(label: "\(applier.leftTotal) left for a desktop", color: fm.textSecondary)
                    }
                }
                if applier.appliedTotal > 0 {
                    Text("Applied items are committed to this device only. Push the repository to publish them.")
                        .fmText(.bodySmall, color: fm.textTertiary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                // Why items went back, so "left for a desktop" is never a
                // number the reader cannot act on.
                ForEach(Array(applier.leftReasons.prefix(3))) { (reason: NodeSyncLeftReason) in
                    Text("· \(reason.reason)\(reason.count > 1 ? " (×\(reason.count))" : "")")
                        .fmText(.bodySmall, color: fm.textTertiary)
                        .fixedSize(horizontal: false, vertical: true)
                        .fmMessageTools(reason.reason)
                }
            }
            .padding(EdgeInsets(top: 0, leading: FmSpace.x4, bottom: FmSpace.x3, trailing: FmSpace.x4))
        }
    }
}
