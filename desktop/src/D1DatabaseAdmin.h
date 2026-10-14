#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

// The Logs page's Database tab talks to /api/accounts/admin-database: a live
// D1 table inventory annotated with the #682 audit, paged rows with sealed
// `data` blobs decrypted, and audited insert/update/delete. These helpers are
// the parts with a contract (the exact bytes an administrator signs, how an
// edited cell is typed, what the audit says about a table) so they can be
// tested without a window or a network. app/src/admin_database.py is the
// other side of every signed text here.
namespace forkmesh {

struct D1DatabaseColumn {
    QString name;
    QString type; // declared SQLite type, e.g. "INTEGER NOT NULL" → "INTEGER"
    bool primaryKey = false;
    bool notNull = false;
    bool hasDefault = false;
};

struct D1DatabaseTable {
    QString name;
    qint64 rows = 0;
    // Rows are served only by the table's purpose-built API.
    bool restricted = false;
    bool writable = false;
    bool deletable = false;
    QString writeBlockedReason;
    // WITHOUT ROWID tables page by offset and cannot be edited by row.
    bool rowid = true;
    // "sealed", "sealed+indexed", "indexed" or "plaintext".
    QString storage;
    // The `data` column holds plaintext JSON rather than ciphertext.
    bool plaintextData = false;
    QList<D1DatabaseColumn> columns;
    // Live encryption findings, e.g. a plaintext secret-named column.
    QStringList gaps;
    // The #682 audit's verdict ("drop", "custody", "runtime-only", ...).
    QString auditCategory;
    QString auditNote;
};

QList<D1DatabaseColumn> parseD1DatabaseColumns(const QJsonArray &columns);
QList<D1DatabaseTable> parseD1DatabaseInventory(const QJsonObject &inventory);

// One read. Every field is covered by the signature, in this order.
struct D1DatabaseReadRequest {
    QString op; // "inventory", "rows" or "schema"
    QString table;
    QString cursor;
    QString limit;
    QString filterColumn;
    QString filterMode; // "", "equals", "contains", "blind" or "null"
    QString filterValue;
    QString order; // "" (newest first) or "asc"
};

QByteArray d1DatabaseReadCanonical(const QString &node, const QString &ts,
                                   const D1DatabaseReadRequest &request);

// The percent-encoded query string for a signed read. Every value is fully
// encoded (a '+' stays a plus, not a space) so the Worker decodes exactly
// the text that was signed.
QByteArray d1DatabaseReadQueryString(const D1DatabaseReadRequest &request,
                                     const QString &node, const QString &ts,
                                     const QString &signature,
                                     bool refresh = false);

// A write signs its payload's exact bytes by digest; the payload travels as
// an opaque string beside the signature.
QByteArray d1DatabaseWriteCanonical(const QString &node, const QString &ts,
                                    const QString &requestId,
                                    const QByteArray &payload);

// A single-use id for one write (the Worker refuses a repeat).
QString newD1DatabaseRequestId();

// An edited cell, typed by the column's SQLite affinity: INTEGER columns get
// an integer and REAL columns a number when the text parses as one; anything
// else, and text that does not parse, stays a string for SQLite to judge.
QJsonValue d1DatabaseEditorValue(const QString &text,
                                 const QString &declaredType);

// One line of cell text: NULL, numbers without exponent noise, byte blobs
// by size, JSON compactly.
QString d1DatabaseCellText(const QJsonValue &value);

// "Drop" for "drop", "Drop after readers are removed" for
// "drop-after-readers", and so on; empty for no finding.
QString d1DatabaseAuditCategoryLabel(const QString &category);
bool d1DatabaseAuditIsDropCandidate(const QString &category);

// Table names a SQL statement reads or writes (FROM, JOIN, INTO, UPDATE),
// lowercased, without duplicates.
QStringList d1DatabaseQueryTables(const QString &sql);

// The D1 tab's ranked query insights that touch `table`, most rows read
// first, from the /api/accounts/admin-d1-usage-report payload.
QJsonArray d1DatabaseTableQueryInsights(const QJsonObject &d1UsageReport,
                                        const QString &table);

// A migration body that drops `tables`, one statement each, headed by the
// order the audit requires: readers and schema.py DDL leave first, in an
// earlier deploy, or the next cold isolate re-creates the table.
QString d1DatabaseDropMigration(const QStringList &tables);

} // namespace forkmesh
