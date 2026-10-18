import FMTools
import SwiftUI

struct EyeControlScreen: View {
    @Environment(\.fmPalette) private var fm
    @Bindable private var tracker: EyeTracker = EyeTracker.shared
    @State private var practiced: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                statusCard
                if let problem = tracker.problem {
                    FmListRow(
                        title: problem,
                        leading: AnyView(FmIcon("error", color: fm.danger)),
                        onTap: settingsAction,
                        showChevron: tracker.cameraDenied)
                }
                calibrationSection
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    FmSectionHeader(title: "Click with", icon: "touch_app")
                    FmSegmentTabBar(
                        tabs: EyeBlinkLength.allCases.map { FmSegmentTab(label: $0.label) },
                        index: EyeBlinkLength.allCases.firstIndex(of: tracker.blinkLength) ?? 1,
                        onChanged: { (index: Int) in tracker.blinkLength = EyeBlinkLength.allCases[index] })
                    Text(tracker.blinkLength.detail)
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    FmSectionHeader(title: "Cursor", icon: "tune")
                    FmSegmentTabBar(
                        tabs: EyeSmoothing.allCases.map { FmSegmentTab(label: $0.label) },
                        index: EyeSmoothing.allCases.firstIndex(of: tracker.smoothing) ?? 1,
                        onChanged: { (index: Int) in tracker.smoothing = EyeSmoothing.allCases[index] })
                    Text("Steady holds still on small targets; Quick follows your eyes with less delay but shakes more.")
                        .fmText(.bodySmall, color: fm.textSecondary)
                    Toggle(isOn: $tracker.snapping) {
                        Text("Snap to buttons")
                            .fmText(.bodyMedium, color: fm.textPrimary)
                    }
                    .tint(fm.accent)
                    .accessibilityIdentifier("eye-control-snapping")
                    Text("The cursor locks onto the button you are looking at and outlines it, so a blink presses that button even when your gaze lands a little off.")
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
                practiceSection
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    FmSectionHeader(title: "How it works", icon: "visibility")
                    Text(
                        "The front TrueDepth camera and its infrared sensor follow your face and eyes. Where your gaze meets the screen, the cursor goes; close both eyes firmly and it taps whatever is under it. Eye Control works inside ForkMesh only, and camera frames stay on this phone."
                    )
                    .fmText(.bodySmall, color: fm.textSecondary)
                    Text("Hold the phone at reading distance with your face fully in view. Use the Stop eye control button, or blink on it, to turn it off.")
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
            }
            .padding(FmSpace.x4)
        }
        .background(fm.canvas)
        .navigationTitle("Eye Control")
        .navigationBarTitleDisplayMode(.inline)
        .fullScreenCover(isPresented: calibratingBinding) {
            EyeCalibrationView(tracker: tracker)
        }
    }

    private var calibratingBinding: Binding<Bool> {
        Binding<Bool>(
            get: { tracker.calibrating },
            set: { (shown: Bool) in
                if !shown { tracker.cancelCalibration() }
            })
    }

    private var statusCard: some View {
        FmGlassSurface(cornerRadius: FmRadius.lg) {
            HStack(spacing: FmSpace.x3) {
                FmIcon("visibility", size: 28, color: tracker.running ? (tracker.faceVisible ? fm.success : fm.warning) : fm.textTertiary)
                VStack(alignment: .leading, spacing: 2) {
                    Text(tracker.running ? "Eye control is on" : "Eye control is off")
                        .fmText(.titleMedium)
                    Text(statusDetail)
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
                Spacer(minLength: 0)
                if tracker.running {
                    FmOutlinedButton("Stop", icon: "stop", minHeight: 34, onPressed: { tracker.stop() })
                        .accessibilityLabel("Stop eye control")
                        .accessibilityIdentifier("eye-control-stop")
                } else {
                    FmFilledButton("Start", icon: "play_arrow", minHeight: 34, onPressed: { tracker.start() })
                        .disabled(!tracker.supported)
                        .accessibilityLabel("Start eye control")
                        .accessibilityIdentifier("eye-control-start")
                }
            }
            .padding(FmSpace.x3)
        }
    }

    private var statusDetail: String {
        guard tracker.supported else { return "Needs a face-tracking front camera" }
        guard tracker.running else { return "Move a cursor with your eyes, blink to tap" }
        guard tracker.faceVisible else { return "Looking for your face" }
        return tracker.clicks == 0 ? "Following your eyes" : "Following your eyes · \(tracker.clicks) taps"
    }

    private var calibrationSection: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            FmSectionHeader(title: "Calibration", icon: "adjust")
            Text(
                tracker.calibrated
                    ? "Calibrated for your eyes. Calibrate again if the cursor drifts or after you rotate the screen."
                    : "Not calibrated yet, so the cursor uses a rough estimate. Calibrating takes about 20 seconds: look at each dot until its ring fills and it moves."
            )
            .fmText(.bodySmall, color: fm.textSecondary)
            if let detail = learningDetail {
                Text(detail)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .accessibilityIdentifier("eye-control-accuracy")
            }
            HStack(spacing: FmSpace.x2) {
                FmOutlinedButton(tracker.calibrated ? "Calibrate again" : "Calibrate", icon: "adjust", minHeight: 34, onPressed: { tracker.calibrate() })
                    .disabled(!tracker.supported)
                    .accessibilityIdentifier("eye-control-calibrate")
                if tracker.calibrated || tracker.learned > 0 {
                    FmOutlinedButton("Reset", icon: "refresh", minHeight: 34, onPressed: { tracker.resetCalibration() })
                        .accessibilityIdentifier("eye-control-reset-calibration")
                }
            }
            Toggle(isOn: $tracker.learning) {
                Text("Keep learning")
                    .fmText(.bodyMedium, color: fm.textPrimary)
            }
            .tint(fm.accent)
            .accessibilityIdentifier("eye-control-learning")
            Text("Each blink-tap on a button, and each steady look at one, fine-tunes the calibration while you use it, so it keeps up when you shift the phone or how you sit.")
                .fmText(.bodySmall, color: fm.textSecondary)
        }
    }

    private var learningDetail: String? {
        let accuracy: String? = tracker.accuracy.map { "Accurate to within about \(Int($0.rounded())) points" }
        let learned: String? = tracker.learned > 0 ? "learned from \(tracker.learned) taps and looks" : nil
        switch (accuracy, learned) {
        case let (a?, l?): return "\(a), \(l)."
        case let (a?, nil): return "\(a)."
        case let (nil, l?): return "Adjusted live, \(l)."
        default: return nil
        }
    }

    private var practiceSection: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            FmSectionHeader(title: "Practice", icon: "check_circle")
            HStack(spacing: FmSpace.x2) {
                ForEach(["Left", "Middle", "Right"], id: \.self) { (name: String) in
                    FmOutlinedButton(name, icon: practiced == name ? "check_circle" : nil, minHeight: 56, fillWidth: true, onPressed: { practiced = name })
                        .accessibilityIdentifier("eye-control-practice-\(name.lowercased())")
                }
            }
            Text(practiced.map { "Tapped \($0)." } ?? "Look at a button and blink to tap it.")
                .fmText(.bodySmall, color: fm.textSecondary)
        }
    }

    private var settingsAction: (() -> Void)? {
        guard tracker.cameraDenied else { return nil }
        return { openSettings() }
    }

    private func openSettings() {
        guard let url = URL(string: UIApplication.openSettingsURLString) else { return }
        UIApplication.shared.open(url)
    }
}

struct EyeCalibrationView: View {
    @Environment(\.fmPalette) private var fm
    let tracker: EyeTracker

    var body: some View {
        ZStack {
            fm.canvas.ignoresSafeArea()
            VStack(spacing: FmSpace.x2) {
                Text(tracker.faceVisible ? "Look at the dot" : "Bring your face into view")
                    .fmText(.titleMedium)
                Text("Dot \(min(tracker.calibrationStep + 1, 9)) of 9")
                    .fmText(.bodySmall, color: fm.textSecondary)
                FmOutlinedButton("Cancel", icon: "close", minHeight: 34, onPressed: { tracker.cancelCalibration() })
                    .accessibilityIdentifier("eye-control-calibration-cancel")
            }
            GeometryReader { (proxy: GeometryProxy) in
                if let target = tracker.calibrationTarget {
                    let frame: CGRect = proxy.frame(in: .global)
                    ZStack {
                        Circle()
                            .fill(fm.accent)
                            .frame(width: 22 - 12 * tracker.calibrationDotProgress, height: 22 - 12 * tracker.calibrationDotProgress)
                            .overlay(Circle().stroke(fm.accent.opacity(0.35), lineWidth: 10))
                        Circle()
                            .trim(from: 0, to: tracker.calibrationDotProgress)
                            .stroke(fm.accent, style: StrokeStyle(lineWidth: 3, lineCap: .round))
                            .rotationEffect(.degrees(-90))
                            .frame(width: 44, height: 44)
                    }
                    .animation(.linear(duration: 0.1), value: tracker.calibrationDotProgress)
                    .position(x: target.x - frame.minX, y: target.y - frame.minY)
                }
            }
            .ignoresSafeArea()
            .allowsHitTesting(false)
            .accessibilityHidden(true)
        }
    }
}
