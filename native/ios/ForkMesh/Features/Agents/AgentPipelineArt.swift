import SwiftUI

struct AgentPipelinePath: Shape {
    func path(in rect: CGRect) -> Path {
        let w: CGFloat = rect.width
        let h: CGFloat = rect.height
        let low: CGFloat = h * 0.62
        let high: CGFloat = h * 0.26
        var path: Path = Path()
        path.move(to: CGPoint(x: w * 0.06, y: low))
        path.addLine(to: CGPoint(x: w * 0.27, y: low))
        path.addCurve(
            to: CGPoint(x: w * 0.42, y: high),
            control1: CGPoint(x: w * 0.35, y: low), control2: CGPoint(x: w * 0.34, y: high))
        path.addLine(to: CGPoint(x: w * 0.67, y: high))
        path.addCurve(
            to: CGPoint(x: w * 0.82, y: low),
            control1: CGPoint(x: w * 0.75, y: high), control2: CGPoint(x: w * 0.74, y: low))
        path.addLine(to: CGPoint(x: w * 0.94, y: low))
        return path
    }

    static func mainLine(in rect: CGRect) -> Path {
        var path: Path = Path()
        path.move(to: CGPoint(x: rect.width * 0.06, y: rect.height * 0.62))
        path.addLine(to: CGPoint(x: rect.width * 0.94, y: rect.height * 0.62))
        return path
    }
}

struct AgentPipelineDot: Shape {
    var progress: Double
    var radius: CGFloat

    var animatableData: Double {
        get { progress }
        set { progress = newValue }
    }

    func path(in rect: CGRect) -> Path {
        let full: Path = AgentPipelinePath().path(in: rect)
        let point: CGPoint = full.trimmedPath(from: 0, to: max(0.0001, min(1, progress))).currentPoint ?? .zero
        return Path(ellipseIn: CGRect(x: point.x - radius, y: point.y - radius, width: radius * 2, height: radius * 2))
    }
}

struct AgentPipelineArt: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var progress: Double = 0

    struct Stop {
        let label: String
        let x: CGFloat
        let y: CGFloat
        let at: Double
        let above: Bool
    }

    static let stops: [Stop] = [
        Stop(label: "Prompt", x: 0.06, y: 0.62, at: 0, above: false),
        Stop(label: "Mirror", x: 0.27, y: 0.62, at: 0.17, above: false),
        Stop(label: "Branch", x: 0.45, y: 0.26, at: 0.42, above: true),
        Stop(label: "Pull request", x: 0.67, y: 0.26, at: 0.64, above: true),
        Stop(label: "Merged", x: 0.82, y: 0.62, at: 0.86, above: false),
    ]

    static let size: CGSize = CGSize(width: 320, height: 124)
    static let duration: Double = 1.6
    static let delay: Double = 0.15

    var body: some View {
        let rect: CGRect = CGRect(origin: .zero, size: AgentPipelineArt.size)
        ZStack(alignment: .topLeading) {
            AgentPipelinePath.mainLine(in: rect)
                .stroke(fm.border, style: StrokeStyle(lineWidth: 3, lineCap: .round))
            AgentPipelinePath()
                .stroke(fm.border, style: StrokeStyle(lineWidth: 3, lineCap: .round))
            AgentPipelinePath()
                .trim(from: 0, to: progress)
                .stroke(fm.accent, style: StrokeStyle(lineWidth: 3, lineCap: .round))
            ForEach(AgentPipelineArt.stops, id: \.label) { (stop: Stop) in
                node(stop, lit: progress >= stop.at)
            }
            AgentPipelineDot(progress: progress, radius: 13)
                .fill(fm.accent.opacity(0.16))
            AgentPipelineDot(progress: progress, radius: 6)
                .fill(fm.accent)
        }
        .frame(width: rect.width, height: rect.height)
        .contentShape(Rectangle())
        .onTapGesture { play() }
        .onAppear { play() }
        .accessibilityElement()
        .accessibilityLabel(Text("A prompt goes to a mirror, onto a branch, into a pull request, and is merged"))
    }

    private func node(_ stop: Stop, lit: Bool) -> some View {
        let w: CGFloat = AgentPipelineArt.size.width
        let h: CGFloat = AgentPipelineArt.size.height
        let point: CGPoint = CGPoint(x: w * stop.x, y: h * stop.y)
        return ZStack {
            Circle()
                .fill(lit ? fm.accent : fm.bgRaised)
                .frame(width: 13, height: 13)
                .overlay(Circle().strokeBorder(lit ? fm.accent : fm.border, lineWidth: 2.5))
                .position(point)
            Text(stop.label)
                .fmText(.bodySmall, color: fm.textSecondary)
                .fixedSize()
                .position(x: point.x, y: stop.above ? point.y - 20 : point.y + 22)
        }
        .frame(width: w, height: h)
        .animation(reduceMotion || !lit ? nil : .snappy.delay(AgentPipelineArt.delay + AgentPipelineArt.duration * stop.at * 0.8), value: lit)
    }

    private func play() {
        guard !reduceMotion else {
            progress = 1
            return
        }
        var reset: Transaction = Transaction()
        reset.disablesAnimations = true
        withTransaction(reset) { progress = 0 }
        withAnimation(.smooth(duration: AgentPipelineArt.duration).delay(AgentPipelineArt.delay)) { progress = 1 }
    }
}
