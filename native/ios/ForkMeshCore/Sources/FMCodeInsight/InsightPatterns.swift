import Foundation

enum P {
    static let controlStart = InsightRegex(
        "^(if|else|for|foreach|while|do|switch|try|catch|finally|synchronized|using|lock|unsafe|"
            + "checked|unchecked|fixed|with|when|loop|match|select|defer|go|return|case|guard|"
            + "repeat|until|unless|elif|then|throw|new|await|yield|get|set|willSet|didSet)\\b")
    static let typeHeader = InsightRegex(
        "\\b(class|struct|interface|enum|namespace|union|trait|impl|protocol|extension|record|"
            + "object|module|package|actor)\\b")
    static let functionKeyword = InsightRegex("\\b(function|func|fn|fun|def|sub)\\b")
    static let computedProperty = InsightRegex("\\bvar\\s+([A-Za-z_]\\w*)\\s*:[^=]*$")
    static let callName = InsightRegex(
        "([A-Za-z_$~][\\w$]*(?:(?:::|\\.)~?[A-Za-z_$][\\w$]*)*|operator\\s*[^\\s(]+)\\s*(?:<[^()]*>)?\\s*\\(")
    static let arrowName = InsightRegex(
        "([A-Za-z_$][\\w$]*)\\s*[:=]\\s*(?:async\\s*)?(?:\\([^)]*\\)|[A-Za-z_$][\\w$]*)\\s*(?::[^=]*)?=>")
    static let tailAfterParen = InsightRegex("^[\\s\\w:&*<>,\\[\\].?!\\-|@]*$")
    static let decisionsBase = InsightRegex(
        "\\b(if|for|foreach|while|case|catch|guard|when|except|elif|elsif|elseif|unless|until|rescue)\\b|&&|\\|\\|")
    static let ternaryStrict = InsightRegex("\\s\\?\\s")
    static let rustArms = InsightRegex("=>")
    static let pythonDecisions = InsightRegex("\\b(if|elif|for|while|except|and|or|case)\\b")
    static let keywordDecisions = InsightRegex(
        "\\b(if|elsif|elseif|unless|while|until|for|when|rescue|and|or|cond|case)\\b|&&|\\|\\|")
    static let pythonDef = InsightRegex("^(?:async\\s+)?def\\s+([A-Za-z_]\\w*)\\s*\\(")
    static let pythonClass = InsightRegex("^class\\s+([A-Za-z_]\\w*)")
    static let pythonBlock = InsightRegex(
        "^(if|elif|else|for|while|try|except|finally|with|match|case|async\\s+for|async\\s+with)\\b.*:$")
    static let keywordDef = InsightRegex(
        "^\\s*(?:local\\s+)?(?:def|defp|defmacro|function)\\s+([A-Za-z_][\\w.:?!]*)")
    static let keywordOpen = InsightRegex(
        "\\b(if|unless|while|until|for|case|begin|do|function|def|defp|class|module)\\b")
    static let keywordEnd = InsightRegex("\\bend\\b")
    static let typeDeclaration = InsightRegex(
        "\\b(class|struct|interface|enum|protocol|record|object|actor|trait|typealias)\\s+([A-Z][A-Za-z0-9_]*)")
    static let packageDeclaration = InsightRegex("^\\s*(?:package|namespace)\\s+([A-Za-z_][\\w.]*)", multiline: true)
    static let capitalized = InsightRegex("\\b[A-Z][A-Za-z0-9_]{2,}\\b")
    static let pureVirtual = InsightRegex("\\)\\s*(?:const\\s*)?=\\s*0\\s*;")
    static let cInclude = InsightRegex("^\\s*#\\s*(?:include|import)\\s*([\"<])([^\">\\n]+)[\">]", multiline: true)
    static let objcModule = InsightRegex("^\\s*@import\\s+([\\w.]+)", multiline: true)
    static let jsImport = InsightRegex(
        "\\bimport\\s+(?:type\\s+)?(?:[\\w$*{}\\s,]+?\\s+from\\s+)?[\"']([^\"'\\n]+)[\"']")
    static let jsExport = InsightRegex(
        "\\bexport\\s+(?:type\\s+)?(?:\\*|\\{[^}]*\\}|\\*\\s+as\\s+\\w+)\\s+from\\s+[\"']([^\"'\\n]+)[\"']")
    static let jsRequire = InsightRegex("\\b(?:require|import)\\s*\\(\\s*[\"']([^\"'\\n]+)[\"']\\s*\\)")
    static let pyImport = InsightRegex(
        "^[ \\t]*import[ \\t]+([\\w.]+(?:[ \\t]+as[ \\t]+\\w+)?(?:[ \\t]*,[ \\t]*[\\w.]+(?:[ \\t]+as[ \\t]+\\w+)?)*)",
        multiline: true)
    static let pyFrom = InsightRegex(
        "^[ \\t]*from[ \\t]+(\\.*[\\w.]*)[ \\t]+import[ \\t]+(\\([^)]*\\)|[^\\n]+)", multiline: true)
    static let goBlock = InsightRegex("\\bimport\\s*\\(([^)]*)\\)")
    static let goSingle = InsightRegex("\\bimport\\s+(?:[\\w.]+\\s+)?\"([^\"\\n]+)\"")
    static let quoted = InsightRegex("\"([^\"\\n]+)\"")
    static let rustMod = InsightRegex("^\\s*(?:pub(?:\\([^)]*\\))?\\s+)?mod\\s+([A-Za-z_]\\w*)\\s*;", multiline: true)
    static let rustUse = InsightRegex("\\buse\\s+((?:crate|super|self)(?:::[A-Za-z_]\\w*)+)")
    static let jvmImport = InsightRegex("^\\s*import\\s+(?:static\\s+)?([A-Za-z_][\\w.]*)(\\.\\*)?", multiline: true)
    static let csUsing = InsightRegex("^\\s*(?:global\\s+)?using\\s+(?:static\\s+)?([A-Za-z_][\\w.]*)\\s*;", multiline: true)
    static let swiftImport = InsightRegex("^\\s*(?:@\\w+\\s+)*import\\s+(?:\\w+\\s+)?([A-Za-z_][\\w.]*)", multiline: true)
    static let rubyRequire = InsightRegex("\\brequire(_relative)?\\s*\\(?\\s*[\"']([^\"'\\n]+)[\"']")
    static let phpInclude = InsightRegex(
        "\\b(?:require|include)(?:_once)?\\s*\\(?\\s*(?:__DIR__\\s*\\.\\s*)?[\"']([^\"'\\n]+)[\"']")
    static let phpUse = InsightRegex("^\\s*use\\s+([A-Za-z_][\\w\\\\]*)", multiline: true)
    static let dartImport = InsightRegex("^\\s*(?:import|export|part)\\s+[\"']([^\"'\\n]+)[\"']", multiline: true)
    static let luaRequire = InsightRegex("\\brequire\\s*\\(?\\s*[\"']([^\"'\\n]+)[\"']")
    static let cssImport = InsightRegex("@(?:import|use|forward)\\s+(?:url\\()?\\s*[\"']([^\"'\\n]+)[\"']")
    static let qmlImport = InsightRegex("^\\s*import\\s+([A-Za-z_][\\w.]*|\"[^\"]+\")", multiline: true)
    static let shellSource = InsightRegex(
        "(?:^|[;&|]\\s*|\\s)(?:source|\\.)\\s+[\"']?([\\w./${}-]+\\.(?:sh|bash))", multiline: true)
    static let elixirAlias = InsightRegex("^\\s*(?:alias|import|use|require)\\s+([A-Z][\\w.]*)", multiline: true)
    static let fixPattern = InsightRegex(
        "\\b(fix|fixes|fixed|fixing|bug|bugs|bugfix|hotfix|defect|crash|crashes|regression|broken|revert)\\b",
        caseless: true)
    static let goModule = InsightRegex("^\\s*module\\s+(\\S+)", multiline: true)
    static let dartName = InsightRegex("^name:\\s*([A-Za-z0-9_]+)", multiline: true)
}
