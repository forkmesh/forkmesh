import FMAccount
import FMApi
import FMBase
import FMChat
import SwiftUI
import UIKit

struct ChatPersonKey: Identifiable {
    let name: String
    let account: String
    let group: MemberGroup?

    var id: String { "\(name.lowercased())|\(account)" }

    init(group: MemberGroup) {
        self.name = group.name
        self.account = group.account
        self.group = group
    }

    init(name: String, account: String = "", group: MemberGroup? = nil) {
        self.name = name
        self.account = account
        self.group = group
    }
}

struct ChatPersonSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let person: ChatPersonKey
    let onMessage: (ChatRecipient) -> Void
    let onProfile: (String) -> Void
    @State private var model: AccountProfileModel?
    @State private var avatars: ChatAvatarCache = ChatAvatarCache.shared
    @State private var pictures: MessagesPeoplePictures = MessagesPeoplePictures.shared
    @State private var viewingAvatar: Bool = false

    private var handle: String {
        let account: String = person.account.isEmpty ? (model?.profile?.name ?? "") : person.account
        return account.isEmpty ? person.name : account
    }

    private var group: MemberGroup? {
        let key: String = person.name.lowercased()
        return hub.memberGroups.first { $0.name.lowercased() == key || (!person.account.isEmpty && $0.account == person.account) }
            ?? person.group
    }

    private var zone: TimeZone? {
        ChatLocalTime.zone(profile: model?.profile?.timeZone ?? "", presence: group?.timeZone ?? "")
    }

    private var picture: String {
        let custom: String = pictures.picture(for: person.name)
        if !custom.isEmpty { return custom }
        let cached: String = avatars.png(for: handle)
        return cached.isEmpty ? (model?.profile?.avatar ?? "") : cached
    }

    private var isSelf: Bool { group?.isSelf ?? false }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: FmSpace.x4) {
                    Button(action: { viewingAvatar = true }) {
                        FmAvatar(name: person.name, avatarPng: picture, size: 120, fontSize: 48)
                            .overlay(alignment: .bottomTrailing) {
                                if let group {
                                    PresenceDot(online: group.online, away: group.online && group.userStatus.away, size: 22)
                                        .overlay { Circle().strokeBorder(fm.bgBase, lineWidth: 3) }
                                        .offset(x: -4, y: -4)
                                }
                            }
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Show \(person.name)'s picture full screen")
                    .padding(.top, FmSpace.x4)
                    VStack(spacing: FmSpace.x1) {
                        Text(person.name)
                            .fmText(.titleLarge, color: fm.textPrimary)
                            .multilineTextAlignment(.center)
                        if handle.lowercased() != person.name.lowercased() {
                            Text("@\(handle)").fmText(.bodyMedium, color: fm.textSecondary)
                        }
                        presenceLine
                    }
                    if let status = group?.userStatus, !status.isEmpty {
                        HStack(spacing: FmSpace.x2) {
                            UserStatusEmoji(status: status, size: FmType.body)
                            Text(userStatusSummary(status, now: Date()))
                                .fmText(.bodyMedium, color: fm.textSecondary)
                                .lineLimit(2)
                        }
                    }
                    localTime
                    if let bio = model?.profile?.bio, !bio.isEmpty {
                        Text(bio)
                            .fmText(.bodyMedium, color: fm.textSecondary)
                            .multilineTextAlignment(.center)
                            .padding(.horizontal, FmSpace.x4)
                    }
                    actions
                }
                .frame(maxWidth: .infinity)
                .padding(FmSpace.x4)
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
        }
        .task {
            await avatars.refresh(handle)
            guard model == nil, let api = Services.get(ApiService.self) else { return }
            let loaded: AccountProfileModel = AccountProfileModel(name: handle, client: api)
            guard loaded.validName else { return }
            model = loaded
            await loaded.load()
        }
        .fullScreenCover(isPresented: $viewingAvatar) {
            ChatAvatarViewer(name: person.name, png: picture)
        }
        .presentationDetents([.medium, .large])
    }

    @ViewBuilder private var presenceLine: some View {
        if let group {
            let away: Bool = group.online && group.userStatus.away
            Text(group.online ? (away ? "Away" : "Online") : "Offline")
                .fmText(.labelMedium, color: group.online ? (away ? fm.warning : fm.success) : fm.textTertiary)
        }
    }

    @ViewBuilder private var localTime: some View {
        if let zone {
            TimelineView(.everyMinute) { (context: TimelineViewDefaultContext) in
                HStack(spacing: FmSpace.x3) {
                    Image(systemName: "clock")
                        .font(.system(size: 20, weight: .medium))
                        .foregroundStyle(fm.accent)
                        .frame(width: 32)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("\(ChatLocalTime.time(context.date, in: zone)) local time")
                            .fmText(.titleMedium, color: fm.textPrimary)
                        Text(
                            isSelf
                                ? ChatLocalTime.place(zone)
                                : "\(ChatLocalTime.place(zone)) \u{00B7} \(ChatLocalTime.difference(zone, from: .current, at: context.date))"
                        )
                        .fmText(.bodySmall, color: fm.textTertiary)
                    }
                    Spacer(minLength: 0)
                }
                .padding(FmSpace.x3)
                .background(FmRoundedShape(radius: FmRadius.lg).fill(fm.bgRaised))
                .accessibilityElement(children: .combine)
            }
        }
    }

    private var actions: some View {
        VStack(spacing: FmSpace.x2) {
            if !isSelf {
                Button(action: message) {
                    Label("Message", systemImage: "bubble.left.fill")
                        .font(.system(size: 17, weight: .semibold))
                        .foregroundStyle(Color.white)
                        .frame(maxWidth: .infinity, minHeight: 50)
                        .background(Capsule().fill(MessagesColors.blue))
                }
                .buttonStyle(.plain)
                .accessibilityHint("Opens your direct messages with \(person.name)")
            }
            if NodeLink.isNodeName(handle.lowercased()) {
                Button(action: {
                    let name: String = handle
                    dismiss()
                    onProfile(name)
                }) {
                    Label("View full profile", systemImage: "person.crop.circle")
                        .font(.system(size: 17, weight: .medium))
                        .foregroundStyle(fm.accent)
                        .frame(maxWidth: .infinity, minHeight: 50)
                        .background(Capsule().fill(fm.accentSubtle))
                }
                .buttonStyle(.plain)
            }
        }
        .padding(.top, FmSpace.x2)
    }

    private func message() {
        var recipient: ChatRecipient = group.map { ChatHub.recipient(for: $0) } ?? ChatRecipient.person(person.name)
        if recipient.account.isEmpty { recipient.account = handle.lowercased() }
        dismiss()
        onMessage(recipient)
    }
}

struct ChatAvatarViewer: View {
    @Environment(\.dismiss) private var dismiss
    let name: String
    let png: String
    @State private var scale: CGFloat = 1

    var body: some View {
        ZStack(alignment: .topTrailing) {
            Color.black.ignoresSafeArea()
            Group {
                if let image = FmImageCache.image(base64: png) {
                    Image(uiImage: image)
                        .resizable()
                        .scaledToFit()
                        .scaleEffect(scale)
                        .gesture(
                            MagnifyGesture()
                                .onChanged { (value: MagnifyGesture.Value) in scale = max(1, min(4, value.magnification)) }
                                .onEnded { _ in withAnimation(.spring) { scale = 1 } })
                } else {
                    FmAvatar(name: name, size: 280, fontSize: 120)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .accessibilityLabel("\(name)'s picture")
            Button(action: { dismiss() }) {
                Image(systemName: "xmark")
                    .font(.system(size: 18, weight: .semibold))
                    .foregroundStyle(Color.white)
                    .frame(width: 44, height: 44)
                    .background(Circle().fill(Color.white.opacity(0.18)))
            }
            .buttonStyle(.plain)
            .padding(FmSpace.x4)
            .accessibilityLabel("Close")
        }
        .onTapGesture(count: 1) { dismiss() }
    }
}

struct ChatOnlineSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let onMessage: (ChatRecipient) -> Void
    let onProfile: (String) -> Void
    @State private var person: ChatPersonKey?

    private var online: [MemberGroup] {
        let groups: [MemberGroup] = hub.memberGroups.filter(\.online)
        return groups.filter(\.isSelf) + groups.filter { !$0.isSelf }
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    if online.filter({ !$0.isSelf }).isEmpty {
                        Text("Nobody else is online right now.")
                            .fmText(.bodyMedium, color: fm.textSecondary)
                            .frame(maxWidth: .infinity)
                            .padding(FmSpace.x6)
                    }
                    ForEach(online) { (group: MemberGroup) in
                        row(group)
                    }
                }
                .padding(.horizontal, FmSpace.x3)
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle("Online \u{00B7} \(online.count)")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
        }
        .sheet(item: $person) { (key: ChatPersonKey) in
            ChatPersonSheet(hub: hub, person: key, onMessage: onMessage, onProfile: onProfile)
        }
        .presentationDetents([.medium, .large])
    }

    private func row(_ group: MemberGroup) -> some View {
        let away: Bool = group.userStatus.away
        let visitor: Bool = ChatVisitorPresence.isTransientVisitor(name: group.name)
        return HStack(spacing: FmSpace.x3) {
            Button(action: { person = ChatPersonKey(group: group) }) {
                ZStack(alignment: .bottomTrailing) {
                    MessagesPersonAvatar(name: group.name, size: 40)
                    PresenceDot(online: true, away: away, size: 11)
                        .overlay { Circle().strokeBorder(fm.bgBase, lineWidth: 1.5) }
                        .offset(x: 2, y: 2)
                }
                .frame(width: 44, height: 44)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(visitor)
            .accessibilityLabel("\(group.name)'s profile")
            Button(action: { onMessage(ChatHub.recipient(for: group)) }) {
                HStack(spacing: FmSpace.x2) {
                    VStack(alignment: .leading, spacing: 1) {
                        HStack(spacing: 4) {
                            Text(group.isSelf ? "\(group.name) (you)" : group.name)
                                .fmText(.bodyLarge, color: fm.textPrimary)
                                .lineLimit(1)
                            UserStatusEmoji(status: group.userStatus, size: FmType.body)
                        }
                        Text(visitor ? "Visitor" : (away ? "Away" : "Online"))
                            .fmText(.labelSmall, color: away ? fm.warning : fm.textTertiary)
                    }
                    Spacer(minLength: 0)
                    if !group.isSelf && !visitor {
                        Image(systemName: "bubble.left")
                            .font(.system(size: 16, weight: .medium))
                            .foregroundStyle(fm.accent)
                    }
                }
                .frame(maxWidth: .infinity, minHeight: 52, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(group.isSelf || visitor)
            .accessibilityHint(group.isSelf || visitor ? "" : "Opens your direct messages with \(group.name)")
        }
        .padding(.vertical, FmSpace.x1)
    }
}
