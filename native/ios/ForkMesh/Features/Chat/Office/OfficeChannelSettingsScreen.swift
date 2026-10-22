import FMApi
import FMBase
import FMChat
import SwiftUI

struct OfficeChannelSettingsScreen: View {
    @Environment(\.dismiss) private var dismiss
    let office: OfficeChannelService
    let room: OfficeRoom
    @State private var name: String
    @State private var topic: String
    @State private var isPrivate: Bool
    @State private var members: [String] = []
    @State private var newMember: String = ""
    @State private var working: Bool = false
    @State private var error: String?
    @State private var confirmDelete: Bool = false

    init(office: OfficeChannelService, room: OfficeRoom) {
        self.office = office
        self.room = room
        _name = State(initialValue: room.name)
        _topic = State(initialValue: room.topic)
        _isPrivate = State(initialValue: room.isPrivate)
        _members = State(initialValue: room.members)
    }

    var body: some View {
        Form {
            Section("Name") {
                TextField("channel-name", text: $name)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
            }
            Section {
                TextField("What this channel is for", text: $topic, axis: .vertical)
                    .lineLimit(1...4)
            } header: {
                Text("Topic")
            } footer: {
                Text("\(topic.count)/\(OfficeFrames.maxTopicChars)")
            }
            Section {
                Toggle("Private", isOn: $isPrivate)
            } footer: {
                Text(isPrivate
                    ? "Only members can read it, and members' messages are end-to-end encrypted between devices. Webhook messages are visible to the server."
                    : "Everyone in this project can read it; ForkBot and webhooks can take part.")
            }
            if room.isPrivate {
                Section("Members") {
                    ForEach(members, id: \.self) { (member: String) in
                        Text(member)
                            .swipeActions {
                                Button(role: .destructive) { Task { await remove(member) } } label: { Label("Remove", systemImage: "person.badge.minus") }
                            }
                    }
                    HStack {
                        TextField("username", text: $newMember)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                        Button("Add") { Task { await add() } }
                            .disabled(working || newMember.trimmingCharacters(in: .whitespaces).isEmpty)
                    }
                }
            }
            if let error {
                Section { Text(error).foregroundStyle(.red).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error) }
            }
            Section {
                Button("Delete channel", role: .destructive) { confirmDelete = true }
            }
        }
        .navigationTitle("#\(room.name)")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Close") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") { Task { await save() } }.disabled(working)
            }
        }
        .task {
            if room.isPrivate, let list = try? await office.members(room) { members = list }
        }
        .confirmationDialog("Delete #\(room.name)?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete for everyone", role: .destructive) { Task { await delete() } }
        } message: {
            Text("The channel and its history go away for every member.")
        }
    }

    private func save() async {
        working = true
        defer { working = false }
        do {
            try await office.update(
                room, name: name == room.name ? nil : name, topic: topic == room.topic ? nil : topic,
                isPrivate: isPrivate == room.isPrivate ? nil : isPrivate)
            dismiss()
        } catch {
            self.error = "\(error)"
        }
    }

    private func add() async {
        working = true
        defer { working = false }
        do {
            try await office.addMember(room, username: newMember.trimmingCharacters(in: .whitespaces))
            members = try await office.members(room)
            newMember = ""
        } catch {
            self.error = "\(error)"
        }
    }

    private func remove(_ member: String) async {
        do {
            try await office.removeMember(room, username: member)
            members.removeAll { $0 == member }
        } catch {
            self.error = "\(error)"
        }
    }

    private func delete() async {
        do {
            try await office.delete(room)
            dismiss()
        } catch {
            self.error = "\(error)"
        }
    }
}
