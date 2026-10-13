#pragma once

#include <QCoreApplication>
#include <QEvent>
#include <QRegion>
#include <QSplitter>
#include <QSplitterHandle>

namespace forkmesh::ui {

class SplitterHairline : public QObject
{
public:
    static constexpr int kGrabMargin = 4;

    using QObject::QObject;

    static void install(QCoreApplication *app)
    {
        app->installEventFilter(new SplitterHairline(app));
    }

    static void apply(QSplitterHandle *handle)
    {
        handle->setAttribute(Qt::WA_MouseNoMask);
        if (handle->orientation() == Qt::Horizontal)
            handle->setContentsMargins(kGrabMargin, 0, kGrabMargin, 0);
        else
            handle->setContentsMargins(0, kGrabMargin, 0, kGrabMargin);
        handle->setMask(QRegion(handle->contentsRect()));
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::Resize || !watched->isWidgetType())
            return false;
        auto *handle = qobject_cast<QSplitterHandle *>(watched);
        if (!handle || !handle->splitter() || handle->splitter()->handleWidth() > 1)
            return false;
        apply(handle);
        return true;
    }
};

} // namespace forkmesh::ui
