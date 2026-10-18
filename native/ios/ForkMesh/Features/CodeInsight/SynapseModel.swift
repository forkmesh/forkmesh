import FMCodeInsight
import Foundation
import Observation
import QuartzCore
import SwiftUI

struct SynapseLayoutResult: Sendable {
    let graph: SynapseGraph
    let positions: [SynapseVector]
}

@MainActor
enum SynapseLayoutCache {
    private static var entries: [(key: String, value: SynapseLayoutResult)] = []

    static func get(_ key: String) -> SynapseLayoutResult? {
        entries.first { $0.key == key }?.value
    }

    static func put(_ key: String, _ value: SynapseLayoutResult) {
        entries.removeAll { $0.key == key }
        entries.append((key, value))
        if entries.count > 4 { entries.removeFirst(entries.count - 4) }
    }
}

final class SynapseOwner: @unchecked Sendable {
    weak var model: SynapseModel?

    init(_ model: SynapseModel) {
        self.model = model
    }
}

final class SynapsePublishGate: @unchecked Sendable {
    private let lock = NSLock()
    private var busy = false

    func claim() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if busy { return false }
        busy = true
        return true
    }

    func release() {
        lock.lock()
        busy = false
        lock.unlock()
    }
}

final class SynapseViewport {
    struct Flight {
        var from: SynapseCamera
        var to: SynapseCamera
        var start: Double
        var duration: Double
    }

    var graph: SynapseGraph?
    var positions: [SynapseVector] = []
    var lobeCentres: [SynapseVector] = []
    var style = SynapseStyle()
    var styleRevision = 0
    var camera = SynapseCamera()
    var flight: Flight?
    var lastTime: Double?
    var pauseUntil: Double = 0
    var touching = false
    var size = CGSize(width: 1, height: 1)
    var reduceMotion = false
    var fileNames: [String] = []
    let frame = SynapseFrame()
    let stars = Synapse.stars()
    private(set) var dust: [SynapseVector] = []
    private var dustShape: SynapseBrainShape?

    var aspect: Float { Float(max(1, size.width) / max(1, size.height)) }

    static func now() -> Double { CACurrentMediaTime() }

    func fileName(_ file: Int) -> String {
        file >= 0 && file < fileNames.count ? fileNames[file] : ""
    }

    func prepareDust(_ shape: SynapseBrainShape) {
        if dustShape == shape { return }
        dust = Synapse.dust(shape)
        dustShape = shape
    }

    func pause(_ now: Double = SynapseViewport.now()) {
        pauseUntil = now + 2.5
    }

    func advance(to now: Double, autoRotate: Bool) -> Bool {
        let dt = lastTime.map { min(max(now - $0, 0), 0.1) } ?? 0
        lastTime = now
        var landed = false
        if let flight {
            let u = Float(min(max((now - flight.start) / flight.duration, 0), 1))
            camera = SynapseCamera.interpolate(flight.from, flight.to, SynapseCamera.easeInOut(u))
            if u >= 1 {
                self.flight = nil
                pause(now)
                landed = true
            }
        }
        if autoRotate && !reduceMotion && !touching && flight == nil && graph != nil && now >= pauseUntil {
            camera.yaw += Float(dt) * 0.12
        }
        return landed
    }

    func fly(to target: SynapseCamera, animate: Bool) {
        var destination = target.clamped()
        destination.yaw = camera.yaw
        if !animate || reduceMotion {
            flight = nil
            camera = destination
            return
        }
        let now = Self.now()
        flight = Flight(from: camera, to: destination, start: now, duration: 0.9)
        pause(now)
    }
}

@MainActor
@Observable
final class SynapseModel {
    private(set) var snapshot = InsightSnapshot()
    private(set) var snapshotKey = ""
    private(set) var orphans: [Int] = []
    private(set) var leaves: [Int] = []
    private(set) var mostImported: [Int] = []
    private(set) var mostImporting: [Int] = []
    private(set) var imports = 0
    private(set) var summary = ""
    private(set) var running = false
    private(set) var progress: Double = 0
    private(set) var settled = false
    private(set) var revision = 0
    private(set) var showMode = SynapseShowMode.everything
    private(set) var selected = -1
    private(set) var isolatedCycle = -1
    var colourBy = SynapseColourBy.module {
        didSet { if colourBy != oldValue { restyle() } }
    }
    var depth = 1 {
        didSet { if depth != oldValue { restyle() } }
    }
    var pulses = true {
        didSet { if pulses != oldValue { restyle() } }
    }
    var query = "" {
        didSet { if query != oldValue { restyle() } }
    }
    var autoRotate = true
    var sheetShown = false
    var flying = false
    var interacting = false

    @ObservationIgnored let viewport = SynapseViewport()
    @ObservationIgnored private var task: Task<Void, Never>?
    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var includeIsolated = false
    @ObservationIgnored private var fittedOnce = false

    var graph: SynapseGraph? { viewport.graph }

    var style: SynapseStyle { viewport.style }

    static func key(_ snapshot: InsightSnapshot) -> String {
        "\(snapshot.source)|\(snapshot.name)|\(snapshot.head)|\(snapshot.analyzedAt)|\(snapshot.files.count)"
    }

    var unusable: String? {
        if !snapshot.error.isEmpty { return snapshot.error }
        if snapshotKey.isEmpty { return nil }
        if snapshot.files.isEmpty {
            return "This repository has no source files Synapse can read. Pick another repository."
        }
        return nil
    }

    var emptyGraph: String? {
        _ = revision
        if let graph, graph.nodes.isEmpty, settled {
            return "No file imports another file of this repository yet."
        }
        return nil
    }

    var animating: Bool {
        if running || flying || interacting { return true }
        if viewport.reduceMotion { return false }
        return autoRotate || viewport.style.animating
    }

    func load(_ snapshot: InsightSnapshot) {
        let key = Self.key(snapshot)
        if key == snapshotKey && graph != nil { return }
        cancel()
        self.snapshot = snapshot
        snapshotKey = key
        orphans = Synapse.orphans(snapshot)
        leaves = Synapse.leaves(snapshot)
        mostImported = Synapse.mostImported(snapshot, 10)
        mostImporting = Synapse.mostImporting(snapshot, 10)
        imports = Synapse.importCount(snapshot)
        summary = snapshot.error.isEmpty && !snapshot.files.isEmpty
            ? Synapse.summary(snapshot, format: { $0.formatted() })
            : ""
        selected = -1
        isolatedCycle = -1
        fittedOnce = false
        viewport.graph = nil
        viewport.positions = []
        viewport.fileNames = snapshot.files.map { Synapse.fileName($0.path) }
        viewport.style = SynapseStyle()
        revision += 1
        if snapshot.error.isEmpty && !snapshot.files.isEmpty { startLayout() }
    }

    func cancel() {
        task?.cancel()
        task = nil
        generation += 1
        running = false
    }

    private func startLayout() {
        cancel()
        let key = snapshotKey + (includeIsolated ? "|orphans" : "|connected")
        if let cached = SynapseLayoutCache.get(key) {
            install(cached.graph, cached.positions)
            progress = 1
            settled = true
            fit(animate: fittedOnce)
            fittedOnce = true
            return
        }
        running = true
        progress = 0
        settled = false
        let token = generation
        let snapshot = self.snapshot
        let isolated = includeIsolated
        let owner = SynapseOwner(self)
        task = Task.detached(priority: .userInitiated) {
            let graph = Synapse.buildGraph(snapshot, includeIsolated: isolated)
            if Task.isCancelled { return }
            let seeds = Synapse.seedPositions(graph)
            await owner.model?.graphReady(graph, seeds, token)
            let gate = SynapsePublishGate()
            let positions = Synapse.runLayout(graph, publish: { positions, fraction in
                if fraction < 1 && !gate.claim() { return }
                Task { @MainActor in
                    gate.release()
                    owner.model?.layoutStep(positions, fraction, token)
                }
            }, stop: { Task.isCancelled })
            if Task.isCancelled { return }
            await owner.model?.layoutDone(SynapseLayoutResult(graph: graph, positions: positions), key, token)
        }
    }

    func resume() {
        if snapshotKey.isEmpty || settled || running || unusable != nil { return }
        startLayout()
    }

    private func install(_ graph: SynapseGraph, _ positions: [SynapseVector]) {
        let keep = selected
        viewport.graph = graph
        viewport.positions = positions
        viewport.lobeCentres = Synapse.lobeCentres(graph, positions)
        viewport.prepareDust(graph.shape)
        if !fittedOnce {
            var camera = SynapseCamera()
            camera.yaw = viewport.camera.yaw
            viewport.camera = SynapseCamera.fitted(camera, points: [], whole: true, shape: graph.shape, spacing: graph.spacing,
                                                   aspect: viewport.aspect, autoRotate: autoRotate)
        }
        selected = keep
        restyle()
    }

    private func graphReady(_ graph: SynapseGraph, _ seeds: [SynapseVector], _ token: Int) {
        guard token == generation else { return }
        install(graph, seeds)
    }

    private func layoutStep(_ positions: [SynapseVector], _ fraction: Double, _ token: Int) {
        guard token == generation, let graph, positions.count == graph.nodes.count, !settled else { return }
        viewport.positions = positions
        viewport.lobeCentres = Synapse.lobeCentres(graph, positions)
        progress = max(progress, fraction)
        revision += 1
    }

    private func layoutDone(_ result: SynapseLayoutResult, _ key: String, _ token: Int) {
        guard token == generation else { return }
        SynapseLayoutCache.put(key, result)
        viewport.positions = result.positions
        viewport.lobeCentres = Synapse.lobeCentres(result.graph, result.positions)
        progress = 1
        settled = true
        running = false
        task = nil
        if selected < 0 { fit(animate: true) }
        fittedOnce = true
        revision += 1
    }

    func restyle() {
        guard let graph else {
            revision += 1
            return
        }
        var input = SynapseStyleInput()
        input.selected = selected
        input.isolatedCycle = isolatedCycle
        input.showMode = showMode
        input.depth = depth
        input.query = query
        input.colourBy = colourBy
        input.pulses = pulses
        viewport.style = SynapseStyle.make(graph, snapshot, input)
        viewport.styleRevision += 1
        revision += 1
    }

    func setShowMode(_ mode: SynapseShowMode) {
        showMode = mode
        isolatedCycle = -1
        if mode.includesIsolated != includeIsolated {
            includeIsolated = mode.includesIsolated
            restyle()
            startLayout()
        } else {
            restyle()
            fit(animate: true)
        }
    }

    func select(_ file: Int, fly: Bool) {
        guard file >= 0 && file < snapshot.files.count else {
            clearSelection()
            return
        }
        if let graph, graph.nodeFor(file) < 0, !includeIsolated {
            selected = file
            restyle()
            return
        }
        if let graph, isolatedCycle >= 0, isolatedCycle < graph.cycles.count,
           snapshot.files[file].cycle != graph.cycles[isolatedCycle].index {
            isolatedCycle = -1
        }
        selected = file
        restyle()
        if fly { flyToFile(file) }
    }

    func clearSelection() {
        if selected < 0 && isolatedCycle < 0 { return }
        selected = -1
        isolatedCycle = -1
        restyle()
    }

    func isolateCycle(_ cycle: Int) {
        guard let graph, cycle >= 0, cycle < graph.cycles.count else { return }
        showMode = .cycles
        selected = -1
        if includeIsolated {
            includeIsolated = false
            isolatedCycle = cycle
            startLayout()
            return
        }
        isolatedCycle = cycle
        restyle()
        flyToCycle(cycle)
    }

    func graphCycle(for file: Int) -> Int {
        guard let graph, file >= 0, file < snapshot.files.count else { return -1 }
        let cycle = snapshot.files[file].cycle
        return graph.cycles.firstIndex { $0.index == cycle } ?? -1
    }

    func fit(animate: Bool) {
        guard let graph else { return }
        let camera = Synapse.fit(viewport.camera, graph: graph, positions: viewport.positions, style: viewport.style,
                                 settled: settled, aspect: viewport.aspect, autoRotate: autoRotate)
        fly(camera, animate: animate)
    }

    func flyToFile(_ file: Int) {
        guard let graph,
              let camera = Synapse.flightToFile(viewport.camera, graph: graph, positions: viewport.positions, file: file,
                                                aspect: viewport.aspect, autoRotate: autoRotate) else { return }
        fly(camera, animate: true)
    }

    func flyToCycle(_ cycle: Int) {
        guard let graph,
              let camera = Synapse.flightToCycle(viewport.camera, graph: graph, positions: viewport.positions, cycle: cycle,
                                                 aspect: viewport.aspect, autoRotate: autoRotate) else { return }
        fly(camera, animate: true)
    }

    private func fly(_ camera: SynapseCamera, animate: Bool) {
        viewport.fly(to: camera, animate: animate)
        flying = viewport.flight != nil
        revision += 1
    }

    func landed() {
        if flying { flying = false }
    }

    func pick(at point: CGPoint) -> Int {
        guard let graph else { return -1 }
        let node = viewport.frame.pick(x: Float(point.x), y: Float(point.y), nearRadius: 28, slop: 10)
        return node >= 0 && node < graph.nodes.count ? graph.nodes[node].file : -1
    }

    func tap(at point: CGPoint) {
        let file = pick(at: point)
        if file >= 0 {
            select(file, fly: false)
            sheetShown = true
        } else if selected >= 0 {
            clearSelection()
        }
    }

    func doubleTap(at point: CGPoint) {
        let file = pick(at: point)
        if file >= 0 {
            select(file, fly: true)
        } else {
            fit(animate: true)
        }
    }

    var suggestions: [Int] {
        Synapse.matches(snapshot, query, limit: 12)
    }

    func submitSearch() {
        let text = query.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.isEmpty { return }
        var found = snapshot.indexOf(text)
        if found < 0 {
            for (i, file) in snapshot.files.enumerated() where file.path.range(of: text, options: .caseInsensitive) != nil {
                if graph.map({ $0.nodeFor(i) >= 0 }) ?? true {
                    found = i
                    break
                }
            }
        }
        if found >= 0 { choose(found) }
    }

    func choose(_ file: Int) {
        query = ""
        select(file, fly: true)
        sheetShown = true
    }

    func toggleAutoRotate() {
        autoRotate.toggle()
        viewport.pauseUntil = 0
    }

    func moduleName(_ file: Int) -> String? {
        guard let graph else { return nil }
        let node = graph.nodeFor(file)
        guard node >= 0 else { return nil }
        return graph.lobes[graph.nodes[node].lobe].name
    }
}
