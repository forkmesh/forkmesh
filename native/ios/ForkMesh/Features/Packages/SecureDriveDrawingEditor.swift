import FMPackages
import SwiftUI
import UIKit

struct SecureDriveDrawingEditor: View {
    let readOnly: Bool
    let onEdit: ([UInt8]) -> Void
    @State private var drawing: SecureDriveDrawing
    @State private var history: [SecureDriveDrawing] = []
    @State private var tool: SecureDriveShape = .pen
    @State private var color = "#1f2328"
    @State private var width: Double = 4
    @State private var current: SecureDriveStroke?
    @State private var start: SecureDrivePoint?
    @State private var dragging = false
    @State private var erased = false

    static let palette = ["#1f2328", "#cf222e", "#0969da", "#1a7f37", "#bf8700", "#8250df", "#ffffff"]

    init(drawing: SecureDriveDrawing, readOnly: Bool, onEdit: @escaping ([UInt8]) -> Void) {
        self.readOnly = readOnly
        self.onEdit = onEdit
        _drawing = State(initialValue: drawing)
    }

    var body: some View {
        VStack(spacing: 0) {
            if !readOnly {
                toolbar
                Divider()
            }
            GeometryReader { (proxy: GeometryProxy) in
                let page = pageRect(proxy.size)
                Canvas { (context: inout GraphicsContext, _: CGSize) in
                    context.fill(Path(page), with: .color(Self.color(drawing.background)))
                    context.stroke(Path(page), with: .color(Color(uiColor: .separator)), lineWidth: 1)
                    context.clip(to: Path(page))
                    let scale = page.width / CGFloat(max(drawing.width, 1))
                    for stroke in drawing.strokes { draw(stroke, in: &context, page: page, scale: scale) }
                    if let current { draw(current, in: &context, page: page, scale: scale) }
                }
                .contentShape(Rectangle())
                .gesture(DragGesture(minimumDistance: 0).onChanged { (value: DragGesture.Value) in
                    moved(toDrawing(value.location, page))
                }.onEnded { (_: DragGesture.Value) in
                    ended()
                }, including: readOnly ? .none : .all)
                .accessibilityLabel("Drawing with \(drawing.strokes.count) strokes")
            }
            .background(Color(uiColor: .secondarySystemBackground))
        }
    }

    private var toolbar: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 10) {
                Picker("Tool", selection: $tool) {
                    Image(systemName: "pencil.tip").tag(SecureDriveShape.pen).accessibilityLabel("Pen")
                    Image(systemName: "line.diagonal").tag(SecureDriveShape.line).accessibilityLabel("Line")
                    Image(systemName: "rectangle").tag(SecureDriveShape.box).accessibilityLabel("Box")
                    Image(systemName: "oval").tag(SecureDriveShape.oval).accessibilityLabel("Oval")
                    Image(systemName: "eraser").tag(SecureDriveShape.eraser).accessibilityLabel("Eraser")
                }
                .pickerStyle(.segmented)
                .frame(width: 240)
                ForEach(Self.palette, id: \.self) { (hex: String) in
                    Button {
                        color = hex
                        if tool == .eraser { tool = .pen }
                    } label: {
                        Circle()
                            .fill(Self.color(hex))
                            .frame(width: 24, height: 24)
                            .overlay(Circle().stroke(color == hex ? Color.accentColor : Color(uiColor: .separator), lineWidth: color == hex ? 3 : 1))
                    }
                    .accessibilityLabel("Colour \(hex)")
                }
                Stepper(value: $width, in: 1...40, step: 1) {
                    Text("\(Int(width)) pt").font(.caption.monospacedDigit())
                }
                .frame(width: 140)
                .accessibilityLabel("Line width")
                Button {
                    undo()
                } label: {
                    Image(systemName: "arrow.uturn.backward")
                }
                .disabled(history.isEmpty)
                .accessibilityLabel("Undo")
                Button(role: .destructive) {
                    clear()
                } label: {
                    Image(systemName: "trash")
                }
                .disabled(drawing.strokes.isEmpty)
                .accessibilityLabel("Clear")
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
        }
    }

    private func draw(_ stroke: SecureDriveStroke, in context: inout GraphicsContext, page: CGRect, scale: CGFloat) {
        guard let first = stroke.points.first else { return }
        let lineWidth = max(1, CGFloat(stroke.width) * scale)
        let screen = { (p: SecureDrivePoint) in CGPoint(x: page.minX + CGFloat(p.x) * scale, y: page.minY + CGFloat(p.y) * scale) }
        if stroke.points.count == 1 {
            let centre = screen(first)
            let dot = CGRect(x: centre.x - lineWidth / 2, y: centre.y - lineWidth / 2, width: lineWidth, height: lineWidth)
            context.fill(Path(ellipseIn: dot), with: .color(Self.color(stroke.color)))
            return
        }
        var path = Path()
        path.move(to: screen(first))
        for point in stroke.points.dropFirst() { path.addLine(to: screen(point)) }
        context.stroke(path, with: .color(Self.color(stroke.color)), style: StrokeStyle(lineWidth: lineWidth, lineCap: .round, lineJoin: .round))
    }

    private func pageRect(_ size: CGSize) -> CGRect {
        let area = CGRect(origin: .zero, size: size).insetBy(dx: 12, dy: 12)
        let scale = min(area.width / CGFloat(max(drawing.width, 1)), area.height / CGFloat(max(drawing.height, 1)))
        let fitted = CGSize(width: CGFloat(drawing.width) * scale, height: CGFloat(drawing.height) * scale)
        return CGRect(x: area.midX - fitted.width / 2, y: area.midY - fitted.height / 2, width: fitted.width, height: fitted.height)
    }

    private func toDrawing(_ point: CGPoint, _ page: CGRect) -> SecureDrivePoint {
        let scale = page.width / CGFloat(max(drawing.width, 1))
        guard scale > 0 else { return SecureDrivePoint(0, 0) }
        let x = min(max(Double((point.x - page.minX) / scale), 0), Double(drawing.width))
        let y = min(max(Double((point.y - page.minY) / scale), 0), Double(drawing.height))
        return SecureDrivePoint(x, y)
    }

    private func remember() {
        history.append(drawing)
        if history.count > 100 { history.removeFirst() }
    }

    private func moved(_ point: SecureDrivePoint) {
        if !dragging {
            dragging = true
            remember()
            if tool == .eraser {
                erased = false
                erase(point)
                return
            }
            start = point
            current = SecureDriveStroke(color: color, width: width, points: [point])
            return
        }
        if tool == .eraser {
            erase(point)
            return
        }
        guard var stroke = current else { return }
        if tool == .pen {
            if let last = stroke.points.last, hypot(last.x - point.x, last.y - point.y) >= 1 { stroke.points.append(point) }
        } else if let start {
            stroke.points = SecureDriveDrawing.shape(tool, from: start, to: point)
        }
        current = stroke
    }

    private func erase(_ point: SecureDrivePoint) {
        if drawing.erase(at: point, width: width) { erased = true }
    }

    private func ended() {
        dragging = false
        if tool == .eraser {
            if erased {
                onEdit(drawing.toSvg())
            } else if !history.isEmpty {
                history.removeLast()
            }
            erased = false
            return
        }
        if let stroke = current {
            drawing.strokes.append(stroke)
            onEdit(drawing.toSvg())
        }
        current = nil
        start = nil
    }

    private func undo() {
        guard let previous = history.popLast() else { return }
        drawing = previous
        onEdit(drawing.toSvg())
    }

    private func clear() {
        guard !drawing.strokes.isEmpty else { return }
        remember()
        drawing.strokes = []
        onEdit(drawing.toSvg())
    }

    static func color(_ hex: String) -> Color {
        guard SecureDriveDrawing.validColor(hex), let value = UInt32(hex.dropFirst(), radix: 16) else { return .black }
        return Color(red: Double((value >> 16) & 0xff) / 255, green: Double((value >> 8) & 0xff) / 255, blue: Double(value & 0xff) / 255)
    }
}
