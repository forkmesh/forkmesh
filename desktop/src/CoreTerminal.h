#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

#include "TerminalBackend.h"

class CoreClient;
class QTimer;

namespace forkmesh::coreterminal {

void setClient(CoreClient *client);
CoreClient *client();
bool available();
void sweepOrphans(CoreClient *client);

}

class CoreTerminalBackend : public TerminalBackend
{
public:
    static CoreTerminalBackend *start(QObject *parent, const QString &commandLine, const QString &cwd,
                                      const QStringList &extraEnv, bool keepHostShell, int cols, int rows,
                                      const QString &persistKey, Callbacks callbacks);
    static CoreTerminalBackend *resume(QObject *parent, const QString &persistKey, int cols, int rows,
                                       Callbacks callbacks);
    static QString persistedTerminal(const QString &persistKey);

    ~CoreTerminalBackend() override;

    bool running() const override { return m_running; }
    QString terminalId() const { return m_id; }
    void write(const QByteArray &bytes) override;
    void resize(int cols, int rows) override;
    void close() override;
    void keepRunning() override;

private:
    CoreTerminalBackend(QObject *parent, CoreClient *client, const QString &id, const QString &persistKey,
                        Callbacks callbacks);
    void listen();
    void onEvent(const QString &event, const QJsonObject &data);
    void onStarted();
    void finish(int code, bool forgetId = true);
    void fail(const QString &error);
    void flushInput();
    void sendResize();
    void forget();

    QPointer<CoreClient> m_client;
    QString m_id;
    QString m_persistKey;
    Callbacks m_callbacks;
    bool m_running = true;
    bool m_attached = false;
    bool m_keep = false;
    bool m_closed = false;
    bool m_resuming = false;
    QByteArray m_early;
    QByteArray m_pendingInput;
    int m_cols = 0;
    int m_rows = 0;
    int m_sentCols = 0;
    int m_sentRows = 0;
    QTimer *m_resizeTimer = nullptr;
};
