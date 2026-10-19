import FMPackages
import SwiftUI

extension DjStudioModel {
    func dragQueue(_ item: DjQueueItem, to point: CGPoint) {
        if queueDrag == nil {
            DjTheme.tick()
            reacting = false
            draggedId = item.id
        }
        queueDrag = DjQueueDrag(item: item, location: point)
        let target = dropTarget(point)
        guard target != dropSide else { return }
        dropSide = target
        if target != nil { DjTheme.tick() }
    }

    func dropQueue(at point: CGPoint) {
        defer { cancelQueueDrag() }
        guard let drag = queueDrag, let side = dropTarget(point) else { return }
        DjTheme.tick()
        playFromQueue(drag.item, on: side)
    }

    func cancelQueueDrag() {
        queueDrag = nil
        dropSide = nil
        draggedId = nil
    }

    private func dropTarget(_ point: CGPoint) -> DjDeckSide? {
        DjDeckSide.allCases
            .compactMap { side -> (DjDeckSide, CGFloat)? in
                guard let frame = deckFrames[side], frame.insetBy(dx: -28, dy: -28).contains(point) else { return nil }
                return (side, hypot(frame.midX - point.x, frame.midY - point.y))
            }
            .min { $0.1 < $1.1 }?.0
    }
}

struct DjQueueView: View {
    let model: DjStudioModel
    let u: CGFloat
    let vertical: Bool

    var body: some View {
        let _ = DjDiagnostics.viewed("queue")
        let items = model.shownQueue
        let layout = vertical ? AnyLayout(VStackLayout(spacing: 6 * u)) : AnyLayout(HStackLayout(spacing: 8 * u))
        layout {
            header
            if vertical, model.inJam {
                DjPeopleStrip(model: model, size: 22 * u)
                    .frame(height: 40 * u)
            }
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
                            DjQueueThumb(model: model, item: item, next: index == 0, dragging: model.draggedId == item.id, u: u, vertical: vertical)
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
    let dragging: Bool
    let u: CGFloat
    let vertical: Bool
    @State private var choosing = false
    @GestureState private var held = false

    var body: some View {
        DjQueueArt(item: item, next: next, u: u, vertical: vertical, showsAdder: model.inJam)
            .opacity(dragging ? 0.35 : 1)
            .scaleEffect(held && !dragging ? 0.94 : 1)
            .animation(.easeOut(duration: 0.15), value: held)
            .contentShape(Rectangle())
            .onTapGesture {
                DjTheme.tick()
                choosing = true
            }
            .gesture(
                LongPressGesture(minimumDuration: 0.2)
                    .sequenced(before: DragGesture(minimumDistance: 0, coordinateSpace: .named(DjStudioView.space)))
                    .updating($held) { _, state, _ in state = true }
                    .onChanged { value in
                        guard case let .second(true, drag?) = value else { return }
                        model.dragQueue(item, to: drag.location)
                    }
                    .onEnded { value in
                        guard case let .second(true, drag?) = value else {
                            model.cancelQueueDrag()
                            return
                        }
                        model.dropQueue(at: drag.location)
                    })
            .onChange(of: held) { _, now in
                if !now, model.draggedId == item.id { model.cancelQueueDrag() }
            }
            .confirmationDialog(item.title.isEmpty ? "YouTube video" : item.title, isPresented: $choosing, titleVisibility: .visible) {
                Button("Load on Deck A") { model.playFromQueue(item, on: .a) }
                Button("Load on Deck B") { model.playFromQueue(item, on: .b) }
                Button("Tags and Playlists…") { model.editSong(item.videoId) }
                Button("Remove from Queue", role: .destructive) { model.removeFromQueue(item) }
            } message: {
                Text("Or hold the thumbnail and drag it onto a deck.")
            }
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(item.title.isEmpty ? "Queued video" : "Queued: \(item.title)")
            .accessibilityAddTraits(.isButton)
            .accessibilityAction(named: "Load on Deck A") { model.playFromQueue(item, on: .a) }
            .accessibilityAction(named: "Load on Deck B") { model.playFromQueue(item, on: .b) }
            .accessibilityAction(named: "Remove from Queue") { model.removeFromQueue(item) }
    }
}

struct DjQueueArt: View {
    let item: DjQueueItem
    let next: Bool
    let u: CGFloat
    let vertical: Bool
    let showsAdder: Bool

    var body: some View {
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
            if showsAdder, !item.addedBy.isEmpty {
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
}

struct DjQueueGhost: View {
    let model: DjStudioModel

    var body: some View {
        if let drag = model.queueDrag {
            DjQueueArt(item: drag.item, next: false, u: 1, vertical: false, showsAdder: false)
                .frame(width: 96, height: 58)
                .scaleEffect(model.dropSide == nil ? 1 : 1.12)
                .shadow(color: .black.opacity(0.6), radius: 10, y: 6)
                .overlay(alignment: .bottom) {
                    Text(model.dropSide.map { "Load on Deck \($0.label)" } ?? "Drop on a deck")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundStyle(DjTheme.ink)
                        .padding(.horizontal, 8)
                        .padding(.vertical, 3)
                        .background(model.dropSide == nil ? DjTheme.panel : DjTheme.active, in: Capsule())
                        .fixedSize()
                        .offset(y: 16)
                }
                .position(x: drag.location.x, y: drag.location.y - 40)
                .animation(.easeOut(duration: 0.12), value: model.dropSide)
                .allowsHitTesting(false)
                .accessibilityHidden(true)
        }
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
                        let status = model.status(of: member)
                        HStack(spacing: 10) {
                            DjPersonIcon(name: member.name, status: status, host: member.status == "owner", shake: .still, energy: 0, reaction: nil, size: 26)
                            Text(member.name)
                            Spacer()
                            Text(member.status == "owner" ? "Host · \(status.label)" : status.label)
                                .font(.footnote)
                                .foregroundStyle(status.live ? DjTheme.active : .secondary)
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
                    Toggle("Share My Phone’s Motion", isOn: Binding(get: { model.shareMotion }, set: { model.setShareMotion($0) }))
                } footer: {
                    Text("While you are in the jam with the studio open or music playing, your icon shakes with your phone’s accelerometer for everyone in the jam. Only movement is shared, never your location.")
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
