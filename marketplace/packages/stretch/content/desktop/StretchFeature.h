#pragma once

#include "NativePackageExtension.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QCheckBox;
class QComboBox;
class QHideEvent;
class QLabel;
class QProcess;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QTableWidget;
class QTimer;

namespace forkmesh::stretch {

struct StretchFigure {
    double width = 200;
    double height = 264;
    QPointF hip{100, 158};
    double hipShift = 16;
    double kneeDip = 14;
    double torso = 80;
    double shoulderHalf = 30;
    double shrug = 7;
    double neck = 24;
    double head = 16;
    double upperArm = 36;
    double forearm = 32;
    double hipHalf = 12;
    QPointF leftFoot{78, 254};
    QPointF rightFoot{122, 254};
    double tapLift = 4;
    double tapHz = 3;
};

struct StretchPose {
    double hipX = 0;
    double knees = 0;
    double lean = 0;
    double twist = 0;
    double fold = 0;
    double shrug = 0;
    double headTilt = 0;
    double headTurn = 0;
    double headNod = 0;
    double lArm = 8;
    double lElbow = 0;
    double rArm = 8;
    double rElbow = 0;
};

struct StretchMotion {
    double period = 4;
    QString tap;
    QVector<double> times;
    QVector<StretchPose> frames;
};

struct StretchBody {
    QPointF hip;
    QPointF chest;
    QPointF leftShoulder;
    QPointF rightShoulder;
    QPointF leftElbow;
    QPointF rightElbow;
    QPointF leftHand;
    QPointF rightHand;
    QPointF leftKnee;
    QPointF rightKnee;
    QPointF leftFoot;
    QPointF rightFoot;
    QPointF headCentre;
    double headRadius = 16;
    QPointF leftEye;
    QPointF rightEye;
};

struct StretchStep {
    QString id;
    QString title;
    QString say;
    QString half;
    QString motion;
    int seconds = 0;
};

struct StretchRoutine {
    QString id;
    QString title;
    QString summary;
    int seconds = 0;
    QVector<StretchStep> steps;
};

class StretchFigureView;

class StretchFeature final : public forkmesh::market::NativePackageWidget
{
public:
    using ReplyHandler =
        std::function<void(bool, const QJsonObject &, const QString &)>;
    using Request = std::function<void(const QByteArray &, const QString &,
                                       const QJsonObject &, ReplyHandler)>;

    explicit StretchFeature(const forkmesh::market::NativePackageHost &host,
                            QWidget *parent = nullptr);
    ~StretchFeature() override;
    void refresh() override;
    bool exportData(forkmesh::market::NativePackageReplyHandler done) override;
    QList<forkmesh::market::NativePackageSearchHit>
    searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;

    static StretchFigure parseFigure(const QJsonObject &figure);
    static QHash<QString, StretchMotion> parseMotions(const QJsonObject &motions,
                                                      const QJsonArray &poseFields);
    static QVector<StretchRoutine> parseRoutines(const QJsonArray &routines);
    static StretchPose poseAt(const StretchMotion &motion, double seconds);
    static StretchBody bodyFor(const StretchFigure &figure, const StretchPose &pose,
                               const QString &tap, double seconds);
    static QString clockLabel(int seconds);
    static QString minutesLabel(int seconds);
    static QString withDay(const QString &path, const QString &day);
    static QString errorText(const QJsonObject &payload, const QString &error);
    static bool exportHasData(const QJsonObject &payload);
    static int loggedSeconds(qint64 activeMs);
    static bool shouldLogStop(qint64 activeMs);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    enum class Voice { None, MacSay, WindowsSpeech, EspeakNg, Espeak, SpdSay };

    void request(const QByteArray &method, const QString &path,
                 const QJsonObject &body, ReplyHandler handler);
    QString apiPath(const QString &suffix = QString()) const;
    QString today() const;
    void applyInstallState();
    void installPlugin();
    void load();
    void applyLibrary(const QJsonObject &library);
    void render();
    void renderRoutines();
    void renderRoutineDetail();
    void renderRecent();
    const StretchRoutine *routine(const QString &id) const;
    void startSession();
    void tick();
    void advanceTo(int index);
    void finishSession();
    void stopSession();
    void endSession(bool completed);
    void togglePause();
    void stepBack();
    void stepSkip();
    void renderSession();
    void updateClockRate();
    void logSession(const QJsonObject &session);
    void deleteSession(const QString &id);
    void saveVoice(bool on);
    void setVoiceState(bool on);
    void speak(const QString &line);
    void silence();
    void exportMine();
    void uninstallPlugin();
    bool saveExportBundle(const QJsonObject &payload);
    void setStatus(const QString &text, bool error = false);
    static Voice detectVoice(QString *program);
    QString voiceName() const;

    Request m_request;
    std::function<void()> m_searchIndexChanged;
    QLabel *m_status = nullptr;
    QWidget *m_installPanel = nullptr;
    QPushButton *m_installButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_uninstallButton = nullptr;
    QWidget *m_content = nullptr;
    QWidget *m_pickerPanel = nullptr;
    QComboBox *m_routine = nullptr;
    QLabel *m_routineSummary = nullptr;
    QLabel *m_routineSteps = nullptr;
    QCheckBox *m_voiceCheck = nullptr;
    QLabel *m_voiceNote = nullptr;
    QPushButton *m_startButton = nullptr;
    QLabel *m_todayLabel = nullptr;
    QLabel *m_streakLabel = nullptr;
    QLabel *m_bestLabel = nullptr;
    QLabel *m_weekLabel = nullptr;
    QLabel *m_totalLabel = nullptr;
    QTableWidget *m_recent = nullptr;
    QLabel *m_recentEmpty = nullptr;
    QWidget *m_sessionPanel = nullptr;
    StretchFigureView *m_figure = nullptr;
    QLabel *m_sessionRoutine = nullptr;
    QLabel *m_stepTitle = nullptr;
    QLabel *m_stepSay = nullptr;
    QLabel *m_countdown = nullptr;
    QProgressBar *m_overall = nullptr;
    QLabel *m_overallLabel = nullptr;
    QLabel *m_nextLabel = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_backButton = nullptr;
    QPushButton *m_skipButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPushButton *m_voiceButton = nullptr;
    QTimer *m_clock = nullptr;
    QElapsedTimer m_lastTick;
    QProcess *m_speech = nullptr;
    QString m_voiceProgram;
    Voice m_voice = Voice::None;
    StretchFigure m_figureBox;
    QHash<QString, StretchMotion> m_motions;
    QVector<StretchRoutine> m_routines;
    StretchRoutine m_active;
    QJsonObject m_overview;
    QString m_notice;
    QString m_sessionDay;
    qint64 m_sessionAt = 0;
    qint64 m_activeMs = 0;
    qint64 m_stepMs = 0;
    int m_stepIndex = 0;
    bool m_halfSpoken = false;
    bool m_nextSpoken = false;
    bool m_running = false;
    bool m_paused = false;
    bool m_voiceOn = true;
    bool m_checked = false;
    bool m_installed = false;
    bool m_loading = false;
    bool m_reloadPending = false;
};

} // namespace forkmesh::stretch
