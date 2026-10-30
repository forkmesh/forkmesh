import FMPackages
import SwiftUI
import UIKit

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
    @State private var pressed = false
    @GestureState private var tracking = false

    var body: some View {
        draggable(
            DjQueueArt(item: item, next: next, u: u, vertical: vertical, showsAdder: model.inJam)
                .opacity(dragging ? 0.35 : 1)
                .scaleEffect(pressed && !dragging ? 0.94 : 1)
                .animation(.easeOut(duration: 0.15), value: pressed)
                .contentShape(Rectangle())
                .onTapGesture {
                    DjTheme.tick()
                    choosing = true
                }
                .onLongPressGesture(minimumDuration: 0.35, maximumDistance: 8) {
                    DjTheme.tick()
                    choosing = true
                } onPressingChanged: { now in
                    pressed = now
                }
        )
        .onChange(of: tracking) { _, now in
            if !now, model.draggedId == item.id { model.cancelQueueDrag() }
        }
        .confirmationDialog(item.title.isEmpty ? "YouTube video" : item.title, isPresented: $choosing, titleVisibility: .visible) {
            Button("Load on Deck A") { model.playFromQueue(item, on: .a) }
            Button("Load on Deck B") { model.playFromQueue(item, on: .b) }
            Button("Tags and Playlists…") { model.editSong(item.videoId) }
            Button("Remove from Queue", role: .destructive) { model.removeFromQueue(item) }
        } message: {
            Text([item.addedLine.map { $0 + "." }, "Or drag the thumbnail onto a deck."].compactMap { $0 }.joined(separator: "\n"))
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel([item.title.isEmpty ? "Queued video" : "Queued: \(item.title)", model.inJam ? item.addedLine : nil].compactMap { $0 }.joined(separator: ", "))
        .accessibilityAddTraits(.isButton)
        .accessibilityAction(named: "Load on Deck A") { model.playFromQueue(item, on: .a) }
        .accessibilityAction(named: "Load on Deck B") { model.playFromQueue(item, on: .b) }
        .accessibilityAction(named: "Remove from Queue") { model.removeFromQueue(item) }
    }

    private var across: Axis { vertical ? .horizontal : .vertical }

    @ViewBuilder private func draggable<Content: View>(_ content: Content) -> some View {
        if #available(iOS 18.0, *) {
            content.gesture(DjQueuePan(across: across, moved: { model.dragQueue(item, to: $0) }, ended: { point in
                guard model.draggedId == item.id else { return }
                if let point { model.dropQueue(at: point) } else { model.cancelQueueDrag() }
            }))
        } else {
            content.simultaneousGesture(
                DragGesture(minimumDistance: 8, coordinateSpace: .named(DjStudioView.space))
                    .updating($tracking) { _, state, _ in state = true }
                    .onChanged { value in
                        if model.draggedId != item.id {
                            guard model.queueDrag == nil, DjQueueLane.heads(across, value.translation), hypot(value.translation.width, value.translation.height) < 40 else { return }
                        }
                        model.dragQueue(item, to: value.location)
                    }
                    .onEnded { value in
                        if model.draggedId == item.id { model.dropQueue(at: value.location) }
                    })
        }
    }
}

@available(iOS 18.0, *)
private struct DjQueuePan: UIGestureRecognizerRepresentable {
    let across: Axis
    let moved: (CGPoint) -> Void
    let ended: (CGPoint?) -> Void

    func makeCoordinator(converter: CoordinateSpaceConverter) -> Coordinator { Coordinator() }

    func makeUIGestureRecognizer(context: Context) -> UIPanGestureRecognizer {
        let pan = UIPanGestureRecognizer()
        pan.maximumNumberOfTouches = 1
        pan.delegate = context.coordinator
        context.coordinator.across = across
        return pan
    }

    func updateUIGestureRecognizer(_ recognizer: UIPanGestureRecognizer, context: Context) {
        context.coordinator.across = across
    }

    func handleUIGestureRecognizerAction(_ recognizer: UIPanGestureRecognizer, context: Context) {
        let point: CGPoint = context.converter.location(in: .named(DjStudioView.space))
        switch recognizer.state {
        case .began, .changed:
            moved(point)
        case .ended:
            ended(point)
        case .cancelled, .failed:
            ended(nil)
        default:
            break
        }
    }

    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        var across: Axis = .horizontal

        func gestureRecognizerShouldBegin(_ gestureRecognizer: UIGestureRecognizer) -> Bool {
            guard let pan = gestureRecognizer as? UIPanGestureRecognizer else { return false }
            let velocity: CGPoint = pan.velocity(in: pan.view)
            let heading: CGPoint = velocity == .zero ? pan.translation(in: pan.view) : velocity
            return DjQueueLane.heads(across, CGSize(width: heading.x, height: heading.y))
        }

        func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, shouldBeRequiredToFailBy other: UIGestureRecognizer) -> Bool {
            other.view is UIScrollView
        }
    }
}

private enum DjQueueLane {
    static func heads(_ across: Axis, _ moved: CGSize) -> Bool {
        across == .horizontal ? abs(moved.width) > abs(moved.height) : abs(moved.height) > abs(moved.width)
    }
}

struct DjQueueArt: View {
    let item: DjQueueItem
    let next: Bool
    let u: CGFloat
    let vertical: Bool
    let showsAdder: Bool
    @State private var loaded: UIImage?

    var body: some View {
        Group {
            if let image = loaded ?? DjQueueThumbs.cached(item.thumbnail) {
                Image(uiImage: image).resizable().scaledToFill()
            } else {
                Color.white.opacity(0.08)
            }
        }
        .task(id: item.thumbnail) { loaded = await DjQueueThumbs.load(item.thumbnail) }
        .frame(maxWidth: vertical ? .infinity : nil)
        .frame(width: vertical ? nil : 72 * u, height: vertical ? nil : 44 * u)
        .aspectRatio(16 / 9, contentMode: .fit)
        .clipShape(RoundedRectangle(cornerRadius: 4 * u, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 4 * u, style: .continuous).strokeBorder(next ? DjTheme.active.opacity(0.8) : DjTheme.edge, lineWidth: next ? 1.5 : 1))
        .overlay(alignment: .bottomLeading) {
            if showsAdder, !item.addedBy.isEmpty {
                VStack(alignment: .leading, spacing: 0) {
                    Text(item.addedBy)
                        .font(.system(size: 8 * u, weight: .semibold))
                    if let stamp = item.addedStamp {
                        Text(stamp)
                            .font(.system(size: 7 * u, weight: .medium).monospacedDigit())
                            .opacity(0.8)
                    }
                }
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

extension DjQueueItem {
    var addedStamp: String? {
        guard let date = addedDate else { return nil }
        if Calendar.current.isDateInToday(date) { return date.formatted(date: .omitted, time: .shortened) }
        return date.formatted(.dateTime.month(.abbreviated).day().hour().minute())
    }

    var addedLine: String? {
        let when = addedDate?.formatted(date: .abbreviated, time: .shortened)
        if !addedBy.isEmpty, let when { return "Added by \(addedBy) on \(when)" }
        if !addedBy.isEmpty { return "Added by \(addedBy)" }
        if let when { return "Added on \(when)" }
        return nil
    }
}

@MainActor
enum DjQueueThumbs {
    private static let cache = NSCache<NSURL, UIImage>()

    static func cached(_ url: URL) -> UIImage? { cache.object(forKey: url as NSURL) }

    static func load(_ url: URL) async -> UIImage? {
        if let hit = cached(url) { return hit }
        guard let (data, _) = try? await URLSession.shared.data(from: url), let image = UIImage(data: data) else { return nil }
        cache.setObject(image, forKey: url as NSURL)
        return image
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
                .animation(.easeOut(duration: 0.12), value: model.dropSide)
                .position(x: drag.location.x, y: drag.location.y - 40)
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
