#include "AgentProcess.h"
#include "CodexAppServerSession.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

namespace {

class FakeProcess : public AgentProcess
{
public:
    explicit FakeProcess(QObject *parent) : AgentProcess(parent) {}
    void setWorkingDirectory(const QString &) override {}
    void setProcessEnvironment(const QProcessEnvironment &) override {}
    void setProcessChannelMode(QProcess::ProcessChannelMode) override {}
    void setProgram(const QString &) override {}
    void setArguments(const QStringList &) override {}
    void start() override { begin(); }
    void startContained(const QString &, const QStringList &, const QString &) override { begin(); }
    qint64 write(const QByteArray &data) override
    {
        writes.append(data);
        return data.size();
    }
    void closeWriteChannel() override { closed = true; }
    QByteArray readAllStandardOutput() override
    {
        QByteArray out;
        out.swap(pending);
        return out;
    }
    QByteArray readAllStandardError() override { return {}; }
    QProcess::ProcessState state() const override { return m_state; }
    qint64 processId() const override { return 4242; }
    bool isWritable() const override { return m_state == QProcess::Running && !closed; }
    void terminate() override {}
    void kill() override {}
    bool waitForStarted(int) override { return true; }
    bool waitForFinished(int) override { return true; }
    QString errorString() const override { return {}; }
    int exitCode() const override { return 0; }
    QProcess::ExitStatus exitStatus() const override { return QProcess::NormalExit; }

    void feed(const QByteArray &line)
    {
        pending.append(line);
        emit readyReadStandardOutput();
    }

    QByteArray pending;
    QList<QByteArray> writes;
    bool closed = false;

private:
    void begin()
    {
        m_state = QProcess::Running;
        emit started();
    }
    QProcess::ProcessState m_state = QProcess::NotRunning;
};

FakeProcess *g_process = nullptr;

QJsonArray drainWrites()
{
    QJsonArray out;
    if (!g_process)
        return out;
    for (const QByteArray &write : std::as_const(g_process->writes))
        out.append(QString::fromUtf8(write));
    g_process->writes.clear();
    return out;
}

QJsonObject runScenario(const QJsonObject &scenario)
{
    CodexAppServerSession session;
    QJsonArray events;
    QJsonArray models;
    QObject::connect(&session, &AgentRuntimeSession::event, [&events](const QJsonObject &event) {
        events.append(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
    });
    QObject::connect(&session, &CodexAppServerSession::modelsListed,
                     [&models](const QJsonArray &list) { models.append(list); });
    session.setAppServerCommand(QStringLiteral("fake-codex"));
    const QJsonObject launch = scenario.value(QStringLiteral("launch")).toObject();
    session.start(launch.value(QStringLiteral("cwd")).toString(), {},
                  launch.value(QStringLiteral("initialPrompt")).toString(),
                  launch.value(QStringLiteral("resumeThreadId")).toString(),
                  launch.value(QStringLiteral("model")).toString(),
                  launch.value(QStringLiteral("mode")).toString(),
                  launch.value(QStringLiteral("effort")).toString(), 0,
                  launch.value(QStringLiteral("resumeFallbackPrompt")).toString(),
                  launch.value(QStringLiteral("sessionId")).toString());
    QJsonArray steps;
    QJsonObject first{{QStringLiteral("writes"), drainWrites()}, {QStringLiteral("events"), events}};
    events = QJsonArray();
    steps.append(first);
    for (const QJsonValue &value : scenario.value(QStringLiteral("ops")).toArray()) {
        const QJsonObject op = value.toObject();
        bool ok = true;
        if (op.contains(QStringLiteral("feed")))
            g_process->feed(op.value(QStringLiteral("feed")).toString().toUtf8() + '\n');
        else if (op.contains(QStringLiteral("send")))
            ok = session.sendUserText(op.value(QStringLiteral("send")).toString());
        else if (op.contains(QStringLiteral("respond"))) {
            const QJsonArray args = op.value(QStringLiteral("respond")).toArray();
            ok = session.respondToRequest(args.at(0).toString(), args.at(1).toString());
        } else if (op.contains(QStringLiteral("options"))) {
            const QJsonArray args = op.value(QStringLiteral("options")).toArray();
            session.setTurnOptions(args.at(0).toString(), args.at(1).toString(), args.at(2).toString());
        } else if (op.contains(QStringLiteral("interrupt"))) {
            session.interrupt();
        }
        steps.append(QJsonObject{{QStringLiteral("ok"), ok},
                                 {QStringLiteral("writes"), drainWrites()},
                                 {QStringLiteral("events"), events},
                                 {QStringLiteral("models"), models}});
        events = QJsonArray();
        models = QJsonArray();
    }
    QJsonObject out = scenario;
    out.insert(QStringLiteral("steps"), steps);
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3)
        return 2;
    AgentProcess::setFactory([](QObject *parent, const AgentProcess::Options &) -> AgentProcess * {
        g_process = new FakeProcess(parent);
        return g_process;
    });
    QFile in(QString::fromLocal8Bit(argv[1]));
    if (!in.open(QIODevice::ReadOnly))
        return 1;
    QJsonArray results;
    for (const QJsonValue &scenario : QJsonDocument::fromJson(in.readAll()).array())
        results.append(runScenario(scenario.toObject()));
    QFile out(QString::fromLocal8Bit(argv[2]));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 1;
    out.write(QJsonDocument(results).toJson(QJsonDocument::Indented));
    return 0;
}
