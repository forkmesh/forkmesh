import FMAssistant
import RealityKit
import UIKit

@MainActor
final class SpiderRoomBody {
    let root: Entity = Entity()
    private let frame: Entity = Entity()
    private let shadow: ModelEntity
    private var hips: [Entity] = []
    private var knees: [Entity] = []
    private var yaws: [Float] = []

    private static let upper: Float = 0.035
    private static let lower: Float = 0.065
    private static let spread: [Float] = [0.75, 1.25, 1.85, 2.4]
    private static let lift: Float = 0.5
    private static let bend: Float = 1.56

    static func color(_ red: CGFloat, _ green: CGFloat, _ blue: CGFloat, _ alpha: CGFloat = 1) -> UIColor {
        UIColor(red: red, green: green, blue: blue, alpha: alpha)
    }

    static func see(through color: UIColor, opacity: Float) -> UnlitMaterial {
        var material: UnlitMaterial = UnlitMaterial(color: color)
        material.blending = .transparent(opacity: .init(scale: opacity))
        return material
    }

    init() {
        let shell: SimpleMaterial = SimpleMaterial(color: Self.color(0.10, 0.10, 0.20), roughness: 0.35, isMetallic: false)
        let shellLight: SimpleMaterial = SimpleMaterial(color: Self.color(0.22, 0.22, 0.36), roughness: 0.4, isMetallic: false)
        let violet: SimpleMaterial = SimpleMaterial(color: Self.color(0.40, 0.24, 0.66), roughness: 0.5, isMetallic: false)
        let lilac: SimpleMaterial = SimpleMaterial(color: Self.color(0.56, 0.40, 0.82), roughness: 0.5, isMetallic: false)
        let silver: SimpleMaterial = SimpleMaterial(color: Self.color(0.80, 0.82, 0.88), roughness: 0.2, isMetallic: true)
        let glow: UnlitMaterial = UnlitMaterial(color: Self.color(0.25, 0.95, 0.86))
        let white: SimpleMaterial = SimpleMaterial(color: Self.color(0.97, 0.97, 1.0), roughness: 0.15, isMetallic: false)
        let pitch: UnlitMaterial = UnlitMaterial(color: Self.color(0.03, 0.03, 0.08))
        let plum: UnlitMaterial = UnlitMaterial(color: Self.color(0.21, 0.13, 0.38))
        shadow = ModelEntity(
            mesh: .generatePlane(width: 0.15, depth: 0.15, cornerRadius: 0.075),
            materials: [Self.see(through: .black, opacity: 0.32)])
        shadow.position = SIMD3<Float>(0, -RoomSpider.stand + 0.002, 0)
        root.addChild(shadow)
        root.addChild(frame)

        add(.generateSphere(radius: 0.032), shell, at: SIMD3<Float>(0, 0, 0))
        add(.generateSphere(radius: 0.026), shellLight, at: SIMD3<Float>(0, -0.004, -0.012), scale: SIMD3<Float>(1, 0.8, 1.1))
        add(.generateSphere(radius: 0.022), violet, at: SIMD3<Float>(0, 0.03, -0.002), scale: SIMD3<Float>(1.05, 0.72, 1.05))
        add(.generateSphere(radius: 0.0225), silver, at: SIMD3<Float>(0, 0.024, -0.002), scale: SIMD3<Float>(1.08, 0.22, 1.08))
        let plume: ModelEntity = add(.generateSphere(radius: 0.009), lilac, at: SIMD3<Float>(0, 0.052, -0.014), scale: SIMD3<Float>(0.55, 1.5, 0.55))
        plume.orientation = simd_quatf(angle: -0.5, axis: SIMD3<Float>(1, 0, 0))
        add(.generateSphere(radius: 0.0055), glow, at: SIMD3<Float>(0, 0.029, 0.021))

        for side in [Float(1), Float(-1)] {
            add(.generateSphere(radius: 0.0078), white, at: SIMD3<Float>(side * 0.0115, 0.006, 0.027))
            add(.generateSphere(radius: 0.0042), pitch, at: SIMD3<Float>(side * 0.0115, 0.006, 0.0335))
            add(.generateSphere(radius: 0.0032), pitch, at: SIMD3<Float>(side * 0.021, 0.014, 0.021))
            add(.generateSphere(radius: 0.0026), pitch, at: SIMD3<Float>(side * 0.006, 0.017, 0.027))
        }
        add(.generateBox(size: SIMD3<Float>(0.009, 0.0025, 0.003), cornerRadius: 0.0012), plum, at: SIMD3<Float>(0, -0.008, 0.031))

        for side in [Float(1), Float(-1)] {
            for angle in Self.spread {
                let hip: Entity = Entity()
                hip.position = SIMD3<Float>(side * 0.024 * sin(angle), -0.006, 0.024 * cos(angle))
                let thigh: ModelEntity = ModelEntity(
                    mesh: .generateBox(size: SIMD3<Float>(Self.upper, 0.0062, 0.0062), cornerRadius: 0.003), materials: [shell])
                thigh.position = SIMD3<Float>(Self.upper / 2, 0, 0)
                hip.addChild(thigh)
                let knee: Entity = Entity()
                knee.position = SIMD3<Float>(Self.upper, 0, 0)
                hip.addChild(knee)
                let shin: ModelEntity = ModelEntity(
                    mesh: .generateBox(size: SIMD3<Float>(Self.lower, 0.005, 0.005), cornerRadius: 0.0025), materials: [shell])
                shin.position = SIMD3<Float>(Self.lower / 2, 0, 0)
                knee.addChild(shin)
                let tip: ModelEntity = ModelEntity(mesh: .generateSphere(radius: 0.0032), materials: [glow])
                tip.position = SIMD3<Float>(Self.lower, 0, 0)
                knee.addChild(tip)
                frame.addChild(hip)
                hips.append(hip)
                knees.append(knee)
                yaws.append(side > 0 ? angle - .pi / 2 : -(angle + .pi / 2))
            }
        }
    }

    @discardableResult
    private func add(_ mesh: MeshResource, _ material: Material, at position: SIMD3<Float>, scale: SIMD3<Float> = .one) -> ModelEntity {
        let part: ModelEntity = ModelEntity(mesh: mesh, materials: [material])
        part.position = position
        part.scale = scale
        frame.addChild(part)
        return part
    }

    func place(at position: RoomPoint, up: RoomPoint, heading: RoomPoint) {
        let y: RoomPoint = RoomMath.normalized(up)
        let z: RoomPoint = RoomMath.normalized(RoomMath.flat(heading, on: y), or: RoomMath.perpendicular(to: y))
        let x: RoomPoint = RoomMath.cross(y, z)
        root.transform = Transform(scale: .one, rotation: simd_quatf(simd_float3x3(columns: (x, y, z))), translation: position)
    }

    func pose(_ spider: RoomSpider, time: Float) {
        let walking: Float = spider.walking
        var bob: Float = 0
        var turn: simd_quatf = simd_quatf(angle: 0, axis: SIMD3<Float>(0, 1, 0))
        shadow.isEnabled = spider.mode == .crawling
        for index in hips.indices {
            let group: Float = ((index % 4) + (index / 4)) % 2 == 0 ? 0 : .pi
            let phase: Float = spider.stride + group
            let side: Float = index < 4 ? 1 : -1
            let front: Bool = index % 4 == 0
            let rear: Bool = index % 4 == 3
            var yaw: Float = yaws[index] + side * 0.28 * walking * sin(phase)
            var lift: Float = Self.lift + 0.3 * walking * max(0, cos(phase))
            var bend: Float = Self.bend
            switch spider.mode {
            case .flying, .held:
                lift += 0.55 + 0.15 * sin(time * 22 + Float(index))
                bend -= 0.7
            case .silk:
                lift += 0.35
                bend += 0.35 + 0.1 * sin(time * 3 + Float(index))
            default:
                break
            }
            switch spider.act {
            case .nap?:
                lift -= 0.35
                bend += 0.5
            case .dance?:
                if front { lift += 0.9 + 0.3 * sin(time * 9) }
            case .knock?:
                if index == 0 { lift += 1.0 + 0.4 * sin(time * 18) }
            case .wave?, .hello?:
                if index == 0 {
                    lift += 1.2
                    yaw += 0.4 * sin(time * 10)
                }
            case .weave?:
                if rear { lift += 0.5 + 0.4 * sin(time * 14 + side) }
            case .cheer?, .climb?:
                lift += 0.4
            default:
                break
            }
            hips[index].orientation = simd_quatf(angle: yaw, axis: SIMD3<Float>(0, 1, 0)) * simd_quatf(angle: lift, axis: SIMD3<Float>(0, 0, 1))
            knees[index].orientation = simd_quatf(angle: -bend, axis: SIMD3<Float>(0, 0, 1))
        }
        switch spider.mode {
        case .flying:
            turn = simd_quatf(angle: spider.spin, axis: RoomMath.normalized(SIMD3<Float>(1, 0.2, 0.4)))
        case .held:
            turn = simd_quatf(angle: 0.2 * sin(time * 9), axis: SIMD3<Float>(0, 0, 1))
        default:
            switch spider.act {
            case .nap?:
                bob = -0.012 + 0.002 * sin(time * 2)
            case .dance?:
                bob = 0.012 * abs(sin(time * 9))
                turn = simd_quatf(angle: 0.15 * sin(time * 9), axis: SIMD3<Float>(0, 0, 1))
            case .cheer?, .hello?, .climb?:
                bob = 0.018 * abs(sin(time * 12))
            case .giggle?:
                turn = simd_quatf(angle: 0.18 * sin(time * 30), axis: SIMD3<Float>(0, 0, 1))
                bob = 0.004 * abs(sin(time * 30))
            case .dizzy?:
                turn = simd_quatf(angle: 0.22, axis: RoomMath.normalized(SIMD3<Float>(sin(time * 7), 0, cos(time * 7))))
            case .peek?, .sniff?:
                bob = 0.008
                turn = simd_quatf(angle: -0.25 + 0.05 * sin(time * 6), axis: SIMD3<Float>(1, 0, 0))
            case .knock?:
                turn = simd_quatf(angle: -0.15, axis: SIMD3<Float>(1, 0, 0))
            case .weave?:
                turn = simd_quatf(angle: 0.08 * sin(time * 14), axis: SIMD3<Float>(0, 1, 0))
            default:
                bob = 0.0025 * sin(spider.stride * 2) * walking
            }
        }
        frame.position = SIMD3<Float>(0, bob, 0)
        frame.orientation = turn
    }
}

@MainActor
enum SpiderRoomSilk {
    static let unit: MeshResource = .generateBox(size: 1)
    static let thread: UnlitMaterial = SpiderRoomBody.see(through: .white, opacity: 0.8)

    static func strand(from a: RoomPoint, to b: RoomPoint, width: Float = 0.0012) -> ModelEntity {
        let entity: ModelEntity = ModelEntity(mesh: unit, materials: [thread])
        stretch(entity, from: a, to: b, width: width)
        return entity
    }

    static func stretch(_ entity: Entity, from a: RoomPoint, to b: RoomPoint, width: Float = 0.0012) {
        let span: RoomPoint = b - a
        let size: Float = max(RoomMath.length(span), 0.001)
        let direction: RoomPoint = span / size
        entity.transform = Transform(
            scale: SIMD3<Float>(width, size, width),
            rotation: simd_quatf(from: SIMD3<Float>(0, 1, 0), to: direction),
            translation: (a + b) / 2)
    }

    static func web(_ web: RoomWeb) -> Entity {
        let holder: Entity = Entity()
        let y: RoomPoint = RoomMath.normalized(web.normal)
        let z: RoomPoint = RoomMath.normalized(RoomMath.flat(web.across, on: y), or: RoomMath.perpendicular(to: y))
        let x: RoomPoint = RoomMath.cross(y, z)
        holder.transform = Transform(scale: .one, rotation: simd_quatf(simd_float3x3(columns: (x, y, z))), translation: web.center)
        let spokes: Int = 8
        let radius: Float = web.size / 2
        func at(_ angle: Float, _ distance: Float) -> RoomPoint {
            RoomPoint(cos(angle) * distance, 0, sin(angle) * distance)
        }
        for index in 0..<spokes {
            let angle: Float = Float(index) / Float(spokes) * 2 * .pi
            holder.addChild(strand(from: .zero, to: at(angle, radius), width: 0.001))
        }
        for ring in [Float(0.3), 0.55, 0.78, 1.0] {
            for index in 0..<spokes {
                let a: Float = Float(index) / Float(spokes) * 2 * .pi
                let b: Float = Float(index + 1) / Float(spokes) * 2 * .pi
                holder.addChild(strand(from: at(a, radius * ring), to: at(b, radius * ring), width: 0.0009))
            }
        }
        return holder
    }
}
