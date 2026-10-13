#pragma once

#include <QColor>
#include <QDialog>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QVector>

class MarkupCanvas;
class QKeyEvent;

class ScreenshotMarkupWindow : public QDialog
{
    Q_OBJECT
public:
    explicit ScreenshotMarkupWindow(const QImage &screenshot, QWidget *parent = nullptr);

signals:
    void imageAccepted(const QImage &image); // base + annotations, ready to save

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void onAccept();

    MarkupCanvas *m_canvas = nullptr;
};
