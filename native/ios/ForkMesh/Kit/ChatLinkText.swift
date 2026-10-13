// The ChatLinkText view. The parsing it paints is in ChatLinkParsing.swift.

import FMApi
import FMChat
import FMCode
import SwiftUI

/// A chat message body whose links are tappable where they were written,
/// instead of having to be sent as a message of their own.
///
/// The caller styles the view from outside
/// (`ChatLinkText(text: body).fmText(.bodyLarge)`). Links draw in the accent,
/// underlined. `onOpen` overrides how a tapped link is opened; without it the
/// address goes to the system.
struct ChatLinkText: View {
    @Environment(\.fmPalette) private var fm

    private let text: String
    private let segments: [ChatTextSegment]
    private let onOpen: ((String) -> Void)?

    init(text: String, repo: RepoRef? = ReferenceLinks.chatRepo, onOpen: ((String) -> Void)? = nil) {
        self.text = text
        self.segments = referenceChatSegments(parseChatText(text), repo: repo)
        self.onOpen = onOpen
    }

    private var hasLinks: Bool {
        return segments.contains { (segment: ChatTextSegment) -> Bool in
            return segment.isLink
        }
    }

    private var attributed: AttributedString { chatRichAttributed(segments) }

    private var formatted: Bool { ChatRichText.hasFormatting(text) }

    private var opener: OpenURLAction {
        let handler: ((String) -> Void)? = onOpen
        return OpenURLAction { (url: URL) -> OpenURLAction.Result in
            guard let handler = handler else {
                return referenceOpenResult(url)
            }
            handler(url.absoluteString)
            return OpenURLAction.Result.handled
        }
    }

    var body: some View {
        if hasLinks || formatted {
            Text(attributed)
                .tint(fm.accent)
                .environment(\.openURL, opener)
        } else {
            Text(text)
        }
    }
}

func chatRichAttributed(_ segments: [ChatTextSegment]) -> AttributedString {
    var result: AttributedString = AttributedString()
    for segment in segments {
        if let address = segment.url, let url = URL(string: address) {
            var part: AttributedString = AttributedString(segment.text)
            part.link = url
            part.underlineStyle = Text.LineStyle.single
            result.append(part)
            continue
        }
        for span in ChatRichText.spans(segment.text) {
            var part: AttributedString = AttributedString(span.text)
            var intent: InlinePresentationIntent = []
            if span.bold { intent.insert(.stronglyEmphasized) }
            if span.italic { intent.insert(.emphasized) }
            if span.code { intent.insert(.code) }
            if !intent.isEmpty { part.inlinePresentationIntent = intent }
            if span.strike { part.strikethroughStyle = Text.LineStyle.single }
            result.append(part)
        }
    }
    return result
}
