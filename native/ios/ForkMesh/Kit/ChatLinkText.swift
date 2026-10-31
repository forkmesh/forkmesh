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
    private let profiles: Bool

    init(
        text: String, repo: RepoRef? = ReferenceLinks.chatRepo, profiles: Bool = true, onOpen: ((String) -> Void)? = nil
    ) {
        self.text = text
        self.segments = ChatTextCache.segments(text, repo: repo)
        self.profiles = profiles
        self.onOpen = onOpen
    }

    private var hasLinks: Bool {
        return segments.contains { (segment: ChatTextSegment) -> Bool in
            return segment.isLink
        }
    }

    private var attributed: AttributedString {
        chatRichAttributed(segments, own: mentionNames, mentions: ChatMentionColors.of(fm), profiles: profiles)
    }

    private var formatted: Bool { ChatRichText.hasFormatting(text) || text.contains("@") }

    private var opener: OpenURLAction {
        let handler: ((String) -> Void)? = onOpen
        return OpenURLAction { (url: URL) -> OpenURLAction.Result in
            guard let handler = handler, !url.absoluteString.hasPrefix(ProfileLinks.prefix) else {
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

enum ChatTextCache {
    private final class Box<Value> {
        let value: Value

        init(_ value: Value) {
            self.value = value
        }
    }

    static let countLimit: Int = 1024

    private static let segmentCache: NSCache<NSString, Box<[ChatTextSegment]>> = {
        let cache: NSCache<NSString, Box<[ChatTextSegment]>> = NSCache<NSString, Box<[ChatTextSegment]>>()
        cache.countLimit = countLimit
        return cache
    }()

    private static let attributedCache: NSCache<NSString, Box<AttributedString>> = {
        let cache: NSCache<NSString, Box<AttributedString>> = NSCache<NSString, Box<AttributedString>>()
        cache.countLimit = countLimit
        return cache
    }()

    private static func key(_ text: String, repo: RepoRef?) -> NSString {
        ((repo.map { "\($0.owner)/\($0.name)" } ?? "") + "\u{0}" + text) as NSString
    }

    static func segments(_ text: String, repo: RepoRef?) -> [ChatTextSegment] {
        let cacheKey: NSString = key(text, repo: repo)
        if let hit = segmentCache.object(forKey: cacheKey) { return hit.value }
        let value: [ChatTextSegment] = referenceChatSegments(parseChatText(text), repo: repo)
        segmentCache.setObject(Box(value), forKey: cacheKey)
        return value
    }

    static func attributed(_ text: String, repo: RepoRef?) -> AttributedString {
        let cacheKey: NSString = key(text, repo: repo)
        if let hit = attributedCache.object(forKey: cacheKey) { return hit.value }
        let value: AttributedString = chatRichAttributed(segments(text, repo: repo))
        attributedCache.setObject(Box(value), forKey: cacheKey)
        return value
    }
}

func chatRichAttributed(
    _ segments: [ChatTextSegment], own: [String] = [], mentions: ChatMentionColors? = nil, profiles: Bool = true
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
                span.code || (mentions == nil && !profiles) ? [ChatMentionPiece(span.text)] : ChatSmartMentions.split(span.text, own: own)
            for (index, piece) in pieces.enumerated() {
                var part: AttributedString = AttributedString(piece.text)
                var intent: InlinePresentationIntent = []
                if span.bold || (mentions != nil && piece.kind != .text) { intent.insert(.stronglyEmphasized) }
                if span.italic { intent.insert(.emphasized) }
                if span.code { intent.insert(.code) }
                if !intent.isEmpty { part.inlinePresentationIntent = intent }
                if span.strike { part.strikethroughStyle = Text.LineStyle.single }
                if profiles, piece.kind == .person, let url = chatProfileURL(pieces, index) {
                    part.link = url
                }
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

func chatProfileURL(_ pieces: [ChatMentionPiece], _ index: Int) -> URL? {
    if index + 1 < pieces.count, pieces[index + 1].text.lowercased().hasPrefix("[bot]") {
        return nil
    }
    return ProfileLinks.url(pieces[index].text)
}
