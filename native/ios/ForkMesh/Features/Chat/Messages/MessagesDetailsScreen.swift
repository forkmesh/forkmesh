import FMAccount
import FMApi
import FMBase
import FMChat
import PhotosUI
import SwiftUI
import UIKit

struct MessagesDetailsScreen: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let onRoute: (Route) -> Void
    @State private var naming: Bool = false
    @State private var nameText: String = ""
    @State private var photoItem: PhotosPickerItem?
    @State private var choosingPhoto: Bool = false
    @State private var pictureFor: String?
    @State private var pictures: MessagesPeoplePictures = MessagesPeoplePictures.shared
    @State private var background: Bool = false
    @State private var adding: Bool = false
    @State private var newMember: String = ""
    @State private var officeMembers: [String]?
    @State private var managing: OfficeRoom?
    @State private var confirmClear: Bool = false
    @State private var confirmBlock: Bool = false
    @State private var viewing: MessagesPhotoBox?
    @State private var working: Bool = false
    @State private var error: String?

    private var conversation: String { hub.currentConversation }
    private var officeRoom: OfficeRoom? { hub.office?.room(for: conversation) }
    private var group: Bool { hub.inGroupConversation }
    private var members: [String] { officeMembers ?? hub.conversationMembers }
    private var person: Bool { ChatHub.isPersonConversation(conversation) }
    private var handle: String { person ? hub.conversationHandle(conversation) : "" }

    var body: some View {
        NavigationStack {
            List {
                Section { identity }
                    .listRowBackground(Color.clear)
                if hub.canChangeLook || hub.inConversation {
                    Section {
                        Button(action: { background = true }) {
                            HStack {
                                Label("Background", systemImage: "photo.artframe")
                                Spacer()
                                MessagesBackdrop(look: hub.conversationLook, canvas: fm.bgBase)
                                    .frame(width: 34, height: 48)
                                    .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
                                    .overlay(RoundedRectangle(cornerRadius: 6, style: .continuous).stroke(fm.border, lineWidth: 0.5))
                            }
                        }
                        if hub.conversationOwnBackground {
                            Button(action: { hub.setOwnBackground(nil) }) {
                                Label(
                                    hub.conversationSharedLook.hasBackground ? "Use Shared Background" : "Remove My Background",
                                    systemImage: "arrow.uturn.backward.circle")
                            }
                            .accessibilityIdentifier("chat-background-use-shared")
                        } else if hub.canChangeLook && hub.conversationSharedLook.hasBackground {
                            Button(role: .destructive, action: { change(ChatLookChange(field: .background)) }) {
                                Label("Remove Background", systemImage: "xmark.circle")
                            }
                        }
                    } footer: {
                        Text(backgroundFooter)
                    }
                }
                if group { peopleSection }
                if DirectMessageFrames.isDirect(conversation) { safetySection }
                photosSection
                Section {
                    ChatRetentionMenu(hub: hub, conversation: conversation)
                    if let peer = hub.conversationPeer, !peer.name.isEmpty {
                        Button(action: { onRoute(.profile(name: peer.owner.isEmpty ? peer.name : peer.owner)) }) {
                            Label("View Profile", systemImage: "person.crop.circle")
                        }
                    }
                } footer: {
                    Text(hub.sealSummary)
                }
                if let error {
                    Section { Text(error).foregroundStyle(fm.danger).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error) }
                }
                Section {
                    if let room = officeRoom, room.canManage {
                        Button(action: { managing = room }) { Label("Group Settings", systemImage: "gearshape") }
                    }
                    if !handle.isEmpty {
                        Button(role: .destructive, action: { confirmBlock = true }) {
                            Label("Block \(handle)", systemImage: "hand.raised")
                        }
                    }
                    Button(role: .destructive, action: { confirmClear = true }) {
                        Label("Delete Conversation", systemImage: "trash")
                    }
                }
            }
            .listStyle(.insetGrouped)
            .navigationTitle(group ? "Group" : "Details")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .overlay { if working { ProgressView() } }
            .task { await loadMembers() }
            .sheet(isPresented: $background) { MessagesBackgroundPicker(hub: hub) }
            .sheet(item: $managing) { (room: OfficeRoom) in
                if let office = hub.office {
                    NavigationStack { OfficeChannelSettingsScreen(office: office, room: room) }
                }
            }
            .fullScreenCover(item: $viewing) { (box: MessagesPhotoBox) in MessagesPhotoViewer(attachment: box.attachment) }
            .photosPicker(isPresented: $choosingPhoto, selection: $photoItem, matching: .images)
            .onChange(of: photoItem) { _, item in
                guard let item else { return }
                Task { await usePhoto(item) }
            }
            .alert("Name this conversation", isPresented: $naming) {
                TextField("Group name", text: $nameText)
                Button("Cancel", role: .cancel) {}
                Button("Done") { change(ChatLookChange(field: .name, name: nameText)) }
            } message: {
                Text("Everyone in the conversation sees the new name.")
            }
            .alert("Add someone", isPresented: $adding) {
                TextField("username", text: $newMember)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button("Cancel", role: .cancel) { newMember = "" }
                Button("Add") { Task { await addMember() } }
            }
            .confirmationDialog("Delete this conversation?", isPresented: $confirmClear, titleVisibility: .visible) {
                Button("Delete", role: .destructive) {
                    hub.clearConversationHistory()
                    dismiss()
                }
            } message: {
                Text(
                    person
                        ? "It is removed from this phone only, and comes back if they message you again. They keep their copy."
                        : "Its messages are removed from this phone only. Everyone else keeps their copy.")
            }
            .confirmationDialog("Block \(handle)?", isPresented: $confirmBlock, titleVisibility: .visible) {
                Button("Block", role: .destructive) {
                    hub.setBlocked(handle, true)
                    dismiss()
                }
            } message: {
                Text("Their messages, typing and alerts are hidden on this phone. You can unblock them from their profile.")
            }
        }
    }

    private var identity: some View {
        VStack(spacing: FmSpace.x2) {
            MessagesConversationAvatar(hub: hub, size: 88)
            Text(hub.conversationName)
                .font(.system(size: 24, weight: .semibold))
                .multilineTextAlignment(.center)
            Text(hub.conversationSubtitle)
                .fmText(.bodyMedium, color: fm.textSecondary)
            if group && hub.canChangeLook {
                HStack(spacing: FmSpace.x3) {
                    Button(action: {
                        nameText = hub.conversationLook.name
                        naming = true
                    }) {
                        Label("Name", systemImage: "pencil")
                    }
                    Button(action: { choosePicture(for: nil) }) {
                        Label("Photo", systemImage: "camera")
                    }
                    if hub.conversationLook.photo != nil {
                        Button(role: .destructive, action: { change(ChatLookChange(field: .photo)) }) {
                            Label("Remove", systemImage: "xmark")
                        }
                    }
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .padding(.top, FmSpace.x1)
            } else if !group && !hub.conversationName.isEmpty {
                HStack(spacing: FmSpace.x3) { personPictureButtons(hub.conversationName) }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                    .padding(.top, FmSpace.x1)
                Text("Only you see this picture.")
                    .fmText(.labelSmall, color: fm.textTertiary)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, FmSpace.x2)
    }

    @ViewBuilder private var peopleSection: some View {
        Section {
            ForEach(members, id: \.self) { (name: String) in
                HStack(spacing: FmSpace.x3) {
                    MessagesPersonAvatar(name: name, size: 36)
                    Text(name)
                    Spacer()
                }
                .contextMenu { personPictureButtons(name) }
                .swipeActions {
                    if let room = officeRoom, room.canManage {
                        Button(role: .destructive) { Task { await removeMember(name, room: room) } } label: {
                            Label("Remove", systemImage: "person.badge.minus")
                        }
                    }
                }
            }
            if let room = officeRoom, room.canManage {
                Button(action: { adding = true }) { Label("Add People", systemImage: "person.badge.plus") }
            }
        } header: {
            Text(members.count == 1 ? "1 Person" : "\(members.count) People")
        }
    }

    private var safetySection: some View {
        let verified: Bool = hub.safetyVerified(conversation)
        return Section {
            NavigationLink {
                MessagesSafetyNumberScreen(hub: hub, conversation: conversation)
            } label: {
                HStack {
                    Label(DirectTrustText.safetyNumber, systemImage: "checkmark.shield")
                    Spacer()
                    Text(verified ? DirectTrustText.verified : DirectTrustText.notVerified)
                        .foregroundStyle(verified ? fm.success : fm.textSecondary)
                }
            }
            .accessibilityIdentifier("chat-details-safety-number")
        } footer: {
            Text(DirectTrustText.safetyExplainer(hub.trustName(conversation)))
        }
    }

    @ViewBuilder private var photosSection: some View {
        let photos: [ChatAttachment] = hub.conversationPhotos
        if !photos.isEmpty {
            Section("Photos") {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 84), spacing: 4)], spacing: 4) {
                    ForEach(photos, id: \.messageId) { (photo: ChatAttachment) in
                        thumbnail(photo)
                    }
                }
                .padding(.vertical, FmSpace.x1)
            }
        }
    }

    @ViewBuilder private func thumbnail(_ photo: ChatAttachment) -> some View {
        if let picture = FmImageCache.image(photo.data) {
            Button(action: { viewing = MessagesPhotoBox(attachment: photo) }) {
                Color.clear
                    .aspectRatio(1, contentMode: .fit)
                    .overlay(Image(uiImage: picture).resizable().scaledToFill())
                    .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Photo")
        }
    }

    private var backgroundFooter: String {
        if hub.conversationOwnBackground {
            return hub.conversationSharedLook.hasBackground
                ? "Only you see this background. It covers the shared one on this phone."
                : "Only you see this background."
        }
        return hub.inDirect
            ? "Share a background so you both see it, end-to-end encrypted, or choose one just for you."
            : "Share a background with everyone in the conversation, or choose one just for you."
    }

    private func change(_ next: ChatLookChange) {
        working = true
        Task { @MainActor in
            _ = await hub.setLook(next)
            working = false
        }
    }

    @ViewBuilder private func personPictureButtons(_ name: String) -> some View {
        Button(action: { choosePicture(for: name) }) {
            Label(pictures.has(name) ? "Change Picture" : "Set Picture", systemImage: "camera")
        }
        if pictures.has(name) {
            Button(role: .destructive, action: { pictures.set(nil, for: name) }) {
                Label("Remove Picture", systemImage: "xmark")
            }
        }
    }

    private func choosePicture(for name: String?) {
        pictureFor = name
        choosingPhoto = true
    }

    private func usePhoto(_ item: PhotosPickerItem) async {
        defer { photoItem = nil }
        guard let data = try? await item.loadTransferable(type: Data.self) else { return }
        guard let jpeg = MessagesImages.jpeg(data, maxSide: 512, maxBytes: MessagesPeoplePictures.maxBytes) else {
            error = "That photo could not be used."
            return
        }
        if let name = pictureFor {
            if !pictures.set(jpeg, for: name) { error = "That picture could not be saved." }
            return
        }
        change(ChatLookChange(field: .photo, image: jpeg))
    }

    private func loadMembers() async {
        guard let room = officeRoom, let office = hub.office else { return }
        if let list = try? await office.members(room) { officeMembers = list }
    }

    private func addMember() async {
        let name: String = newMember.trimmingCharacters(in: .whitespaces)
        newMember = ""
        guard !name.isEmpty, let room = officeRoom, let office = hub.office else { return }
        working = true
        defer { working = false }
        do {
            try await office.addMember(room, username: name)
            officeMembers = try await office.members(room)
            error = nil
        } catch {
            self.error = "\(error)"
        }
    }

    private func removeMember(_ name: String, room: OfficeRoom) async {
        guard let office = hub.office else { return }
        do {
            try await office.removeMember(room, username: name)
            officeMembers?.removeAll { $0 == name }
            error = nil
        } catch {
            self.error = "\(error)"
        }
    }
}
