import Foundation

extension Strata {
    enum RuleSlot: Int {
        case moduleCycles = 0, fileCycles, stableDependencies, zoneOfPain, zoneOfUselessness, hubFiles, tooManyImports,
             complexFunctions, longFunctions, deepNesting, tooManyParameters, brainMethods, largeFiles, duplicatedCode,
             uncommentedComplexity, knowledgeIslands, formerContributors
    }

    static func capped(_ value: Double, _ cap: Double) -> Double { max(0, min(cap, value)) }

    static func clamp(_ value: Double, _ low: Double, _ high: Double) -> Double { max(low, min(high, value)) }

    public static func ruleCatalog() -> [StrataRule] {
        var rules: [StrataRule] = []
        func add(_ id: String, _ title: String, _ category: StrataCategory, _ severity: StrataSeverity, _ moduleScope: Bool, _ description: String, _ fix: String) {
            var rule = StrataRule()
            rule.id = id
            rule.title = title
            rule.category = category
            rule.severity = severity
            rule.moduleScope = moduleScope
            rule.description = description
            rule.howToFix = fix
            rules.append(rule)
        }
        add("ARC01", "Avoid module dependency cycles", .architecture, .critical, true,
            "Modules that depend on each other, directly or through other modules, can only be understood, tested, built and released together. A cycle turns separate layers into one big tangle and every change risks rippling around the loop.",
            "Find the weakest edge of the cycle (fewest imports, shown in red) and remove it: move the shared code down into a lower module, or invert the dependency with an interface or callback owned by the lower module.")
        add("ARC02", "Avoid file import cycles", .architecture, .high, false,
            "Files that import each other in a loop cannot be read, reused or tested on their own and make initialization order fragile.",
            "Extract the definitions both files need into a third file that imports neither, or pass the dependency in instead of importing it.")
        add("ARC03", "Depend in the direction of stability", .architecture, .medium, true,
            "Stable dependencies principle: a module should only depend on modules that are more stable than itself. When a stable module (many dependents, few dependencies) depends on a clearly less stable one, changes in the volatile module ripple into everything that relies on the stable one.",
            "Move the code the stable module needs into it or into a lower module, or let the stable module define an abstraction that the volatile module implements.")
        add("ARC04", "Modules in the zone of pain", .architecture, .medium, true,
            "A module that is concrete (few abstract types) and stable (used by many, depends on few) is hard to change because every dependent breaks, yet offers no abstractions to extend it. It sits far below the main sequence in the abstractness × instability diagram.",
            "Introduce interfaces or abstract types that dependents use instead of the concrete code, or split the module so that fewer modules depend on the concrete parts.")
        add("ARC05", "Modules in the zone of uselessness", .architecture, .low, true,
            "A module that is mostly abstract yet has no dependents is abstraction nobody uses: interfaces without callers, speculative extension points.",
            "Remove the unused abstractions, or merge them into the module that implements them.")
        add("DES01", "Avoid hub files", .design, .medium, false,
            "A file that many files import and that itself imports many files is a hub: every change to its dependencies can break its many dependents. Hubs are usually god objects or grab-bag utility files.",
            "Split the file by responsibility so that each dependent imports only the part it needs.")
        add("DES02", "Avoid too many imports in one file", .design, .low, false,
            "A file with a very long import list knows too much about the rest of the system and is likely doing several jobs.",
            "Split the file along its responsibilities, or group related imports behind a facade.")
        add("CS01", "Avoid complex functions", .codeSmells, .high, false,
            "Cyclomatic complexity counts the independent paths through a function. Above 10 a function becomes hard to test fully; above 20 it is hard to understand at all.",
            "Extract the branches into well-named helper functions, replace conditionals with lookup tables or polymorphism, and return early instead of nesting.")
        add("CS02", "Avoid long functions", .codeSmells, .medium, false,
            "Functions longer than 60 lines rarely do one thing; above 100 lines they are hard to name, read and reuse.",
            "Extract cohesive blocks into functions whose names say what the block does.")
        add("CS03", "Avoid deep nesting", .codeSmells, .medium, false,
            "Code nested more than five levels deep forces the reader to hold every enclosing condition in mind.",
            "Return early, invert conditions, and extract inner loops into their own functions.")
        add("CS04", "Avoid functions with too many parameters", .codeSmells, .low, false,
            "More than six parameters make calls hard to read and easy to get wrong.",
            "Group related parameters into a struct or options object, or split the function.")
        add("CS05", "Avoid brain methods", .codeSmells, .high, false,
            "A brain method is long, complex and deeply nested at the same time: it centralizes the logic of its file and is where most bugs hide.",
            "Break it up step by step: extract each nested block into a named function, then simplify the remaining control flow.")
        add("MNT01", "Avoid large files", .maintainability, .medium, false,
            "Files with more than 1,000 lines of code are hard to navigate and attract merge conflicts; above 2,000 lines they usually hold several unrelated concerns.",
            "Split the file by responsibility, one concept per file.")
        add("MNT02", "Avoid duplicated code", .maintainability, .medium, false,
            "Copied blocks must be fixed in every copy; a bug fixed in one copy lives on in the others.",
            "Extract the repeated block into a shared function and call it from each place.")
        add("MNT03", "Comment complex files", .maintainability, .low, false,
            "A complex file with almost no comments or documentation leaves its reasoning in its author's head.",
            "Document why the code is shaped the way it is, near the code or in the project's docs.")
        add("MNT04", "Avoid knowledge islands", .maintainability, .medium, false,
            "One person wrote more than 90 % of a file that changes often. When they are away, nobody else knows the code.",
            "Pair on the next changes, review them with a second person, and write down how the file works.")
        add("MNT05", "Hotspots written by former contributors", .maintainability, .medium, false,
            "Most of this frequently changed code was written by people who have not committed in the last six months. The knowledge behind it has left with them.",
            "Give the file a current owner, cover it with tests before changing it, and document what you learn.")
        return rules
    }

    public static func components(_ adjacency: [[Int]]) -> [[Int]] {
        let n = adjacency.count
        var index = [Int](repeating: -1, count: n)
        var low = [Int](repeating: 0, count: n)
        var onStack = [Bool](repeating: false, count: n)
        var stack: [Int] = []
        var calls: [(Int, Int)] = []
        var out: [[Int]] = []
        var counter = 0
        for start in 0..<n where index[start] < 0 {
            index[start] = counter
            low[start] = counter
            counter += 1
            stack.append(start)
            onStack[start] = true
            calls.append((start, 0))
            while !calls.isEmpty {
                let v = calls[calls.count - 1].0
                let next = calls[calls.count - 1].1
                if next < adjacency[v].count {
                    calls[calls.count - 1].1 += 1
                    let w = adjacency[v][next]
                    if w < 0 || w >= n { continue }
                    if index[w] < 0 {
                        index[w] = counter
                        low[w] = counter
                        counter += 1
                        stack.append(w)
                        onStack[w] = true
                        calls.append((w, 0))
                    } else if onStack[w] {
                        low[v] = min(low[v], index[w])
                    }
                    continue
                }
                calls.removeLast()
                if let parent = calls.last?.0 {
                    low[parent] = min(low[parent], low[v])
                }
                if low[v] == index[v] {
                    var component: [Int] = []
                    var w = -1
                    repeat {
                        w = stack.removeLast()
                        onStack[w] = false
                        component.append(w)
                    } while w != v
                    component.sort()
                    out.append(component)
                }
            }
        }
        return out
    }

    public static func normalizeGraph(_ graph: inout InsightModuleGraph) {
        let n = graph.nodes.count
        func valid(_ edge: InsightModuleEdge) -> Bool {
            edge.from >= 0 && edge.from < n && edge.to >= 0 && edge.to < n
        }
        var adjacency = [[Int]](repeating: [], count: n)
        for edge in graph.edges where valid(edge) && edge.from != edge.to {
            adjacency[edge.from].append(edge.to)
        }
        let parts = components(adjacency)
        graph.cycles = []
        for i in graph.nodes.indices { graph.nodes[i].cycle = -1 }
        for part in parts where part.count >= 2 {
            for v in part { graph.nodes[v].cycle = graph.cycles.count }
            graph.cycles.append(part)
        }
        for e in graph.edges.indices {
            let edge = graph.edges[e]
            graph.edges[e].inCycle = valid(edge) && graph.nodes[edge.from].cycle >= 0 && graph.nodes[edge.from].cycle == graph.nodes[edge.to].cycle
        }
        let coupled = graph.nodes.contains { $0.afferent > 0 || $0.efferent > 0 }
        if !coupled && !graph.edges.isEmpty {
            var incoming = [Set<Int>](repeating: [], count: n)
            var outgoing = [Set<Int>](repeating: [], count: n)
            for edge in graph.edges where valid(edge) && edge.from != edge.to {
                outgoing[edge.from].insert(edge.to)
                incoming[edge.to].insert(edge.from)
            }
            for i in 0..<n {
                var node = graph.nodes[i]
                node.afferent = incoming[i].count
                node.efferent = outgoing[i].count
                let sum = node.afferent + node.efferent
                node.instability = sum > 0 ? Double(node.efferent) / Double(sum) : 0
                node.abstractness = node.types > 0 ? Double(node.abstractTypes) / Double(node.types) : 0
                node.distance = abs(node.abstractness + node.instability - 1.0)
                graph.nodes[i] = node
            }
        }
    }

    public static func evaluateRules(_ snapshot: InsightSnapshot, _ graph: InsightModuleGraph) -> [StrataRule] {
        var rules = ruleCatalog()
        let fileCount = snapshot.files.count
        func moduleOf(_ file: Int) -> Int {
            file >= 0 && file < graph.moduleOfFile.count ? graph.moduleOfFile[file] : -1
        }
        func add(_ slot: RuleSlot, _ violation: StrataViolation) {
            var v = violation
            v.debt = v.debt.rounded()
            rules[slot.rawValue].violations.append(v)
        }

        for edge in graph.edges where edge.inCycle && edge.from >= 0 && edge.to >= 0 {
            let from = graph.nodes[edge.from]
            let to = graph.nodes[edge.to]
            var v = StrataViolation()
            v.module = edge.from
            v.peerModule = edge.to
            v.file = edge.fileEdges.first?.from ?? -1
            v.severity = .critical
            v.message = "\(from.name) uses \(to.name) (\(edge.weight) imports) and \(to.name) depends back on \(from.name)"
            v.debt = capped(30 + 15.0 * Double(max(1, edge.weight)), 480)
            add(.moduleCycles, v)
        }

        let parts = components(snapshot.files.map(\.dependsOn))
        for part in parts where part.count >= 2 {
            let members = Set(part)
            for f in part {
                var names: [String] = []
                var inside = 0
                for d in snapshot.files[f].dependsOn where members.contains(d) {
                    inside += 1
                    if names.count < 3 { names.append(fileName(snapshot.files[d].path)) }
                }
                var v = StrataViolation()
                v.file = f
                v.module = moduleOf(f)
                v.severity = .high
                v.message = "Part of an import cycle of \(part.count) files; imports " + names.joined(separator: ", ") + (inside > names.count ? " …" : "")
                v.debt = capped(15.0 * Double(inside) + 5.0 * Double(min(part.count, 20)), 240)
                add(.fileCycles, v)
            }
        }

        for edge in graph.edges where !edge.inCycle && edge.from >= 0 && edge.to >= 0 {
            let from = graph.nodes[edge.from]
            let to = graph.nodes[edge.to]
            let gap = to.instability - from.instability
            if gap <= 0.25 { continue }
            var v = StrataViolation()
            v.module = edge.from
            v.peerModule = edge.to
            v.file = edge.fileEdges.first?.from ?? -1
            v.severity = .medium
            v.message = "\(from.name) (I = \(fixed(from.instability, 2))) depends on the less stable \(to.name) (I = \(fixed(to.instability, 2)))"
            v.debt = capped(20 + 120.0 * (gap - 0.25) + 4.0 * Double(edge.weight), 240)
            add(.stableDependencies, v)
        }

        for (i, node) in graph.nodes.enumerated() where node.types >= 1 {
            if node.codeLines >= 150 && node.afferent >= 2 && node.abstractness <= 0.2 && node.instability <= 0.3 {
                let over = clamp((node.distance - 0.5) / 0.5, 0, 1)
                var v = StrataViolation()
                v.module = i
                v.severity = .medium
                v.message = "Concrete and stable: A = \(fixed(node.abstractness, 2)), I = \(fixed(node.instability, 2)), used by \(node.afferent) modules"
                v.debt = capped(30 + 150.0 * over * min(3.0, (Double(node.codeLines) / 1000.0).squareRoot()) + 5.0 * Double(node.afferent), 960)
                add(.zoneOfPain, v)
            }
            if node.codeLines >= 50 && node.abstractness >= 0.7 && node.instability >= 0.7 {
                var v = StrataViolation()
                v.module = i
                v.severity = .low
                v.message = "Abstract and unused: A = \(fixed(node.abstractness, 2)), I = \(fixed(node.instability, 2))"
                v.debt = capped(20 + 100.0 * clamp((node.distance - 0.4) / 0.6, 0, 1), 240)
                add(.zoneOfUselessness, v)
            }
        }

        let changed = snapshot.files.map(\.commits).filter { $0 > 0 }.sorted()
        let frequent = max(8, changed.isEmpty ? 0 : changed[Int(Double(changed.count) * 0.75)])
        let team = snapshot.authors.count >= 2

        for f in 0..<fileCount {
            let file = snapshot.files[f]
            let module = moduleOf(f)
            func fileViolation(_ severity: StrataSeverity, _ line: Int, _ message: String, _ debt: Double) -> StrataViolation {
                StrataViolation(file: f, module: module, line: line, severity: severity, message: message, debt: debt)
            }
            let fanIn = file.dependedOnBy.count
            let fanOut = file.dependsOn.count
            if fanIn >= 10 && fanOut >= 10 {
                add(.hubFiles, fileViolation(fanIn >= 25 && fanOut >= 25 ? .high : .medium, 0,
                                             "Imported by \(fanIn) files and imports \(fanOut)",
                                             capped(30 + 3.0 * Double((fanIn - 10) + (fanOut - 10)), 480)))
            }
            let imports = max(file.imports.count, fanOut + file.externalImports.count)
            if imports > 25 {
                add(.tooManyImports, fileViolation(imports > 50 ? .medium : .low, 1, "\(imports) imports",
                                                   capped(10 + 2.0 * Double(imports - 25), 240)))
            }
            for fn in file.functions {
                let name = fn.name.isEmpty ? "(anonymous)" : fn.name
                if fn.complexity > 10 {
                    add(.complexFunctions, fileViolation(fn.complexity > 20 ? .high : .medium, fn.line,
                                                         "\(name) has cyclomatic complexity \(fn.complexity)",
                                                         capped(15 + 6.0 * Double(fn.complexity - 10), 480)))
                }
                if fn.length > 60 {
                    add(.longFunctions, fileViolation(fn.length > 100 ? .medium : .low, fn.line,
                                                      "\(name) is \(fn.length) lines long",
                                                      capped(10 + 0.6 * Double(fn.length - 60), 480)))
                }
                if fn.nesting > 5 {
                    add(.deepNesting, fileViolation(fn.nesting > 7 ? .high : .medium, fn.line,
                                                    "\(name) nests \(fn.nesting) levels deep",
                                                    capped(20 + 15.0 * Double(fn.nesting - 5), 240)))
                }
                if fn.parameters > 6 {
                    add(.tooManyParameters, fileViolation(fn.parameters > 9 ? .medium : .low, fn.line,
                                                          "\(name) takes \(fn.parameters) parameters",
                                                          capped(10 + 6.0 * Double(fn.parameters - 6), 120)))
                }
                if fn.length > 80 && fn.complexity > 15 && fn.nesting >= 4 {
                    let debt = 60 + 0.5 * Double(fn.length - 80) + 5.0 * Double(fn.complexity - 15) + 15.0 * Double(fn.nesting - 4)
                    add(.brainMethods, fileViolation(.high, fn.line,
                                                     "\(name): \(fn.length) lines, complexity \(fn.complexity), nesting \(fn.nesting)",
                                                     capped(debt, 960)))
                }
            }
            if file.codeLines > 1000 {
                add(.largeFiles, fileViolation(file.codeLines > 2000 ? .high : .medium, 1,
                                               "\(grouped(file.codeLines)) lines of code",
                                               capped(30 + 0.06 * Double(file.codeLines - 1000), 960)))
            }
            if file.duplicatedLines >= 12 {
                add(.duplicatedCode, fileViolation(file.duplicatedLines >= 100 ? .medium : .low, 1,
                                                   "\(file.duplicatedLines) duplicated lines",
                                                   capped(10 + 1.5 * Double(file.duplicatedLines), 960)))
            }
            let commented = file.codeLines + file.commentLines
            if !file.test && file.complexity >= 40 && file.codeLines >= 150 && commented > 0
                && Double(file.commentLines) / Double(commented) < 0.05 {
                add(.uncommentedComplexity, fileViolation(.low, 1,
                                                          "Complexity \(file.complexity) with \(file.commentLines) comment lines",
                                                          capped(15 + 0.25 * Double(file.complexity - 40), 240)))
            }
            if team && file.commits >= frequent && file.mainAuthorShare > 0.9 && !file.mainAuthor.isEmpty {
                add(.knowledgeIslands, fileViolation(.medium, 0,
                                                     "\(file.mainAuthor) wrote \(formatPercent(file.mainAuthorShare)) of \(file.commits) commits' lines",
                                                     capped(30 + 4.0 * Double(file.commits - frequent) + 0.02 * Double(file.codeLines), 480)))
            }
            if file.commits > 0 && file.formerAuthorShare > 0.5 && file.hotspot >= 0.3 {
                let severe = file.formerAuthorShare > 0.8 && file.hotspot > 0.6
                add(.formerContributors, fileViolation(severe ? .high : .medium, 0,
                                                       "\(formatPercent(file.formerAuthorShare)) written by former contributors, hotspot \(fixed(file.hotspot, 2))",
                                                       capped(20 + 200.0 * file.hotspot * file.formerAuthorShare, 480)))
            }
        }

        for r in rules.indices {
            rules[r].debt = rules[r].violations.reduce(0) { $0 + $1.debt }
            rules[r].violations = stableSorted(rules[r].violations) { a, b in
                if a.severity != b.severity { return a.severity < b.severity }
                return a.debt > b.debt
            }
        }
        return rules
    }

    public static func evaluateGates(_ snapshot: InsightSnapshot, _ report: StrataReport) -> [StrataGate] {
        var gates: [StrataGate] = []
        do {
            var gate = StrataGate()
            gate.name = "Technical debt rating"
            gate.status = report.total.rating >= 4 ? .fail : report.total.rating == 3 ? .warn : .pass
            gate.measured = "\(ratingLetter(report.total.rating)) · \(formatPercent(report.total.ratio))"
            gate.detail = "Warns at D (debt above 20 % of the effort), fails at E (above 50 %)."
            gates.append(gate)
        }
        do {
            var critical = 0
            var ruleCount = 0
            for rule in report.rules {
                let here = rule.violations.filter { $0.severity == .critical }.count
                critical += here
                ruleCount += here > 0 ? 1 : 0
            }
            var gate = StrataGate()
            gate.name = "No critical rule violated"
            gate.status = critical > 0 ? .fail : .pass
            gate.measured = critical > 0 ? "\(critical) in \(ruleCount) \(ruleCount == 1 ? "rule" : "rules")" : "None"
            gate.detail = "Fails when any violation has critical severity."
            gates.append(gate)
        }
        do {
            var gate = StrataGate()
            gate.name = "No module dependency cycle"
            let cycles = report.graph.cycles.count
            gate.status = cycles > 0 ? .fail : .pass
            gate.measured = cycles > 0 ? "\(cycles) \(cycles == 1 ? "cycle" : "cycles")" : "None"
            gate.detail = "Fails when modules depend on each other in a loop."
            gates.append(gate)
        }
        do {
            var gate = StrataGate()
            gate.name = "No function above complexity 50"
            let worst = report.metrics.maxComplexity
            gate.status = worst > 50 ? .fail : worst > 30 ? .warn : .pass
            gate.measured = report.metrics.maxComplexityFunction.isEmpty ? "\(worst)" : "\(worst) (\(report.metrics.maxComplexityFunction))"
            gate.detail = "Warns above 30, fails above 50."
            gates.append(gate)
        }
        do {
            var gate = StrataGate()
            gate.name = "Debt in the most-changed files"
            var changed = snapshot.files.indices.filter { snapshot.files[$0].commits > 0 }
            if changed.isEmpty {
                gate.status = .pass
                gate.measured = "No history"
                gate.detail = "Needs git history: the debt ratio of the 10 % most-changed files must stay below 20 %."
            } else {
                changed = stableSorted(changed) { snapshot.files[$0].commits > snapshot.files[$1].commits }
                let take = max(1, Int((Double(changed.count) * 0.1).rounded(.up)))
                var debt = 0.0
                var effort = 0.0
                for i in 0..<take {
                    let f = changed[i]
                    if f < report.files.count {
                        debt += report.files[f].debt
                        effort += report.files[f].effort
                    }
                }
                let ratio = effort > 0 ? debt / effort : 0
                gate.status = ratio >= 0.2 ? .fail : ratio >= 0.1 ? .warn : .pass
                gate.measured = "\(formatPercent(ratio)) in \(take) files"
                gate.detail = "Debt ratio of the 10 % most-changed files: warns at 10 %, fails at 20 %."
            }
            gates.append(gate)
        }
        return gates
    }

    public static func buildReport(_ snapshot: InsightSnapshot, _ graph: InsightModuleGraph) -> StrataReport {
        var report = StrataReport()
        report.graph = graph
        normalizeGraph(&report.graph)
        let fileCount = snapshot.files.count
        let moduleCount = report.graph.nodes.count
        if report.graph.moduleOfFile.count != fileCount {
            report.graph.moduleOfFile = [Int](repeating: -1, count: fileCount)
            for m in 0..<moduleCount {
                for f in report.graph.nodes[m].files where f >= 0 && f < fileCount {
                    report.graph.moduleOfFile[f] = m
                }
            }
        }
        report.rules = evaluateRules(snapshot, report.graph)
        report.files = [StrataScore](repeating: StrataScore(), count: fileCount)
        report.modules = [StrataScore](repeating: StrataScore(), count: moduleCount)
        report.fileViolations = [[StrataViolationRef]](repeating: [], count: fileCount)
        report.moduleViolations = [[StrataViolationRef]](repeating: [], count: moduleCount)

        let years = snapshot.historyDays > 0 ? Double(snapshot.historyDays) / 365.0 : 1.0
        report.history = snapshot.files.contains { $0.commits > 0 }
        func interestFor(_ debt: Double, _ commits: Int) -> Double {
            if commits <= 0 || debt <= 0 { return 0 }
            let perYear = Double(commits) / max(0.1, years)
            return min(2.0 * debt, debt * 0.05 * perYear)
        }
        func mark(_ score: inout StrataScore, _ v: StrataViolation) {
            score.debt += v.debt
            score.violations += 1
            let s = v.severity.rawValue
            if score.worst < 0 || s < score.worst { score.worst = s }
        }

        for r in report.rules.indices {
            let rule = report.rules[r]
            for (i, v) in rule.violations.enumerated() {
                report.bySeverity[v.severity.rawValue] += 1
                if rule.moduleScope || v.file < 0 {
                    if v.module >= 0 && v.module < moduleCount {
                        report.moduleViolations[v.module].append(StrataViolationRef(rule: r, index: i))
                        mark(&report.modules[v.module], v)
                    }
                } else if v.file < fileCount {
                    report.fileViolations[v.file].append(StrataViolationRef(rule: r, index: i))
                    mark(&report.files[v.file], v)
                }
                report.total.debt += v.debt
                report.total.violations += 1
                let s = v.severity.rawValue
                if report.total.worst < 0 || s < report.total.worst { report.total.worst = s }
            }
        }

        var m = StrataMetrics()
        var complexitySum = 0.0
        var duplicated = 0
        for f in 0..<fileCount {
            let file = snapshot.files[f]
            var score = report.files[f]
            score.effort = effortMinutes(file.codeLines)
            score.ratio = score.effort > 0 ? score.debt / score.effort : (score.debt > 0 ? 1.0 : 0.0)
            score.rating = ratingFor(score.ratio)
            score.interest = interestFor(score.debt, file.commits)
            report.files[f] = score
            report.total.interest += score.interest
            m.codeLines += file.codeLines
            m.commentLines += file.commentLines
            m.types += file.types
            m.functions += file.functions.count
            duplicated += file.duplicatedLines
            if file.test { m.testFiles += 1 }
            for fn in file.functions {
                complexitySum += Double(fn.complexity)
                if fn.complexity > m.maxComplexity {
                    m.maxComplexity = fn.complexity
                    m.maxComplexityFunction = fn.name
                    m.maxComplexityFile = f
                }
            }
        }
        for i in 0..<moduleCount {
            let node = report.graph.nodes[i]
            var score = report.modules[i]
            let ownDebt = score.debt
            var codeLines = 0
            var commits = 0
            for f in node.files where f >= 0 && f < fileCount {
                let fileScore = report.files[f]
                score.debt += fileScore.debt
                score.violations += fileScore.violations
                score.interest += fileScore.interest
                if fileScore.worst >= 0 && (score.worst < 0 || fileScore.worst < score.worst) { score.worst = fileScore.worst }
                codeLines += snapshot.files[f].codeLines
                commits = max(commits, snapshot.files[f].commits)
            }
            if node.codeLines > 0 { codeLines = node.codeLines }
            let ownInterest = interestFor(ownDebt, node.commits > 0 ? node.commits : commits)
            score.interest += ownInterest
            report.total.interest += ownInterest
            score.effort = effortMinutes(codeLines)
            score.ratio = score.effort > 0 ? score.debt / score.effort : (score.debt > 0 ? 1.0 : 0.0)
            score.rating = ratingFor(score.ratio)
            report.modules[i] = score
        }
        m.files = fileCount
        m.modules = moduleCount
        m.averageComplexity = m.functions > 0 ? complexitySum / Double(m.functions) : 0
        m.commentPercent = (m.codeLines + m.commentLines) > 0 ? 100.0 * Double(m.commentLines) / Double(m.codeLines + m.commentLines) : 0
        m.duplicatedPercent = m.codeLines > 0 ? min(100.0, 100.0 * Double(duplicated) / Double(m.codeLines)) : 0
        report.metrics = m
        report.total.effort = effortMinutes(m.codeLines)
        report.total.ratio = report.total.effort > 0 ? report.total.debt / report.total.effort : 0
        report.total.rating = ratingFor(report.total.ratio)
        report.gates = evaluateGates(snapshot, report)
        return report
    }

    public static func summaryLine(_ report: StrataReport) -> String {
        var parts: [String] = []
        parts.append("Rating \(ratingLetter(report.total.rating))")
        parts.append("\(formatDebt(report.total.debt)) debt")
        let failing = report.gatesWith(.fail)
        let warning = report.gatesWith(.warn)
        if failing > 0 {
            parts.append("\(failing) \(failing == 1 ? "gate" : "gates") failing")
        } else if warning > 0 {
            parts.append("\(warning) \(warning == 1 ? "gate" : "gates") warning")
        } else {
            parts.append("all gates pass")
        }
        return parts.joined(separator: " · ")
    }
}
