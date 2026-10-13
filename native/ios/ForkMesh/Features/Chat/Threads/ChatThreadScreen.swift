import FMAccount
import FMApi
import FMBase
import FMChat
import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

struct ChatThreadScreen: View {
    let channel: String
    let rootId: String
    @State private var hub: ChatHub? = ChatFeature.hub()

    var body: some View {
        if let hub = hub {
            ChatThreadBody(hub: hub, channel: channel, rootId: rootId)
                .fmQuietSounds()
        } else {
            FmEmptyState(icon: "chat_bubble_outline", title: "Chat is not available")
        }
    }
}

private struct ChatThreadBody: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let channel: String
    let rootId: String
    @State private var draft: String = ""
    @State private var focused: Bool = false
    @State private var notice: FmNotice?
    @State private var editing: ChatMessage?
    @State private var editText: String = ""
    @State private var pushed: Route?

    private var root: ChatMessage? { hub.rootMessage(rootId, in: channel) }

    var body: some View {
        VStack(spacing: 0) {
            transcript
            if notice != nil {
                FmNoticeLine($notice)
                    .padding(.horizontal, FmSpace.x4)
            }
        }
            .shellDock { composer }
            .background(fm.bgBase)
            .navigationTitle("Thread in \(channel)")
            .navigationBarTitleDisplayMode(.inline)
            .navigationDestination(item: $pushed) { (route: Route) in RouteDestinationView(route: route).shellPageClearance() }
            .onChange(of: hub.notice) { _, posted in
                if let posted = posted {
                    notice = FmNotice(posted.text)
                    hub.clearNotice()
                }
            }
            .alert("Edit reply", isPresented: Binding(get: { editing != nil }, set: { if !$0 { editing = nil } })) {
                TextField("Reply", text: $editText)
                Button("Cancel", role: .cancel) { editing = nil }
                Button("Save") {
                    if let message = editing { hub.edit(message, text: editText) }
                    editing = nil
                }
            }
    }

    private var transcript: some View {
        ScrollViewReader { (proxy: ScrollViewProxy) in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    if let root = root {
                        ChatMessageRow(
                            row: rootRow(root), liveStatus: hub.liveStatus(for: root),
                            actions: rootActions(root))
                        Text(repliesLabel)
                            .fmText(.labelMedium, color: fm.textTertiary)
                            .padding(.horizontal, FmSpace.x4)
                            .padding(.vertical, FmSpace.x2)
                        FmHairline()
                    } else {
                        Text("The first message of this thread is not on this phone.")
                            .fmText(.bodySmall, color: fm.textTertiary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .fmMessageTools("The first message of this thread is not on this phone.")
                            .padding(FmSpace.x4)
                    }
                    let replies: [ChatRow] = hub.threadRows(rootId)
                    ForEach(Array(replies.enumerated()), id: \.element.id) { (entry: (offset: Int, element: ChatRow)) in
                        let row: ChatRow = entry.element
                        if ChatHub.startsNewDay(row.message, after: entry.offset > 0 ? replies[entry.offset - 1].message : root) {
                            ChatDayDivider(date: row.message.timestamp)
                        }
                        ChatMessageRow(row: row, liveStatus: hub.liveStatus(for: row.message), actions: replyActions(row))
                            .id(row.id)
                    }
                }
                .padding(.vertical, FmSpace.x3)
            }
            .defaultScrollAnchor(.bottom)
            .scrollDismissesKeyboard(.interactively)
            .background(fm.bgBase)
            .onChange(of: hub.threadRows(rootId).last?.id ?? "") { _, last in
                withAnimation(.easeOut(duration: 0.2)) { proxy.scrollTo(last, anchor: .bottom) }
            }
            .chatKeepsEndInView(focused: focused) {
                if let last: ChatRow = hub.threadRows(rootId).last {
                    withAnimation(.easeOut(duration: 0.25)) { proxy.scrollTo(last.id, anchor: .bottom) }
                }
            }
        }
    }

    @ViewBuilder private var composer: some View {
        Group {
            if let blocked = hub.guestComposeBlocked(channel) {
                GuestComposeNotice(action: blocked, onLogIn: {
                    if let signIn = Services.get(SignInCoordinator.self) {
                        Task { _ = await signIn.requireAccount(action: blocked) }
                    }
                })
            } else {
                ChatComposerBar(
                    hub: hub, placeholder: "Reply...", allowAttachments: false, draft: $draft, focused: $focused,
                    onSend: send, glass: true)
            }
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
    }

    private var repliesLabel: String {
        let count: Int = hub.threadRows(rootId).count
        return count == 1 ? "1 reply" : "\(count) replies"
    }

    private func rootRow(_ root: ChatMessage) -> ChatRow {
        hub.rows.first { $0.id == root.id } ?? ChatRow(message: root, continuesGroup: false, queued: false, deletable: false)
    }

    private func rootActions(_ root: ChatMessage) -> ChatRowActions {
        ChatRowActions(
            onCopied: { notice = FmNotice("Copied to clipboard") },
            onReact: { (value: String) in hub.toggleReaction(root, value: value) }, onThread: nil, onEdit: nil, onDelete: nil,
            onIssue: nil, onPrompt: nil, onProfile: nil)
    }

    private func replyActions(_ row: ChatRow) -> ChatRowActions {
        let message: ChatMessage = row.message
        var edit: (() -> Void)? = nil
        var delete: (() -> Void)? = nil
        var profile: (() -> Void)? = nil
        if row.editable {
            edit = {
                editText = row.text
                editing = message
            }
        }
        if row.deletable {
            delete = { hub.deleteReply(message) }
        }
        if !message.isSelf && message.senderId != "forkbot" {
            profile = { pushed = .profile(name: message.senderName) }
        }
        return ChatRowActions(
            onCopied: { notice = FmNotice("Copied to clipboard") },
            onReact: { (value: String) in hub.toggleReaction(message, value: value) }, onThread: nil, onEdit: edit,
            onDelete: delete, onIssue: nil, onPrompt: nil, onProfile: profile, onHide: { hub.hide(message) })
    }

    private func send() {
        let text: String = draft
        Task {
            if await hub.replyInThread(channel: channel, rootId: rootId, text: text) {
                draft = ""
            }
        }
    }
}
