import Foundation

struct CharSet: Sendable {
    var lo: UInt64 = 0
    var hi: UInt64 = 0
    var nonAscii = false

    @inline(__always)
    func contains(_ c: UInt16) -> Bool {
        if c < 64 { return (lo >> UInt64(c)) & 1 == 1 }
        if c < 128 { return (hi >> UInt64(c - 64)) & 1 == 1 }
        return nonAscii
    }

    mutating func insert(_ c: UInt16) {
        if c < 64 { lo |= 1 << UInt64(c) } else if c < 128 { hi |= 1 << UInt64(c - 64) }
    }

    mutating func insert(range: ClosedRange<UInt16>) {
        for c in range { insert(c) }
    }

    mutating func formUnion(_ other: CharSet) {
        lo |= other.lo
        hi |= other.hi
        nonAscii = nonAscii || other.nonAscii
    }

    var inverted: CharSet { CharSet(lo: ~lo, hi: ~hi, nonAscii: !nonAscii) }

    var isEmpty: Bool { lo == 0 && hi == 0 && !nonAscii }

    static let word: CharSet = {
        var set = CharSet()
        set.insert(range: 0x30...0x39)
        set.insert(range: 0x41...0x5A)
        set.insert(range: 0x61...0x7A)
        set.insert(0x5F)
        return set
    }()

    static let space: CharSet = {
        var set = CharSet()
        set.insert(range: 9...13)
        set.insert(32)
        return set
    }()

    static let digit: CharSet = {
        var set = CharSet()
        set.insert(range: 0x30...0x39)
        return set
    }()

    static let dot: CharSet = {
        var set = CharSet().inverted
        set.lo &= ~(1 << 10)
        return set
    }()
}

struct RegexMatch {
    var caps: [Int]

    var start: Int { caps[0] }
    var end: Int { caps[1] }
    var range: Range<Int> { caps[0]..<caps[1] }

    func group(_ index: Int) -> Range<Int>? {
        let a = caps[index * 2]
        let b = caps[index * 2 + 1]
        return a >= 0 && b >= 0 ? a..<b : nil
    }

    func string(_ index: Int, in text: [UInt16]) -> String {
        guard let range = group(index) else { return "" }
        return String(decoding: text[range], as: UTF16.self)
    }

    func units(_ index: Int, in text: [UInt16]) -> [UInt16] {
        guard let range = group(index) else { return [] }
        return Array(text[range])
    }
}

struct InsightRegex: @unchecked Sendable {
    fileprivate enum Op: UInt8 {
        case char, split, jmp, save, bol, eol, wordB, notWordB, star, match
    }

    fileprivate struct Inst {
        var op: Op
        var a: Int32 = 0
        var b: Int32 = 0
        var c: Int32 = 0
    }

    fileprivate struct Entry {
        var pc: Int32
        var sp: Int
        var undo: Int
        var lo: Int
    }

    private let code: UnsafePointer<Inst>
    private let sets: UnsafePointer<CharSet>
    private let multiline: Bool
    let groupCount: Int
    private let anchored: Bool
    private let lineAnchored: Bool
    private let first: CharSet?

    init(_ source: String, multiline: Bool = false, caseless: Bool = false) {
        let compiler = RegexCompiler(source, multiline: multiline, caseless: caseless)
        let program = UnsafeMutablePointer<Inst>.allocate(capacity: compiler.program.count)
        program.initialize(from: compiler.program, count: compiler.program.count)
        let table = UnsafeMutablePointer<CharSet>.allocate(capacity: max(1, compiler.sets.count))
        table.initialize(from: compiler.sets, count: compiler.sets.count)
        code = UnsafePointer(program)
        sets = UnsafePointer(table)
        self.multiline = multiline
        groupCount = compiler.groupCount
        anchored = compiler.anchored
        lineAnchored = compiler.lineAnchored
        first = compiler.first
    }
    func firstMatch(in text: [UInt16], range: Range<Int>, from: Int? = nil) -> RegexMatch? {
        text.withUnsafeBufferPointer { buffer in
            search(buffer, lower: range.lowerBound, upper: range.upperBound, from: from ?? range.lowerBound)
        }
    }

    func firstMatch(in text: [UInt16]) -> RegexMatch? {
        firstMatch(in: text, range: 0..<text.count)
    }

    func matches(_ text: [UInt16], range: Range<Int>) -> Bool {
        firstMatch(in: text, range: range) != nil
    }

    func matches(_ text: [UInt16]) -> Bool {
        firstMatch(in: text, range: 0..<text.count) != nil
    }

    func allMatches(in text: [UInt16], range: Range<Int>, limit: (() -> Bool)? = nil) -> [RegexMatch] {
        var found: [RegexMatch] = []
        text.withUnsafeBufferPointer { buffer in
            var from = range.lowerBound
            while from <= range.upperBound {
                if let limit, !limit() { break }
                guard let match = search(buffer, lower: range.lowerBound, upper: range.upperBound, from: from) else { break }
                found.append(match)
                from = match.end > match.start ? match.end : match.end + 1
            }
        }
        return found
    }

    func allMatches(in text: [UInt16]) -> [RegexMatch] {
        allMatches(in: text, range: 0..<text.count)
    }

    func forEachMatch(in text: [UInt16], range: Range<Int>, _ body: (RegexMatch) -> Bool) {
        text.withUnsafeBufferPointer { buffer in
            var from = range.lowerBound
            while from <= range.upperBound {
                guard let match = search(buffer, lower: range.lowerBound, upper: range.upperBound, from: from) else { break }
                if !body(match) { break }
                from = match.end > match.start ? match.end : match.end + 1
            }
        }
    }

    func count(in text: [UInt16], range: Range<Int>) -> Int {
        var total = 0
        forEachMatch(in: text, range: range) { _ in
            total += 1
            return true
        }
        return total
    }

    @inline(__always)
    private static func isWord(_ c: UInt16) -> Bool {
        CharSet.word.contains(c)
    }

    private func search(_ s: UnsafeBufferPointer<UInt16>, lower: Int, upper: Int, from: Int) -> RegexMatch? {
        if from > upper { return nil }
        var caps = [Int](repeating: -1, count: (groupCount + 1) * 2)
        var stack: [Entry] = []
        var undo: [Int] = []
        var start = from
        while start <= upper {
            if anchored && start != lower { return nil }
            if lineAnchored && start != lower && !(s[start - 1] == 10 && start < upper) {
                start += 1
                continue
            }
            if let first {
                if start >= upper { return nil }
                if !first.contains(s[start]) {
                    start += 1
                    continue
                }
            }
            for k in caps.indices { caps[k] = -1 }
            stack.removeAll(keepingCapacity: true)
            undo.removeAll(keepingCapacity: true)
            if run(s, lower: lower, upper: upper, start: start, caps: &caps, stack: &stack, undo: &undo) {
                return RegexMatch(caps: caps)
            }
            start += 1
        }
        return nil
    }

    private func run(_ s: UnsafeBufferPointer<UInt16>, lower: Int, upper: Int, start: Int,
                     caps: inout [Int], stack: inout [Entry], undo: inout [Int]) -> Bool {
        var pc = 0
        var sp = start
        let multiline = self.multiline
        let code = self.code
        let sets = self.sets
        do {
            do {
                while true {
                    let inst = code[pc]
                    var ok = true
                    switch inst.op {
                    case .char:
                        if sp < upper && sets[Int(inst.a)].contains(s[sp]) {
                            sp += 1
                            pc += 1
                        } else {
                            ok = false
                        }
                    case .split:
                        stack.append(Entry(pc: inst.b, sp: sp, undo: undo.count, lo: -1))
                        pc = Int(inst.a)
                    case .jmp:
                        pc = Int(inst.a)
                    case .save:
                        let slot = Int(inst.a)
                        undo.append(slot)
                        undo.append(caps[slot])
                        caps[slot] = sp
                        pc += 1
                    case .bol:
                        if sp == lower || (multiline && s[sp - 1] == 10 && sp < upper) {
                            pc += 1
                        } else {
                            ok = false
                        }
                    case .eol:
                        if sp == upper || (s[sp] == 10 && (multiline || sp == upper - 1)) {
                            pc += 1
                        } else {
                            ok = false
                        }
                    case .wordB, .notWordB:
                        let before = sp > lower && InsightRegex.isWord(s[sp - 1])
                        let after = sp < upper && InsightRegex.isWord(s[sp])
                        if (before != after) == (inst.op == .wordB) {
                            pc += 1
                        } else {
                            ok = false
                        }
                    case .star:
                        let set = sets[Int(inst.a)]
                        let minimum = Int(inst.b)
                        let limit = inst.c < 0 ? upper : min(upper, sp + Int(inst.c))
                        var p = sp
                        while p < limit && set.contains(s[p]) { p += 1 }
                        if p - sp < minimum {
                            ok = false
                        } else {
                            let lo = sp + minimum
                            if p > lo {
                                stack.append(Entry(pc: Int32(pc + 1), sp: p - 1, undo: undo.count, lo: lo))
                            }
                            sp = p
                            pc += 1
                        }
                    case .match:
                        return true
                    }
                    if ok { continue }
                    guard let entry = stack.popLast() else { return false }
                    while undo.count > entry.undo {
                        let old = undo.removeLast()
                        let slot = undo.removeLast()
                        caps[slot] = old
                    }
                    pc = Int(entry.pc)
                    sp = entry.sp
                    if entry.lo >= 0 && sp > entry.lo {
                        stack.append(Entry(pc: entry.pc, sp: sp - 1, undo: entry.undo, lo: entry.lo))
                    }
                }
            }
        }
    }
}

private final class RegexCompiler {
    fileprivate typealias Inst = InsightRegex.Inst

    private indirect enum Node {
        case set(CharSet)
        case bol
        case eol
        case wordBoundary
        case notWordBoundary
        case group(Int, Node)
        case seq([Node])
        case alt([Node])
        case rep(Node, Int, Int, Bool)
    }

    var program: [Inst] = []
    var sets: [CharSet] = []
    let multiline: Bool
    let caseless: Bool
    var groupCount = 0
    var anchored = false
    var lineAnchored = false
    var first: CharSet?

    private var pattern: [UInt8]
    private var at = 0

    init(_ source: String, multiline: Bool, caseless: Bool) {
        self.multiline = multiline
        self.caseless = caseless
        pattern = Array(source.utf8)
        let root = parseAlternation()
        precondition(at == pattern.count, "bad pattern \(source)")
        let info = firstInfo(root)
        first = info.nullable ? nil : info.set
        if case .seq(let items) = root, case .bol = items.first {
            if multiline { lineAnchored = true } else { anchored = true }
        }
        program.append(Inst(op: .save, a: 0))
        emit(root)
        program.append(Inst(op: .save, a: 1))
        program.append(Inst(op: .match))
        pattern = []
    }

    private func peek() -> UInt8? { at < pattern.count ? pattern[at] : nil }

    private func parseAlternation() -> Node {
        var options = [parseSequence()]
        while peek() == UInt8(ascii: "|") {
            at += 1
            options.append(parseSequence())
        }
        return options.count == 1 ? options[0] : .alt(options)
    }

    private func parseSequence() -> Node {
        var items: [Node] = []
        while let c = peek(), c != UInt8(ascii: "|"), c != UInt8(ascii: ")") {
            var atom = parseAtom()
            while let q = quantifier() {
                atom = .rep(atom, q.0, q.1, q.2)
            }
            items.append(atom)
        }
        return items.count == 1 ? items[0] : .seq(items)
    }

    private func readNumber() -> Int? {
        var value: Int?
        while let c = peek(), c >= 0x30, c <= 0x39 {
            value = (value ?? 0) * 10 + Int(c - 0x30)
            at += 1
        }
        return value
    }

    private func quantifier() -> (Int, Int, Bool)? {
        guard let c = peek() else { return nil }
        var minimum = 0
        var maximum = -1
        switch c {
        case UInt8(ascii: "*"):
            at += 1
        case UInt8(ascii: "+"):
            at += 1
            minimum = 1
        case UInt8(ascii: "?"):
            at += 1
            maximum = 1
        case UInt8(ascii: "{"):
            let save = at
            at += 1
            guard let low = readNumber() else {
                at = save
                return nil
            }
            minimum = low
            if peek() == UInt8(ascii: ",") {
                at += 1
                maximum = readNumber() ?? -1
            } else {
                maximum = low
            }
            guard peek() == UInt8(ascii: "}") else {
                at = save
                return nil
            }
            at += 1
        default:
            return nil
        }
        var greedy = true
        if peek() == UInt8(ascii: "?") {
            at += 1
            greedy = false
        }
        return (minimum, maximum, greedy)
    }

    private func literal(_ c: UInt8) -> CharSet {
        var set = CharSet()
        addLiteral(c, to: &set)
        return set
    }

    private func addLiteral(_ c: UInt8, to set: inout CharSet) {
        set.insert(UInt16(c))
        if caseless {
            if c >= 0x41, c <= 0x5A { set.insert(UInt16(c + 32)) }
            if c >= 0x61, c <= 0x7A { set.insert(UInt16(c - 32)) }
        }
    }

    private func escapeSet(_ c: UInt8) -> CharSet? {
        switch c {
        case UInt8(ascii: "w"): return CharSet.word
        case UInt8(ascii: "W"): return CharSet.word.inverted
        case UInt8(ascii: "s"): return CharSet.space
        case UInt8(ascii: "S"): return CharSet.space.inverted
        case UInt8(ascii: "d"): return CharSet.digit
        case UInt8(ascii: "D"): return CharSet.digit.inverted
        default: return nil
        }
    }

    private func escapeChar(_ c: UInt8) -> UInt8 {
        switch c {
        case UInt8(ascii: "n"): return 10
        case UInt8(ascii: "t"): return 9
        case UInt8(ascii: "r"): return 13
        case UInt8(ascii: "f"): return 12
        case UInt8(ascii: "v"): return 11
        default: return c
        }
    }

    private func parseAtom() -> Node {
        let c = pattern[at]
        at += 1
        switch c {
        case UInt8(ascii: "("):
            var index = -1
            if peek() == UInt8(ascii: "?") {
                at += 2
            } else {
                groupCount += 1
                index = groupCount
            }
            let body = parseAlternation()
            precondition(peek() == UInt8(ascii: ")"))
            at += 1
            return .group(index, body)
        case UInt8(ascii: "["):
            return .set(parseClass())
        case UInt8(ascii: "."):
            return .set(CharSet.dot)
        case UInt8(ascii: "^"):
            return .bol
        case UInt8(ascii: "$"):
            return .eol
        case UInt8(ascii: "\\"):
            let e = pattern[at]
            at += 1
            if e == UInt8(ascii: "b") { return .wordBoundary }
            if e == UInt8(ascii: "B") { return .notWordBoundary }
            if let set = escapeSet(e) { return .set(set) }
            return .set(literal(escapeChar(e)))
        default:
            return .set(literal(c))
        }
    }

    private func parseClass() -> CharSet {
        var set = CharSet()
        var negate = false
        if peek() == UInt8(ascii: "^") {
            negate = true
            at += 1
        }
        var firstItem = true
        while true {
            let c = pattern[at]
            if c == UInt8(ascii: "]") && !firstItem {
                at += 1
                break
            }
            firstItem = false
            at += 1
            var low: UInt8
            if c == UInt8(ascii: "\\") {
                let e = pattern[at]
                at += 1
                if let named = escapeSet(e) {
                    set.formUnion(named)
                    continue
                }
                low = escapeChar(e)
            } else {
                low = c
            }
            if peek() == UInt8(ascii: "-"), at + 1 < pattern.count, pattern[at + 1] != UInt8(ascii: "]") {
                at += 1
                var high = pattern[at]
                at += 1
                if high == UInt8(ascii: "\\") {
                    high = escapeChar(pattern[at])
                    at += 1
                }
                for value in low...high { addLiteral(value, to: &set) }
            } else {
                addLiteral(low, to: &set)
            }
        }
        return negate ? set.inverted : set
    }

    private func firstInfo(_ node: Node) -> (set: CharSet, nullable: Bool) {
        switch node {
        case .set(let set):
            return (set, false)
        case .bol, .eol, .wordBoundary, .notWordBoundary:
            return (CharSet(), true)
        case .group(_, let body):
            return firstInfo(body)
        case .seq(let items):
            var set = CharSet()
            for item in items {
                let info = firstInfo(item)
                set.formUnion(info.set)
                if !info.nullable { return (set, false) }
            }
            return (set, true)
        case .alt(let options):
            var set = CharSet()
            var nullable = false
            for option in options {
                let info = firstInfo(option)
                set.formUnion(info.set)
                nullable = nullable || info.nullable
            }
            return (set, nullable)
        case .rep(let body, let minimum, _, _):
            let info = firstInfo(body)
            return (info.set, info.nullable || minimum == 0)
        }
    }

    @discardableResult
    private func add(_ inst: Inst) -> Int {
        program.append(inst)
        return program.count - 1
    }

    private func setIndex(_ set: CharSet) -> Int32 {
        sets.append(set)
        return Int32(sets.count - 1)
    }

    private func emit(_ node: Node) {
        switch node {
        case .set(let set):
            add(Inst(op: .char, a: setIndex(set)))
        case .bol:
            add(Inst(op: .bol))
        case .eol:
            add(Inst(op: .eol))
        case .wordBoundary:
            add(Inst(op: .wordB))
        case .notWordBoundary:
            add(Inst(op: .notWordB))
        case .group(let index, let body):
            if index >= 0 { add(Inst(op: .save, a: Int32(index * 2))) }
            emit(body)
            if index >= 0 { add(Inst(op: .save, a: Int32(index * 2 + 1))) }
        case .seq(let items):
            for item in items { emit(item) }
        case .alt(let options):
            var jumps: [Int] = []
            for option in options.dropLast() {
                let split = add(Inst(op: .split))
                program[split].a = Int32(split + 1)
                emit(option)
                jumps.append(add(Inst(op: .jmp)))
                program[split].b = Int32(program.count)
            }
            emit(options[options.count - 1])
            for jump in jumps { program[jump].a = Int32(program.count) }
        case .rep(let body, let minimum, let maximum, let greedy):
            if case .set(let set) = body, greedy {
                add(Inst(op: .star, a: setIndex(set), b: Int32(minimum), c: Int32(maximum)))
                return
            }
            for _ in 0..<minimum { emit(body) }
            if maximum < 0 {
                let split = add(Inst(op: .split))
                emit(body)
                add(Inst(op: .jmp, a: Int32(split)))
                let out = Int32(program.count)
                if greedy {
                    program[split].a = Int32(split + 1)
                    program[split].b = out
                } else {
                    program[split].a = out
                    program[split].b = Int32(split + 1)
                }
            } else if maximum > minimum {
                var splits: [Int] = []
                for _ in minimum..<maximum {
                    splits.append(add(Inst(op: .split)))
                    emit(body)
                }
                let out = Int32(program.count)
                for split in splits {
                    if greedy {
                        program[split].a = Int32(split + 1)
                        program[split].b = out
                    } else {
                        program[split].a = out
                        program[split].b = Int32(split + 1)
                    }
                }
            }
        }
    }

}
