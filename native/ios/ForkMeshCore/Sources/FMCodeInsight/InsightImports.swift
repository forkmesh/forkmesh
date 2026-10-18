import Foundation

enum Imports {
    static func firstWord(_ s: Units) -> Units {
        let t = U.trimmed(s)
        var end = 0
        while end < t.count && !CharSet.space.contains(t[end]) { end += 1 }
        return Array(t[0..<end])
    }

    static func fromStripped(_ language: String, _ text: Units) -> [String] {
        var specs: [String] = []
        let all = 0..<text.count
        func collect(_ re: InsightRegex, _ source: Units, _ range: Range<Int>, _ group: Int) {
            re.forEachMatch(in: source, range: range) { m in
                if let r = m.group(group) {
                    let value = U.trim(source, r)
                    if !value.isEmpty { specs.append(U.string(source, value)) }
                }
                return true
            }
        }
        switch language {
        case "C", "C++", "Objective-C":
            P.cInclude.forEachMatch(in: text, range: all) { m in
                let path = U.string(text, U.trim(text, m.group(2) ?? 0..<0))
                specs.append(m.string(1, in: text) == "<" ? "<" + path + ">" : path)
                return true
            }
            if language == "Objective-C" {
                P.objcModule.forEachMatch(in: text, range: all) { m in
                    specs.append("<" + m.string(1, in: text) + ">")
                    return true
                }
            }
        case "JavaScript", "TypeScript", "Vue", "Svelte":
            collect(P.jsImport, text, all, 1)
            collect(P.jsExport, text, all, 1)
            collect(P.jsRequire, text, all, 1)
        case "Python":
            P.pyImport.forEachMatch(in: text, range: all) { m in
                for part in U.split(m.units(1, in: text), U.comma) {
                    let module = firstWord(part)
                    if !module.isEmpty { specs.append(U.string(module)) }
                }
                return true
            }
            P.pyFrom.forEachMatch(in: text, range: all) { m in
                let module = m.string(1, in: text)
                specs.append(module)
                let names = m.units(2, in: text).filter { $0 != U.openParen && $0 != U.closeParen && $0 != U.backslash }
                for raw in U.split(names, U.comma) {
                    let name = U.string(firstWord(raw))
                    if name.isEmpty || name == "*" { continue }
                    let joined = module.hasSuffix(".") ? module + name : module + "." + name
                    specs.append("?" + joined)
                }
                return true
            }
        case "Go":
            P.goBlock.forEachMatch(in: text, range: all) { m in
                let inner = m.units(1, in: text)
                collect(P.quoted, inner, 0..<inner.count, 1)
                return true
            }
            collect(P.goSingle, text, all, 1)
        case "Rust":
            P.rustMod.forEachMatch(in: text, range: all) { m in
                specs.append("mod " + m.string(1, in: text))
                return true
            }
            collect(P.rustUse, text, all, 1)
        case "Java", "Kotlin", "Scala", "Groovy":
            P.jvmImport.forEachMatch(in: text, range: all) { m in
                let star = m.group(2).map { !$0.isEmpty } ?? false
                specs.append(m.string(1, in: text) + (star ? ".*" : ""))
                return true
            }
        case "C#":
            collect(P.csUsing, text, all, 1)
        case "Swift":
            collect(P.swiftImport, text, all, 1)
        case "Ruby":
            P.rubyRequire.forEachMatch(in: text, range: all) { m in
                let relative = m.group(1).map { !$0.isEmpty } ?? false
                specs.append(relative ? "./" + m.string(2, in: text) : m.string(2, in: text))
                return true
            }
        case "Elixir":
            collect(P.elixirAlias, text, all, 1)
        case "PHP":
            collect(P.phpInclude, text, all, 1)
            collect(P.phpUse, text, all, 1)
        case "Dart":
            collect(P.dartImport, text, all, 1)
        case "Lua":
            collect(P.luaRequire, text, all, 1)
        case "CSS", "SCSS", "Less":
            collect(P.cssImport, text, all, 1)
        case "QML":
            P.qmlImport.forEachMatch(in: text, range: all) { m in
                var value = m.units(1, in: text)
                if value.first == U.quote {
                    value = U.units("./") + Array(value[1..<max(1, value.count - 1)])
                }
                specs.append(U.string(value))
                return true
            }
        case "Shell":
            collect(P.shellSource, text, all, 1)
        default:
            break
        }
        var unique: [String] = []
        var seen = Set<String>()
        for spec in specs where seen.insert(spec).inserted {
            unique.append(spec)
        }
        return unique
    }
}
