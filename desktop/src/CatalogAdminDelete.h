#pragma once

#include <QByteArray>
#include <QString>
#include <QUrlQuery>

// The administrator's repository purge: DELETE /api/repositories signed with
// the admin account's own key (adhoc #196).
//
// The Network Repos catalog lists every repository the relay advertises, most
// of which this machine holds no copy of. Deleting the local copy is all the
// row's Delete button could ever do, so for a repository that was never
// cloned here it had nothing to delete. An administrator instead signs the
// canonical below and the relay purges the repository everywhere: the catalog
// row, every repo-scoped table, and a tombstone so the owner's node cannot
// republish it on its next heartbeat.
//
// Header-only, like MirrorSyncVerdict.h, so forkmesh-control-tests can check
// the exact signed bytes without a window or a network. The other side of the
// contract is the DELETE branch of catalog_handler in app/src/catalog_routes.py
// — a changed context string or a reordered field simply stops verifying.
namespace forkmesh {

// Domain separation: an owner's own catalog delete signs
// "forkmesh-catalog-delete-v1", so neither signature can ever be replayed as
// the other.
inline QByteArray catalogAdminDeleteContext()
{
    return QByteArrayLiteral("forkmesh-admin-repo-delete-v1");
}

// The exact bytes the administrator signs: the context, the acting
// administrator's account, the target repository, and the request timestamp.
// Naming both accounts and the repository inside the signature means a
// captured proof cannot be re-aimed at a different repository.
inline QByteArray catalogAdminDeleteCanonical(const QString &user,
                                              const QString &owner,
                                              const QString &name,
                                              const QString &ts)
{
    return catalogAdminDeleteContext() + '\n' + user.toLower().toUtf8() +
           '\n' + owner.toLower().toUtf8() + '\n' + name.toLower().toUtf8() +
           '\n' + ts.toUtf8();
}

// The request query for that purge. `user` names the administrator, not a
// machine: one account owns many nodes, and this is the account the relay
// checks `is_admin` on.
inline QUrlQuery catalogAdminDeleteQuery(const QString &user,
                                        const QString &owner,
                                        const QString &name,
                                        const QString &ts,
                                        const QString &signature)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("owner"), owner.toLower());
    query.addQueryItem(QStringLiteral("name"), name.toLower());
    query.addQueryItem(QStringLiteral("user"), user.toLower());
    query.addQueryItem(QStringLiteral("ts"), ts);
    query.addQueryItem(QStringLiteral("sig"), signature);
    return query;
}

} // namespace forkmesh
