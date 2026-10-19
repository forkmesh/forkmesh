#pragma once

#include <QAbstractButton>
#include <QMessageBox>
#include <QObject>

#include <utility>

namespace forkmesh::ui {

template <typename Handler>
QMetaObject::Connection onMessageBoxClicked(QMessageBox *box, QObject *context,
                                            Handler &&handler)
{
    return QObject::connect(box, &QMessageBox::buttonClicked, context,
                            std::forward<Handler>(handler),
                            Qt::QueuedConnection);
}

}
