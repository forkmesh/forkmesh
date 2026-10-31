#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

class QWidget;

namespace forkmesh::notes {

using PageReply = std::function<void(bool, const QJsonObject &, const QString &)>;
using PageRequest = std::function<void(const QByteArray &, const QString &,
                                       const QJsonObject &, PageReply)>;

bool pickNoteEmoji(QWidget *parent, const QString &current, QString *chosen);

struct PageCustomizeHooks {
    std::function<void(const QJsonObject &, std::function<void(bool, const QString &)>)>
        save;
    std::function<void()> remove;
};

void customizePage(QWidget *parent, const QJsonObject &page, const QJsonObject &notes,
                   const QJsonArray &myNotes, PageCustomizeHooks hooks);

void sharePage(QWidget *parent, const QJsonObject &page, PageRequest request,
               std::function<void(const QJsonArray &)> changed);

}
