#include "CaptureFeature.h"

#include "ScreenCaptureOverlay.h"
#include "ScreenshotMarkupWindow.h"

#include <QDialog>
#include <QWidget>

#include <utility>

namespace forkmesh::capture {

CaptureFeature::CaptureFeature(QWidget *dialogParent,
                               ImageAccepted imageAccepted, Status status,
                               QObject *parent)
    : QObject(parent), m_dialogParent(dialogParent),
      m_imageAccepted(std::move(imageAccepted)), m_status(std::move(status))
{
}

CaptureFeature::~CaptureFeature()
{
    shutdown();
}

bool CaptureFeature::isActive() const
{
    return !m_overlay.isNull() || !m_markup.isNull();
}

void CaptureFeature::start()
{
    if (isActive()) {
        if (m_status)
            m_status(QStringLiteral("Capture is already open."));
        return;
    }

    ScreenCaptureOverlay *overlay = ScreenCaptureOverlay::begin();
    if (!overlay) {
        if (m_status)
            m_status(QStringLiteral(
                "Couldn't grab the screen for a region screenshot."));
        return;
    }
    m_overlay = overlay;
    connect(overlay, &QObject::destroyed, this, [this] { m_overlay = nullptr; });
    connect(overlay, &ScreenCaptureOverlay::cancelled, this,
            [this] { m_overlay = nullptr; });
    connect(overlay, &ScreenCaptureOverlay::captured, this,
            [this](const QImage &image) {
                m_overlay = nullptr;
                if (image.isNull())
                    return;
                auto *markup = new ScreenshotMarkupWindow(image, m_dialogParent);
                m_markup = markup;
                connect(markup, &QObject::destroyed, this,
                        [this] { m_markup = nullptr; });
                connect(markup, &ScreenshotMarkupWindow::imageAccepted, this,
                        [this](const QImage &annotated) {
                            if (m_imageAccepted)
                                m_imageAccepted(annotated);
                        });
                markup->show();
                markup->raise();
                markup->activateWindow();
            });
}

void CaptureFeature::shutdown()
{
    if (m_overlay) {
        ScreenCaptureOverlay *overlay = m_overlay.data();
        m_overlay = nullptr;
        QObject::disconnect(overlay, nullptr, this, nullptr);
        overlay->cancel();
    }
    if (m_markup) {
        ScreenshotMarkupWindow *markup = m_markup.data();
        m_markup = nullptr;
        QObject::disconnect(markup, nullptr, this, nullptr);
        markup->reject();
        markup->deleteLater();
    }
}

} // namespace forkmesh::capture
