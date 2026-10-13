// A compact "you are posting as `username`" header above every compose surface (chat, issues, PRs, discussions, reviews,
// comments), with the account's avatar or an initials circle.

import FMAccount
import FMBase
import SwiftUI

struct ComposeIdentityBar: View {
    @Environment(\.fmPalette) private var fm
    @State private var auth: AuthService? = Services.get(AuthService.self)

    /// Optional leading verb ("Posting", "Replying", "Reviewing"); nil shows just "as name".
    private let verb: String?

    init(verb: String? = nil) {
        self.verb = verb
    }

    private var label: String {
        guard let verb = verb, !verb.isEmpty else { return "as " }
        return "\(verb) as "
    }

    var body: some View {
        if let session = auth?.session {
            let name: String = session.nodeName.isEmpty ? session.email : session.nodeName
            HStack(spacing: 8) {
                FmAvatar(name: name, avatarPng: session.avatarPng, size: 28, fontSize: FmType.meta)
                (Text(label).foregroundColor(fm.textSecondary)
                    + Text(name).fontWeight(FmType.emphasis).foregroundColor(fm.textPrimary))
                    .fmText(.bodyMedium)
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
            .padding(.vertical, 6)
        }
    }
}
