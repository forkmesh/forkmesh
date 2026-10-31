import FMAgents
import FMBase
import FMControl
import Observation
import SwiftUI
import UIKit

struct AgentLineDeleteRequest {
    let id: String
    let deletion: AgentLineDeletion
    let desktopKey: String
    let key: String

    init?(_ line: AgentLine) {
        guard let deletion = AgentLines.deletion(line) else { return nil }
        id = line.id
        self.deletion = deletion
        if case .control(let row) = line.source { desktopKey = row.desktopKey } else { desktopKey = "" }
        key = AgentLines.deletionKey(line) ?? deletion.key
    }

    init(id: String, deletion: AgentLineDeletion) {
        self.id = id
        self.deletion = deletion
        desktopKey = ""
        key = deletion.key
    }

    var asksFirst: Bool {
        if case .outside = deletion { return true }
        return false
    }

    @MainActor func run() async -> AgentDeleteOutcome {
        switch deletion {
        case .outside(let id):
            guard let client = AgentOpsFeature.client(desktopKey: desktopKey) else { return .failed("The desktop is not in reach.") }
            let model = AgentDeleteModel(client: client, sessionId: id)
            await model.prepare()
            if await model.run() { return .deleted("Ended and removed on the desktop.") }
            return .failed(model.errorText.isEmpty ? "Could not delete the agent." : model.errorText)
        case .desktop(let sessionId):
            guard let client = AgentOpsFeature.client(desktopKey: desktopKey) else { return .failed("The desktop is not in reach.") }
            let model = AgentDeleteModel(client: client, sessionId: sessionId)
            await model.prepare()
            if await model.run() {
                return .deleted(model.report?.summary(place: "on the desktop") ?? "Deleted on the desktop.")
            }
            return .failed(model.errorText.isEmpty ? "Could not delete the agent." : model.errorText)
        case .runner(let ref, let merged, let active):
            guard let api = RunnerAgentFeature.api else { return .failed("Sign in to delete runs on ForkMesh.") }
            let model = RunnerDeleteModel(api: api, ref: ref, merged: merged, active: active)
            if await model.run() { return .deleted(model.report.summary(place: "on ForkMesh")) }
            return .failed(model.errorText.isEmpty ? "Could not delete the agent." : model.errorText)
        }
    }

    @MainActor func runAfterUndoWindow() {
        let request: AgentLineDeleteRequest = self
        AgentPendingDeletes.shared.schedule(key, message: "Agent deleted.") {
            let outcome: AgentDeleteOutcome = await request.run()
            if !outcome.failed { await HostsFeature.agents?.refresh() }
            return outcome
        }
    }
}

@MainActor
@Observable
final class AgentUndoDeleteSeen {
    static let shared = AgentUndoDeleteSeen()

    var failures = 0
    var reports = 0
}

struct AgentUndoDeleteLine: View {
    private var pending: AgentPendingDeletes { .shared }
    private var seen: AgentUndoDeleteSeen { .shared }

    @MainActor static var hasNotice: Bool {
        let pending: AgentPendingDeletes = .shared
        let seen: AgentUndoDeleteSeen = .shared
        return pending.offer != nil
            || (pending.failure != nil && pending.failures > seen.failures)
            || (pending.report != nil && pending.reports > seen.reports)
    }

    @MainActor static func markSeen() {
        let pending: AgentPendingDeletes = .shared
        let seen: AgentUndoDeleteSeen = .shared
        seen.failures = pending.failures
        seen.reports = pending.reports
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if let offer = pending.offer {
                FmNoticeLine(offer.message, actionLabel: "Undo", onAction: { pending.undo(offer.key) })
            }
            if let failure = pending.failure, pending.failures > seen.failures {
                FmNoticeLine(failure, isError: true, onDismiss: { seen.failures = pending.failures })
            }
            if let report = pending.report, pending.reports > seen.reports {
                FmNoticeLine(report, onDismiss: { seen.reports = pending.reports })
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

extension View {
    func agentOutsideDeleteConfirmation(_ pending: Binding<AgentLineDeleteRequest?>, run: @escaping (AgentLineDeleteRequest) -> Void)
        -> some View
    {
        confirmationDialog(
            "Stop and delete this agent?",
            isPresented: Binding(get: { pending.wrappedValue != nil }, set: { if !$0 { pending.wrappedValue = nil } }),
            titleVisibility: .visible,
            presenting: pending.wrappedValue
        ) { request in
            Button("Delete", role: .destructive) { run(request) }
            Button("Cancel", role: .cancel) {}
        } message: { _ in
            Text("It was started outside ForkMesh. Deleting it ends that process on the desktop.")
        }
    }
}

struct AgentSwipeToDelete: ViewModifier {
    @Environment(\.fmPalette) private var fm
    let id: String
    @Binding var openId: String?
    let enabled: Bool
    let onDelete: () -> Void

    @GestureState(resetTransaction: Transaction(animation: .easeOut(duration: 0.2))) private var track = SwipeTrack()
    @State private var dragX: CGFloat?

    private static let actionWidth: CGFloat = 76
    private static let fullSwipe: CGFloat = 200

    private var isOpen: Bool { enabled && openId == id }

    private var base: CGFloat { isOpen ? -Self.actionWidth : 0 }

    private var offset: CGFloat { min(0, base + (dragX ?? 0) + (track.horizontal == true ? track.dx : 0)) }

    private static func sideways(_ moved: CGSize) -> Bool { abs(moved.width) > abs(moved.height) * 1.5 }

    func body(content: Content) -> some View {
        swipeGesture(
            content
                .overlay {
                    if isOpen {
                        Color.clear
                            .contentShape(Rectangle())
                            .onTapGesture { close() }
                    }
                }
                .offset(x: offset)
                .background(alignment: .trailing) {
                    if enabled && offset < 0 {
                        Button {
                            close()
                            onDelete()
                        } label: {
                            VStack(spacing: 2) {
                                Image(systemName: "trash")
                                    .font(.system(size: 14, weight: .semibold))
                                Text("Delete")
                                    .font(.system(size: 10, weight: .semibold))
                            }
                            .foregroundStyle(.white)
                            .frame(width: max(Self.actionWidth, -offset))
                            .frame(maxHeight: .infinity)
                            .background(fm.danger)
                            .clipShape(FmRoundedShape(radius: FmRadius.sm))
                        }
                        .buttonStyle(.plain)
                        .accessibilityIdentifier("agents-line-delete")
                    }
                }
        )
        .accessibilityActions {
            if enabled {
                Button("Delete") { onDelete() }
            }
        }
        .onDisappear {
            if isOpen { openId = nil }
        }
    }

    @ViewBuilder private func swipeGesture<Row: View>(_ row: Row) -> some View {
        if #available(iOS 18.0, *) {
            row.gesture(AgentSidewaysPan(enabled: enabled, moved: moved, ended: ended))
        } else {
            row.simultaneousGesture(
                DragGesture(minimumDistance: 16)
                    .updating($track) { (value: DragGesture.Value, state: inout SwipeTrack, _: inout Transaction) in
                        if state.horizontal == nil { state.horizontal = Self.sideways(value.translation) }
                        if state.horizontal == true { state.dx = value.translation.width }
                    }
                    .onChanged { (value: DragGesture.Value) in
                        if Self.sideways(value.translation) { closeOthers() }
                    }
                    .onEnded { (value: DragGesture.Value) in
                        guard Self.sideways(value.translation) else { return }
                        ended(value.translation.width, value.predictedEndTranslation.width)
                    },
                including: enabled ? .all : .subviews)
        }
    }

    private func closeOthers() {
        if !isOpen, openId != nil {
            withAnimation(.easeOut(duration: 0.2)) { openId = nil }
        }
    }

    private func moved(_ dx: CGFloat) {
        if dragX == nil { closeOthers() }
        dragX = dx
    }

    private func ended(_ dx: CGFloat, _ flung: CGFloat) {
        let reach = min(0, base + dx)
        withAnimation(.easeOut(duration: 0.2)) {
            dragX = nil
            if reach < -Self.fullSwipe {
                openId = nil
            } else if reach < -Self.actionWidth / 2 || (!isOpen && flung < -Self.actionWidth * 1.5) {
                openId = id
            } else {
                openId = nil
            }
        }
        if reach < -Self.fullSwipe {
            FmHaptics.tap()
            onDelete()
        }
    }

    private func close() {
        withAnimation(.easeOut(duration: 0.2)) {
            if isOpen { openId = nil }
        }
    }
}

private struct SwipeTrack {
    var dx: CGFloat = 0
    var horizontal: Bool?
}

@available(iOS 18.0, *)
private struct AgentSidewaysPan: UIGestureRecognizerRepresentable {
    let enabled: Bool
    let moved: (CGFloat) -> Void
    let ended: (CGFloat, CGFloat) -> Void

    private static let flingSeconds: CGFloat = 0.25

    func makeCoordinator(converter: CoordinateSpaceConverter) -> Coordinator { Coordinator() }

    func makeUIGestureRecognizer(context: Context) -> UIPanGestureRecognizer {
        let pan = UIPanGestureRecognizer()
        pan.maximumNumberOfTouches = 1
        pan.delegate = context.coordinator
        context.coordinator.enabled = enabled
        return pan
    }

    func updateUIGestureRecognizer(_ recognizer: UIPanGestureRecognizer, context: Context) {
        context.coordinator.enabled = enabled
    }

    func handleUIGestureRecognizerAction(_ recognizer: UIPanGestureRecognizer, context: Context) {
        let dx: CGFloat = recognizer.translation(in: recognizer.view).x
        switch recognizer.state {
        case .began, .changed:
            moved(dx)
        case .ended:
            ended(dx, dx + recognizer.velocity(in: recognizer.view).x * Self.flingSeconds)
        case .cancelled, .failed:
            ended(0, 0)
        default:
            break
        }
    }

    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        var enabled = true

        func gestureRecognizerShouldBegin(_ gestureRecognizer: UIGestureRecognizer) -> Bool {
            guard enabled, let pan = gestureRecognizer as? UIPanGestureRecognizer else { return false }
            let velocity: CGPoint = pan.velocity(in: pan.view)
            let heading: CGPoint = velocity == .zero ? pan.translation(in: pan.view) : velocity
            return abs(heading.x) > abs(heading.y) * 1.5
        }
    }
}

extension View {
    func agentSwipeToDelete(id: String, openId: Binding<String?>, enabled: Bool, onDelete: @escaping () -> Void)
        -> some View
    {
        modifier(AgentSwipeToDelete(id: id, openId: openId, enabled: enabled, onDelete: onDelete))
    }
}
