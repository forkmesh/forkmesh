import FMTools
import SwiftUI

struct GlassesAlertScreen: View {
    @Environment(\.fmPalette) private var fm
    @Bindable private var watcher: GlassesWatcher = GlassesWatcher.shared
    @State private var picked: NearbyDevice?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                statusCard
                if let reading = watcher.homing {
                    GlassesHomingCard(reading: reading, watcher: watcher)
                }
                if let problem = watcher.problem {
                    FmListRow(
                        title: problem,
                        leading: AnyView(FmIcon("bluetooth_disabled", color: fm.danger)),
                        onTap: { openSettings() },
                        showChevron: watcher.bluetoothDenied)
                }
                rangeSection
                Toggle(isOn: $watcher.keepAwake) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Keep the screen on while watching")
                            .fmText(.bodyMedium)
                        Text("iPhone only scans for glasses while ForkMesh is open, so keep it on screen.")
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                }
                .tint(fm.accent)
                .accessibilityIdentifier("glasses-alert-keep-awake")
                FmSectionHeader(title: "Nearby now", icon: "visibility", count: watcher.nearby.count)
                if watcher.nearby.isEmpty {
                    Text(watcher.watching ? "No Meta glasses in range." : "Start watching to scan for Meta glasses.")
                        .fmText(.bodyMedium, color: fm.textSecondary)
                } else {
                    ForEach(watcher.nearby) { (sighting: GlassesSighting) in
                        sightingRow(sighting)
                    }
                }
                if watcher.watching {
                    NearbyDevicesSection(devices: watcher.others, onPick: { picked = $0 })
                        .padding(.top, FmSpace.x2)
                }
            }
            .padding(FmSpace.x4)
        }
        .background(fm.bgBase)
        .navigationTitle("Glasses Alert")
        .navigationBarTitleDisplayMode(.inline)
        .sheet(item: $picked) { (device: NearbyDevice) in
            NearbyDeviceSheet(device: device, watcher: watcher)
        }
        .fullScreenCover(isPresented: ringingBinding) {
            if let sighting = watcher.ringing {
                GlassesAlarmView(sighting: sighting, onStop: { watcher.silence() })
            }
        }
        .onAppear {
            if watcher.resumesOnOpen && !watcher.watching {
                watcher.start()
            }
        }
    }

    private var ringingBinding: Binding<Bool> {
        Binding<Bool>(
            get: { watcher.ringing != nil },
            set: { (shown: Bool) in
                if !shown { watcher.silence() }
            })
    }

    private var statusCard: some View {
        FmGlassSurface(cornerRadius: FmRadius.lg) {
            HStack(spacing: FmSpace.x3) {
                HStack(spacing: FmSpace.x3) {
                    FmIcon(
                        watcher.watching ? "bluetooth_searching" : "bluetooth", size: 28,
                        color: watcher.watching ? fm.success : fm.textTertiary)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(watcher.watching ? "Watching for smart glasses" : "Not watching")
                            .fmText(.titleMedium)
                        Text(statusDetail)
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                    Spacer(minLength: 0)
                }
                VStack(alignment: .trailing, spacing: FmSpace.x1) {
                    if watcher.watching {
                        FmOutlinedButton("Stop", icon: "stop", minHeight: 34, onPressed: { watcher.stop() })
                            .accessibilityLabel("Stop watching")
                            .accessibilityIdentifier("glasses-alert-stop")
                    } else {
                        FmFilledButton("Start", icon: "play_arrow", minHeight: 34, onPressed: { watcher.start() })
                            .accessibilityLabel("Start watching")
                            .accessibilityIdentifier("glasses-alert-start")
                    }
                    FmOutlinedButton("Test", icon: "vibration", minHeight: 34, onPressed: { watcher.test() })
                        .accessibilityLabel("Test alarm")
                        .accessibilityIdentifier("glasses-alert-test")
                }
            }
            .padding(FmSpace.x3)
        }
    }

    private var statusDetail: String {
        if watcher.watching {
            return watcher.nearby.isEmpty ? "Scanning nearby" : "\(watcher.nearby.count) nearby right now"
        }
        return "Ready to scan"
    }

    private var rangeSection: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            FmSectionHeader(title: "Alert range", icon: "radar")
            FmSegmentTabBar(
                tabs: GlassesRange.allCases.map { FmSegmentTab(label: $0.label) },
                index: GlassesRange.allCases.firstIndex(of: watcher.range) ?? 1,
                onChanged: { (index: Int) in watcher.range = GlassesRange.allCases[index] })
        }
    }

    private func sightingRow(_ sighting: GlassesSighting) -> some View {
        let parts: [String] = [
            sighting.distance, "\(sighting.rssi) dBm", sighting.maker?.label,
            "seen " + sighting.seenAt.formatted(date: .omitted, time: .shortened),
        ].compactMap { $0 }
        return FmListRow(
            title: sighting.label,
            subtitle: parts.joined(separator: " · "),
            leading: AnyView(FmIcon("visibility", color: sighting.rssi >= -70 ? fm.danger : fm.warning)),
            showChevron: false)
    }

    private func openSettings() {
        guard let url = URL(string: UIApplication.openSettingsURLString) else { return }
        UIApplication.shared.open(url)
    }
}

struct GlassesAlarmView: View {
    let sighting: GlassesSighting
    let onStop: () -> Void

    var body: some View {
        ZStack {
            Color.red.opacity(0.92).ignoresSafeArea()
            VStack(spacing: 24) {
                Spacer()
                Image(systemName: "eyeglasses")
                    .font(.system(size: 72, weight: .bold))
                    .symbolEffect(.pulse)
                    .accessibilityHidden(true)
                Text("Smart glasses nearby")
                    .font(.largeTitle.bold())
                    .multilineTextAlignment(.center)
                Text(GlassesSignatures.alertText(sighting))
                    .font(.title3)
                    .multilineTextAlignment(.center)
                Spacer()
                Button(action: onStop) {
                    Text("Stop alarm")
                        .font(.title2.bold())
                        .frame(maxWidth: .infinity, minHeight: 64)
                }
                .buttonStyle(.borderedProminent)
                .tint(.white)
                .foregroundStyle(Color.red)
                .accessibilityIdentifier("glassesAlarmStop")
            }
            .foregroundStyle(.white)
            .padding(32)
        }
    }
}
