import FMPackages
import SwiftUI

struct DjQueueView: View {
    let model: DjStudioModel
    let u: CGFloat
    let vertical: Bool

    var body: some View {
        let items = model.shownQueue
        let layout = vertical ? AnyLayout(VStackLayout(spacing: 6 * u)) : AnyLayout(HStackLayout(spacing: 8 * u))
        layout {
            header
            if items.isEmpty {
                Button {
                    DjTheme.tick()
                    model.openLoader(.queue)
                } label: {
                    Text(model.inJam ? "Queue videos for the jam" : "Tap + to queue videos")
                        .font(.system(size: 11 * u, weight: .medium))
                        .foregroundStyle(DjTheme.muted)
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .contentShape(Rectangle())
                }
                .buttonStyle(DjPressStyle())
            } else {
                ScrollView(vertical ? .vertical : .horizontal, showsIndicators: false) {
                    let list = vertical ? AnyLayout(VStackLayout(spacing: 6 * u)) : AnyLayout(HStackLayout(spacing: 6 * u))
                    list {
                        ForEach(Array(items.enumerated()), id: \.element.id) { index, item in
                            DjQueueThumb(model: model, item: item, next: index == 0, u: u, vertical: vertical)
                        }
                    }
                    .padding(vertical ? .vertical : .horizontal, 2 * u)
                }
            }
        }
        .padding(6 * u)
        .background(DjTheme.panel.opacity(0.85), in: RoundedRectangle(cornerRadius: 6 * u, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 6 * u, style: .continuous).strokeBorder(DjTheme.edge, lineWidth: 1))
        .padding(.horizontal, vertical ? 6 * u : 0)
        .animation(.easeOut(duration: 0.2), value: items.map(\.id))
    }

    @ViewBuilder private var header: some View {
        let layout = vertical ? AnyLayout(HStackLayout(spacing: 6 * u)) : AnyLayout(VStackLayout(spacing: 6 * u))
        layout {
            DjJamButton(model: model, size: 26 * u)
            if vertical {
                VStack(alignment: .leading, spacing: 0) {
                    Text("QUEUE")
                        .font(.system(size: 10 * u, weight: .bold))
                        .foregroundStyle(DjTheme.ink.opacity(0.85))
                    if let jam = model.jam, jam.joined {
                        Text("\(jam.listeners.count) in jam")
                            .font(.system(size: 9 * u))
                            .foregroundStyle(DjTheme.active)
                    }
                }
                .lineLimit(1)
                Spacer(minLength: 0)
            }
            DjRoundButton(size: 26 * u, action: { model.openLoader(.queue) }) {
                Image(systemName: "plus")
            }
            .accessibilityLabel("Add a video to the queue")
        }
    }
}

struct DjQueueThumb: View {
    let model: DjStudioModel
    let item: DjQueueItem
    let next: Bool
    let u: CGFloat
    let vertical: Bool

    var body: some View {
        Menu {
            Section(item.title.isEmpty ? "YouTube video" : item.title) {
                Button("Load on Deck A", systemImage: "a.circle") { model.playFromQueue(item, on: .a) }
                Button("Load on Deck B", systemImage: "b.circle") { model.playFromQueue(item, on: .b) }
                Button("Remove from Queue", systemImage: "trash", role: .destructive) { model.removeFromQueue(item) }
            }
        } label: {
            AsyncImage(url: item.thumbnail) { image in
                image.resizable().scaledToFill()
            } placeholder: {
                Color.white.opacity(0.08)
            }
            .frame(maxWidth: vertical ? .infinity : nil)
            .frame(width: vertical ? nil : 72 * u, height: vertical ? nil : 44 * u)
            .aspectRatio(16 / 9, contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: 4 * u, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 4 * u, style: .continuous).strokeBorder(next ? DjTheme.active.opacity(0.8) : DjTheme.edge, lineWidth: next ? 1.5 : 1))
            .overlay(alignment: .bottomLeading) {
                if model.inJam, !item.addedBy.isEmpty {
                    Text(item.addedBy)
                        .font(.system(size: 8 * u, weight: .semibold))
                        .lineLimit(1)
                        .padding(.horizontal, 3 * u)
                        .padding(.vertical, 1 * u)
                        .background(Color.black.opacity(0.65), in: RoundedRectangle(cornerRadius: 2 * u))
                        .foregroundStyle(DjTheme.ink)
                        .padding(2 * u)
                }
            }
        }
        .accessibilityLabel(item.title.isEmpty ? "Queued video" : "Queued: \(item.title)")
    }
}

struct DjJamButton: View {
    let model: DjStudioModel
    let size: CGFloat

    var body: some View {
        DjRoundButton(size: size, lit: model.inJam, action: { model.sheet = .jam }) {
            Image(systemName: model.inJam ? "person.2.fill" : "person.2")
        }
        .overlay(alignment: .topTrailing) {
            if model.pendingInvites > 0 {
                Circle().fill(DjTheme.record).frame(width: size * 0.3, height: size * 0.3)
            }
        }
        .accessibilityLabel(model.inJam ? "Jam session" : "Start or join a jam session")
        .accessibilityValue(model.pendingInvites > 0 ? "\(model.pendingInvites) invitations" : "")
    }
}

struct DjJamSheet: View {
    @Bindable var model: DjStudioModel

    var body: some View {
        List {
            if let jam = model.jam, jam.joined {
                Section {
                    LabeledContent("Session", value: jam.title.isEmpty ? "Jam" : jam.title)
                    LabeledContent("Host", value: jam.owner)
                } footer: {
                    Text("Everyone in the jam shares both decks and the queue. Volume and the crossfader stay on your own phone.")
                }
                Section("People") {
                    ForEach(jam.members) { (member: DjJamMember) in
                        HStack {
                            Text(member.name)
                            Spacer()
                            Text(member.status == "owner" ? "Host" : member.status == "joined" ? "In the jam" : "Invited")
                                .font(.footnote)
                                .foregroundStyle(member.joined ? DjTheme.active : .secondary)
                        }
                        .swipeActions {
                            if jam.isOwner, member.status != "owner" {
                                Button("Remove", role: .destructive) { model.removeMember(member.name) }
                            }
                        }
                    }
                    HStack(spacing: 8) {
                        Image(systemName: "person.badge.plus").foregroundStyle(.secondary)
                        TextField("Add people by username", text: $model.inviteText)
                            .autocorrectionDisabled()
                            .textInputAutocapitalization(.never)
                            .submitLabel(.send)
                            .onSubmit { model.invite(names(model.inviteText)) }
                            .onChange(of: model.inviteText) { _, text in model.suggestPeople(text) }
                        if model.jamBusy { ProgressView() }
                    }
                    ForEach(model.inviteSuggestions, id: \.self) { name in
                        Button {
                            model.invite([name])
                        } label: {
                            Label("Invite \(name)", systemImage: "plus.circle")
                        }
                    }
                }
                Section {
                    Button(jam.isOwner ? "End Jam for Everyone" : "Leave Jam", role: .destructive) { model.leaveJam() }
                }
            } else {
                Section {
                    Button {
                        model.startJam()
                    } label: {
                        Label("Start a Jam Session", systemImage: "person.2.wave.2")
                    }
                    .disabled(model.jamBusy)
                } footer: {
                    Text("Invite other ForkMesh users to share your decks: they hear the same songs, and anyone in the jam can queue videos.")
                }
                let invites = model.jams.filter(\.invited)
                if !invites.isEmpty {
                    Section("Invitations") {
                        ForEach(invites) { (row: DjJamSummary) in
                            HStack {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(row.title.isEmpty ? "Jam" : row.title)
                                    Text("From \(row.owner)").font(.footnote).foregroundStyle(.secondary)
                                }
                                Spacer()
                                Button("Join") { model.joinJam(row.id) }
                                    .buttonStyle(.borderedProminent)
                                    .controlSize(.small)
                            }
                        }
                    }
                }
                let mine = model.jams.filter { !$0.invited }
                if !mine.isEmpty {
                    Section("Your Jams") {
                        ForEach(mine) { (row: DjJamSummary) in
                            Button {
                                model.openJam(row.id)
                            } label: {
                                HStack {
                                    Text(row.title.isEmpty ? "Jam" : row.title)
                                    Spacer()
                                    Text("\(row.members) in").font(.footnote).foregroundStyle(.secondary)
                                }
                            }
                        }
                    }
                }
            }
            if !model.jamProblem.isEmpty {
                Section {
                    Text(model.jamProblem).font(.footnote).foregroundStyle(.secondary)
                }
            }
        }
        .scrollContentBackground(.hidden)
        .navigationTitle("Jam Session")
        .onAppear { model.refreshJams() }
    }

    private func names(_ text: String) -> [String] {
        text.split(whereSeparator: { $0 == "," || $0 == " " }).map { String($0).lowercased() }.filter { !$0.isEmpty }
    }
}
