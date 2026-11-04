#if DEBUG
import FMAccount
import FMAssistant
import FMBase
import Foundation
import UIKit

@MainActor
enum CloudDragonLab {
    nonisolated static let key: String = "FMDragonLab"
    nonisolated static let file: String = "dragon-lab.txt"

    nonisolated static var on: Bool { UserDefaults.standard.bool(forKey: key) }

    private static var running: Bool = false

    static func start() {
        guard on, !running else { return }
        running = true
        Task { @MainActor in
            while !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(300))
                poll()
            }
        }
    }

    private static var url: URL? {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first?.appendingPathComponent(file)
    }

    private static func poll() {
        guard let url = url, let text = try? String(contentsOf: url, encoding: .utf8) else { return }
        try? FileManager.default.removeItem(at: url)
        for line in text.split(separator: "\n") {
            run(line.split(separator: " ").map(String.init))
        }
    }

    private static func run(_ words: [String]) {
        guard let command = words.first else { return }
        let hub: GenieHub = GenieHub.shared
        let crawler: GenieCrawler = GenieCrawler.shared
        let director: CloudDragonDirector = CloudDragonDirector.shared
        let args: [String] = Array(words.dropFirst())
        let number: (Int) -> Double = { (i: Int) -> Double in i < args.count ? Double(args[i]) ?? 0 : 0 }
        switch command {
        case "mascot":
            hub.setMascot(GenieMascot(stored: args.first))
        case "hide":
            hub.setSpiderHidden(true)
        case "show":
            hub.setSpiderHidden(false)
        case "activity":
            if let act = DragonActivity(rawValue: args.first ?? "") { director.begin(act, facing: number(1)) }
        case "end":
            director.end()
        case "habit":
            director.perform(args.first ?? "yawn")
        case "reaction":
            if let reaction = DragonReaction(rawValue: args.first ?? "") { director.react(reaction) }
        case "sleep":
            director.sleepNow()
        case "wake":
            director.wakeNow()
        case "stroll":
            crawler.stroll(to: CGPoint(x: number(0), y: number(1)))
        case "fling":
            crawler.fling(CGSize(width: number(0), height: number(1)))
        case "reset":
            crawler.reset()
        case "scale":
            hub.resized(to: number(0))
        case "menu":
            hub.openMenu()
        case "closemenu":
            hub.closeMenu()
        case "cue":
            switch args.first ?? "" {
            case "wave": GenieCues.shared.play(.wave)
            case "cheer": GenieCues.shared.play(.cheer)
            case "surprise": GenieCues.shared.play(.surprise)
            case "dizzy": GenieCues.shared.play(.dizzy)
            default: GenieCues.shared.play(.nod)
            }
        case "wander":
            crawler.wander()
        case "surprise":
            GenieHandCoach.shared.surprise()
        case "trick":
            if let trick = GenieTricks.named(args.first ?? "") {
                _ = GenieHandCoach.shared.perform(GenieImprovisation(trick: trick, line: "", byModel: false), at: Date())
            }
        case "aquarium":
            Services.get(SettingsService.self)?.setAquariumOn(number(0) > 0)
        case "plays":
            GenieScreenPlayer.shared.hidden = number(0) <= 0
        case "wanders":
            crawler.setWanders(number(0) > 0)
        default:
            break
        }
    }
}
#endif
