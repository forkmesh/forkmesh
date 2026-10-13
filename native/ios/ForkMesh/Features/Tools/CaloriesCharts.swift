// The Calorie Counter's ring, day chart and weight chart as Canvas views over FMTools'
// CaloriesRing, CaloriesDayChart and CaloriesWeightChart geometry.

import FMBase
import FMTools
import SwiftUI

struct CaloriesRingView: View {
    let ring: CaloriesRing
    let track: Color
    let accent: Color
    let overColor: Color

    var body: some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            let radius: CGFloat = min(size.width, size.height) / 2 - 8
            let center: CGPoint = CGPoint(x: size.width / 2, y: size.height / 2)
            let circle: Path = Path(ellipseIn: CGRect(x: center.x - radius, y: center.y - radius, width: radius * 2, height: radius * 2))
            let style: StrokeStyle = StrokeStyle(lineWidth: 10, lineCap: .round)
            context.stroke(circle, with: .color(track), style: style)
            if ring.ratio > 0 {
                var arc: Path = Path()
                arc.addArc(center: center, radius: radius, startAngle: Angle.degrees(-90),
                           endAngle: Angle.degrees(-90 + 360 * ring.ratio), clockwise: false)
                context.stroke(arc, with: .color(ring.over ? overColor : accent), style: style)
            }
        }
    }
}

struct CaloriesDayChartView: View {
    let days: [JSONObject]
    let goal: Int
    let accent: Color
    let over: Color
    let goalColor: Color
    let grid: Color
    let ink: Color

    var body: some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            let chart: CaloriesDayChart = CaloriesDayChart(days: days, goal: goal, width: Double(size.width), height: Double(size.height))
            let left: CGFloat = CGFloat(chart.plot.x)
            let right: CGFloat = CGFloat(chart.plot.x + chart.plot.width)
            for y in chart.gridLines {
                var line: Path = Path()
                line.move(to: CGPoint(x: left, y: CGFloat(y)))
                line.addLine(to: CGPoint(x: right, y: CGFloat(y)))
                context.stroke(line, with: .color(grid), lineWidth: 1)
            }
            for bar in chart.bars {
                let rect: CGRect = CGRect(x: bar.x, y: bar.y, width: bar.width, height: bar.height)
                context.fill(Path(roundedRect: rect, cornerRadius: 2), with: .color(bar.over ? over : accent))
            }
            if let goalY = chart.goalY {
                for dash in chart.dashes {
                    var line: Path = Path()
                    line.move(to: CGPoint(x: dash.0, y: goalY))
                    line.addLine(to: CGPoint(x: dash.1, y: goalY))
                    context.stroke(line, with: .color(goalColor), lineWidth: 2)
                }
            }
            for label in chart.labels {
                CaloriesChartText.draw(&context, label.text, x: label.x, y: label.y, width: label.width, alignRight: label.alignRight, ink: ink)
            }
        }
    }
}

struct CaloriesWeightChartView: View {
    let points: [JSONObject]
    let target: Double
    let imperial: Bool
    let line: Color
    let goalColor: Color
    let grid: Color
    let ink: Color
    let surface: Color

    var body: some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            let chart: CaloriesWeightChart = CaloriesWeightChart(
                points: points, target: target, imperial: imperial, width: Double(size.width), height: Double(size.height))
            if chart.points.isEmpty {
                return
            }
            let left: CGFloat = CGFloat(chart.plot.x)
            let right: CGFloat = CGFloat(chart.plot.x + chart.plot.width)
            for row in chart.grid {
                var rule: Path = Path()
                rule.move(to: CGPoint(x: left, y: CGFloat(row.y)))
                rule.addLine(to: CGPoint(x: right, y: CGFloat(row.y)))
                context.stroke(rule, with: .color(grid), lineWidth: 1)
                CaloriesChartText.draw(&context, row.label, x: 0, y: row.y - 6, width: CaloriesWeightChart.left - 4, alignRight: true, ink: ink)
            }
            if let targetY = chart.targetY {
                for dash in chart.dashes {
                    var mark: Path = Path()
                    mark.move(to: CGPoint(x: dash.0, y: targetY))
                    mark.addLine(to: CGPoint(x: dash.1, y: targetY))
                    context.stroke(mark, with: .color(goalColor), lineWidth: 2)
                }
            }
            var path: Path = Path()
            for (index, point) in chart.points.enumerated() {
                let at: CGPoint = CGPoint(x: point.x, y: point.y)
                if index == 0 {
                    path.move(to: at)
                } else {
                    path.addLine(to: at)
                }
            }
            context.stroke(path, with: .color(line), style: StrokeStyle(lineWidth: 2, lineJoin: .round))
            for point in chart.points {
                let dot: Path = Path(ellipseIn: CGRect(x: point.x - 4, y: point.y - 4, width: 8, height: 8))
                context.fill(dot, with: .color(line))
                context.stroke(dot, with: .color(surface), lineWidth: 1.5)
            }
        }
    }
}

enum CaloriesChartText {
    /// A 10pt label in a box of `width` at (x, y), left or right aligned.
    static func draw(_ context: inout GraphicsContext, _ text: String, x: Double, y: Double, width: Double, alignRight: Bool, ink: Color) {
        let resolved: GraphicsContext.ResolvedText = context.resolve(Text(text).font(Font.system(size: 10)).foregroundColor(ink))
        let measured: CGSize = resolved.measure(in: CGSize(width: width, height: 20))
        let left: CGFloat = alignRight ? CGFloat(x + width) - measured.width : CGFloat(x)
        context.draw(resolved, at: CGPoint(x: left, y: CGFloat(y)), anchor: .topLeading)
    }
}
