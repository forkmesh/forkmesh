#pragma once

#include "NoteComments.h"

#include <QPixmap>
#include <QWidget>

#include <functional>

namespace forkmesh::notes {

class NotePresenceFaces : public QWidget
{
public:
    explicit NotePresenceFaces(QWidget *parent = nullptr);

    void setPeers(const QList<NotePeer> &peers);
    QList<NotePeer> peers() const { return m_peers; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    std::function<QPixmap(const QString &)> picture;
    std::function<void(const QString &)> openProfile;

protected:
    void paintEvent(QPaintEvent *event) override;
    bool event(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    int slotCount() const;
    QRect slotRect(int slot) const;
    int slotAt(const QPoint &point) const;
    void showEveryone();

    QList<NotePeer> m_peers;
};

}
