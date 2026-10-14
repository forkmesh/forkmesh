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
    @Environment(\.chatShellActions) private var shell
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
    @State private var touching: Bool = false
    @State private var viewOnceOpen: ViewOnceEntry?

    private var look: ChatLook { hub.conversationLook }

    private var colors: MessagesColors {
        MessagesColors(dark: MessagesBackdrop.isDark(look) ?? fm.dark, backdrop: look.hasBackground)
    }

    var body: some View {
        let palette: MessagesColors = colors
        let conversation: String = hub.relay.currentConversation
        ZStack(alignment: .leading) {
            transcript(palette)
                .overlay(alignment: .topTrailing) {
                    if let live = hub.live, hub.inDirect {
                        MessagesLivePartnerMotion(engine: live, conversation: conversation, partner: hub.conversationName)
                            .padding(.trailing, FmSpace.x3)
                            .padding(.top, FmSpace.x2)
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
                            hub: hub, colors: palette, draft: $draft, focused: $focused, touching: $touching, onSend: onSend)
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
            if touching, let live = hub.live, hub.inDirect {
                MessagesLiveTouchSurface(
                    engine: live, conversation: conversation, partner: hub.conversationName,
                    onDone: { withAnimation(.easeOut(duration: 0.2)) { touching = false } }
                )
                .environment(\.colorScheme, palette.dark ? .dark : .light)
                .transition(.opacity)
                .zIndex(2)
            }
            drawer
        }
        .onChange(of: conversation) { (previous: String, _: String) in
            touching = false
            stopSharing(previous)
        }
        .onDisappear { stopSharing(conversation) }
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
        return ScrollViewReader { (proxy: ScrollViewProxy) in
            ScrollView {
                LazyVStack(spacing: 0) {
                    if items.isEmpty {
                        MessagesEmptyIntro(hub: hub, colors: palette)
                    }
                    ForEach(items) { (item: ChatConversationItem) in
                        itemView(item, palette: palette, group: group)
                            .id(item.id)
                    }
                    if !typing.isEmpty {
                        MessagesTypingBubble(colors: palette, names: typing)
                    }
                    Color.clear.frame(height: 6).id(MessagesConversationView.endId)
                }
                .padding(.top, FmSpace.x2)
            }
            .defaultScrollAnchor(.bottom)
            .scrollDismissesKeyboard(.interactively)
            .scrollContentBackground(.hidden)
            .onTapGesture { focused = false }
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
        }
    }

    static var endId: String { "messages-end" }

    @ViewBuilder private func itemView(_ item: ChatConversationItem, palette: MessagesColors, group: Bool) -> some View {
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
                messageRow(bubble, palette: palette, group: group)
            }
        }
    }

    private func messageRow(_ bubble: ChatBubble, palette: MessagesColors, group: Bool) -> some View {
        MessagesBubbleRow(
            bubble: bubble, group: group, colors: palette, actions: actions(bubble.row),
            onTapback: {
                FmHaptics.selection()
                withAnimation(.spring(response: 0.3, dampingFraction: 0.8)) { tapbackTarget = bubble.row }
            },
            onPhoto: { viewing = MessagesPhotoBox(attachment: $0) })
    }

    private func stopSharing(_ conversation: String) {
        guard let live = hub.live else { return }
        live.setSharingMotion(conversation, false)
        if live.sharingMotion.isEmpty { DirectLiveMotionFeed.shared.stop() }
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

    private func header(_ palette: MessagesColors) -> some View {
        MessagesHeaderLayout(spacing: FmSpace.x2) {
            HStack(alignment: .top) {
                if let onBack {
                    Button(action: onBack) {
                        ZStack(alignment: .topTrailing) {
                            Image(systemName: "chevron.left")
                                .font(.system(size: 19, weight: .semibold))
                                .foregroundStyle(palette.dark ? Color.white : Color.black)
                                .frame(width: 44, height: 44)
                            if hub.otherRoomsUnread {
                                Circle().fill(fm.danger).frame(width: 9, height: 9).padding(7)
                            }
                        }
                        .chatGlass(Circle(), border: fm.border)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Conversations")
                } else {
                    Color.clear.frame(width: 44, height: 44)
                }
            }
            .zIndex(1)
            Button(action: { details = true }) {
                VStack(spacing: 4) {
                    MessagesConversationAvatar(hub: hub, size: 50)
                    HStack(spacing: 2) {
                        Text(hub.conversationName)
                            .font(.system(size: 12, weight: .medium))
                            .lineLimit(1)
                        Image(systemName: "chevron.right")
                            .font(.system(size: 9, weight: .bold))
                            .foregroundStyle(palette.meta)
                    }
                    .foregroundStyle(palette.dark ? Color.white : Color.black)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 4)
                    .chatGlass(Capsule(), border: fm.border)
                    if !hub.serverCopiesLine.isEmpty {
                        Text(hub.serverCopiesLine)
                            .font(.system(size: 10, weight: .medium))
                            .foregroundStyle(palette.meta)
                            .lineLimit(1)
                            .accessibilityIdentifier("chat-direct-server-copies")
                    }
                }
                .frame(maxWidth: 220)
            }
            .buttonStyle(.plain)
            .accessibilityElement(children: .combine)
            .accessibilityLabel(
                [hub.conversationName, hub.conversationSubtitle, hub.serverCopiesLine]
                    .filter { !$0.isEmpty }.joined(separator: ", "))
            .accessibilityHint("Shows details, people, photos and the background")
            .accessibilityIdentifier("chat-room-header")
            HStack(alignment: .top) {
                DirectCallHeaderButtons(hub: hub, dark: palette.dark)
                if let onNewAgent = shell.onNewAgent {
                    ShellNewAgentButton(onNewAgent: onNewAgent, onPalette: shell.onPalette)
                }
                if let onHub = shell.onHub {
                    Button(action: onHub) {
                        FmAvatar(name: shell.name, avatarPng: shell.avatarPng, size: 30, fontSize: FmType.meta)
                            .frame(width: 44, height: 44)
                            .chatGlass(Circle(), border: fm.border)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("You: projects, tools and settings")
                } else {
                    Color.clear.frame(width: 44, height: 44)
                }
            }
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

struct MessagesHeaderLayout: Layout {
    var spacing: CGFloat

    private struct Frames {
        var leading: CGSize
        var title: CGSize
        var trailing: CGSize
        var titleX: CGFloat
        var trailingX: CGFloat

        var height: CGFloat { max(leading.height, title.height, trailing.height) }
    }

    private func frames(_ width: CGFloat, _ subviews: Subviews) -> Frames {
        let leading: CGSize = subviews[0].sizeThatFits(.unspecified)
        let trailing: CGSize = subviews[2].sizeThatFits(.unspecified)
        let room: CGFloat = max(0, width - leading.width - trailing.width - spacing * 2)
        let fitted: CGSize = subviews[1].sizeThatFits(ProposedViewSize(width: room, height: nil))
        let title: CGSize = CGSize(width: min(fitted.width, room), height: fitted.height)
        let low: CGFloat = leading.width + spacing
        let high: CGFloat = width - trailing.width - spacing - title.width
        let titleX: CGFloat = max(low, min((width - title.width) / 2, high))
        let trailingX: CGFloat = max(low, width - trailing.width)
        return Frames(leading: leading, title: title, trailing: trailing, titleX: titleX, trailingX: trailingX)
    }

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        guard subviews.count == 3 else { return .zero }
        let width: CGFloat = proposal.width ?? 360
        return CGSize(width: width, height: frames(width, subviews).height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        guard subviews.count == 3 else { return }
        let laid: Frames = frames(bounds.width, subviews)
        subviews[0].place(at: CGPoint(x: bounds.minX, y: bounds.minY), proposal: ProposedViewSize(laid.leading))
        subviews[1].place(at: CGPoint(x: bounds.minX + laid.titleX, y: bounds.minY), proposal: ProposedViewSize(laid.title))
        subviews[2].place(
            at: CGPoint(x: bounds.minX + laid.trailingX, y: bounds.minY), proposal: ProposedViewSize(laid.trailing))
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
    @Binding var touching: Bool
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
            DirectLiveTouchButton(hub: hub, dark: colors.dark, touching: $touching, focused: $focused)
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
