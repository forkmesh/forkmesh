// The shell's header. The page navigation lives in the bottom bar
// (ShellBottomBar.swift), so this names the project (its icon opens the
// project switcher) and holds the account overflow: status, the
// tools and Settings. Also the badge glyph, the guest's sign-in chip and the
// notifications button.

import FMAccount
import FMApi
import FMBase
import FMDesktopRemote
import FMInsights
import SwiftUI
import TipKit
import UIKit

extension ShellRelayState {
    /// The Kit's copy of the relay state.
    var fmState: FmConnectionState {
        switch self {
        case .offline: return .offline
        case .connecting: return .connecting
        case .connected: return .connected
        }
    }
}

/// Everything the bar draws, resolved by HomeShell.
struct ShellTopBarState {
    let relayState: ShellRelayState
    let onlineCount: Int
    let name: String
    let avatarPng: String
    let notificationUnread: Int
    let guest: Bool
    /// The member's status line ("🌴 Vacationing · until Sep 25"), or "".
    let statusDescription: String
    let statusEmoji: String
    let statusAway: Bool
    /// Whether "Set a status" belongs in the account menu.
    let canSetStatus: Bool
    let diagnosticsTroubled: Bool
}

struct ShellTopBar: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @State private var motion: GenieMotion = GenieMotion.shared

    let state: ShellTopBarState
    let onOpenHub: () -> Void
    let onOpenSearch: () -> Void
    let onSignIn: () -> Void
    var onHome: () -> Void = {}
    var onNewAgent: (() -> Void)? = nil
    var onPalette: () -> Void = {}
    var onVoice: ((CGRect) -> Void)? = nil
    var showsSearch: Bool = true
    var showsSignIn: Bool = true

    let hubTip = HubTip()

    private var statusLabel: String {
        return shellStatusLabel(state.relayState, onlineCount: state.onlineCount)
    }

    @ViewBuilder private var trailing: some View {
        HStack(spacing: 0) {
            if showsSearch {
                FmIconButton(label: "Search", onPressed: onOpenSearch) {
                    Image(systemName: "magnifyingglass")
                        .font(.system(size: 19, weight: .regular))
                        .foregroundStyle(fm.textPrimary)
                }
                .accessibilityIdentifier("top-search-button")
            }
            if let onNewAgent = onNewAgent {
                ShellNewAgentButton(onNewAgent: onNewAgent, onPalette: onPalette, onVoice: onVoice)
                    .padding(.leading, FmSpace.x1)
            }
            if state.guest {
                Button(action: onOpenHub) {
                    Image(systemName: "person.crop.circle")
                        .font(.system(size: 22, weight: .regular))
                        .foregroundStyle(fm.textPrimary)
                        .frame(width: 44, height: 44)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text("You: projects, tools and settings"))
                .accessibilityIdentifier("top-settings-button")
                if showsSignIn {
                    ShellGuestSignInChip(onPressed: onSignIn)
                }
            } else {
                Button(action: onOpenHub) {
                    AvatarWithDot(
                        state: state.relayState.fmState, label: state.name, avatarPng: state.avatarPng,
                        statusEmoji: state.statusEmoji, away: state.statusAway,
                        diagnosticsTroubled: state.diagnosticsTroubled, size: 36, rounded: true)
                        .rotation3DEffect(.degrees(reduceMotion ? 0 : motion.lean.height * 8),
                                          axis: (x: 1, y: 0, z: 0))
                        .rotation3DEffect(.degrees(reduceMotion ? 0 : -motion.lean.width * 8),
                                          axis: (x: 0, y: 1, z: 0))
                        .frame(minWidth: 44, minHeight: 44)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .popoverTip(hubTip)
                .accessibilityLabel(Text(shellAvatarLabel(statusLabel: statusLabel, described: state.statusDescription)))
                .accessibilityHint(Text("Opens your hub: projects, tools and settings"))
                .accessibilityIdentifier("home-status-avatar")
                .padding(.leading, FmSpace.x2)
            }
        }
    }

    /// The page navigation moved to the bottom bar (ShellBottomBar); the
    /// header now names the project and holds the account overflow.
    var body: some View {
        ZStack {
            if !ShellIslandStatusDots.earsShown(compact: verticalSizeClass == .compact) {
                TelemetryStrip()
                    .padding(.horizontal, 64)
            }
            HStack(spacing: FmSpace.x3) {
                ShellProjectSlot(onHome: onHome)
                    .genieKeepsClear()
                HStack(spacing: FmSpace.x2) {
                    DjNowPlayingChip()
                    GlassesRadarChip()
                    StepsWalkChip()
                    ShellSuggestionChips()
                }
                .genieKeepsClear()
                Spacer(minLength: FmSpace.x2)
                trailing
                    .genieKeepsClear()
                    .layoutPriority(1)
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x1)
        .accessibilityElement(children: .contain)
        .onAppear { if !reduceMotion { motion.begin() } }
        .onDisappear { motion.end() }
        .onChange(of: reduceMotion) { _, reduced in
            if reduced { motion.end() } else { motion.begin() }
        }
        .accessibilityIdentifier("home-top-bar")
    }
}

struct ShellProjectSlot: View {
    static let holdSeconds: Double = 0.5

    @Environment(\.fmPalette) var fm
    @State private var settings: SettingsService? = Services.get(SettingsService.self)
    @State private var switching: Bool = false
    @State private var held: Bool = false
    var onHome: () -> Void = {}

    var body: some View {
        if let settings = settings {
            let project: RelayProfile = settings.project
            Button(action: openSwitcher) {
                ProjectIcon(project: project, size: 40)
                    .frame(minWidth: 44, minHeight: 44)
                    .contentShape(Rectangle())
            }
            .buttonStyle(ShellBarButtonStyle())
            .simultaneousGesture(LongPressGesture(minimumDuration: ShellProjectSlot.holdSeconds).onEnded { _ in
                held = true
                goHome()
            })
            .accessibilityLabel(Text("Project \(project.label), \(project.displayOrigin)"))
            .accessibilityHint(Text("Switch project or add a new one. Touch and hold to go Home"))
            .accessibilityAction(named: Text("Go Home")) { goHome() }
            .accessibilityIdentifier("home-project-slot")
            .projectDrawer(isPresented: $switching)
        } else {
            Image("Logo")
                .resizable()
                .scaledToFit()
                .frame(width: 40, height: 40)
                .frame(minWidth: 44, minHeight: 44)
                .contentShape(Rectangle())
                .onLongPressGesture(minimumDuration: ShellProjectSlot.holdSeconds) { goHome() }
                .accessibilityLabel(Text("ForkMesh"))
                .accessibilityHint(Text("Touch and hold to go Home"))
                .accessibilityAction(named: Text("Go Home")) { goHome() }
                .accessibilityIdentifier("home-project-slot")
        }
    }

    private func openSwitcher() {
        if held {
            held = false
            return
        }
        FmHaptics.tap()
        switching = true
    }

    private func goHome() {
        CaptureHold.noteHandled()
        FmHaptics.selection()
        onHome()
    }
}

struct ShellGlass<GlassShape: Shape>: ViewModifier {
    @Environment(\.fmPalette) var fm
    let shape: GlassShape
    var interactive: Bool = true

    func body(content: Content) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            content.contentShape(shape).glassEffect(.regular.interactive(interactive), in: shape)
        } else {
            material(content)
        }
        #else
        material(content)
        #endif
    }

    private func material<Inner: View>(_ inner: Inner) -> some View {
        inner
            .background(shape.fill(.ultraThinMaterial))
            .overlay(shape.stroke(fm.border.opacity(0.6), lineWidth: 0.5))
    }
}

extension View {
    func shellGlass<GlassShape: Shape>(_ shape: GlassShape, interactive: Bool = true) -> some View {
        modifier(ShellGlass(shape: shape, interactive: interactive))
    }
}

struct ShellVersionFooter: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.openURL) var openURL
    let version: String
    let update: AvailableUpdate?
    let desktop: ShellLocalDesktop
    let fleet: [FleetLight]
    let systems: [StatusLight]
    let desktopLoad: DesktopStrain?
    let buildMac: BuildMacLight?
    let runnerAgents: [String: Int]
    let onOpenDesktop: () -> Void
    let onOpenStatus: () -> Void

    var body: some View {
        ViewThatFits(in: .horizontal) {
            row(compact: false)
            row(compact: true)
            HStack(spacing: FmSpace.x1) {
                machineItems
                stackedLights
            }
            .shellFooterGlass()
            VStack(spacing: 0) {
                HStack(spacing: FmSpace.x2) {
                    versionItems(compact: false)
                }
                statusDots(compact: false)
            }
            .shellFooterGlass()
        }
        .frame(maxWidth: .infinity)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("home-version-footer")
    }

    private func row(compact: Bool) -> some View {
        HStack(spacing: compact ? FmSpace.x1 : FmSpace.x2) {
            versionItems(compact: compact)
            statusDots(compact: compact)
        }
        .shellFooterGlass()
    }

    @ViewBuilder private func statusDots(compact: Bool) -> some View {
        if !systems.isEmpty {
            ShellStatusDots(lights: systems, compact: compact, onPressed: onOpenStatus)
        }
    }

    private var stackedLights: some View {
        VStack(alignment: .leading, spacing: 2) {
            if !fleet.isEmpty {
                ShellFleetLights(lights: fleet, runnerAgents: runnerAgents, compact: true, onPressed: onOpenDesktop, minHeight: 14)
            }
            if !systems.isEmpty {
                ShellStatusDots(lights: systems, compact: true, onPressed: onOpenStatus, minHeight: 10, inset: 0)
            }
        }
        .frame(minHeight: 28)
    }

    private func versionItems(compact: Bool) -> some View {
        Group {
            machineItems
            if !fleet.isEmpty {
                ShellFleetLights(lights: fleet, runnerAgents: runnerAgents, compact: compact, onPressed: onOpenDesktop)
            }
        }
    }

    private var machineItems: some View {
        Group {
            if let update = update, let store = update.storeURL ?? ClientVersionGate.defaultStoreURL {
                Button {
                    FmHaptics.tap()
                    openURL(store)
                } label: {
                    SwiftUI.Label("Update \(update.label)", systemImage: "arrow.down.circle.fill")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(Color.white)
                        .lineLimit(1)
                        .padding(.horizontal, FmSpace.x2)
                        .padding(.vertical, 3)
                        .background(Capsule().fill(fm.accent))
                        .frame(minHeight: 28)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text("Install ForkMesh \(update.label)"))
                .accessibilityHint(Text("Opens ForkMesh in the App Store"))
                .accessibilityIdentifier("home-app-update")
            }
            Text(verbatim: "v\(version)")
                .fmMono(size: FmType.micro, color: fm.textSecondary, height: FmType.leadingTight)
                .lineLimit(1)
                .fixedSize()
                .accessibilityLabel(Text(verbatim: "App version \(version)"))
                .accessibilityIdentifier("home-app-version")
            ShellFooterDesktopLight(desktop: desktop, load: desktopLoad, onPressed: onOpenDesktop)
            if let buildMac = buildMac {
                ShellFooterMacLight(light: buildMac, onPressed: onOpenDesktop)
            }
        }
    }
}

struct ShellFooterGlass: ViewModifier {
    @Environment(\.fmPalette) var fm

    static let radius: CGFloat = 14

    private var shape: RoundedRectangle {
        RoundedRectangle(cornerRadius: ShellFooterGlass.radius, style: .continuous)
    }

    func body(content: Content) -> some View {
        glass(content.padding(.horizontal, FmSpace.x2))
    }

    @ViewBuilder private func glass<Inner: View>(_ inner: Inner) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            inner.glassEffect(.regular, in: shape)
        } else {
            material(inner)
        }
        #else
        material(inner)
        #endif
    }

    private func material<Inner: View>(_ inner: Inner) -> some View {
        inner
            .background(shape.fill(.ultraThinMaterial))
            .overlay(shape.strokeBorder(fm.border.opacity(0.6), lineWidth: 0.5))
    }
}

extension View {
    func shellFooterGlass() -> some View {
        modifier(ShellFooterGlass())
    }
}

struct ShellStatusDots: View {
    let lights: [StatusLight]
    var compact: Bool = false
    let onPressed: () -> Void
    var minHeight: CGFloat = 20
    var inset: CGFloat? = nil

    var body: some View {
        Button {
            FmHaptics.tap()
            onPressed()
        } label: {
            HStack(spacing: compact ? -3 : -2) {
                ForEach(lights) { (light: StatusLight) in
                    ShellStatusDot(light: light, ringed: true)
                }
            }
            .frame(minHeight: minHeight)
            .padding(.horizontal, inset ?? (compact ? FmSpace.x1 : FmSpace.x2))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(lights.map(\.label).joined(separator: "; ")))
        .accessibilityHint(Text("Opens the Status page"))
        .accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("footer-status-dots")
    }
}

struct ShellIslandStatusDots: View {
    static let islandInset: CGFloat = 54
    static let islandBottom: CGFloat = 48
    static let islandWidth: CGFloat = 126
    static let clockClear: CGFloat = 12
    static let earHeight: CGFloat = 12
    static let earMargin: CGFloat = 10
    static let earLeadingInset: CGFloat = 24
    static let dotSize: CGFloat = 5

    let lights: [StatusLight]
    var fleet: [FleetLight] = []
    var runnerAgents: [String: Int] = [:]
    var desktop: ShellLocalDesktop? = nil
    var desktops: [ShellDesktopLight] = []
    var desktopLoad: DesktopStrain? = nil
    var buildMac: BuildMacLight? = nil

    static let stackOverlap: CGFloat = -4

    private var otherDesktops: [ShellDesktopLight] {
        desktops.count > 1 ? desktops.filter { !$0.active } : []
    }

    private var cores: [FleetLight] { fleet.filter { $0.kind == .core } }

    private var runners: [FleetLight] { fleet.filter { $0.kind == .runner } }

    static func gap(top: CGFloat) -> CGFloat? {
        top >= islandInset ? top - islandBottom : nil
    }

    static func earTop(gap: CGFloat) -> CGFloat {
        max(1, gap - clockClear)
    }

    static func earWidth(screen: CGFloat) -> CGFloat {
        max(0, (screen - islandWidth) / 2)
    }

    static func earsShown(compact: Bool) -> Bool {
        !compact && gap(top: GenieKeepClear.keyWindow()?.safeAreaInsets.top ?? 0) != nil
    }

    static func screenTop(origin: CGFloat, inset: CGFloat) -> CGFloat {
        max(GenieKeepClear.keyWindow()?.safeAreaInsets.top ?? 0, origin + inset)
    }

    private var earShown: Bool {
        desktop != nil || buildMac != nil || !fleet.isEmpty
    }

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            let frame: CGRect = proxy.frame(in: .global)
            let top: CGFloat = ShellIslandStatusDots.screenTop(origin: frame.minY, inset: proxy.safeAreaInsets.top)
            let earWidth: CGFloat = ShellIslandStatusDots.earWidth(screen: frame.width)
            if let gap = ShellIslandStatusDots.gap(top: top) {
                ZStack(alignment: .topLeading) {
                    if !lights.isEmpty {
                        dots
                            .frame(width: frame.width, height: gap)
                    }
                    if earShown {
                        ear
                            .frame(width: max(0, earWidth - ShellIslandStatusDots.earLeadingInset))
                            .padding(.leading, ShellIslandStatusDots.earLeadingInset)
                            .padding(.top, ShellIslandStatusDots.earTop(gap: gap))
                    }
                    TelemetryEarSparks(
                        width: max(0, earWidth - ShellIslandStatusDots.earMargin * 2),
                        height: ShellIslandStatusDots.earHeight)
                        .frame(width: earWidth)
                        .padding(.top, ShellIslandStatusDots.earTop(gap: gap))
                        .frame(width: frame.width, alignment: .topTrailing)
                }
                .frame(width: frame.width, alignment: .topLeading)
                .offset(y: -frame.minY)
            }
        }
        .ignoresSafeArea(edges: .top)
        .allowsHitTesting(false)
    }

    private var dots: some View {
        HStack(spacing: 2) {
            ForEach(lights) { (light: StatusLight) in
                ShellStatusDot(light: light, size: ShellIslandStatusDots.dotSize)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(lights.map(\.label).joined(separator: "; ")))
        .accessibilityIdentifier("top-island-status-dots")
    }

    private var ear: some View {
        HStack(spacing: 8) {
            if let desktop = desktop {
                HStack(spacing: ShellIslandStatusDots.stackOverlap) {
                    ShellFooterDesktopLight(
                        desktop: desktop, load: desktopLoad, onPressed: {}, glyph: 10, side: ShellIslandStatusDots.earHeight)
                    ForEach(otherDesktops) { (light: ShellDesktopLight) in
                        ShellStackedDesktopLight(light: light)
                    }
                }
            }
            if let buildMac = buildMac {
                ShellFooterMacLight(light: buildMac, onPressed: {}, glyph: 10, side: ShellIslandStatusDots.earHeight)
            }
            if !cores.isEmpty {
                HStack(spacing: ShellIslandStatusDots.stackOverlap) {
                    ForEach(cores) { (light: FleetLight) in
                        ShellFleetLight(light: light, agents: 0)
                    }
                }
            }
            if !runners.isEmpty {
                HStack(spacing: 1) {
                    ForEach(runners) { (light: FleetLight) in
                        ShellFleetLight(light: light, agents: runnerAgents[light.name.lowercased()] ?? 0)
                    }
                }
            }
        }
        .frame(height: ShellIslandStatusDots.earHeight)
        .fixedSize()
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(earLabel))
        .accessibilityIdentifier("top-clock-lights")
    }

    private var earLabel: String {
        var parts: [String] = []
        if let desktop = desktop {
            parts.append(desktop.label)
            parts += otherDesktops.map(\.label)
        }
        if let buildMac = buildMac {
            parts.append(buildMac.label)
        }
        return (parts + cores.map(\.label) + runners.map(\.label)).joined(separator: "; ")
    }
}

struct ShellStatusDot: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion
    @State private var dim: Bool = false
    let light: StatusLight
    var ringed: Bool = false
    var size: CGFloat = 7

    private var color: Color {
        switch light.state {
        case .operational: return fm.success
        case .degraded: return fm.warning
        case .down: return fm.danger
        case .unknown, .future: return fm.textTertiary
        }
    }

    var body: some View {
        Circle()
            .fill(color)
            .frame(width: size, height: size)
            .opacity(light.troubled && dim ? 0.25 : 1)
            .background {
                if ringed {
                    Circle().fill(fm.bgBase).padding(-1)
                }
            }
            .onAppear { blink() }
            .onChange(of: light.state) { (_: ServiceState, _: ServiceState) in blink() }
    }

    private func blink() {
        guard light.troubled, !reduceMotion else {
            dim = false
            return
        }
        withAnimation(.easeInOut(duration: 0.6).repeatForever(autoreverses: true)) {
            dim = true
        }
    }
}

struct ShellFooterDesktopLight: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion
    @State private var dim: Bool = false
    let desktop: ShellLocalDesktop
    let load: DesktopStrain?
    let onPressed: () -> Void
    var glyph: CGFloat = 12
    var side: CGFloat = 28

    private var relayed: Bool {
        if case .relay = desktop { return true }
        return false
    }

    private var alarmed: Bool {
        !desktop.isConnected || load == .strained
    }

    private var lightColor: Color {
        if alarmed { return fm.danger }
        return load == .busy ? fm.warning : fm.success
    }

    private var usage: NodeResources? {
        guard desktop.isConnected, let vitals = Services.get(DesktopVitals.self), vitals.watching else { return nil }
        return NodeResources(cpu: vitals.latest(.cpu), memory: vitals.latest(.memory), disk: vitals.latest(.disk))
    }

    private var label: String {
        var parts: [String] = [desktop.label]
        if desktop.isConnected, let load = load {
            parts.append(load.label)
        }
        if let usage = usage {
            parts.append(usage.label)
        }
        return parts.joined(separator: ", ")
    }

    var body: some View {
        Button {
            FmHaptics.tap()
            onPressed()
        } label: {
            Image(systemName: "desktopcomputer")
                .font(.system(size: glyph, weight: .semibold))
                .foregroundStyle(fm.textSecondary)
                .overlay(alignment: .topTrailing) {
                    Circle()
                        .fill(lightColor)
                        .frame(width: 6, height: 6)
                        .background(Circle().fill(fm.bgBase).padding(-1))
                        .opacity(alarmed && dim ? 0.25 : 1)
                        .offset(x: 3, y: -2)
                }
                .overlay(alignment: .bottomLeading) {
                    if relayed && usage != nil {
                        relayGlobe.offset(x: -4, y: 3)
                    }
                }
                .overlay(alignment: .bottomTrailing) {
                    if let usage = usage {
                        ShellResourceBars(values: usage.values)
                            .offset(x: 3, y: 2)
                    } else if relayed {
                        relayGlobe.offset(x: 4, y: 3)
                    }
                }
                .frame(minWidth: side, minHeight: side)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onAppear { blink() }
        .onChange(of: alarmed) { (_: Bool, _: Bool) in blink() }
        .accessibilityLabel(Text(label))
        .accessibilityHint(Text("Opens your desktops, runners and core nodes"))
        .accessibilityIdentifier("footer-desktop-light")
    }

    private var relayGlobe: some View {
        Image(systemName: "globe")
            .font(.system(size: 7, weight: .bold))
            .foregroundStyle(fm.textSecondary)
            .background(Circle().fill(fm.bgBase).padding(-1))
    }

    private func blink() {
        guard alarmed, !reduceMotion else {
            withAnimation(.default) { dim = false }
            return
        }
        withAnimation(.easeInOut(duration: 0.6).repeatForever(autoreverses: true)) {
            dim = true
        }
    }
}

struct ShellStackedDesktopLight: View {
    @Environment(\.fmPalette) var fm
    let light: ShellDesktopLight

    private var dot: Color {
        switch light.presence {
        case .online: return fm.success
        case .offline: return fm.danger
        case .checking: return fm.warning
        case .unknown: return fm.textTertiary
        }
    }

    var body: some View {
        Image(systemName: "desktopcomputer")
            .font(.system(size: 10, weight: .semibold))
            .foregroundStyle(fm.textSecondary)
            .padding(1)
            .background(RoundedRectangle(cornerRadius: 3, style: .continuous).fill(fm.bgBase))
            .overlay(alignment: .topTrailing) {
                Circle()
                    .fill(dot)
                    .frame(width: 5, height: 5)
                    .background(Circle().fill(fm.bgBase).padding(-1))
                    .offset(x: 2, y: -2)
            }
            .frame(minHeight: ShellIslandStatusDots.earHeight)
            .accessibilityLabel(Text(light.label))
    }
}

struct ShellFleetLights: View {
    let lights: [FleetLight]
    let runnerAgents: [String: Int]
    var compact: Bool = false
    let onPressed: () -> Void
    var minHeight: CGFloat = 28

    private func agents(_ light: FleetLight) -> Int {
        light.kind == .runner ? runnerAgents[light.name.lowercased()] ?? 0 : 0
    }

    private func label(_ light: FleetLight) -> String {
        switch agents(light) {
        case 0: return light.label
        case 1: return light.label + ", running 1 agent"
        case let count: return light.label + ", running \(count) agents"
        }
    }

    var body: some View {
        Button {
            FmHaptics.tap()
            onPressed()
        } label: {
            HStack(spacing: compact ? -4 : -3) {
                ForEach(lights) { (light: FleetLight) in
                    ShellFleetLight(light: light, agents: agents(light))
                }
            }
            .frame(minHeight: minHeight)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(lights.map { label($0) }.joined(separator: "; ")))
        .accessibilityHint(Text("Opens your desktops, runners and core nodes"))
        .accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("footer-fleet-lights")
    }
}

struct ShellFleetLight: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion
    @State private var dim: Bool = false
    let light: FleetLight
    let agents: Int

    var body: some View {
        Image(systemName: light.kind == .core ? "server.rack" : "cpu")
            .font(.system(size: 11, weight: .semibold))
            .foregroundStyle(light.healthy ? fm.success : fm.danger)
            .opacity(!light.healthy && dim ? 0.25 : 1)
            .padding(1)
            .background(RoundedRectangle(cornerRadius: 3, style: .continuous).fill(fm.bgBase))
            .overlay(alignment: .bottomTrailing) {
                if let resources = light.resources {
                    ShellResourceBars(values: resources.values)
                }
            }
            .overlay(alignment: .topTrailing) {
                if agents > 0 {
                    ShellRunnerAgentCount(count: agents)
                        .offset(x: 4, y: -5)
                        .transition(.scale.combined(with: .opacity))
                }
            }
            .padding(.trailing, agents > 0 ? 7 : 0)
            .onAppear { blink() }
            .onChange(of: light.healthy) { (_: Bool, _: Bool) in blink() }
    }

    private func blink() {
        guard !light.healthy, !reduceMotion else {
            dim = false
            return
        }
        withAnimation(.easeInOut(duration: 0.6).repeatForever(autoreverses: true)) {
            dim = true
        }
    }
}

struct ShellResourceBars: View {
    static let warning: Double = 75
    static let bad: Double = 90

    @Environment(\.fmPalette) var fm
    let values: [Double?]
    var barWidth: CGFloat = 1.5
    var height: CGFloat = 6

    var body: some View {
        HStack(alignment: .bottom, spacing: 0.5) {
            ForEach(Array(values.enumerated()), id: \.offset) { (entry: (offset: Int, element: Double?)) in
                bar(entry.element)
            }
        }
        .padding(0.75)
        .background(RoundedRectangle(cornerRadius: 1.5, style: .continuous).fill(fm.bgBase))
        .accessibilityHidden(true)
    }

    private func bar(_ value: Double?) -> some View {
        ZStack(alignment: .bottom) {
            Rectangle().fill(fm.textTertiary.opacity(0.35))
            if let value {
                Rectangle()
                    .fill(color(value))
                    .frame(height: max(1.5, height * CGFloat(min(100, max(0, value)) / 100)))
            }
        }
        .frame(width: barWidth, height: height)
    }

    private func color(_ value: Double) -> Color {
        if value >= ShellResourceBars.bad { return fm.danger }
        return value >= ShellResourceBars.warning ? fm.warning : fm.success
    }
}

struct ShellRunnerAgentCount: View {
    @Environment(\.fmPalette) var fm
    let count: Int

    var body: some View {
        Text(verbatim: count > 99 ? "99+" : "\(count)")
            .font(.system(size: 7, weight: .heavy, design: .rounded))
            .monospacedDigit()
            .foregroundStyle(fm.accent)
            .lineLimit(1)
            .fixedSize()
            .shadow(color: fm.bgBase, radius: 0.5)
            .shadow(color: fm.bgBase, radius: 0.5)
            .accessibilityHidden(true)
    }
}

struct ShellFooterMacLight: View {
    @Environment(\.fmPalette) var fm
    let light: BuildMacLight
    let onPressed: () -> Void
    var glyph: CGFloat = 12
    var side: CGFloat = 28

    private var lightColor: Color {
        switch light.state {
        case .idle: return fm.success
        case .building: return fm.accent
        case .offline: return fm.danger
        }
    }

    var body: some View {
        Button {
            FmHaptics.tap()
            onPressed()
        } label: {
            Image(systemName: "laptopcomputer")
                .font(.system(size: glyph, weight: .semibold))
                .foregroundStyle(fm.textSecondary)
                .overlay(alignment: .topTrailing) {
                    Circle()
                        .fill(lightColor)
                        .frame(width: 6, height: 6)
                        .background(Circle().fill(fm.bgBase).padding(-1))
                        .offset(x: 3, y: -2)
                }
                .frame(minWidth: side, minHeight: side)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(light.label))
        .accessibilityHint(Text("Opens your desktops, runners and core nodes"))
        .accessibilityIdentifier("footer-mac-light")
    }
}

/// A glyph with the blue count when there is one.
struct ShellBadgedIcon: View {
    @Environment(\.fmPalette) var fm
    let icon: String
    let badge: Int
    let color: Color
    var size: CGFloat = 22

    var body: some View {
        FmIcon(icon, size: size, color: color)
            .overlay(alignment: .topTrailing) {
                if badge > 0 {
                    Text(verbatim: "\(badge)")
                        .fmText(.labelSmall, color: fm.count, weight: FmType.emphasis, tracking: 0, tabular: true)
                        .lineLimit(1)
                        .frame(minWidth: 16, minHeight: 16)
                        .offset(x: 8, y: -6)
                        .accessibilityHidden(true)
                }
            }
    }
}

struct ShellMenuRow: View {
    @Environment(\.fmPalette) var fm
    let icon: String
    let label: String
    let selected: Bool
    let badge: Int
    let onTap: () -> Void

    private var tone: Color {
        return selected ? fm.accent : fm.textSecondary
    }

    var body: some View {
        Button(action: onTap) {
            HStack(spacing: FmSpace.x3) {
                ShellBadgedIcon(icon: icon, badge: badge, color: tone)
                Text(label)
                    .fmText(
                        .bodyMedium,
                        color: selected ? fm.textPrimary : tone,
                        weight: selected ? FmType.emphasis : FmType.regular)
                    .lineLimit(1)
                    .truncationMode(.tail)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, FmSpace.x4)
            .frame(minHeight: 44)
            .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.06)))
        .accessibilityAddTraits(selected ? [.isSelected] : [])
    }
}

/// Sign-in control for a guest, in the slot a signed-in user's avatar takes.
struct ShellGuestSignInChip: View {
    @Environment(\.fmPalette) var fm
    let onPressed: () -> Void

    var body: some View {
        FmOutlinedButton(
            "Sign in",
            icon: "person_outline_rounded",
            foreground: fm.textPrimary,
            minHeight: 36,
            onPressed: onPressed)
            .accessibilityIdentifier("guest-top-sign-in")
    }
}

struct ShellNotificationButton: View {
    @Environment(\.fmPalette) var fm
    let unreadCount: Int
    let onPressed: () -> Void

    var body: some View {
        FmIconButton(label: unreadCount > 0 ? "Notifications, \(unreadCount) unread" : "Notifications", onPressed: onPressed) {
            ShellBadgedIcon(icon: FmIcons.notificationLine, badge: unreadCount, color: fm.textPrimary)
        }
        .accessibilityIdentifier("top-notifications-button")
    }
}
