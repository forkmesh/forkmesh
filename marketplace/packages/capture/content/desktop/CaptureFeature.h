#pragma once

#include <QImage>
#include <QObject>
#include <QPointer>

#include <functional>

class ScreenCaptureOverlay;
class ScreenshotMarkupWindow;
class QWidget;

namespace forkmesh::capture {

// Runtime for the Capture package. The host supplies only two bounded
// callbacks: accepted output and human-readable status. No MainWindow or
// marketplace internals leak into the package.
class CaptureFeature final : public QObject {
public:
    using ImageAccepted = std::function<void(const QImage &)>;
    using Status = std::function<void(const QString &)>;

    CaptureFeature(QWidget *dialogParent, ImageAccepted imageAccepted,
                   Status status, QObject *parent = nullptr);
    ~CaptureFeature() override;

    void start();
    void shutdown();
    bool isActive() const;

private:
    QWidget *m_dialogParent = nullptr;
    ImageAccepted m_imageAccepted;
    Status m_status;
    QPointer<ScreenCaptureOverlay> m_overlay;
    QPointer<ScreenshotMarkupWindow> m_markup;
};

} // namespace forkmesh::capture
