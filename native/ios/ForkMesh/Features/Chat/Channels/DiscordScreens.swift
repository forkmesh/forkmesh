import FMApi
import FMBase
import FMChat
import FMDiscord
import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

struct DiscordRouteScreen: View {
    let guild: String
    let channel: String?
    @State private var bridge: DiscordBridge? = Services.get(DiscordBridge.self)

    var body: some View {
        if let bridge = bridge {
            if let channel = channel, DiscordBridge.validChannelId(channel) {
                DiscordChannelScreen(bridge: bridge, channelId: channel)
                    .fmQuietSounds()
            } else {
                DiscordHomeScreen(bridge: bridge, guild: guild)
                    .fmQuietSounds()
            }
        } else {
            FmEmptyState(
                icon: "desktop_windows", title: "Pair a desktop first",
                message: "Discord runs on your desktop, which keeps the bot token. Pair this phone with it to read and reply here.")
        }
    }
}

struct DiscordHomeScreen: View {
    @Environment(\.fmPalette) private var fm
    let bridge: DiscordBridge
    let guild: String

    private var guilds: [DiscordGuild] {
        guild.isEmpty ? bridge.guilds : bridge.guilds.filter { $0.name == guild }
    }

    var body: some View {
        List {
            Section {
                HStack(spacing: FmSpace.x2) {
                    Circle().fill(bridge.status.connected ? fm.success : fm.textTertiary).frame(width: 8, height: 8)
                    Text(statusLine).fmText(.bodySmall, color: fm.textSecondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(statusLine)
                }
                if let error = bridge.lastError {
                    Text(error).fmText(.bodySmall, color: fm.danger)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                }
            } footer: {
                Text("The desktop keeps the Discord bot token. This phone only reads through it and asks it to send.")
            }
            ForEach(guilds) { (entry: DiscordGuild) in
                Section(entry.name) {
                    ForEach(entry.channels) { (channel: DiscordChannel) in
                        NavigationLink(value: Route.discord(guild: entry.name, channel: channel.id)) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(channel.name.isEmpty ? channel.display : channel.name)
                                if !channel.topic.isEmpty {
                                    Text(channel.topic).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                                }
                            }
                        }
                    }
                }
            }
        }
        .overlay {
            if bridge.loading && bridge.channels.isEmpty {
                FmCircularProgressIndicator(size: 28, strokeWidth: 2.4)
            }
        }
        .navigationTitle("Discord")
        .fmRefreshable { await bridge.refresh() }
        .task {
            bridge.startWatching()
            await bridge.refresh()
        }
    }

    private var statusLine: String {
        if !bridge.status.available { return "The Discord package is not running on your desktop." }
        if !bridge.status.configured { return "Add a bot token in the desktop's Discord package." }
        let bot: String = bridge.status.bot.isEmpty ? "the bot" : bridge.status.bot
        return bridge.status.connected ? "Connected as \(bot)" : "The desktop is reconnecting to Discord."
    }
}

struct DiscordChannelScreen: View {
    @Environment(\.fmPalette) private var fm
    let bridge: DiscordBridge
    let channelId: String
    @State private var draft: String = ""
    @State private var replyTo: DiscordMessage?
    @State private var pending: DiscordSendConfirmation?
    @State private var working: Bool = false
    @State private var notice: FmNotice?

    private var channel: DiscordChannel? { bridge.channel(channelId) }
    private var messages: [DiscordMessage] { bridge.messages[channelId] ?? [] }

    var body: some View {
        VStack(spacing: 0) {
            ScrollViewReader { (proxy: ScrollViewProxy) in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: FmSpace.x3) {
                        ForEach(messages) { (message: DiscordMessage) in
                            row(message).id(message.id)
                        }
                    }
                    .padding(FmSpace.x3)
                }
                .background(fm.bgBase)
                .onChange(of: messages.last?.id ?? "") { _, last in proxy.scrollTo(last, anchor: .bottom) }
            }
            if notice != nil {
                FmNoticeLine($notice)
                    .padding(.horizontal, FmSpace.x3)
            }
            if channel?.canSend == true {
                composer
            } else {
                Text("The desktop's bot cannot post here.")
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools("The desktop's bot cannot post here.")
                    .padding(FmSpace.x3)
            }
        }
        .navigationTitle(channel?.name ?? "Discord")
        .navigationBarTitleDisplayMode(.inline)
        .task {
            bridge.watching.insert(channelId)
            bridge.startWatching()
            if bridge.channels.isEmpty { await bridge.refresh() }
            await bridge.load(channelId)
        }
        .onDisappear { bridge.watching.remove(channelId) }
        .fmRefreshable { await bridge.load(channelId) }
        .confirmationDialog(
            "Send through your desktop?", isPresented: Binding(get: { pending != nil }, set: { if !$0 { pending = nil } }),
            titleVisibility: .visible
        ) {
            Button("Send") { Task { await confirm() } }
            Button("Cancel", role: .cancel) { pending = nil }
        } message: {
            Text(pending?.summary ?? "")
        }
    }

    private func row(_ message: DiscordMessage) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: FmSpace.x2) {
                Text(message.sender).fmText(.titleSmall, color: FmColors.senderColor(message.sender))
                Text(ChatHub.timeLabel(message.timestamp)).fmText(.bodySmall, color: fm.textTertiary, tabular: true)
                if message.edited { Text("(edited)").fmText(.labelSmall, color: fm.textTertiary) }
            }
            if !message.rootId.isEmpty {
                Text("Reply").fmText(.labelSmall, color: fm.textTertiary)
            }
            if !message.text.isEmpty {
                ChatLinkText(text: message.text)
            }
            if message.truncated {
                Text("Shortened; open Discord for the rest.").fmText(.labelSmall, color: fm.textTertiary)
            }
            if !message.fileName.isEmpty {
                HStack(spacing: FmSpace.x1) {
                    FmIcon("attach_file", size: 14, color: fm.textTertiary)
                    Text(message.fileName).fmText(.bodySmall, color: fm.textSecondary).lineLimit(1)
                }
            }
            if !message.reactions.isEmpty {
                HStack(spacing: FmSpace.x1) {
                    ForEach(message.reactions) { (reaction: DiscordReaction) in
                        Text("\(reaction.emoji) \(reaction.count)")
                            .fmText(.labelSmall, color: fm.textSecondary)
                            .padding(.horizontal, FmSpace.x2)
                            .padding(.vertical, 2)
                            .background(fm.bgOverlay)
                            .clipShape(Capsule())
                    }
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .contentShape(Rectangle())
        .contextMenu {
            if channel?.canSend == true {
                Button(action: { replyTo = message }) { Label("Reply", systemImage: "arrowshape.turn.up.left") }
            }
            Button(action: { copy(message.text) }) { Label("Copy text", systemImage: "doc.on.doc") }
        }
    }

    private var composer: some View {
        FmBottomActionBar(padding: EdgeInsets.fmAll(FmSpace.x3)) {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                if let replyTo = replyTo {
                    HStack {
                        Text("Replying to \(replyTo.sender)").fmText(.labelSmall, color: fm.textTertiary)
                        Spacer()
                        Button(action: { self.replyTo = nil }) { Image(systemName: "xmark.circle.fill") }
                            .buttonStyle(.plain)
                            .accessibilityLabel("Cancel reply")
                    }
                }
                HStack(spacing: FmSpace.x2) {
                    TextField("Message #\(channel?.name ?? "")", text: $draft, axis: .vertical)
                        .lineLimit(1...5)
                        .padding(.horizontal, FmSpace.x4)
                        .padding(.vertical, FmSpace.x3)
                        .background(fm.bgBase)
                        .clipShape(FmRoundedShape(radius: FmRadius.lg))
                    Button(action: { Task { await prepare() } }) {
                        FmIcon("send", size: 18, color: Color.white)
                            .frame(width: 48, height: 48)
                            .background(fm.accent)
                            .clipShape(FmRoundedShape(radius: FmRadius.md))
                    }
                    .buttonStyle(.plain)
                    .disabled(working || draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    .accessibilityLabel("Send through your desktop")
                }
                Text("\(draft.count)/\(DiscordBridge.maxSendChars)")
                    .fmText(.labelSmall, color: draft.count > DiscordBridge.maxSendChars ? fm.danger : fm.textTertiary, tabular: true)
            }
        }
    }

    private func prepare() async {
        working = true
        defer { working = false }
        do {
            pending = try await bridge.prepareSend(channel: channelId, text: draft, replyTo: replyTo?.id ?? "")
        } catch {
            notice = FmNotice.error("\(error)")
        }
    }

    private func confirm() async {
        guard let confirmation = pending else { return }
        pending = nil
        working = true
        defer { working = false }
        do {
            try await bridge.send(confirmation)
            draft = ""
            replyTo = nil
        } catch {
            notice = FmNotice.error("\(error)")
        }
    }

    private func copy(_ text: String) {
        #if canImport(UIKit)
        UIPasteboard.general.string = text
        #endif
        notice = FmNotice("Copied to clipboard")
    }
}
