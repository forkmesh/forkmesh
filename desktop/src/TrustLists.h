#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QtGlobal>

// Scoped, community-operated trust lists (forkmesh-trust-list-v1).
//
// An independent port of the reference resolver in app/src/trust_lists.py;
// the JavaScript and Dart clients carry their own, and
// conformance/trust-lists/v1/vectors.json keeps every implementation
// producing identical resolutions -- rejection codes, ordering, and telemetry
// counts included. Plain functions, no QObject, so forkmesh-trust-list-tests
// checks it without linking the app.
//
// Nothing is trusted unless the caller follows a list; trust never leaves the
// role and repository/organization/community scope a list was issued for;
// equivocation, forked history, and revocations cut a list back to its last
// good version; disagreeing lists surface as conflicts that only a local
// override resolves.
namespace forkmesh::trust {

// Sorted-key JSON with no whitespace. Strings and keys must be printable
// ASCII without quote or backslash and numbers must be integers of magnitude
// at most 2^53 - 1; anything else clears *ok and returns an empty string.
QString canonicalJson(const QJsonValue &value, bool *ok = nullptr);

// "sha256:" + hex SHA-256 of the raw document's UTF-8 bytes.
QString documentId(const QString &raw);

// "urn:forkmesh:trust-list:sha256:..." naming one issuer's list.
QString listId(const QString &issuer, const QString &name);

// Parses and verifies one raw signed document (list version, revocation, or
// appeal). Returns the stable rejection code, or an empty string when the
// document is valid, in which case *document receives its parsed body.
QString verifyDocument(const QString &raw, QJsonObject *document = nullptr);

// Verifies, composes, and merges trust lists for one local client. `follows`
// holds {issuer, name, roles?, communities?} objects and `overrides`
// {subject, role, scope, stance} objects; both are local, never signed.
QJsonObject resolve(const QStringList &documents, qint64 now,
                    const QJsonArray &follows = {},
                    const QJsonArray &overrides = {});

} // namespace forkmesh::trust
