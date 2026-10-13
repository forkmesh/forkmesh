#pragma once


#include <QString>
#include <QStringList>

#if defined(__GNUC__) || defined(__clang__) || \
    (defined(_MSC_VER) && _MSC_VER >= 1926)
#define FORKMESH_LOG_SOURCE_FILE __builtin_FILE()
#define FORKMESH_LOG_SOURCE_LINE __builtin_LINE()
#define FORKMESH_LOG_SOURCE_FUNCTION __builtin_FUNCTION()
#else
#define FORKMESH_LOG_SOURCE_FILE nullptr
#define FORKMESH_LOG_SOURCE_LINE 0
#define FORKMESH_LOG_SOURCE_FUNCTION nullptr
#endif

namespace forkmesh {

enum class LogSeverity {
    Automatic,
    Informational,
    Error,
};

QString logSourceRelativePath(const char *absolutePath);

QString logSourceSuffix(
    const QString &relativePath, int line,
    LogSeverity severity = LogSeverity::Automatic);

bool splitLogSource(const QString &message, QString *body, QString *path,
                    int *line, LogSeverity *severity = nullptr);

QString logMessageBody(const QString &message);

QString logSourceLabel(const QString &relativePath, int line);

QString resolveLogSourcePath(const QString &relativePath,
                             const QStringList &extraRoots = {});

QString logSourceBuildRoot();

// A captured call site. Give a helper a `LogSource source = logSourceHere()`
// parameter and the builtins expand at the *caller*, so a chokepoint that every
// caller funnels through (waitForGit, runGitCapture, the background-activity
// bus) can report the line that asked for the work instead of its own. Forward
// the value on by hand to keep that attribution as it travels further down the
// chain. `file` points at a string literal, so a captured source stays valid for
// as long as anything holds it, across threads included.
//
// Always capture through logSourceHere(). Aggregate-initialising the builtins
// straight into a default argument does not follow the call site: GCC pins them
// to the declaration (Clang does not), while their expansion inside this
// function's own default arguments follows the caller on both.
struct LogSource {
    const char *file = nullptr;
    int line = 0;
    // The enclosing function at the capture site, as __builtin_FUNCTION()
    // spells it: unqualified ("refreshRepoChangeBadge"), and "operator()" from
    // inside a lambda. Read it through logSourceFunctionName(), which blanks
    // the latter. Like `file`, it points at a string literal.
    const char *function = nullptr;

    bool isValid() const { return file && *file && line > 0; }
};

inline LogSource logSourceHere(const char *file = FORKMESH_LOG_SOURCE_FILE,
                               int line = FORKMESH_LOG_SOURCE_LINE,
                               const char *function =
                                   FORKMESH_LOG_SOURCE_FUNCTION)
{
    LogSource source;
    source.file = file;
    source.line = line;
    source.function = function;
    return source;
}

// The function a source was captured in, or empty when that says nothing: a
// lambda's "operator()" names every closure in the program, so the file:line
// the source also carries is the only thing worth showing for one.
inline QString logSourceFunctionName(const LogSource &source)
{
    if (!source.function || !*source.function)
        return QString();
    const QString name = QString::fromUtf8(source.function);
    if (name == QLatin1String("operator()") || name.startsWith(QLatin1String("<lambda")))
        return QString();
    return name;
}

} // namespace forkmesh
