import AVKit
import FMAccount
import FMAgents
import FMApi
import FMBase
import FMChat
import PhotosUI
import SwiftUI
import UniformTypeIdentifiers
#if canImport(UIKit)
import UIKit
#endif

struct ChatReactionBar: View {
    @Environment(\.fmPalette) private var fm
    let chips: [ReactionChip]
    let onToggle: (String) -> Void
    var onSticker: ((String) -> Void)? = nil

    var body: some View {
        if !chips.isEmpty {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: FmSpace.x2) {
                    ForEach(chips) { (chip: ReactionChip) in
                        Button(action: { onToggle(chip.value) }) {
                            HStack(spacing: 6) {
                                Text(chip.emoji).font(.system(size: 17))
                                Text("\(chip.count)")
                                    .font(.system(size: 15, weight: .semibold).monospacedDigit())
                                    .foregroundStyle(chip.mine ? fm.accent : fm.textPrimary)
                            }
                            .padding(.horizontal, 11)
                            .frame(minHeight: 34)
                            .background(Capsule().fill(chip.mine ? fm.accentSubtle : fm.bgOverlay))
                            .overlay(Capsule().stroke(chip.mine ? fm.accent.opacity(0.7) : fm.border, lineWidth: 1))
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel("\(chip.label), \(chip.count). \(chip.names.joined(separator: ", "))")
                    }
                    ChatAddReactionButton(onPick: onToggle)
                    if let onSticker {
                        ChatAddStickerButton(onPick: onSticker)
                    }
                }
                .padding(.vertical, 1)
            }
            .padding(.top, FmSpace.x2)
        }
    }
}

struct ChatAddReactionButton: View {
    @Environment(\.fmPalette) private var fm
    let onPick: (String) -> Void

    var body: some View {
        Menu {
            ForEach(ReactionCatalog.entries, id: \.value) { (entry: ReactionEntry) in
                Button("\(entry.emoji)  \(entry.label)") { onPick(entry.value) }
            }
        } label: {
            Image(systemName: "face.smiling")
                .font(.system(size: 18, weight: .regular))
                .foregroundStyle(fm.textSecondary)
                .overlay(alignment: .topTrailing) {
                    Image(systemName: "plus")
                        .font(.system(size: 8, weight: .black))
                        .foregroundStyle(fm.textSecondary)
                        .padding(1.5)
                        .background(Circle().fill(fm.bgOverlay))
                        .offset(x: 4, y: -3)
                }
                .frame(width: 44, height: 34)
                .background(Capsule().fill(fm.bgOverlay))
                .overlay(Capsule().stroke(fm.border, lineWidth: 1))
        }
        .accessibilityLabel("Add reaction")
    }
}

struct ChatReactionMenu: View {
    let onPick: (String) -> Void

    var body: some View {
        Menu {
            ForEach(ReactionCatalog.entries, id: \.value) { (entry: ReactionEntry) in
                Button("\(entry.emoji)  \(entry.label)") { onPick(entry.value) }
            }
        } label: {
            Label("React", systemImage: "face.smiling")
        }
    }
}

struct ChatStickerBadge: View {
    let chip: StickerChip
    let onToggle: (String) -> Void

    private var placedBy: String { chip.names.isEmpty ? "" : "Placed by \(chip.names.joined(separator: ", "))" }

    var body: some View {
        Button(action: { onToggle(chip.sticker.slug) }) {
            HStack(spacing: 3) {
                Text(chip.sticker.text)
                if !chip.countLabel.isEmpty {
                    Text(chip.countLabel)
                }
            }
            .font(.system(size: 12, weight: .bold))
            .foregroundStyle(Color.white)
            .lineLimit(1)
            .fixedSize()
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .background(
                RoundedRectangle(cornerRadius: 6, style: .continuous).fill(Color(fmARGB: 0xFF00_0000 | chip.sticker.rgb)))
            .overlay(RoundedRectangle(cornerRadius: 6, style: .continuous).stroke(Color.white, lineWidth: 2))
            .shadow(color: Color.black.opacity(0.28), radius: 2, x: 0, y: 1)
            .rotationEffect(.degrees(chip.sticker.tilt))
            .padding(3)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(placedBy)
        .contextMenu {
            if !placedBy.isEmpty {
                Text(placedBy)
            }
            Button(action: { onToggle(chip.sticker.slug) }) {
                Label(chip.mine ? "Remove your sticker" : "Add this sticker", systemImage: chip.mine ? "minus.circle" : "plus.circle")
            }
        }
        .accessibilityLabel("\(chip.sticker.text) sticker, \(chip.count). \(placedBy)")
        .accessibilityHint(chip.mine ? "Removes your sticker" : "Adds your sticker")
    }
}

struct ChatStickerStack: View {
    let chips: [StickerChip]
    let onToggle: ((String) -> Void)?

    var body: some View {
        if !chips.isEmpty {
            HStack(spacing: -8) {
                ForEach(chips) { (chip: StickerChip) in
                    ChatStickerBadge(chip: chip, onToggle: { (slug: String) in onToggle?(slug) })
                        .allowsHitTesting(onToggle != nil)
                }
            }
        }
    }
}

struct ChatAddStickerButton: View {
    @Environment(\.fmPalette) private var fm
    let onPick: (String) -> Void

    var body: some View {
        Menu {
            ChatStickerItems(onPick: onPick)
        } label: {
            Image(systemName: "seal")
                .font(.system(size: 17, weight: .regular))
                .foregroundStyle(fm.textSecondary)
                .frame(width: 44, height: 34)
                .background(Capsule().fill(fm.bgOverlay))
                .overlay(Capsule().stroke(fm.border, lineWidth: 1))
        }
        .accessibilityLabel("Add sticker")
    }
}

struct ChatStickerItems: View {
    let onPick: (String) -> Void

    var body: some View {
        ForEach(StickerCatalog.entries, id: \.slug) { (entry: StickerEntry) in
            Button("\(entry.fallback)  \(entry.text)") { onPick(entry.slug) }
        }
    }
}

struct ChatStickerMenu: View {
    let onPick: (String) -> Void

    var body: some View {
        Menu {
            ChatStickerItems(onPick: onPick)
        } label: {
            Label("Add sticker", systemImage: "seal")
        }
    }
}

struct ChatThreadSummaryButton: View {
    @Environment(\.fmPalette) private var fm
    let summary: ChatThreadSummary
    let onOpen: () -> Void

    var body: some View {
        Button(action: onOpen) {
            HStack(spacing: FmSpace.x2) {
                HStack(spacing: -6) {
                    ForEach(summary.faces, id: \.self) { (name: String) in
                        FmAvatar(name: name, size: 20, fontSize: FmType.micro)
                    }
                }
                Text(summary.label).fmText(.labelMedium, color: fm.accent)
                if let at = summary.latestAt {
                    Text("Last reply \(ChatHub.timeLabel(at))").fmText(.labelSmall, color: fm.textTertiary)
                }
            }
            .frame(minHeight: 32)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("\(summary.label), latest from \(summary.latestReplier)")
    }
}

struct ChatAttachmentView: View {
    @Environment(\.fmPalette) private var fm
    let fileName: String
    let attachment: ChatAttachment?
    @State private var player: AVPlayer?
    @State private var fullImage: Bool = false

    var body: some View {
        if let attachment {
            switch attachment.kind {
            case .image:
                image(attachment)
            case .audio, .video:
                media(attachment)
            case .file:
                fileRow(attachment)
            }
        } else {
            HStack(spacing: FmSpace.x1) {
                FmIcon("attach_file", size: 14, color: fm.textTertiary)
                Text(fileName).fmText(.bodyMedium, color: fm.accent).lineLimit(1)
            }
        }
    }

    @ViewBuilder private func image(_ attachment: ChatAttachment) -> some View {
        #if canImport(UIKit)
        if let picture = FmImageCache.image(attachment.data) {
            Button(action: { fullImage = true }) {
                Image(uiImage: picture)
                    .resizable()
                    .scaledToFit()
                    .frame(maxWidth: 260, maxHeight: 260, alignment: .leading)
                    .clipShape(FmRoundedShape(radius: FmRadius.md))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Image \(attachment.fileName)")
            .sheet(isPresented: $fullImage) {
                NavigationStack {
                    ScrollView([.horizontal, .vertical]) {
                        Image(uiImage: picture).resizable().scaledToFit()
                    }
                    .toolbar {
                        ToolbarItem(placement: .cancellationAction) { Button("Close") { fullImage = false } }
                        ToolbarItem(placement: .primaryAction) {
                            ShareLink(item: Image(uiImage: picture), preview: SharePreview(attachment.fileName, image: Image(uiImage: picture)))
                        }
                    }
                }
            }
        } else {
            fileRow(attachment)
        }
        #else
        fileRow(attachment)
        #endif
    }

    @ViewBuilder private func media(_ attachment: ChatAttachment) -> some View {
        if let player {
            VideoPlayer(player: player)
                .frame(width: 260, height: attachment.kind == .audio ? 64 : 180)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
        } else {
            Button(action: { player = ChatAttachmentView.makePlayer(attachment) }) {
                HStack(spacing: FmSpace.x2) {
                    FmIcon("play_arrow", size: 20, color: fm.accent)
                    Text(attachment.fileName).fmText(.bodyMedium, color: fm.accent).lineLimit(1)
                }
                .padding(.horizontal, FmSpace.x3)
                .frame(minHeight: 44)
                .background(fm.bgOverlay)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Play \(attachment.fileName)")
        }
    }

    private func fileRow(_ attachment: ChatAttachment) -> some View {
        HStack(spacing: FmSpace.x2) {
            FmIcon("insert_drive_file_outlined", size: 16, color: fm.textTertiary)
            Text(attachment.fileName).fmText(.bodyMedium, color: fm.accent).lineLimit(1)
            if let url = ChatAttachmentView.temporaryFile(attachment) {
                ShareLink(item: url) { Image(systemName: "square.and.arrow.up") }
            }
        }
    }

    static func temporaryFile(_ attachment: ChatAttachment) -> URL? {
        let safe = attachment.fileName.replacingOccurrences(of: "/", with: "_").replacingOccurrences(of: "..", with: "_")
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("chat-media", isDirectory: true)
        let url = directory.appendingPathComponent("\(attachment.messageId.prefix(24))-\(safe)")
        if FileManager.default.fileExists(atPath: url.path) { return url }
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try attachment.data.write(to: url, options: [.atomic, .completeFileProtection])
            return url
        } catch {
            return nil
        }
    }

    static let mediaFolder: String = "chat-media"
    static let leftoverPrefixes: [String] = ["viewonce-", "messages-voice"]

    static func sweepTemporaryFiles(includingRecordings: Bool) {
        let files: FileManager = FileManager.default
        let temporary: URL = files.temporaryDirectory
        let media: URL = temporary.appendingPathComponent(mediaFolder, isDirectory: true)
        var targets: [URL] = ((try? files.contentsOfDirectory(at: media, includingPropertiesForKeys: nil)) ?? [])
        if includingRecordings {
            let names: [String] = (try? files.contentsOfDirectory(atPath: temporary.path)) ?? []
            for name in names where leftoverPrefixes.contains(where: { name.hasPrefix($0) }) {
                targets.append(temporary.appendingPathComponent(name))
            }
        }
        for file in targets where file.standardizedFileURL.path.hasPrefix(temporary.standardizedFileURL.path) {
            try? files.removeItem(at: file)
        }
    }

    static func makePlayer(_ attachment: ChatAttachment) -> AVPlayer? {
        guard let url = temporaryFile(attachment) else { return nil }
        let player = AVPlayer(url: url)
        player.play()
        return player
    }
}

struct ChatAppBlocksView: View {
    @Environment(\.fmPalette) private var fm
    let app: AppMessage
    let busy: (String) -> Bool
    let onAction: (String, String) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            ForEach(Array(app.blocks.enumerated()), id: \.offset) { (_: Int, block: AppBlock) in
                blockView(block)
            }
        }
        .padding(.top, FmSpace.x1)
    }

    @ViewBuilder private func blockView(_ block: AppBlock) -> some View {
        switch block {
        case let .section(text, fields):
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                if !text.isEmpty {
                    Text(text).fmText(.bodyMedium).textSelection(.enabled)
                }
                if !fields.isEmpty {
                    LazyVGrid(
                        columns: [GridItem(.flexible(), alignment: .topLeading), GridItem(.flexible(), alignment: .topLeading)],
                        alignment: .leading, spacing: FmSpace.x1
                    ) {
                        ForEach(Array(fields.enumerated()), id: \.offset) { (_: Int, field: String) in
                            Text(field).fmText(.bodySmall, color: fm.textSecondary)
                        }
                    }
                }
            }
        case let .context(text):
            Text(text).fmText(.bodySmall, color: fm.textTertiary)
        case .divider:
            FmHairline()
        case let .actions(elements):
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: FmSpace.x2) {
                    ForEach(elements) { (element: AppBlockElement) in
                        elementView(element)
                    }
                }
            }
        }
    }

    @ViewBuilder private func elementView(_ element: AppBlockElement) -> some View {
        let enabled: Bool = !app.appMessageId.isEmpty && !busy(element.actionId)
        switch element {
        case let .button(actionId, text, value, style):
            FmToneButton(
                label: text, tone: style == .danger ? .danger : (style == .primary ? .accent : .neutral),
                filled: style == .primary, onPressed: enabled ? { onAction(actionId, value) } : nil)
        case let .select(actionId, placeholder, options):
            Menu {
                ForEach(options) { (option: AppSelectOption) in
                    Button(option.text) { onAction(actionId, option.value) }
                }
            } label: {
                HStack(spacing: FmSpace.x1) {
                    Text(placeholder).fmText(.labelLarge, color: fm.textPrimary).lineLimit(1)
                    FmIcon("expand_more", size: 18, color: fm.textSecondary)
                }
                .padding(.horizontal, FmSpace.x3)
                .frame(minHeight: 34)
                .background(fm.bgOverlay)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
            }
            .disabled(!enabled)
            .opacity(enabled ? 1 : 0.5)
            .accessibilityLabel(placeholder)
        }
    }
}

struct ChatComposerBar: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let placeholder: String
    let allowAttachments: Bool
    @Binding var draft: String
    @Binding var focused: Bool
    let onSend: () -> Void
    var glass: Bool = false
    @State private var photo: PhotosPickerItem?
    @State private var choosingPhoto: Bool = false
    @State private var importing: Bool = false
    @State private var recorder: MessagesVoiceRecorder = MessagesVoiceRecorder()

    private var mention: ChatMentionToken? { ChatComposerTools.mentionToken(draft) }

    private var suggestions: [String] {
        guard let token = mention else { return [] }
        let own: String = Services.get(AuthService.self)?.session?.nodeName ?? ""
        let tags: Set<String> = Set(ChatBroadcast.allCases.map(\.rawValue))
        return ChatComposerTools.mentionCandidates(hub.mentionNames + ["forkbot"], excluding: own, query: token.query, limit: 6)
            .filter { !tags.contains($0.lowercased()) }
    }

    private var broadcasts: [ChatBroadcast] {
        guard let token = mention else { return [] }
        return hub.broadcastSuggestions(token.query, in: hub.currentConversation)
    }

    private func broadcastLabel(_ broadcast: ChatBroadcast) -> String {
        let people: Int = hub.broadcastAudience(broadcast, in: hub.currentConversation)
        let who: String = broadcast == .here ? "online" : (people == 1 ? "person" : "people")
        return "\(broadcast.tag)  \u{2014}  \(broadcast.summary.replacingOccurrences(of: "Notify ", with: "notify ")) (\(people) \(who))"
    }

    private var commands: [ChatSlashCommand] { hub.slashCommands(draft) }

    private var channelSuggestions: [String] {
        guard let token = ChatComposerTools.channelToken(draft) else { return [] }
        return ChatComposerTools.channelCandidates(hub.directoryChannels.map(\.name), query: token.query)
    }

    private var emojiSuggestions: [ChatEmojiShortcode] {
        guard let token = ChatComposerTools.emojiToken(draft) else { return [] }
        return ChatEmojiShortcodes.candidates(token.query, limit: 8)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if !commands.isEmpty {
                suggestionRow(commands.map { "\($0.usage)  \u{2014}  \($0.summary)" }) { picked in
                    let command: String = String(picked.prefix { $0 != " " })
                    draft = command + " "
                }
            } else if !suggestions.isEmpty || !broadcasts.isEmpty {
                let tags: [ChatBroadcast] = broadcasts
                suggestionRow(suggestions.map { "@\($0)" } + tags.map(broadcastLabel)) { picked in
                    if let tag = tags.first(where: { picked.hasPrefix($0.tag + " ") }) {
                        draft = ChatComposerTools.insertMention(draft, name: tag.rawValue)
                    } else {
                        draft = ChatComposerTools.insertMention(draft, name: String(picked.dropFirst()))
                    }
                }
            } else if !channelSuggestions.isEmpty {
                suggestionRow(channelSuggestions.map { "#\($0)" }) { picked in
                    draft = ChatComposerTools.insertChannel(draft, name: String(picked.dropFirst()))
                }
            } else if !emojiSuggestions.isEmpty {
                let shown: [ChatEmojiShortcode] = emojiSuggestions
                suggestionRow(shown.map { "\($0.emoji)  :\($0.name):" }) { picked in
                    if let entry = shown.first(where: { "\($0.emoji)  :\($0.name):" == picked }) {
                        draft = ChatComposerTools.insertEmoji(draft, emoji: entry.emoji)
                    }
                }
            }
            if glass {
                glassRow
            } else {
                HStack(alignment: .bottom, spacing: FmSpace.x2) {
                    toolsMenu
                    field
                        .padding(.horizontal, FmSpace.x4)
                        .padding(.vertical, FmSpace.x3)
                        .background(fm.bgBase)
                        .clipShape(FmRoundedShape(radius: FmRadius.lg))
                    sendButton
                }
            }
        }
        .fmKeepsKeyboard()
        .photosPicker(isPresented: $choosingPhoto, selection: $photo, matching: .any(of: [.images, .videos]))
        .onChange(of: photo) { _, item in
            guard let item else { return }
            Task { await sendPhoto(item) }
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.item]) { result in
            if case .success(let url) = result { sendFile(url) }
        }
        .onAppear {
            recorder.onFinished = { (data: Data) in
                if !hub.sendAttachment(fileName: "Audio Message.m4a", mime: "audio/mp4", data: data) {
                    hub.post("That voice message could not be sent.")
                }
            }
            recorder.onRefused = { hub.post("Allow the microphone in Settings to record voice messages.") }
        }
        .onDisappear { recorder.finish(send: false) }
    }

    private var canRecord: Bool { allowAttachments && draft.dartTrimmed.isEmpty }

    private var recordingField: some View {
        HStack(spacing: FmSpace.x2) {
            Button(action: { recorder.finish(send: false) }) {
                Image(systemName: "xmark.circle.fill")
                    .font(.system(size: 24))
                    .foregroundStyle(fm.textTertiary)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Cancel voice message")
            Circle().fill(fm.danger).frame(width: 9, height: 9)
            TimelineView(.periodic(from: Date(), by: 0.5)) { (context: TimelineViewDefaultContext) in
                Text(MessagesAudioBubble.clock(context.date.timeIntervalSince(recorder.startedAt ?? context.date)))
                    .font(.system(size: 15).monospacedDigit())
                    .foregroundStyle(fm.textPrimary)
            }
            Text("Recording\u{2026}").fmText(.bodySmall, color: fm.textTertiary)
            Spacer(minLength: 0)
        }
        .padding(.leading, FmSpace.x3)
        .frame(minHeight: 48)
    }

    private var micButton: some View {
        Button(action: { recorder.start() }) {
            Image(systemName: "mic")
                .font(.system(size: 19, weight: .regular))
                .foregroundStyle(fm.textSecondary)
                .frame(width: 36, height: 36)
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Record a voice message")
    }

    private var field: some View {
        ChatTextField(
            text: $draft, placeholder: placeholder, focused: $focused, maxLines: 5,
            textColor: UIColor(fm.textPrimary), placeholderColor: UIColor(fm.textTertiary), tint: UIColor(fm.accent),
            onPasteImages: allowAttachments ? { (images: [PastedImage]) in Task { await ChatPastedImages.send(images, hub: hub) } } : nil)
            .accessibilityLabel("Message")
            .onChange(of: draft) { _, text in hub.composerChanged(text) }
    }

    private var glassRow: some View {
        HStack(alignment: .bottom, spacing: 0) {
            if recorder.recording {
                recordingField
                Button(action: { recorder.finish(send: true) }) {
                    FmIcon("send", size: 16, color: Color.white)
                        .frame(width: 36, height: 36)
                        .background(Circle().fill(fm.accent))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Send voice message")
                .padding(6)
            } else {
                toolsMenu
                field
                    .padding(.vertical, FmSpace.x3)
                    .frame(minHeight: 48)
                if canRecord && hub.composerConnected {
                    micButton.padding(6)
                } else {
                    glassSendButton
                        .padding(6)
                }
            }
        }
        .chatGlass(RoundedRectangle(cornerRadius: 24, style: .continuous), border: fm.border)
    }

    private var glassSendButton: some View {
        let connected: Bool = hub.composerConnected
        return Button(action: {
            if connected {
                FmHaptics.send()
                onSend()
            } else {
                hub.connect()
            }
        }) {
            FmIcon(connected ? "send" : "sync", size: 16, color: Color.white)
                .frame(width: 36, height: 36)
                .background(Circle().fill(connected ? fm.accent : fm.textTertiary))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(connected ? "Send message" : "Reconnect to chat")
    }

    private var toolsMenu: some View {
        Menu {
            Menu {
                ForEach(ChatComposerTools.emojiPicker, id: \.self) { (emoji: String) in
                    Button(emoji) { draft += emoji }
                }
            } label: {
                Label("Emoji", systemImage: "face.smiling")
            }
            Menu {
                ForEach(ChatTextFormat.allCases, id: \.self) { (format: ChatTextFormat) in
                    Button(action: { draft = ChatRichText.apply(format, to: draft) }) {
                        Label(format.title, systemImage: format.symbol)
                    }
                }
            } label: {
                Label("Format", systemImage: "textformat")
            }
            .disabled(draft.dartTrimmed.isEmpty)
            Button(action: insertLink) { Label("Insert link", systemImage: "link") }
            Button(action: { draft = (draft.isEmpty || draft.hasSuffix(" ") ? draft : draft + " ") + "@" }) {
                Label("Mention someone", systemImage: "at")
            }
            if !hub.broadcastSuggestions("", in: hub.currentConversation).isEmpty {
                Menu {
                    ForEach(hub.broadcastSuggestions("", in: hub.currentConversation)) { (tag: ChatBroadcast) in
                        Button(action: { draft = (draft.isEmpty || draft.hasSuffix(" ") ? draft : draft + " ") + tag.tag + " " }) {
                            Label("\(tag.tag) \u{2014} \(tag.summary)", systemImage: tag == .here ? "dot.radiowaves.left.and.right" : "megaphone")
                        }
                    }
                } label: {
                    Label("Notify the channel", systemImage: "megaphone")
                }
            }
            Button(action: { draft = "@forkbot " + draft }) { Label("Ask ForkBot", systemImage: "sparkles") }
            if allowAttachments {
                Button(action: { choosingPhoto = true }) { Label("Photo or video", systemImage: "photo") }
                Button(action: { importing = true }) { Label("File", systemImage: "paperclip") }
            }
        } label: {
            Group {
                if glass {
                    Image(systemName: "plus")
                        .font(.system(size: 19, weight: .regular))
                        .foregroundStyle(fm.textSecondary)
                        .frame(width: 36, height: 36)
                        .background(Circle().fill(fm.bgOverlay.opacity(fm.dark ? 0.9 : 1)))
                        .frame(width: 48, height: 48)
                } else {
                    FmIcon("add", size: 22, color: fm.textSecondary)
                        .frame(width: 44, height: 48)
                }
            }
            .accessibilityLabel("More")
        }
    }

    private var sendButton: some View {
        let connected: Bool = hub.composerConnected
        return Button(action: {
            if connected {
                onSend()
            } else {
                hub.connect()
            }
        }) {
            FmIcon(connected ? "send" : "sync", size: 18, color: Color.white)
                .frame(width: 48, height: 48)
                .background(fm.accent)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(connected ? "Send message" : "Reconnect to chat")
    }

    private func suggestionRow(_ items: [String], onPick: @escaping (String) -> Void) -> some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: FmSpace.x2) {
                ForEach(items, id: \.self) { (item: String) in
                    Button(action: { onPick(item) }) {
                        suggestionChip(
                            Text(item)
                                .fmText(.labelMedium, color: fm.textPrimary)
                                .lineLimit(1)
                                .padding(.horizontal, FmSpace.x3)
                                .frame(minHeight: 36))
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.bottom, FmSpace.x2)
        }
    }

    @ViewBuilder private func suggestionChip<Chip: View>(_ chip: Chip) -> some View {
        if glass {
            chip.shellGlass(Capsule())
        } else {
            chip
                .background(fm.bgOverlay)
                .clipShape(Capsule())
        }
    }

    private func insertLink() {
        #if canImport(UIKit)
        let clipboard: String = UIPasteboard.general.hasURLs ? (UIPasteboard.general.url?.absoluteString ?? "") : ""
        #else
        let clipboard: String = ""
        #endif
        let count: Int = draft.count
        draft = ChatComposerTools.linkInsertion(draft, selectionStart: count, selectionEnd: count, clipboard: clipboard).text
    }

    private func sendPhoto(_ item: PhotosPickerItem) async {
        defer { photo = nil }
        guard let data = try? await item.loadTransferable(type: Data.self) else {
            hub.post("That photo or video could not be read.")
            return
        }
        let type: UTType = item.supportedContentTypes.first ?? .jpeg
        if type.conforms(to: .movie) {
            let ext: String = type.preferredFilenameExtension ?? "mov"
            _ = hub.sendAttachment(fileName: "video.\(ext)", mime: type.preferredMIMEType ?? "video/quicktime", data: data)
            return
        }
        await ChatPastedImages.send([PastedImage(name: "photo", data: data)], hub: hub)
    }

    private func sendFile(_ url: URL) {
        let scoped: Bool = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        guard let data = try? Data(contentsOf: url) else { return }
        let type: UTType? = UTType(filenameExtension: url.pathExtension)
        _ = hub.sendAttachment(fileName: url.lastPathComponent, mime: type?.preferredMIMEType ?? "application/octet-stream", data: data)
    }
}
