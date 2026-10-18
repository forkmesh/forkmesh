import ARKit
import Combine
import FMAssistant
import QuartzCore
import RealityKit
import UIKit

@MainActor
@Observable
final class SpiderRoomStage {
    var found: String = ""
    var floorFound: Bool = false
    var tracking: String?
    var webs: Int = 0
    var arrived: Bool = false
    @ObservationIgnored weak var director: SpiderRoomDirector?

    func summon() { director?.summon() }

    func clearWebs() { director?.clearWebs() }
}

@MainActor
final class SpiderRoomDirector: NSObject, UIGestureRecognizerDelegate {
    let arView: ARView
    private let stage: SpiderRoomStage
    private var map: RoomMap = RoomMap()
    private var spider: RoomSpider = RoomSpider(seed: UInt64.random(in: 1...UInt64.max))
    private let body: SpiderRoomBody = SpiderRoomBody()
    private let world: AnchorEntity = AnchorEntity(world: .zero)
    private let thread: ModelEntity = SpiderRoomSilk.strand(from: .zero, to: RoomPoint(0, 0.1, 0), width: 0.0014)
    private let bubble: SpiderRoomBubble = SpiderRoomBubble()
    private var webs: [Int: Entity] = [:]
    private var updates: Cancellable?
    private var running: Bool = false
    private var tracked: Double = 0
    private var holdDistance: Float = 0.5
    private var thumps: Int = 0
    private var started: CFTimeInterval = CACurrentMediaTime()
    private let tap: UIImpactFeedbackGenerator = UIImpactFeedbackGenerator(style: .light)
    private let thud: UIImpactFeedbackGenerator = UIImpactFeedbackGenerator(style: .medium)

    init(stage: SpiderRoomStage) {
        self.stage = stage
        arView = ARView(frame: .zero, cameraMode: .ar, automaticallyConfigureSession: false)
        super.init()
        stage.director = self
        arView.renderOptions.insert(.disableMotionBlur)
        world.addChild(body.root)
        world.addChild(thread)
        body.root.isEnabled = false
        thread.isEnabled = false
        arView.scene.addAnchor(world)
        arView.session.delegate = self
        arView.addSubview(bubble)
        let pan: UIPanGestureRecognizer = UIPanGestureRecognizer(target: self, action: #selector(panned(_:)))
        pan.maximumNumberOfTouches = 1
        pan.delegate = self
        arView.addGestureRecognizer(pan)
        let press: UITapGestureRecognizer = UITapGestureRecognizer(target: self, action: #selector(tapped(_:)))
        arView.addGestureRecognizer(press)
    }

    func start() {
        guard !running else { return }
        let configuration: ARWorldTrackingConfiguration = ARWorldTrackingConfiguration()
        configuration.planeDetection = [.horizontal, .vertical]
        configuration.environmentTexturing = .automatic
        if ARWorldTrackingConfiguration.supportsSceneReconstruction(.mesh) {
            configuration.sceneReconstruction = .mesh
            arView.environment.sceneUnderstanding.options.insert(.occlusion)
        }
        arView.session.run(configuration, options: [.resetTracking, .removeExistingAnchors])
        updates = arView.scene.subscribe(to: SceneEvents.Update.self) { [weak self] (event: SceneEvents.Update) in
            let span: Double = event.deltaTime
            MainActor.assumeIsolated {
                self?.tick(span)
            }
        }
        running = true
        tap.prepare()
    }

    func stop() {
        guard running else { return }
        updates?.cancel()
        updates = nil
        arView.session.pause()
        running = false
    }

    func summon() {
        let spot: RoomPoint = inFront(distance: 0.7, rise: 0.25)
        spider.appear(at: spot, in: map)
        stage.arrived = true
    }

    func clearWebs() {
        spider.clearWebs()
        syncWebs()
    }

    private var camera: RoomPoint { arView.cameraTransform.translation }

    private func inFront(distance: Float, rise: Float) -> RoomPoint {
        let matrix: simd_float4x4 = arView.cameraTransform.matrix
        let ahead: RoomPoint = -RoomPoint(matrix.columns.2.x, matrix.columns.2.y, matrix.columns.2.z)
        let level: RoomPoint = RoomMath.normalized(RoomMath.flat(ahead, on: RoomMath.up), or: RoomPoint(0, 0, -1))
        return camera + level * distance + RoomPoint(0, rise, 0)
    }

    private func tick(_ span: Double) {
        map.look(from: camera)
        if !spider.visible {
            tracked += span
            if map.floorFound || tracked > 4 { summon() }
        }
        spider.step(span, in: map)
        render()
        if spider.thumps != thumps {
            thumps = spider.thumps
            thud.impactOccurred(intensity: 0.7)
        }
        if stage.webs != spider.webs.count { syncWebs() }
        let found: String = map.summary
        if stage.found != found { stage.found = found }
        if stage.floorFound != map.floorFound { stage.floorFound = map.floorFound }
    }

    private func render() {
        body.root.isEnabled = spider.visible
        guard spider.visible else {
            thread.isEnabled = false
            bubble.show(nil, at: nil)
            return
        }
        var up: RoomPoint = spider.up
        var heading: RoomPoint = spider.heading
        if spider.mode == .held || spider.silkAnchor != nil {
            up = RoomMath.up
            heading = RoomMath.flat(camera - spider.position, on: up)
        }
        body.place(at: spider.position, up: up, heading: heading)
        body.pose(spider, time: Float(CACurrentMediaTime() - started))
        if let anchor = spider.silkAnchor {
            thread.isEnabled = true
            SpiderRoomSilk.stretch(thread, from: anchor, to: spider.position + RoomPoint(0, 0.03, 0), width: 0.0014)
        } else {
            thread.isEnabled = false
        }
        let over: RoomPoint = spider.position + up * 0.09
        bubble.show(spider.caption, at: arView.project(over))
    }

    private func syncWebs() {
        let wanted: Set<Int> = Set(spider.webs.map(\.id))
        for (id, entity) in webs where !wanted.contains(id) {
            entity.removeFromParent()
            webs[id] = nil
        }
        for web in spider.webs where webs[web.id] == nil {
            let entity: Entity = SpiderRoomSilk.web(web)
            world.addChild(entity)
            webs[web.id] = entity
        }
        stage.webs = spider.webs.count
    }

    private func touchesSpider(_ point: CGPoint) -> Bool {
        guard spider.visible, let seen = arView.project(spider.position) else { return false }
        let near: CGPoint? = arView.project(spider.position + spider.up * 0.08)
        let size: CGFloat = near.map { hypot($0.x - seen.x, $0.y - seen.y) } ?? 0
        return hypot(point.x - seen.x, point.y - seen.y) < max(60, size * 1.4)
    }

    func gestureRecognizerShouldBegin(_ gestureRecognizer: UIGestureRecognizer) -> Bool {
        guard gestureRecognizer is UIPanGestureRecognizer else { return true }
        return touchesSpider(gestureRecognizer.location(in: arView))
    }

    @objc private func panned(_ gesture: UIPanGestureRecognizer) {
        let point: CGPoint = gesture.location(in: arView)
        let now: Double = CACurrentMediaTime()
        switch gesture.state {
        case .began:
            holdDistance = max(0.25, min(1.5, RoomMath.length(spider.position - camera)))
            spider.grab(at: spider.position, time: now)
            tap.impactOccurred()
        case .changed:
            guard let ray = arView.ray(through: point) else { return }
            spider.drag(to: ray.origin + ray.direction * holdDistance, time: now)
        case .ended, .cancelled, .failed:
            if let ray = arView.ray(through: point) {
                spider.drag(to: ray.origin + ray.direction * holdDistance, time: now)
            }
            spider.release(time: now, in: map)
        default:
            break
        }
    }

    @objc private func tapped(_ gesture: UITapGestureRecognizer) {
        let point: CGPoint = gesture.location(in: arView)
        if touchesSpider(point) {
            spider.poke()
            tap.impactOccurred()
            return
        }
        guard let hit = arView.raycast(from: point, allowing: .existingPlaneGeometry, alignment: .any).first else {
            if let guess = arView.raycast(from: point, allowing: .estimatedPlane, alignment: .horizontal).first {
                let spot: RoomPoint = RoomPoint(guess.worldTransform.columns.3.x, guess.worldTransform.columns.3.y, guess.worldTransform.columns.3.z)
                if abs(spot.y - map.floorY) < 0.15 { send(to: spot, on: RoomMap.floorId) }
            }
            return
        }
        let spot: RoomPoint = RoomPoint(hit.worldTransform.columns.3.x, hit.worldTransform.columns.3.y, hit.worldTransform.columns.3.z)
        guard let id = hit.anchor?.identifier.uuidString, map.surface(id) != nil else { return }
        send(to: spot, on: id)
    }

    private func send(to spot: RoomPoint, on id: String) {
        if !spider.visible {
            spider.appear(at: spot + RoomPoint(0, 0.6, 0), in: map)
            return
        }
        spider.come(to: spot, on: id, in: map)
    }

    fileprivate func apply(_ surfaces: [RoomSurface]) {
        for surface in surfaces { map.put(surface) }
    }

    fileprivate func forget(_ ids: [String]) {
        for id in ids { map.remove(id) }
    }

    fileprivate func track(_ state: ARCamera.TrackingState) {
        let note: String?
        switch state {
        case .normal: note = nil
        case .notAvailable: note = "The camera can't track the room right now."
        case .limited(let reason):
            switch reason {
            case .initializing, .relocalizing: note = "Getting to know your room…"
            case .excessiveMotion: note = "Move the phone a little slower."
            case .insufficientFeatures: note = "Point at something with more detail, or turn on a light."
            @unknown default: note = "Getting to know your room…"
            }
        }
        if stage.tracking != note { stage.tracking = note }
    }
}

extension SpiderRoomDirector: ARSessionDelegate {
    nonisolated static func surface(_ anchor: ARPlaneAnchor) -> RoomSurface {
        let t: simd_float4x4 = anchor.transform
        let origin: RoomPoint = RoomPoint(t.columns.3.x, t.columns.3.y, t.columns.3.z)
        let ax: RoomPoint = RoomPoint(t.columns.0.x, t.columns.0.y, t.columns.0.z)
        let ay: RoomPoint = RoomPoint(t.columns.1.x, t.columns.1.y, t.columns.1.z)
        let az: RoomPoint = RoomPoint(t.columns.2.x, t.columns.2.y, t.columns.2.z)
        let corners: [SIMD3<Float>] = anchor.geometry.boundaryVertices
        var low: SIMD2<Float> = SIMD2<Float>(anchor.center.x - 0.15, anchor.center.z - 0.15)
        var high: SIMD2<Float> = SIMD2<Float>(anchor.center.x + 0.15, anchor.center.z + 0.15)
        if let first = corners.first {
            low = SIMD2<Float>(first.x, first.z)
            high = low
            for corner in corners {
                low = SIMD2<Float>(min(low.x, corner.x), min(low.y, corner.z))
                high = SIMD2<Float>(max(high.x, corner.x), max(high.y, corner.z))
            }
        }
        let middle: SIMD2<Float> = (low + high) / 2
        return RoomSurface(
            id: anchor.identifier.uuidString, kind: kind(anchor), center: origin + ax * middle.x + az * middle.y,
            normal: ay, across: ax, half: (high - low) / 2)
    }

    nonisolated static func kind(_ anchor: ARPlaneAnchor) -> RoomSurfaceKind {
        guard ARPlaneAnchor.isClassificationSupported else {
            return anchor.alignment == .vertical ? .wall : .other
        }
        switch anchor.classification {
        case .wall: return .wall
        case .floor: return .floor
        case .ceiling: return .ceiling
        case .table: return .table
        case .seat: return .seat
        case .window: return .window
        case .door: return .door
        case .none: return anchor.alignment == .vertical ? .wall : .other
        @unknown default: return .other
        }
    }

    nonisolated func session(_ session: ARSession, didAdd anchors: [ARAnchor]) {
        let surfaces: [RoomSurface] = anchors.compactMap { ($0 as? ARPlaneAnchor).map(Self.surface) }
        guard !surfaces.isEmpty else { return }
        MainActor.assumeIsolated { apply(surfaces) }
    }

    nonisolated func session(_ session: ARSession, didUpdate anchors: [ARAnchor]) {
        let surfaces: [RoomSurface] = anchors.compactMap { ($0 as? ARPlaneAnchor).map(Self.surface) }
        guard !surfaces.isEmpty else { return }
        MainActor.assumeIsolated { apply(surfaces) }
    }

    nonisolated func session(_ session: ARSession, didRemove anchors: [ARAnchor]) {
        let ids: [String] = anchors.compactMap { $0 is ARPlaneAnchor ? $0.identifier.uuidString : nil }
        guard !ids.isEmpty else { return }
        MainActor.assumeIsolated { forget(ids) }
    }

    nonisolated func session(_ session: ARSession, cameraDidChangeTrackingState camera: ARCamera) {
        let state: ARCamera.TrackingState = camera.trackingState
        MainActor.assumeIsolated { track(state) }
    }
}

final class SpiderRoomBubble: UIView {
    private let label: UILabel = UILabel()

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = UIColor(white: 1, alpha: 0.94)
        layer.cornerRadius = 14
        layer.shadowColor = UIColor.black.cgColor
        layer.shadowOpacity = 0.25
        layer.shadowRadius = 6
        layer.shadowOffset = CGSize(width: 0, height: 2)
        label.font = UIFont.preferredFont(forTextStyle: .subheadline)
        label.adjustsFontForContentSizeCategory = true
        label.textColor = UIColor(red: 0.12, green: 0.10, blue: 0.22, alpha: 1)
        label.numberOfLines = 2
        label.textAlignment = .center
        addSubview(label)
        isUserInteractionEnabled = false
        isAccessibilityElement = false
        alpha = 0
    }

    required init?(coder: NSCoder) { nil }

    func show(_ text: String?, at point: CGPoint?) {
        guard let text, let point, let host = superview, host.bounds.insetBy(dx: -40, dy: -40).contains(point) else {
            if alpha > 0 { UIView.animate(withDuration: 0.2) { self.alpha = 0 } }
            return
        }
        if label.text != text {
            label.text = text
            if UIAccessibility.isVoiceOverRunning { UIAccessibility.post(notification: .announcement, argument: text) }
        }
        let widest: CGFloat = min(260, host.bounds.width - 32)
        let fits: CGSize = label.sizeThatFits(CGSize(width: widest - 24, height: 80))
        let size: CGSize = CGSize(width: min(widest, fits.width + 24), height: fits.height + 14)
        let x: CGFloat = max(16, min(host.bounds.width - size.width - 16, point.x - size.width / 2))
        let y: CGFloat = max(host.safeAreaInsets.top + 8, min(host.bounds.height - size.height - 16, point.y - size.height - 8))
        frame = CGRect(origin: CGPoint(x: x, y: y), size: size)
        label.frame = bounds.insetBy(dx: 12, dy: 7)
        if alpha < 1 { UIView.animate(withDuration: 0.2) { self.alpha = 1 } }
    }
}
