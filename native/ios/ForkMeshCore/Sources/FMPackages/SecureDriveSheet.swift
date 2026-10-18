import Foundation

public struct SecureDriveSheet: Equatable, Sendable {
    public static let maxRows = 10000
    public static let maxColumns = 702

    struct Value {
        var number: Double = 0
        var text = ""
        var error = ""
        var isText = false
        var empty = false

        static func failure(_ code: String) -> Value {
            var v = Value()
            v.error = code
            return v
        }

        static func of(_ n: Double) -> Value {
            var v = Value()
            v.number = n
            return v
        }
    }

    var cells: [[String]] = []

    public init() {}

    public var rows: Int { cells.count }
    public var columns: Int { cells.map(\.count).max() ?? 0 }

    public static func fromCsv(_ bytes: [UInt8]) -> SecureDriveSheet {
        var sheet = SecureDriveSheet()
        var scalars = Array(String(decoding: bytes, as: UTF8.self).unicodeScalars)
        if scalars.first == "\u{FEFF}" { scalars.removeFirst() }
        var row = [String]()
        var field = String.UnicodeScalarView()
        var quoted = false
        var any = false
        func addRow() {
            if sheet.cells.count < maxRows { sheet.cells.append(Array(row.prefix(maxColumns))) }
        }
        var i = 0
        while i < scalars.count {
            let c = scalars[i]
            if quoted {
                if c == "\"" {
                    if i + 1 < scalars.count && scalars[i + 1] == "\"" {
                        field.append(c)
                        i += 1
                    } else {
                        quoted = false
                    }
                } else {
                    field.append(c)
                }
                i += 1
                continue
            }
            if c == "\"" && field.isEmpty {
                quoted = true
                any = true
            } else if c == "," {
                row.append(String(field))
                field = String.UnicodeScalarView()
                any = true
            } else if c == "\n" || c == "\r" {
                if c == "\r" && i + 1 < scalars.count && scalars[i + 1] == "\n" { i += 1 }
                row.append(String(field))
                field = String.UnicodeScalarView()
                addRow()
                row = []
                any = false
            } else {
                field.append(c)
                any = true
            }
            i += 1
        }
        if any || !field.isEmpty || !row.isEmpty {
            row.append(String(field))
            addRow()
        }
        return sheet
    }

    public func toCsv() -> [UInt8] {
        var lastRow = -1
        for (r, row) in cells.enumerated() where row.contains(where: { !$0.isEmpty }) { lastRow = r }
        if lastRow < 0 { return [] }
        var lines = [String]()
        for r in 0...lastRow {
            var row = cells[r]
            while let last = row.last, last.isEmpty { row.removeLast() }
            let fields = row.map { cell -> String in
                if cell.contains(",") || cell.contains("\"") || cell.unicodeScalars.contains("\n") || cell.unicodeScalars.contains("\r")
                    || cell != cell.trimmingCharacters(in: .whitespacesAndNewlines) {
                    return "\"" + cell.replacingOccurrences(of: "\"", with: "\"\"") + "\""
                }
                return cell
            }
            lines.append(fields.joined(separator: ","))
        }
        return Array((lines.joined(separator: "\r\n") + "\r\n").utf8)
    }

    public func raw(_ row: Int, _ column: Int) -> String {
        guard row >= 0, row < rows, column >= 0, column < cells[row].count else { return "" }
        return cells[row][column]
    }

    public mutating func setRaw(_ row: Int, _ column: Int, _ text: String) {
        guard row >= 0, column >= 0, row < Self.maxRows, column < Self.maxColumns else { return }
        if row >= rows {
            if text.isEmpty { return }
            cells.append(contentsOf: Array(repeating: [], count: row + 1 - rows))
        }
        if column >= cells[row].count {
            if text.isEmpty { return }
            cells[row].append(contentsOf: Array(repeating: "", count: column + 1 - cells[row].count))
        }
        cells[row][column] = text
    }

    public func display(_ row: Int, _ column: Int) -> String {
        let text = raw(row, column)
        if !text.hasPrefix("=") { return text }
        var visiting = Set<Int64>()
        let value = cellValue(row, column, &visiting)
        if !value.error.isEmpty { return value.error }
        if value.isText { return value.text }
        return Self.formatNumber(value.empty ? 0 : value.number)
    }

    static func formatNumber(_ value: Double) -> String {
        if value.isNaN || value.isInfinite { return "#NUM!" }
        if value == value.rounded(.down) && abs(value) < 1e15 { return String(Int64(value)) }
        return String(format: "%.12g", value)
    }

    static func parseNumber(_ text: String) -> Double? {
        if text.contains("x") || text.contains("X") || text.contains("_") { return nil }
        return Double(text)
    }

    func cellValue(_ row: Int, _ column: Int, _ visiting: inout Set<Int64>) -> Value {
        let text = raw(row, column).trimmingCharacters(in: .whitespacesAndNewlines)
        if text.isEmpty {
            var v = Value()
            v.empty = true
            return v
        }
        if text.hasPrefix("=") {
            let key = (Int64(row) << 20) | Int64(column)
            if visiting.contains(key) || visiting.count > 4096 { return .failure("#CYCLE!") }
            visiting.insert(key)
            var parser = FormulaParser(sheet: self, text: Array(String(text.dropFirst()).unicodeScalars))
            let value = parser.run(&visiting)
            visiting.remove(key)
            return value
        }
        if let number = Self.parseNumber(text) { return .of(number) }
        var v = Value()
        v.isText = true
        v.text = text
        return v
    }

    public static func columnName(_ column: Int) -> String {
        var name = ""
        var n = column + 1
        while n > 0 {
            let rem = (n - 1) % 26
            name = String(UnicodeScalar(UInt8(65 + rem))) + name
            n = (n - 1) / 26
        }
        return name
    }

    public static func parseCell(_ reference: String) -> (row: Int, column: Int)? {
        let scalars = Array(reference.unicodeScalars)
        var i = 0
        var letters = [UnicodeScalar]()
        while i < scalars.count, letters.count < 2, isLetter(scalars[i]) {
            letters.append(scalars[i])
            i += 1
        }
        var digits = ""
        while i < scalars.count, digits.count < 5, isDigit(scalars[i]) {
            digits.unicodeScalars.append(scalars[i])
            i += 1
        }
        guard !letters.isEmpty, !digits.isEmpty, i == scalars.count else { return nil }
        return cell(letters, digits)
    }

    static func cell(_ letters: [UnicodeScalar], _ digits: String) -> (row: Int, column: Int)? {
        var c = 0
        for letter in letters {
            let upper = letter.value >= 97 ? letter.value - 32 : letter.value
            c = c * 26 + Int(upper - 64)
        }
        guard let r = Int(digits), r >= 1, r <= maxRows, c >= 1, c <= maxColumns else { return nil }
        return (r - 1, c - 1)
    }

    static func isLetter(_ c: UnicodeScalar) -> Bool { (c.value >= 65 && c.value <= 90) || (c.value >= 97 && c.value <= 122) }
    static func isDigit(_ c: UnicodeScalar) -> Bool { c.value >= 48 && c.value <= 57 }
}

struct FormulaParser {
    typealias Value = SecureDriveSheet.Value

    let sheet: SecureDriveSheet
    let text: [UnicodeScalar]
    var pos = 0

    init(sheet: SecureDriveSheet, text: [UnicodeScalar]) {
        self.sheet = sheet
        self.text = text
    }

    mutating func run(_ visiting: inout Set<Int64>) -> Value {
        let value = comparison(&visiting)
        skip()
        if value.error.isEmpty && pos < text.count { return .failure("#ERROR!") }
        return value
    }

    mutating func skip() {
        while pos < text.count, CharacterSet.whitespacesAndNewlines.contains(text[pos]) { pos += 1 }
    }

    mutating func take(_ token: String) -> Bool {
        skip()
        let wanted = Array(token.uppercased().unicodeScalars)
        guard pos + wanted.count <= text.count else { return false }
        for (i, c) in wanted.enumerated() {
            let have = text[pos + i]
            let upper = (have.value >= 97 && have.value <= 122) ? UnicodeScalar(have.value - 32)! : have
            if upper != c { return false }
        }
        pos += wanted.count
        return true
    }

    static func numeric(_ v: Value) -> Double? {
        if !v.error.isEmpty || v.isText { return nil }
        return v.empty ? 0 : v.number
    }

    mutating func comparison(_ visiting: inout Set<Int64>) -> Value {
        let left = additive(&visiting)
        for op in ["<=", ">=", "<>", "<", ">", "="] {
            if !take(op) { continue }
            let right = additive(&visiting)
            if !left.error.isEmpty { return left }
            if !right.error.isEmpty { return right }
            let result: Bool
            if left.isText || right.isText {
                let order = left.text.compare(right.text, options: .caseInsensitive)
                switch op {
                case "=": result = order == .orderedSame
                case "<>": result = order != .orderedSame
                case "<": result = order == .orderedAscending
                case ">": result = order == .orderedDescending
                case "<=": result = order != .orderedDescending
                default: result = order != .orderedAscending
                }
            } else {
                let a = Self.numeric(left) ?? 0
                let b = Self.numeric(right) ?? 0
                switch op {
                case "=": result = a == b
                case "<>": result = a != b
                case "<": result = a < b
                case ">": result = a > b
                case "<=": result = a <= b
                default: result = a >= b
                }
            }
            return .of(result ? 1 : 0)
        }
        return left
    }

    mutating func additive(_ visiting: inout Set<Int64>) -> Value {
        var left = multiplicative(&visiting)
        while true {
            let plus = take("+")
            if !plus && !take("-") { return left }
            let right = multiplicative(&visiting)
            if !left.error.isEmpty { return left }
            if !right.error.isEmpty { return right }
            guard let a = Self.numeric(left), let b = Self.numeric(right) else { return .failure("#VALUE!") }
            left = .of(plus ? a + b : a - b)
        }
    }

    mutating func multiplicative(_ visiting: inout Set<Int64>) -> Value {
        var left = power(&visiting)
        while true {
            let times = take("*")
            if !times && !take("/") { return left }
            let right = power(&visiting)
            if !left.error.isEmpty { return left }
            if !right.error.isEmpty { return right }
            guard let a = Self.numeric(left), let b = Self.numeric(right) else { return .failure("#VALUE!") }
            if !times && b == 0 { return .failure("#DIV/0!") }
            left = .of(times ? a * b : a / b)
        }
    }

    mutating func power(_ visiting: inout Set<Int64>) -> Value {
        let base = unary(&visiting)
        if !take("^") { return base }
        let exponent = power(&visiting)
        if !base.error.isEmpty { return base }
        if !exponent.error.isEmpty { return exponent }
        guard let a = Self.numeric(base), let b = Self.numeric(exponent) else { return .failure("#VALUE!") }
        return .of(pow(a, b))
    }

    mutating func unary(_ visiting: inout Set<Int64>) -> Value {
        if take("-") {
            let inner = unary(&visiting)
            if !inner.error.isEmpty { return inner }
            guard let a = Self.numeric(inner) else { return .failure("#VALUE!") }
            return .of(-a)
        }
        if take("+") { return unary(&visiting) }
        return primary(&visiting)
    }

    mutating func reference() -> (row: Int, column: Int)? {
        skip()
        var i = pos
        if i < text.count, text[i] == "$" { i += 1 }
        var letters = [UnicodeScalar]()
        while i < text.count, letters.count < 2, SecureDriveSheet.isLetter(text[i]) {
            letters.append(text[i])
            i += 1
        }
        if letters.isEmpty { return nil }
        if i < text.count, text[i] == "$" { i += 1 }
        var digits = ""
        while i < text.count, digits.count < 5, SecureDriveSheet.isDigit(text[i]) {
            digits.unicodeScalars.append(text[i])
            i += 1
        }
        if digits.isEmpty { return nil }
        guard let cell = SecureDriveSheet.cell(letters, digits) else { return nil }
        pos = i
        return cell
    }

    mutating func range(_ values: inout [Value], _ visiting: inout Set<Int64>) -> Bool {
        let start = pos
        guard let first = reference() else { return false }
        if !take(":") {
            values.append(sheet.cellValue(first.row, first.column, &visiting))
            return true
        }
        guard let second = reference() else {
            pos = start
            return false
        }
        for r in min(first.row, second.row)...max(first.row, second.row) {
            for c in min(first.column, second.column)...max(first.column, second.column) {
                values.append(sheet.cellValue(r, c, &visiting))
            }
        }
        return true
    }

    mutating func function(_ name: String, _ visiting: inout Set<Int64>) -> Value {
        var values = [Value]()
        var arguments = [[Value]]()
        if !take(")") {
            while true {
                var argument = [Value]()
                let start = pos
                if range(&argument, &visiting) {
                    skip()
                    if pos < text.count && text[pos] != "," && text[pos] != ")" {
                        pos = start
                        argument = [comparison(&visiting)]
                    }
                } else {
                    argument.append(comparison(&visiting))
                }
                arguments.append(argument)
                values.append(contentsOf: argument)
                if take(",") { continue }
                if take(")") { break }
                return .failure("#ERROR!")
            }
        }
        if let failed = values.first(where: { !$0.error.isEmpty }) { return failed }
        let upper = name.uppercased()
        let numbers = values.filter { !$0.isText && !$0.empty }.map(\.number)
        func first(_ index: Int) -> Value { arguments[index].first ?? Value() }
        switch upper {
        case "SUM": return .of(numbers.reduce(0, +))
        case "AVERAGE":
            if numbers.isEmpty { return .failure("#DIV/0!") }
            return .of(numbers.reduce(0, +) / Double(numbers.count))
        case "MIN": return .of(numbers.min() ?? 0)
        case "MAX": return .of(numbers.max() ?? 0)
        case "COUNT": return .of(Double(numbers.count))
        default: break
        }
        if upper == "ABS", arguments.count == 1, arguments[0].count == 1, let a = Self.numeric(arguments[0][0]) {
            return .of(abs(a))
        }
        if upper == "ROUND" && (arguments.count == 1 || arguments.count == 2) {
            guard let a = Self.numeric(first(0)) else { return .failure("#VALUE!") }
            var b = 0.0
            if arguments.count == 2 {
                guard let digits = Self.numeric(first(1)) else { return .failure("#VALUE!") }
                b = digits
            }
            let scale = pow(10.0, b.rounded())
            return .of((a * scale).rounded() / scale)
        }
        if upper == "IF" && (arguments.count == 2 || arguments.count == 3) {
            guard let test = Self.numeric(first(0)) else { return .failure("#VALUE!") }
            if test != 0 { return first(1) }
            return arguments.count == 3 ? first(2) : .of(0)
        }
        return .failure("#NAME?")
    }

    mutating func primary(_ visiting: inout Set<Int64>) -> Value {
        skip()
        if pos >= text.count { return .failure("#ERROR!") }
        if take("(") {
            let inner = comparison(&visiting)
            if !take(")") { return .failure("#ERROR!") }
            return inner
        }
        if text[pos] == "\"" {
            var out = String.UnicodeScalarView()
            pos += 1
            while pos < text.count {
                let c = text[pos]
                pos += 1
                if c == "\"" {
                    if pos < text.count && text[pos] == "\"" {
                        out.append(c)
                        pos += 1
                        continue
                    }
                    var v = Value()
                    v.isText = true
                    v.text = String(out)
                    return v
                }
                out.append(c)
            }
            return .failure("#ERROR!")
        }
        if let length = numberLength() {
            let literal = String(String.UnicodeScalarView(text[pos..<(pos + length)]))
            pos += length
            return .of(Double(literal) ?? 0)
        }
        if let length = nameLength() {
            let name = String(String.UnicodeScalarView(text[pos..<(pos + length)]))
            pos += length
            _ = take("(")
            return function(name, &visiting)
        }
        if let cell = reference() { return sheet.cellValue(cell.row, cell.column, &visiting) }
        return .failure("#NAME?")
    }

    func digitsFrom(_ index: Int) -> Int {
        var i = index
        while i < text.count, SecureDriveSheet.isDigit(text[i]) { i += 1 }
        return i - index
    }

    func exponentFrom(_ index: Int) -> Int {
        guard index < text.count, text[index] == "e" || text[index] == "E" else { return 0 }
        var i = index + 1
        if i < text.count, text[i] == "-" || text[i] == "+" { i += 1 }
        let digits = digitsFrom(i)
        return digits > 0 ? i + digits - index : 0
    }

    func numberLength() -> Int? {
        let leading = digitsFrom(pos)
        if leading > 0 {
            var i = pos + leading
            if i < text.count, text[i] == "." { i += 1 + digitsFrom(i + 1) }
            i += exponentFrom(i)
            return i - pos
        }
        if pos < text.count, text[pos] == "." {
            let fraction = digitsFrom(pos + 1)
            if fraction == 0 { return nil }
            var i = pos + 1 + fraction
            i += exponentFrom(i)
            return i - pos
        }
        return nil
    }

    func nameLength() -> Int? {
        var i = pos
        while i < text.count, SecureDriveSheet.isLetter(text[i]) { i += 1 }
        if i == pos { return nil }
        var j = i
        while j < text.count, CharacterSet.whitespacesAndNewlines.contains(text[j]) { j += 1 }
        return j < text.count && text[j] == "(" ? i - pos : nil
    }
}
