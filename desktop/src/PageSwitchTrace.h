#pragma once

#include "AppPaths.h"

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <utility>

namespace forkmesh {

class PageSwitchTrace final : public QObject
{
public:
    static PageSwitchTrace &instance()
    {
        static auto *trace = new PageSwitchTrace;
        return *trace;
    }

    void begin(const QString &name)
    {
        if (m_depth++ > 0) {
            ++m_nested;
            return;
        }
        if (m_watching)
            report(QStringLiteral("interrupted"));
        m_name = name;
        m_clock.start();
        m_buildMs = 0;
        m_syncMs = -1;
        m_firstPaintMs = -1;
        m_lastPaintMs = -1;
        m_paints = 0;
        m_nested = 0;
        m_page.clear();
    }

    void addBuild(qint64 ms)
    {
        if (m_depth > 0)
            m_buildMs += ms;
    }

    void end(QWidget *page)
    {
        if (m_depth == 0 || --m_depth > 0)
            return;
        m_syncMs = m_clock.elapsed();
        m_page = page;
        if (!page) {
            report(QStringLiteral("no page"));
            return;
        }
        m_watching = true;
        qApp->installEventFilter(this);
        m_quiet.start();
        m_cap.start();
    }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::Paint || !m_watching || !m_page ||
            !object->isWidgetType())
            return false;
        auto *widget = static_cast<QWidget *>(object);
        if (widget != m_page && !m_page->isAncestorOf(widget))
            return false;
        if (QWidget *focus = QApplication::focusWidget();
            focus && (widget == focus || widget->parentWidget() == focus))
            return false;
        const qint64 now = m_clock.elapsed();
        if (m_firstPaintMs < 0)
            m_firstPaintMs = now;
        m_lastPaintMs = now;
        ++m_paints;
        m_quiet.start();
        return false;
    }

private:
    static constexpr int kQuietMs = 800;
    static constexpr int kCapMs = 6000;

    PageSwitchTrace()
    {
        m_writer.setMaxThreadCount(1);
        m_quiet.setSingleShot(true);
        m_quiet.setInterval(kQuietMs);
        m_cap.setSingleShot(true);
        m_cap.setInterval(kCapMs);
        connect(&m_quiet, &QTimer::timeout, this,
                [this] { report(QStringLiteral("settled")); });
        connect(&m_cap, &QTimer::timeout, this,
                [this] { report(QStringLiteral("still painting")); });
    }

    void report(const QString &outcome)
    {
        if (m_watching)
            qApp->removeEventFilter(this);
        m_watching = false;
        m_quiet.stop();
        m_cap.stop();
        const auto ms = [](qint64 v) {
            return v < 0 ? QStringLiteral("-") : QStringLiteral("%1ms").arg(v);
        };
        const QString line =
            QStringLiteral("[switch] %1 | build %2 | sync %3 | first paint %4 | "
                           "last paint %5 | paints %6 | nested %7 | %8")
                .arg(m_name, ms(m_buildMs), ms(m_syncMs), ms(m_firstPaintMs),
                     ms(m_lastPaintMs))
                .arg(m_paints)
                .arg(m_nested)
                .arg(outcome);
        qInfo().noquote() << line;
        const QString dir = paths::diagnosticsDir();
        const QByteArray bytes =
            (QDateTime::currentDateTime().toString(Qt::ISODateWithMs) +
             QLatin1Char(' ') + line + QLatin1Char('\n'))
                .toUtf8();
        m_writer.start([dir, bytes] {
            QDir().mkpath(dir);
            QFile file(dir + QStringLiteral("/page-switches.log"));
            if (file.size() > kMaxLogBytes)
                file.resize(0);
            if (file.open(QIODevice::Append | QIODevice::Text))
                file.write(bytes);
        });
    }

    static constexpr qint64 kMaxLogBytes = 1024 * 1024;

    QString m_name;
    QElapsedTimer m_clock;
    QTimer m_quiet;
    QTimer m_cap;
    QThreadPool m_writer;
    QPointer<QWidget> m_page;
    qint64 m_buildMs = 0;
    qint64 m_syncMs = -1;
    qint64 m_firstPaintMs = -1;
    qint64 m_lastPaintMs = -1;
    int m_paints = 0;
    int m_nested = 0;
    int m_depth = 0;
    bool m_watching = false;
};

class PageSwitchScope
{
public:
    PageSwitchScope(const QString &name, std::function<QWidget *()> page)
        : m_page(std::move(page))
    {
        PageSwitchTrace::instance().begin(name);
    }

    ~PageSwitchScope() { PageSwitchTrace::instance().end(m_page ? m_page() : nullptr); }

    PageSwitchScope(const PageSwitchScope &) = delete;
    PageSwitchScope &operator=(const PageSwitchScope &) = delete;

private:
    std::function<QWidget *()> m_page;
};

} // namespace forkmesh
