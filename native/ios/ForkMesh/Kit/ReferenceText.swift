import FMApi
import FMCode
import SwiftUI

@MainActor
func openReferenceURL(_ url: URL) -> Bool {
    let scheme: String = url.scheme?.lowercased() ?? ""
    guard scheme == "forkmesh" || scheme == "https" else {
        return false
    }
    return AppNavigator.shared?.handle(url: url) == true
}

func referenceOpenResult(_ url: URL) -> OpenURLAction.Result {
    let handled: Bool = MainActor.assumeIsolated { () -> Bool in
        return openReferenceURL(url)
    }
    return handled ? OpenURLAction.Result.handled : OpenURLAction.Result.systemAction
}

let referenceOpener: OpenURLAction = OpenURLAction { (url: URL) -> OpenURLAction.Result in
    return referenceOpenResult(url)
}

func referenceAttributed(_ runs: [ReferenceRun]) -> AttributedString {
    var result: AttributedString = AttributedString()
    for run in runs {
        var part: AttributedString = AttributedString(run.text)
        if let address = run.url, let url = URL(string: address) {
            part.link = url
            if address.hasPrefix(ProfileLinks.prefix) {
                part.inlinePresentationIntent = .stronglyEmphasized
            } else {
                part.underlineStyle = Text.LineStyle.single
            }
        }
        result.append(part)
    }
    return result
}

func referenceChatSegments(_ segments: [ChatTextSegment], repo: RepoRef?) -> [ChatTextSegment] {
    var out: [ChatTextSegment] = []
    for segment in segments {
        if segment.isLink {
            out.append(segment)
            continue
        }
        for run in ReferenceLinks.runs(segment.text, repo: repo, commits: false, mentions: false) {
            out.append(ChatTextSegment(run.text, run.url))
        }
    }
    return out
}

struct LinkedText: View {
    @Environment(\.fmPalette) private var fm

    let text: String
    let repo: RepoRef?
    let commits: Bool

    init(_ text: String, repo: RepoRef?, commits: Bool = true) {
        self.text = text
        self.repo = repo
        self.commits = commits
    }

    var body: some View {
        let runs: [ReferenceRun] = ReferenceLinks.runs(text, repo: repo, commits: commits)
        if runs.contains(where: { (run: ReferenceRun) -> Bool in run.url != nil }) {
            Text(referenceAttributed(runs))
                .tint(fm.accent)
                .environment(\.openURL, referenceOpener)
        } else {
            Text(text)
        }
    }
}

func referenceRepo(href: String, slug: String = "") -> RepoRef {
    if let repo = RepoRef(slug: slug.trimmingCharacters(in: .whitespacesAndNewlines)) {
        return repo
    }
    let trimmed: String = href.trimmingCharacters(in: .whitespacesAndNewlines)
    if let url = URL(string: trimmed.hasPrefix("/") ? "https://forkmesh.com" + trimmed : trimmed),
        let repo = RouteParser.parse(url)?.stack.last?.repoRef
    {
        return repo
    }
    return ReferenceLinks.chatRepo
}

private struct ReferenceRepoKey: EnvironmentKey {
    static let defaultValue: RepoRef? = nil
}

extension EnvironmentValues {
    var referenceRepo: RepoRef? {
        get { self[ReferenceRepoKey.self] }
        set { self[ReferenceRepoKey.self] = newValue }
    }
}
