import Foundation

public enum AppleHealthTool: String, CaseIterable, Sendable {
    case steps
    case water
    case calories
    case sleep
    case sugarBreak
    case stretch
    case foodGuide
}

public struct AppleHealthUse: Identifiable, Equatable, Sendable {
    public let tool: AppleHealthTool
    public let name: String
    public let reads: String
    public let writes: String
    public let account: String

    public var id: String { tool.rawValue }
}

public enum AppleHealthGuide {
    public static let title = "Apple Health"
    public static let badge = "Works with Apple Health"
    public static let framework = "Uses Apple's HealthKit"
    public static let learnMore = "How ForkMesh uses Apple Health"
    public static let settingsSubtitle = "What ForkMesh reads and writes with HealthKit"
    public static let readsLabel = "Reads from Apple Health"
    public static let writesLabel = "Writes to Apple Health"
    public static let accountLabel = "Saved to your ForkMesh account"
    public static let toolsHeader = "Tools that use Apple Health"
    public static let promisesHeader = "Your data, your choice"
    public static let privacyLabel = "Privacy Policy: Apple Health"

    public static let summary =
        "ForkMesh uses Apple's HealthKit framework to connect seven optional tools to the Apple Health app on this iPhone: "
        + "Pedometer, Water Intake, Calorie Counter, Sleep, Sugar Break, Stretch Break and Food Guide. "
        + "Each tool has its own Apple Health switch, and nothing is read or written until you turn it on and allow it in the iOS permission sheet."

    public static let uses: [AppleHealthUse] = [
        AppleHealthUse(
            tool: .steps, name: "Pedometer",
            reads: "Steps, walking and running distance, flights climbed and active energy.",
            writes: "Heart rate, only when you tap out your pulse and choose Save to Apple Health.",
            account: "One total per day, so your steps show on web and desktop. Heart rate is never sent."),
        AppleHealthUse(
            tool: .water, name: "Water Intake",
            reads: "Water that other apps logged.",
            writes: "Each drink you log, as Water.",
            account: "Water from other apps is added to your drinks log."),
        AppleHealthUse(
            tool: .calories, name: "Calorie Counter",
            reads: "The meals ForkMesh wrote, so edits and deletions stay in step.",
            writes: "Dietary energy, protein, carbohydrates and fat for the meals you log.",
            account: "Nothing from Apple Health."),
        AppleHealthUse(
            tool: .sleep, name: "Sleep",
            reads: "Sleep that an Apple Watch or another app recorded.",
            writes: "Nights your phone detected, only with Save detected sleep to Apple Health on.",
            account: "Each night's sleep, encrypted at rest."),
        AppleHealthUse(
            tool: .sugarBreak, name: "Sugar Break",
            reads: "Dietary sugar that other apps logged.",
            writes: "Nothing.",
            account: "Nothing. Apple Health sugar never leaves this phone."),
        AppleHealthUse(
            tool: .stretch, name: "Stretch Break",
            reads: "Minutes of Flexibility and Mind and Body workouts.",
            writes: "Each guided session you finish, as a workout.",
            account: "Nothing from Apple Health."),
        AppleHealthUse(
            tool: .foodGuide, name: "Food Guide",
            reads: "The last 14 days of dietary energy, protein, fiber, sugar, saturated fat, sodium, iron, calcium, potassium, magnesium, folate and vitamins C, D and B12 that other apps logged.",
            writes: "Nothing.",
            account: "Nothing. Your nutrition history never leaves this phone; meal ideas are worked out on the phone."),
    ]

    public static let promises: [String] = [
        "Apple Health is off until you turn it on in a tool, and iOS asks you before ForkMesh can read or write anything.",
        "ForkMesh never uses Apple Health data for advertising or marketing, and never sells it or shares it with data brokers.",
        "Turn a tool's Apple Health switch off at any time, or change access in the Settings app under Health > Data Access & Devices > ForkMesh.",
        "Uninstalling a tool deletes its data from your ForkMesh account. Apple Health keeps its own copy.",
    ]

    public static func use(_ tool: AppleHealthTool) -> AppleHealthUse {
        uses.first { $0.tool == tool } ?? uses[0]
    }
}
