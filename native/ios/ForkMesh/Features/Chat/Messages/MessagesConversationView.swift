import FMAccount
import FMAgents
import FMApi
import FMBase
import FMChat
import PhotosUI
import SwiftUI
import UniformTypeIdentifiers
import UIKit

struct MessagesConversationView<Drawer: View>: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    let hub: ChatHub
    let onBack: (() -> Void)?
    let showsHeader: Bool
    let drawer: Drawer
    let actions: (ChatRow) -> ChatRowActions
    let onRoute: (Route) -> Void
    @Binding var draft: String
    @Binding var focused: Bool
    let onSend: () -> Void
    @Binding var notice: FmNotice?
    @State private var details: Bool = false
    @State private var tapbackTarget: ChatRow?
    @State private var viewing: MessagesPhotoBox?
    @State private var lastSeenId: String = ""
    @State private var drawing: Bool = false
    @State private var viewOnceOpen: ViewOnceEntry?
    @State private var jumpTarget: String?
    @State private var transcriptSize: CGSize = .zero

    private var look: ChatLook { hub.conversationLook }

    private var colors: MessagesColors {
        MessagesColors(dark: MessagesBackdrop.isDark(look) ?? fm.dark, backdrop: look.hasBackground)
    }

    var body: some View {
        let palette: MessagesColors = colors
        let conversation: String = hub.relay.currentConversation
        ZStack(alignment: .leading) {
            transcript(palette)
                .overlay {
                    if drawing, let live = hub.live, hub.inDirect {
                        MessagesLiveTouchSurface(
                            engine: live, conversation: conversation,
                            onDone: { withAnimation(.easeOut(duration: 0.2)) { drawing = false } }
                        )
                        .environment(\.colorScheme, palette.dark ? .dark : .light)
                        .transition(.opacity)
                    }
                }
                .safeAreaInset(edge: .top, spacing: 0) {
                    if showsHeader {
                        header(palette).transition(.move(edge: .top).combined(with: .opacity))
                    }
                }
                .shellDock {
                    VStack(spacing: 0) {
                        if notice != nil {
                            FmNoticeLine($notice)
                                .padding(.horizontal, FmSpace.x4)
                        }
                        MessagesComposer(
                            hub: hub, colors: palette, draft: $draft, focused: $focused, drawing: $drawing, onSend: onSend)
                    }
                }
                .background {
                    ZStack {
                        MessagesBackdrop(look: look, canvas: palette.canvas)
                        if let live = hub.live, hub.inDirect {
                            MessagesLiveInkLayer(engine: live, conversation: conversation)
                        }
                    }
                    .ignoresSafeArea()
                }
                .environment(\.colorScheme, palette.dark ? .dark : .light)
            if let target = tapbackTarget {
                tapbackOverlay(target)
            }
            drawer
        }
        .onChange(of: conversation) { (previous: String, _: String) in
            drawing = false
            stopSharing(previous)
            startSharing(conversation)
        }
        .onChange(of: hub.revealRequests) { _, _ in
            details = false
            tapbackTarget = nil
            viewing = nil
            viewOnceOpen = nil
        }
        .onAppear { startSharing(conversation) }
        .onDisappear { stopSharing(conversation) }
        .onChange(of: scenePhase) { _, phase in
            if phase == .active {
                startSharing(conversation)
            } else {
                stopSharing(conversation)
            }
        }
        .sheet(isPresented: $details) {
            MessagesDetailsScreen(hub: hub, onRoute: { route in
                details = false
                onRoute(route)
            })
        }
        .fullScreenCover(item: $viewing) { (box: MessagesPhotoBox) in MessagesPhotoViewer(attachment: box.attachment) }
        .fullScreenCover(item: $viewOnceOpen) { (entry: ViewOnceEntry) in MessagesViewOnceViewer(hub: hub, entry: entry) }
    }

    private func transcript(_ palette: MessagesColors) -> some View {
        let items: [ChatConversationItem] = hub.conversationItems
        let typing: [String] = hub.conversationTyping
        let group: Bool = hub.inGroupConversation
        let known: Set<String> = Set(items.map(\.id))
        return ScrollViewReader { (proxy: ScrollViewProxy) in
            ScrollView {
                LazyVStack(spacing: 0) {
                    if items.isEmpty {
                        MessagesEmptyIntro(hub: hub, colors: palette)
                    }
                    ForEach(items) { (item: ChatConversationItem) in
                        itemView(item, palette: palette, group: group, known: known)
                            .id(item.id)
                    }
                    if !typing.isEmpty {
                        TimelineView(.periodic(from: Date(), by: 1)) { (_: TimelineViewDefaultContext) in
                            let names: [String] = hub.conversationTyping
                            if !names.isEmpty { MessagesTypingBubble(colors: palette, names: names) }
                        }
                    }
                    Color.clear.frame(height: 6).id(MessagesConversationView.endId)
                }
                .padding(.top, FmSpace.x2)
            }
            .defaultScrollAnchor(.bottom)
            .scrollDismissesKeyboard(.interactively)
            .scrollContentBackground(.hidden)
            .onGeometryChange(for: CGSize.self, of: { $0.size }) { (size: CGSize) in transcriptSize = size }
            .onTapGesture(coordinateSpace: .local) { (location: CGPoint) in transcriptTapped(at: location) }
            .onAppear { scrollToEnd(proxy, items, animated: false) }
            .onChange(of: items.last?.id ?? "") { _, _ in scrollToEnd(proxy, items, animated: true) }
            .onChange(of: typing.isEmpty) { _, empty in
                if !empty { withAnimation(.easeOut(duration: 0.2)) { proxy.scrollTo(MessagesConversationView.endId, anchor: .bottom) } }
            }
            .chatKeepsEndInView(focused: focused) {
                withAnimation(.easeOut(duration: 0.25)) { proxy.scrollTo(MessagesConversationView.endId, anchor: .bottom) }
            }
            .onTabReselect(.chat) {
                withAnimation(.easeOut(duration: 0.25)) { proxy.scrollTo(MessagesConversationView.endId, anchor: .bottom) }
            }
            .onChange(of: jumpTarget) { _, target in
                guard let target else { return }
                jumpTarget = nil
                withAnimation(.easeOut(duration: 0.25)) { proxy.scrollTo(target, anchor: .center) }
            }
        }
    }

    static var endId: String { "messages-end" }

    @ViewBuilder private func itemView(
        _ item: ChatConversationItem, palette: MessagesColors, group: Bool, known: Set<String>
    ) -> some View {
        switch item {
        case .time(_, let date):
            TimelineView(.everyMinute) { (context: TimelineViewDefaultContext) in
                MessagesSystemLine(text: ChatConversationLayout.timeLabel(date, now: context.date), colors: palette, bold: true)
            }
        case .event(let event):
            MessagesSystemLine(text: event.text, colors: palette)
        case .message(let bubble):
            if let entry = hub.viewOnce(for: bubble.row.message) {
                MessagesViewOnceRow(
                    bubble: bubble, entry: entry, group: group, colors: palette,
                    busy: hub.direct?.viewOnce.isBusy(entry.messageId) ?? false,
                    onOpen: { viewOnceOpen = entry },
                    onUnsend: { Task { await hub.unsendViewOnce(entry.messageId) } })
            } else {
                messageRow(bubble, palette: palette, group: group, known: known)
            }
        }
    }

    private func messageRow(_ bubble: ChatBubble, palette: MessagesColors, group: Bool, known: Set<String>) -> some View {
        let quote: ChatReplyQuote? = bubble.row.message.replyTo
        let shown: Bool = quote.map { known.contains($0.id) } ?? false
        return MessagesBubbleRow(
            bubble: bubble, group: group, colors: palette, actions: actions(bubble.row),
            onTapback: {
                FmHaptics.selection()
                withAnimation(.spring(response: 0.3, dampingFraction: 0.8)) { tapbackTarget = bubble.row }
            },
            onPhoto: { viewing = MessagesPhotoBox(attachment: $0) },
            quoteAuthor: quote.map { hub.replyAuthor($0, in: bubble.row.message.conversation) } ?? "",
            onQuote: shown ? { jumpTarget = quote?.id } : nil,
            life: life(bubble))
    }

    private func life(_ bubble: ChatBubble) -> MessagesBubbleLife {
        let message: ChatMessage = bubble.row.message
        return MessagesBubbleLife(
            expiresAt: hub.expiresAt(message), lifetime: hub.lifetime(message.conversation),
            onServer: hub.isOnServer(message, receipt: bubble.receipt))
    }

    private func transcriptTapped(at location: CGPoint) {
        if focused {
            focused = false
            return
        }
        guard let live = hub.live, hub.inDirect, transcriptSize.width > 0 else { return }
        live.tap(hub.relay.currentConversation, at: DirectLiveGeometry.point(location, in: transcriptSize))
        DirectLiveHaptics.sent()
    }

    private func stopSharing(_ conversation: String) {
        guard let live = hub.live else { return }
        live.setSharingMotion(conversation, false)
        if live.sharingMotion.isEmpty { DirectLiveMotionFeed.shared.stop() }
    }

    private func startSharing(_ conversation: String) {
        guard scenePhase == .active, let live = hub.live, DirectLiveMotionFeed.shared.available else { return }
        live.setSharingMotion(conversation, true)
        DirectLiveMotionFeed.shared.start(live)
    }

    private func scrollToEnd(_ proxy: ScrollViewProxy, _ items: [ChatConversationItem], animated: Bool) {
        guard let last = items.last else { return }
        if last.id == lastSeenId { return }
        lastSeenId = last.id
        if animated {
            withAnimation(.easeOut(duration: 0.2)) { proxy.scrollTo(MessagesConversationView.endId, anchor: .bottom) }
        } else {
            proxy.scrollTo(MessagesConversationView.endId, anchor: .bottom)
        }
    }

    private func tapbackOverlay(_ row: ChatRow) -> some View {
        let chosen: Set<String> = Set(row.reactions.filter(\.mine).map(\.value))
        return ZStack {
            Color.black.opacity(0.25)
                .ignoresSafeArea()
                .onTapGesture { closeTapback() }
            VStack(spacing: FmSpace.x3) {
                MessagesTapbackPicker(chosen: chosen) { (value: String) in
                    actions(row).onReact?(value)
                    FmHaptics.tap()
                    closeTapback()
                }
                if !row.reactions.isEmpty {
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(row.reactions) { (chip: ReactionChip) in
                            Text("\(chip.emoji)  \(chip.names.joined(separator: ", "))")
                                .font(.system(size: 14))
                                .foregroundStyle(Color.white)
                                .lineLimit(2)
                        }
                    }
                    .padding(FmSpace.x3)
                    .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Color.black.opacity(0.55)))
                }
            }
            .padding(FmSpace.x4)
            .transition(.scale(scale: 0.85).combined(with: .opacity))
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .zIndex(3)
    }

    private func closeTapback() {
        withAnimation(.easeOut(duration: 0.18)) { tapbackTarget = nil }
    }

    private var headerDetail: String {
        [hub.conversationSubtitle, hub.serverCopiesLine].filter { !$0.isEmpty }.joined(separator: " \u{00B7} ")
    }

    @ViewBuilder private func livePhone(_ palette: MessagesColors) -> some View {
        if let live = hub.live, hub.inDirect, !hub.inGroupConversation {
            MessagesLivePartnerPhone(
                engine: live, conversation: hub.relay.currentConversation, name: hub.conversationName, dark: palette.dark
            ) {
                MessagesConversationAvatar(hub: hub, size: 22)
            }
        }
    }

    private func header(_ palette: MessagesColors) -> some View {
        let ink: Color = palette.dark ? Color.white : Color.black
        let detail: String = headerDetail
        return HStack(alignment: .center, spacing: FmSpace.x2) {
            if let onBack {
                Button(action: onBack) {
                    ZStack(alignment: .topTrailing) {
                        Image(systemName: "chevron.left")
                            .font(.system(size: 19, weight: .semibold))
                            .foregroundStyle(ink)
                            .frame(width: 44, height: 44)
                        if hub.otherRoomsUnread {
                            Circle().fill(fm.danger).frame(width: 9, height: 9).padding(7)
                        }
                    }
                    .chatGlass(Circle(), border: fm.border)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Conversations")
            }
            Button(action: { details = true }) {
                HStack(spacing: FmSpace.x2) {
                    MessagesConversationAvatar(hub: hub, size: 38)
                    VStack(alignment: .leading, spacing: 1) {
                        HStack(spacing: 3) {
                            Text(hub.conversationName)
                                .font(.system(size: 16, weight: .semibold))
                                .foregroundStyle(ink)
                                .lineLimit(1)
                            Image(systemName: "chevron.right")
                                .font(.system(size: 10, weight: .bold))
                                .foregroundStyle(palette.meta)
                        }
                        if !detail.isEmpty {
                            Text(detail)
                                .font(.system(size: 12))
                                .foregroundStyle(palette.meta)
                                .lineLimit(1)
                                .accessibilityIdentifier("chat-direct-server-copies")
                        }
                    }
                    .layoutPriority(1)
                }
                .padding(.leading, 3)
                .padding(.trailing, FmSpace.x3)
                .padding(.vertical, 3)
                .frame(minHeight: 44)
                .chatGlass(Capsule(), border: fm.border)
            }
            .buttonStyle(.plain)
            .accessibilityElement(children: .combine)
            .accessibilityLabel(
                [hub.conversationName, hub.conversationSubtitle, hub.serverCopiesLine]
                    .filter { !$0.isEmpty }.joined(separator: ", "))
            .accessibilityHint("Shows details, people, photos and the background")
            .accessibilityIdentifier("chat-room-header")
            Spacer(minLength: 0)
            livePhone(palette)
            DirectCallHeaderButtons(hub: hub, dark: palette.dark)
                .dynamicTypeSize(...DynamicTypeSize.xxxLarge)
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.top, FmSpace.x1)
        .padding(.bottom, FmSpace.x2)
        .background {
            LinearGradient(
                colors: [Color.black.opacity(palette.dark ? 0.35 : 0.08), Color.clear], startPoint: .top, endPoint: .bottom)
                .ignoresSafeArea(edges: .top)
                .allowsHitTesting(false)
        }
    }
}

struct MessagesEmptyIntro: View {
    let hub: ChatHub
    let colors: MessagesColors

    var body: some View {
        VStack(spacing: FmSpace.x2) {
            MessagesConversationAvatar(hub: hub, size: 72)
            Text(hub.conversationName)
                .font(.system(size: 20, weight: .semibold))
                .foregroundStyle(colors.dark ? Color.white : Color.black)
            Text(hub.introLine)
                .font(.system(size: 13))
                .foregroundStyle(colors.meta)
                .multilineTextAlignment(.center)
            Label("End-to-end encrypted", systemImage: "lock.fill")
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(colors.meta)
        }
        .padding(.horizontal, FmSpace.x6)
        .padding(.vertical, FmSpace.x7)
        .frame(maxWidth: .infinity)
    }
}

struct MessagesConversationAvatar: View {
    let hub: ChatHub
    let size: CGFloat

    var body: some View {
        Group {
            if hub.inGroupConversation {
                groupAvatar
            } else {
                MessagesPersonAvatar(name: hub.conversationName, size: size)
            }
        }
        .frame(width: size, height: size)
        .accessibilityHidden(true)
    }

    @ViewBuilder private var groupAvatar: some View {
        if let data = hub.conversationLook.photo, let picture = FmImageCache.image(data) {
            Image(uiImage: picture)
                .resizable()
                .scaledToFill()
                .frame(width: size, height: size)
                .clipShape(Circle())
        } else {
            stacked
        }
    }

    private var stacked: some View {
        let others: [String] = Array(hub.conversationMembers.dropFirst().prefix(2))
        return ZStack {
            Circle().fill(Color(fmARGB: 0xFF9AA0A6))
            if others.count >= 2 {
                FmAvatar(name: others[0], size: size * 0.55, fontSize: size * 0.24).offset(x: -size * 0.14, y: -size * 0.1)
                FmAvatar(name: others[1], size: size * 0.55, fontSize: size * 0.24).offset(x: size * 0.14, y: size * 0.12)
            } else {
                Image(systemName: "person.2.fill")
                    .font(.system(size: size * 0.4))
                    .foregroundStyle(Color.white)
            }
        }
    }
}

struct MessagesPersonAvatar: View {
    let name: String
    let size: CGFloat
    @State private var pictures: MessagesPeoplePictures = MessagesPeoplePictures.shared
    @State private var avatars: ChatAvatarCache = ChatAvatarCache.shared

    var body: some View {
        let custom: String = pictures.picture(for: name)
        FmAvatar(name: name, avatarPng: custom.isEmpty ? avatars.png(for: name) : custom, size: size, fontSize: size * 0.42)
            .task(id: name) { await avatars.refresh(name) }
    }
}

struct MessagesComposer: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let colors: MessagesColors
    @Binding var draft: String
    @Binding var focused: Bool
    @Binding var drawing: Bool
    let onSend: () -> Void
    @State private var photos: [PhotosPickerItem] = []
    @State private var choosingPhotos: Bool = false
    @State private var camera: Bool = false
    @State private var background: Bool = false
    @State private var importing: Bool = false
    @State private var recorder: MessagesVoiceRecorder = MessagesVoiceRecorder()
    @State private var viewOnceRecording: MessagesViewOnceRequest?

    private var hasText: Bool { !draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }

    var body: some View {
        VStack(spacing: 0) {
            if let quote = hub.reply(in: hub.currentConversation), hub.composeBlocked == nil {
                replyBar(quote)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
            composer
        }
        .animation(.easeOut(duration: 0.18), value: hub.replyingTo)
    }

    private func replyBar(_ quote: ChatReplyQuote) -> some View {
        HStack(alignment: .center, spacing: FmSpace.x2) {
            MessagesReplyQuote(
                quote: quote, author: hub.replyAuthor(quote, in: hub.currentConversation), inSent: false, colors: colors)
            Button(action: { hub.cancelReply() }) {
                Image(systemName: "xmark.circle.fill")
                    .font(.system(size: 22))
                    .symbolRenderingMode(.hierarchical)
                    .foregroundStyle(colors.meta)
                    .frame(width: 44, height: 44)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Cancel reply")
        }
        .padding(.leading, FmSpace.x3)
        .padding(.trailing, FmSpace.x1)
        .padding(.top, FmSpace.x2)
    }

    private var composer: some View {
        Group {
            if let blocked = hub.composeBlocked {
                GuestComposeNotice(action: blocked, onLogIn: {
                    if let signIn = Services.get(SignInCoordinator.self) {
                        Task { _ = await signIn.requireAccount(action: blocked) }
                    }
                })
                .padding(FmSpace.x3)
            } else {
                bar
            }
        }
        .fmKeepsKeyboard()
        .photosPicker(isPresented: $choosingPhotos, selection: $photos, maxSelectionCount: 6, matching: .images)
        .onChange(of: photos) { _, picked in
            guard !picked.isEmpty else { return }
            Task { await send(picked) }
        }
        .sheet(isPresented: $background) { MessagesBackgroundPicker(hub: hub) }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.item]) { result in
            if case .success(let url) = result { sendFile(url) }
        }
        .fullScreenCover(item: $viewOnceRecording) { (request: MessagesViewOnceRequest) in
            MessagesViewOnceRecorderSheet(hub: hub, kind: request.kind)
        }
        .fullScreenCover(isPresented: $camera) {
            MessagesCameraPicker(
                onImage: { (image: UIImage) in Task { await sendImage(image) } }, onClose: { camera = false })
                .ignoresSafeArea()
        }
        .onAppear {
            recorder.onFinished = { (data: Data) in
                Task { _ = await hub.sendMedia(fileName: "Audio Message.m4a", mime: "audio/mp4", data: data) }
            }
            recorder.onRefused = { hub.post("Allow the microphone in Settings to record voice messages.") }
        }
        .onDisappear { recorder.finish(send: false) }
    }

    private var bar: some View {
        HStack(alignment: .bottom, spacing: FmSpace.x2) {
            plusMenu
            DirectLiveTouchButton(hub: hub, dark: colors.dark, drawing: $drawing, focused: $focused)
            if recorder.recording {
                recordingField
            } else {
                field
            }
        }
        .padding(.horizontal, 10)
        .padding(.top, FmSpace.x2)
        .padding(.bottom, FmSpace.x2)
    }

    private var plusMenu: some View {
        Menu {
            if hub.canSendMedia {
                Button(action: { choosingPhotos = true }) { Label("Photos", systemImage: "photo.on.rectangle") }
                if MessagesCameraPicker.available {
                    Button(action: { camera = true }) { Label("Camera", systemImage: "camera") }
                }
                if !OfficeFrames.isOffice(hub.currentConversation) {
                    Button(action: { importing = true }) { Label("File", systemImage: "paperclip") }
                }
            }
            if hub.canSendViewOnce {
                Button(action: { viewOnceRecording = MessagesViewOnceRequest(kind: .video) }) {
                    Label("Video Message (Plays Once)", systemImage: "video.badge.ellipsis")
                }
                .accessibilityLabel("Record a view-once video message")
                Button(action: { viewOnceRecording = MessagesViewOnceRequest(kind: .voice) }) {
                    Label("Voice Message (Plays Once)", systemImage: "waveform.badge.mic")
                }
                .accessibilityLabel("Record a view-once voice message")
            }
            if hub.canChangeLook {
                Button(action: { background = true }) { Label("Background", systemImage: "photo.artframe") }
            }
            Menu {
                ForEach(ChatComposerTools.emojiPicker, id: \.self) { (emoji: String) in
                    Button(emoji) { draft += emoji }
                }
            } label: {
                Label("Emoji", systemImage: "face.smiling")
            }
        } label: {
            Image(systemName: "plus")
                .font(.system(size: 18, weight: .medium))
                .foregroundStyle(colors.dark ? Color.white : Color.black.opacity(0.7))
                .frame(width: 38, height: 38)
                .chatGlass(Circle(), border: fm.border)
        }
        .accessibilityLabel("More")
    }

    private var field: some View {
        HStack(alignment: .bottom, spacing: 4) {
            ChatTextField(
                text: $draft, placeholder: "Message", focused: $focused, maxLines: 6, fontSize: 17,
                textColor: colors.dark ? .white : .black,
                placeholderColor: UIColor.placeholderText.resolvedColor(
                    with: UITraitCollection(userInterfaceStyle: colors.dark ? .dark : .light)),
                tint: UIColor(MessagesColors.blue),
                onPasteImages: hub.canSendMedia ? { (images: [PastedImage]) in Task { await ChatPastedImages.send(images, hub: hub) } } : nil)
                .padding(.leading, 14)
                .padding(.vertical, 2)
                .accessibilityLabel("Message")
                .onChange(of: draft) { _, text in hub.composerChanged(text) }
            trailingButton
                .padding(.trailing, 4)
                .padding(.bottom, 4)
        }
        .frame(minHeight: 38)
        .chatGlass(RoundedRectangle(cornerRadius: 19, style: .continuous), border: fm.border)
    }

    @ViewBuilder private var trailingButton: some View {
        if !hub.composerConnected {
            Button(action: { hub.connect() }) {
                Image(systemName: "arrow.clockwise.circle.fill")
                    .font(.system(size: 28))
                    .foregroundStyle(fm.textTertiary)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Reconnect to chat")
        } else if hasText {
            Button(action: {
                FmHaptics.send()
                onSend()
            }) {
                Image(systemName: "arrow.up.circle.fill")
                    .font(.system(size: 28))
                    .symbolRenderingMode(.palette)
                    .foregroundStyle(Color.white, MessagesColors.blue)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Send message")
        } else if hub.canSendMedia {
            Button(action: { recorder.start() }) {
                Image(systemName: "mic.fill")
                    .font(.system(size: 17))
                    .foregroundStyle(colors.meta)
                    .frame(width: 30, height: 30)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Record a voice message")
        }
    }

    private var recordingField: some View {
        HStack(spacing: FmSpace.x2) {
            Button(action: { recorder.finish(send: false) }) {
                Image(systemName: "xmark.circle.fill")
                    .font(.system(size: 26))
                    .foregroundStyle(fm.textTertiary)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Cancel voice message")
            Circle().fill(Color.red).frame(width: 9, height: 9)
            TimelineView(.periodic(from: Date(), by: 0.5)) { (context: TimelineViewDefaultContext) in
                Text(MessagesAudioBubble.clock(context.date.timeIntervalSince(recorder.startedAt ?? context.date)))
                    .font(.system(size: 15).monospacedDigit())
                    .foregroundStyle(colors.dark ? Color.white : Color.black)
            }
            Spacer(minLength: 0)
            Button(action: { recorder.finish(send: true) }) {
                Image(systemName: "arrow.up.circle.fill")
                    .font(.system(size: 28))
                    .symbolRenderingMode(.palette)
                    .foregroundStyle(Color.white, MessagesColors.blue)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Send voice message")
        }
        .padding(.horizontal, FmSpace.x2)
        .padding(.vertical, 4)
        .frame(minHeight: 38)
        .chatGlass(RoundedRectangle(cornerRadius: 19, style: .continuous), border: fm.border)
    }

    private func send(_ picked: [PhotosPickerItem]) async {
        defer { photos = [] }
        for item in picked {
            guard let data = try? await item.loadTransferable(type: Data.self) else { continue }
            guard let jpeg = MessagesImages.jpeg(data, maxSide: 1_600, maxBytes: ChatConversationLayout.maxMediaBytes) else {
                hub.post("That photo could not be prepared.")
                continue
            }
            _ = await hub.sendMedia(fileName: MessagesImages.photoName(), mime: "image/jpeg", data: jpeg)
        }
    }

    private func sendImage(_ image: UIImage) async {
        guard let jpeg = MessagesImages.jpeg(image, maxSide: 1_600, maxBytes: ChatConversationLayout.maxMediaBytes) else {
            hub.post("That photo could not be prepared.")
            return
        }
        _ = await hub.sendMedia(fileName: MessagesImages.photoName(), mime: "image/jpeg", data: jpeg)
    }

    private func sendFile(_ url: URL) {
        let scoped: Bool = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        guard let data = try? Data(contentsOf: url) else { return }
        let type: UTType? = UTType(filenameExtension: url.pathExtension)
        let mime = type?.preferredMIMEType ?? "application/octet-stream"
        if DirectMessageFrames.isDirect(hub.currentConversation) {
            Task { await hub.sendDirectFile(fileName: url.lastPathComponent, mime: mime, data: data) }
            return
        }
        _ = hub.sendAttachment(fileName: url.lastPathComponent, mime: mime, data: data)
    }
}
