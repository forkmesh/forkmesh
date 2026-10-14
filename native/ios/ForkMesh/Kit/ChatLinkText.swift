// The ChatLinkText view. The parsing it paints is in ChatLinkParsing.swift.

import FMApi
import FMChat
import FMCode
import SwiftUI

struct ChatMentionNamesKey: EnvironmentKey {
    static let defaultValue: [String] = []
}

extension EnvironmentValues {
    var chatMentionNames: [String] {
        get { self[ChatMentionNamesKey.self] }
        set { self[ChatMentionNamesKey.self] = newValue }
    }
}

struct ChatMentionColors {
    var person: Color
    var you: Color
    var youBackground: Color

    static func of(_ fm: FmPalette) -> ChatMentionColors {
        ChatMentionColors(
            person: fm.accent, you: fm.dark ? Color(fmARGB: 0xFFFFD866) : Color(fmARGB: 0xFF8A5A00),
            youBackground: Color(fmARGB: fm.dark ? 0x40F2C744 : 0x55F2C744))
    }
}

/// A chat message body whose links are tappable where they were written,
/// instead of having to be sent as a message of their own.
///
/// The caller styles the view from outside
/// (`ChatLinkText(text: body).fmText(.bodyLarge)`). Links draw in the accent,
/// underlined. `onOpen` overrides how a tapped link is opened; without it the
/// address goes to the system.
struct ChatLinkText: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.chatMentionNames) private var mentionNames

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

    private var attributed: AttributedString {
        chatRichAttributed(segments, own: mentionNames, mentions: ChatMentionColors.of(fm))
    }

    private var formatted: Bool { ChatRichText.hasFormatting(text) || text.contains("@") }

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

func chatRichAttributed(
    _ segments: [ChatTextSegment], own: [String] = [], mentions: ChatMentionColors? = nil
) -> AttributedString {
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
            let pieces: [ChatMentionPiece] =
                mentions == nil || span.code ? [ChatMentionPiece(span.text)] : ChatSmartMentions.split(span.text, own: own)
            for piece in pieces {
                var part: AttributedString = AttributedString(piece.text)
                var intent: InlinePresentationIntent = []
                if span.bold || piece.kind != .text { intent.insert(.stronglyEmphasized) }
                if span.italic { intent.insert(.emphasized) }
                if span.code { intent.insert(.code) }
                if !intent.isEmpty { part.inlinePresentationIntent = intent }
                if span.strike { part.strikethroughStyle = Text.LineStyle.single }
                if let mentions {
                    switch piece.kind {
                    case .text:
                        break
                    case .person:
                        part.foregroundColor = mentions.person
                    case .you, .broadcast:
                        part.foregroundColor = mentions.you
                        part.backgroundColor = mentions.youBackground
                    }
                }
                result.append(part)
            }
        }
    }
    return result
}
