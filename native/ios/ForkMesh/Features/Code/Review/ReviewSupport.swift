import FMAccount
import FMApi
import FMBase
import FMCode
import FMControl
import FMCrypto
import SwiftUI

@MainActor
enum ReviewEnv {
    static var api: ApiService { CollabEnv.api }

    static var accountName: String { CollabEnv.auth?.session?.nodeName ?? "" }

    static var sessionToken: String { CollabEnv.auth?.session?.sessionToken ?? "" }

    static func writer(owner: String, name: String) -> CollabRecordWriter {
        let signer = CollabEnv.identity.map { CollabSigner(identity: $0, authorName: accountName) }
        return CollabRecordWriter(transport: api, signer: signer, owner: owner, name: name)
    }

    static func repository(_ ref: RepoRef) -> Repository {
        Repository(owner: ref.owner, name: ref.name)
    }

    static func settings(_ ref: RepoRef) -> RepoSettingsClient {
        RepoSettingsClient(transport: api, owner: ref.owner, name: ref.name, sessionToken: { ReviewEnv.sessionToken })
    }

    static var mergeQueue: MergeQueueRemote? {
        HostsFeature.hub.map { MergeQueueRemote(channel: $0) }
    }

    static func tint(_ tone: CollabTone) -> Color {
        switch tone {
        case .success: return .green
        case .danger: return .red
        case .accent, .accentEdge: return .accentColor
        case .secondary, .tertiary: return .secondary
        }
    }

    static func placementTint(_ placement: ReviewThreadPlacement) -> Color {
        switch placement {
        case .current, .legacy: return .accentColor
        case .moved: return .orange
        case .outdated: return .secondary
        }
    }
}

struct ReviewNoticeBanner: View {
    let notice: CollabNotice?

    var body: some View {
        if let notice {
            Label(notice.message, systemImage: notice.isError ? "exclamationmark.triangle" : "checkmark.circle")
                .font(.footnote)
                .foregroundStyle(notice.isError ? Color.red : Color.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(notice.message)
        }
    }
}

struct ReviewMarkdownText: View {
    let text: String
    let repo: RepoRef

    var body: some View {
        let markdown = ReferenceLinks.markdown(text, repo: repo)
        if let attributed = try? AttributedString(
            markdown: markdown, options: AttributedString.MarkdownParsingOptions(interpretedSyntax: .inlineOnlyPreservingWhitespace))
        {
            Text(attributed)
                .textSelection(.enabled)
                .environment(\.openURL, referenceOpener)
        } else {
            Text(text).textSelection(.enabled)
        }
    }
}

extension View {
    func routeDestinations() -> some View {
        navigationDestination(for: Route.self) { (route: Route) in
            AppServices.current?.routes.destination(for: route) ?? AnyView(EmptyView())
        }
    }
}
