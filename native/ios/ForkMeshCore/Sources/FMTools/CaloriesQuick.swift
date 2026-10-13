import FMApi
import FMBase
import Foundation

public struct CaloriesQuickFood: Equatable, Identifiable, Sendable {
    public var name: String
    public var brand: String
    public var portion: String
    public var calories: Double
    public var proteinG: Double
    public var carbsG: Double
    public var fatG: Double
    public var source: String
    public var category: String
    public var times: Int

    public init(name: String, brand: String = "", portion: String = "", calories: Double, proteinG: Double = 0,
                carbsG: Double = 0, fatG: Double = 0, source: String = "", category: String = "", times: Int = 0) {
        self.name = name
        self.brand = brand
        self.portion = portion
        self.calories = calories
        self.proteinG = proteinG
        self.carbsG = carbsG
        self.fatG = fatG
        self.source = source
        self.category = category
        self.times = times
    }

    public init(json: JSONObject) {
        name = ToolsJSON.text(json["name"], "")
        brand = ToolsJSON.text(json["brand"], "")
        portion = ToolsJSON.text(json["portion"], "")
        calories = CaloriesText.num(json["calories"])
        proteinG = CaloriesText.num(json["proteinG"])
        carbsG = CaloriesText.num(json["carbsG"])
        fatG = CaloriesText.num(json["fatG"])
        source = ToolsJSON.text(json["source"], "")
        category = ToolsJSON.text(json["category"], "")
        times = CaloriesText.int(json["times"])
    }

    public var id: String { CaloriesQuick.key(name) }

    public var json: JSONObject {
        [
            "name": .string(name),
            "brand": .string(brand),
            "portion": .string(portion),
            "calories": .int(Int64(TelemetryMath.round(calories))),
            "proteinG": .double(proteinG),
            "carbsG": .double(carbsG),
            "fatG": .double(fatG),
        ]
    }

    public var savedJSON: JSONObject {
        var out = json
        if source == "receipt" { out["source"] = "receipt" }
        return out
    }

    public var meta: String {
        [brand, portion, "\(CaloriesText.kcal(calories)) kcal", times > 0 ? "logged \(times)×" : ""]
            .filter { !$0.isEmpty }.joined(separator: " · ")
    }
}

public enum CaloriesQuickKind: String, Equatable, Sendable {
    case mine, frequent, common
}

public struct CaloriesQuickSection: Equatable, Identifiable, Sendable {
    public let kind: CaloriesQuickKind
    public let title: String
    public let foods: [CaloriesQuickFood]
    public let empty: String

    public var id: String { kind.rawValue }
}

public struct CaloriesWeighIn: Equatable, Sendable {
    public let kg: Double
    public let day: String
}

public enum CaloriesQuick {
    public static let placeholder = "What did you eat? “2 eggs and toast for breakfast”, “weighed 72.5 kg”"
    public static let title = "Log to your Calorie Counter diary"
    public static let receiptHelp = "Read a grocery receipt to fill My foods."
    static let numberWords: [String: Double] = [
        "a": 1, "an": 1, "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6, "seven": 7, "eight": 8,
        "nine": 9, "ten": 10, "half": 0.5, "a half": 0.5, "couple": 2, "a couple of": 2, "a few": 3,
    ]
    static let mealWords: [String: String] = [
        "breakfast": "breakfast", "brunch": "breakfast", "lunch": "lunch", "dinner": "dinner", "supper": "dinner",
        "snack": "snack", "snacks": "snack", "dessert": "snack",
    ]
    static let mealPattern = "(breakfast|brunch|lunch|dinner|supper|snacks?|dessert)"

    public static func key(_ name: String) -> String { name.dartTrimmed.lowercased() }

    public static func foods(_ value: JSONValue?) -> [CaloriesQuickFood] {
        ToolsJSON.maps(value).map { CaloriesQuickFood(json: $0) }.filter { !$0.name.isEmpty }
    }

    public static func frequent(entries: [JSONObject], mine: [CaloriesQuickFood]) -> [CaloriesQuickFood] {
        let known = Set(mine.map { key($0.name) })
        var order = [String]()
        var counts = [String: Int]()
        var first = [String: CaloriesQuickFood]()
        for entry in entries {
            let name = ToolsJSON.text(entry["name"], "")
            let k = key(name)
            if k.isEmpty || known.contains(k) { continue }
            if let seen = counts[k] {
                counts[k] = seen + 1
            } else {
                counts[k] = 1
                order.append(k)
                var food = CaloriesQuickFood(json: entry)
                food.source = ""
                food.category = ""
                first[k] = food
            }
        }
        let ranked = order.enumerated()
            .filter { (counts[$0.element] ?? 0) > 1 }
            .sorted { a, b in
                let ca = counts[a.element] ?? 0, cb = counts[b.element] ?? 0
                return ca != cb ? ca > cb : a.offset < b.offset
            }
            .prefix(8)
        return ranked.compactMap { pair in
            guard var food = first[pair.element] else { return nil }
            food.times = counts[pair.element] ?? 0
            return food
        }
    }

    public static func filter(_ foods: [CaloriesQuickFood], _ query: String) -> [CaloriesQuickFood] {
        let q = key(query)
        if q.isEmpty { return foods }
        return foods.filter { key($0.name).contains(q) || key($0.brand).contains(q) }
    }

    public static func sections(mine: [CaloriesQuickFood], frequent: [CaloriesQuickFood], common: [CaloriesQuickFood],
                                filter query: String) -> [CaloriesQuickSection] {
        let filtering = !key(query).isEmpty
        var out = [CaloriesQuickSection(
            kind: .mine, title: "My foods", foods: filter(mine, query),
            empty: filtering ? "No match." : "Nothing saved yet. Read a receipt or star a food.")]
        if !frequent.isEmpty {
            out.append(CaloriesQuickSection(kind: .frequent, title: "You log often", foods: filter(frequent, query), empty: "No match."))
        }
        out.append(CaloriesQuickSection(kind: .common, title: "Common foods", foods: filter(common, query), empty: "No match."))
        return out
    }

    public static func meal(_ text: String) -> String? {
        guard let found = groups("\\b\(mealPattern)\\b", text), let word = found[1] else { return nil }
        return mealWords[word.lowercased()]
    }

    public static func day(_ text: String, logDay: String, today: String, timeZone: TimeZone = .current) -> String {
        if groups("\\byesterday\\b", text) != nil { return CaloriesText.addDays(today, -1, timeZone: timeZone) }
        if groups("\\btoday\\b|\\bthis (morning|afternoon|evening)\\b|\\btonight\\b", text) != nil { return today }
        return logDay
    }

    public static func weight(_ text: String, imperial: Bool, logDay: String, today: String,
                              timeZone: TimeZone = .current) -> CaloriesWeighIn? {
        let pattern = "^(?:i\\s+)?(?:weigh(?:ed|t)?(?:\\s+in)?|my weight(?:\\s+is)?|weight(?:\\s+is)?)\\s*(?:at|is|was|:|=)?\\s*(\\d{2,3}(?:[.,]\\d+)?)\\s*(kg|kgs|kilos?|kilograms?|lb|lbs|pounds?)?\\s*(?:today|yesterday|this morning)?\\.?$"
        guard let found = groups(pattern, text.dartTrimmed), let number = found[1],
              let value = Double(number.replacingOccurrences(of: ",", with: ".")) else { return nil }
        let unit = (found[2] ?? "").lowercased()
        let kg: Double
        if unit.hasPrefix("k") {
            kg = value
        } else if !unit.isEmpty {
            kg = value * CaloriesText.kgPerLb
        } else {
            kg = imperial ? value * CaloriesText.kgPerLb : value
        }
        return kg > 0 ? CaloriesWeighIn(kg: kg, day: day(text, logDay: logDay, today: today, timeZone: timeZone)) : nil
    }

    public static func singular(_ word: String) -> String {
        let once = replace("(ch|sh|ss|x)es$", in: word, with: "$1", caseInsensitive: false)
        return replace("([^s])s$", in: once, with: "$1", caseInsensitive: false)
    }

    public static func normalise(_ name: String) -> String {
        replace("[^a-z0-9 ]+", in: name.lowercased(), with: " ", caseInsensitive: false)
            .split(whereSeparator: { $0 == " " || $0 == "\t" || $0 == "\n" })
            .map { singular(String($0)) }
            .joined(separator: " ")
    }

    public static func parts(_ text: String) -> [String] {
        var cleaned = text.lowercased()
        cleaned = replace("\\b(for|at|with)\\s+\(mealPattern)\\b", in: cleaned, with: " ")
        cleaned = replace("\\b\(mealPattern)\\s*:", in: cleaned, with: " ")
        cleaned = replace("\\b(yesterday|today|tonight|this (morning|afternoon|evening))\\b", in: cleaned, with: " ")
        cleaned = replace("^\\s*(i\\s+)?(just\\s+)?(had|ate|drank|log|add)\\b", in: cleaned, with: " ")
        return split(",|;|\\n|\\+|&|\\band\\b", cleaned)
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
    }

    public static func match(_ text: String, foods: [CaloriesQuickFood]) -> [CaloriesQuickFood]? {
        var index = [String: CaloriesQuickFood]()
        for food in foods {
            let k = normalise(food.name)
            if !k.isEmpty && index[k] == nil { index[k] = food }
        }
        var matched = [CaloriesQuickFood]()
        for part in parts(text) {
            var rest = part
            var amount = 1.0
            if let found = groups("^(\\d+/\\d+|\\d+(?:[.,]\\d+)?)\\s*(?:x\\s+)?", rest, caseInsensitive: false),
               let whole = found[0], let number = found[1] {
                if number.contains("/") {
                    let pieces = number.split(separator: "/").compactMap { Double(String($0)) }
                    amount = pieces.count == 2 && pieces[1] != 0 ? pieces[0] / pieces[1] : 0
                } else {
                    amount = Double(number.replacingOccurrences(of: ",", with: ".")) ?? 0
                }
                rest = String(rest.dropFirst(whole.count))
            } else if let found = groups("^(a couple of|a few|a half|an|a|one|two|three|four|five|six|seven|eight|nine|ten|half|couple)\\s+", rest, caseInsensitive: false),
                      let whole = found[0], let word = found[1] {
                amount = numberWords[word] ?? 1
                rest = String(rest.dropFirst(whole.count))
            }
            rest = replace("^(?:(?:of|some|an?)\\s+)+", in: rest, with: "", caseInsensitive: false)
            guard let food = index[normalise(rest)], amount > 0, amount <= 20 else { return nil }
            func scaled(_ value: Double) -> Double { Double(DartCore.toStringAsFixed(value * amount, 1)) ?? value * amount }
            matched.append(CaloriesQuickFood(
                name: food.name, brand: food.brand,
                portion: amount == 1 ? food.portion : "\(CaloriesText.grams(amount)) × \(food.portion.isEmpty ? "serving" : food.portion)",
                calories: Double(TelemetryMath.round(food.calories * amount)),
                proteinG: scaled(food.proteinG), carbsG: scaled(food.carbsG), fatG: scaled(food.fatG)))
        }
        return matched.isEmpty ? nil : matched
    }

    public static func loggedMessage(_ items: [CaloriesQuickFood], meal: String, dayLabel: String?, goal: Int,
                                     todayCalories: Double?) -> String {
        let total = items.reduce(0.0) { $0 + $1.calories }
        let list = items.map { "\($0.name) (\(CaloriesText.kcal($0.calories)) kcal)" }.joined(separator: ", ")
        var today = ""
        if dayLabel == nil, goal > 0, let eaten = todayCalories {
            today = " Today: \(CaloriesText.kcal(eaten)) of \(goal) kcal."
        }
        return "Logged to \(meal), \(dayLabel ?? "today"): \(list). \(CaloriesText.kcal(total)) kcal in all.\(today)"
    }

    public static func receiptMessage(_ payload: JSONObject) -> String {
        let added = (payload["added"]?.arrayValue ?? []).map { DartCore.toString($0) }
        let found = (payload["found"]?.arrayValue ?? []).map { DartCore.toString($0) }
        let store = ToolsJSON.text(payload["store"], "")
        let from = store.isEmpty ? "" : " from \(store)"
        if !added.isEmpty {
            return "Added \(added.count) food\(added.count == 1 ? "" : "s")\(from) to My foods: \(added.joined(separator: ", "))."
        }
        return "Read \(found.count) food\(found.count == 1 ? "" : "s")\(from); all were already in My foods."
    }

    public static func receiptError(_ error: Error) -> String {
        let failure = error as? OrgApiException
        switch failure?.code {
        case "rate_limited": return "You have used this hour's AI reads. Try the receipt again later."
        case "no_foods_on_receipt": return "No foods were found on that receipt. Try a sharper, flatter photo."
        case "ai_unavailable": return "Receipt reading needs AI, which is unavailable on this host."
        default: return "Could not read the receipt: \(failure?.message ?? "the host did not answer.")"
        }
    }

    public static func estimateError(_ error: Error) -> String {
        let failure = error as? OrgApiException
        switch failure?.code {
        case "rate_limited": return "You have used this hour's AI estimates. Try again later, or use Quick add."
        case "ai_unavailable": return "AI estimates are unavailable on this host. Use foods from Quick add, like “2 eggs and a banana”."
        case "nothing_identified": return "Nothing recognisable as food there. Try naming the food and amount."
        default: return "Could not estimate: \(failure?.message ?? "the host did not answer.")"
        }
    }

    static func regex(_ pattern: String, caseInsensitive: Bool) -> NSRegularExpression? {
        try? NSRegularExpression(pattern: pattern, options: caseInsensitive ? [.caseInsensitive] : [])
    }

    static func groups(_ pattern: String, _ text: String, caseInsensitive: Bool = true) -> [String?]? {
        guard let re = regex(pattern, caseInsensitive: caseInsensitive),
              let found = re.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) else { return nil }
        return (0..<found.numberOfRanges).map { i in
            Range(found.range(at: i), in: text).map { String(text[$0]) }
        }
    }

    static func replace(_ pattern: String, in text: String, with template: String, caseInsensitive: Bool = true) -> String {
        guard let re = regex(pattern, caseInsensitive: caseInsensitive) else { return text }
        return re.stringByReplacingMatches(in: text, range: NSRange(text.startIndex..., in: text), withTemplate: template)
    }

    static func split(_ pattern: String, _ text: String) -> [String] {
        guard let re = regex(pattern, caseInsensitive: false) else { return [text] }
        var out = [String]()
        var start = text.startIndex
        for found in re.matches(in: text, range: NSRange(text.startIndex..., in: text)) {
            guard let range = Range(found.range, in: text) else { continue }
            out.append(String(text[start..<range.lowerBound]))
            start = range.upperBound
        }
        out.append(String(text[start...]))
        return out
    }
}
