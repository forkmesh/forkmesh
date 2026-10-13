#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Cloudflare's D1 query insights name a statement, never the code that ran it.
// This maps one back onto the checkout: every SQL string literal in the Worker
// source is normalized the same way the provider normalizes a captured query
// (quoted values and numbers collapse to `?`, whitespace collapses to single
// spaces), so the two texts can be compared directly and the D1 tab can offer
// "open app/src/<file>.py at the line that issues this query".
namespace forkmesh {

// One SQL statement found in the Worker source.
struct D1QueryStatement {
    QString path; // repo-relative, e.g. "app/src/repositories.py"
    int line = 0; // 1-based line of the statement's first string literal
    QString normalized; // lowercased provider-shaped SQL
};

// Where a provider-reported query is issued from, empty when nothing in the
// checkout matches it well enough to name a file without guessing.
struct D1QuerySite {
    QString path;
    int line = 0;
    // Further places that issue the identical statement. The Worker source is
    // mid-split, so a query often lives in both entry.py and the module
    // extracted from it; the tab names one place and counts the rest.
    int otherSites = 0;

    bool isValid() const { return !path.isEmpty() && line > 0; }
};

// Provider-shaped SQL for comparison: comments dropped, quoted literals and
// numbers replaced by `?`, whitespace collapsed, lowercased. Idempotent, so it
// can be applied to an already-normalized provider string as well as to raw
// source text.
QString normalizeD1Query(const QString &sql);

// Every SQL statement in one source file. `source` is the file's text and
// `relativePath` is what the returned statements report. Python's implicit
// string concatenation and triple-quoted blocks are joined into one statement,
// which is how nearly every query in the Worker is written.
QList<D1QueryStatement> scanD1QueryStatements(const QString &relativePath,
                                              const QString &source);

// Scan the Worker source of the first root that contains it. Roots are
// checkout directories (the build root, the selected repository); a root is
// used when it holds `app/src`.
QList<D1QueryStatement> buildD1QuerySourceIndex(const QStringList &roots);

// The statement in `index` that issues `providerQuery`, or an invalid site.
// A match must cover the provider text (which the relay truncates at 360
// characters) and must be unambiguous: when two different places match a
// partial query equally well, neither is reported rather than pointing at the
// wrong file.
D1QuerySite matchD1QuerySite(const QString &providerQuery,
                             const QList<D1QueryStatement> &index);

} // namespace forkmesh
