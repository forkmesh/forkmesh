#include "StretchFeature.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace forkmesh::stretch {
namespace {

constexpr int kVisibleTickMs = 33;
constexpr int kHiddenTickMs = 250;
constexpr qint64 kMaxTickMs = 1000;
constexpr qint64 kMinLoggedStopMs = 30000;
constexpr int kMaxSessionSeconds = 14400;
constexpr double kPi = 3.14159265358979323846;

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

int integer(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toInt();
}

double number(const QJsonObject &object, const char *key, double fallback)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isDouble() ? value.toDouble() : fallback;
}

QPointF point(const QJsonObject &object, const char *key, const QPointF &fallback)
{
    const QJsonArray pair = object.value(QLatin1String(key)).toArray();
    if (pair.size() != 2 || !pair.at(0).isDouble() || !pair.at(1).isDouble())
        return fallback;
    return QPointF(pair.at(0).toDouble(), pair.at(1).toDouble());
}

QLabel *mutedLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QLabel *sectionLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

QString plural(int count, const QString &one, const QString &many)
{
    return count == 1 ? QStringLiteral("1 %1").arg(one) : QStringLiteral("%1 %2").arg(count).arg(many);
}

QString dateLabel(const QString &day)
{
    const QDate date = QDate::fromString(day, Qt::ISODate);
    if (!date.isValid())
        return day;
    if (date == QDate::currentDate())
        return QStringLiteral("Today");
    if (date == QDate::currentDate().addDays(-1))
        return QStringLiteral("Yesterday");
    return QLocale().toString(date, QStringLiteral("ddd d MMM"));
}

QString sourceLabel(const QString &source)
{
    if (source == QLatin1String("ios"))
        return QStringLiteral("iPhone");
    if (source == QLatin1String("android"))
        return QStringLiteral("Android");
    if (source == QLatin1String("desktop"))
        return QStringLiteral("Desktop");
    return QStringLiteral("Web");
}

void fitTable(QTableWidget *table, int maxRows)
{
    int height = table->horizontalHeader()->sizeHint().height() + 2 * table->frameWidth();
    for (int row = 0; row < std::min(table->rowCount(), maxRows); ++row)
        height += table->rowHeight(row);
    table->setFixedHeight(height);
}

using PoseMember = double StretchPose::*;

const QList<QPair<QLatin1String, PoseMember>> &poseMembers()
{
    static const QList<QPair<QLatin1String, PoseMember>> members = {
        {QLatin1String("hipX"), &StretchPose::hipX},
        {QLatin1String("knees"), &StretchPose::knees},
        {QLatin1String("lean"), &StretchPose::lean},
        {QLatin1String("twist"), &StretchPose::twist},
        {QLatin1String("fold"), &StretchPose::fold},
        {QLatin1String("shrug"), &StretchPose::shrug},
        {QLatin1String("headTilt"), &StretchPose::headTilt},
        {QLatin1String("headTurn"), &StretchPose::headTurn},
        {QLatin1String("headNod"), &StretchPose::headNod},
        {QLatin1String("lArm"), &StretchPose::lArm},
        {QLatin1String("lElbow"), &StretchPose::lElbow},
        {QLatin1String("rArm"), &StretchPose::rArm},
        {QLatin1String("rElbow"), &StretchPose::rElbow},
    };
    return members;
}

QPointF rotated(double degrees, double x, double y)
{
    const double radians = degrees * kPi / 180.0;
    return QPointF(x * std::cos(radians) - y * std::sin(radians), x * std::sin(radians) + y * std::cos(radians));
}

} // namespace

class StretchFigureView final : public QWidget
{
public:
    explicit StretchFigureView(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(220, 290);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAccessibleName(QStringLiteral("Example person"));
    }

    void setBody(const StretchFigure &figure, const StretchBody &body)
    {
        m_figure = figure;
        m_body = body;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (m_figure.width <= 0 || m_figure.height <= 0)
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const double scale = std::min(width() / m_figure.width, height() / m_figure.height);
        painter.translate((width() - m_figure.width * scale) / 2.0, (height() - m_figure.height * scale) / 2.0);
        painter.scale(scale, scale);
        const QColor ink = palette().color(QPalette::Highlight);
        const QColor background = palette().color(QPalette::Window);
        const auto pen = [&ink](double width) {
            return QPen(ink, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        };
        painter.setBrush(Qt::NoBrush);
        painter.setPen(pen(10));
        const QPointF leftHip = m_body.hip - QPointF(m_figure.hipHalf, 0);
        const QPointF rightHip = m_body.hip + QPointF(m_figure.hipHalf, 0);
        const QPointF leftLeg[] = {leftHip, m_body.leftKnee, m_body.leftFoot};
        const QPointF rightLeg[] = {rightHip, m_body.rightKnee, m_body.rightFoot};
        painter.drawPolyline(leftLeg, 3);
        painter.drawPolyline(rightLeg, 3);
        painter.setPen(pen(14));
        painter.drawLine(m_body.hip, m_body.chest);
        painter.setPen(pen(10));
        painter.drawLine(m_body.leftShoulder, m_body.rightShoulder);
        painter.setPen(pen(9));
        painter.drawLine(m_body.leftShoulder, m_body.leftElbow);
        painter.drawLine(m_body.rightShoulder, m_body.rightElbow);
        painter.setPen(pen(8));
        painter.drawLine(m_body.leftElbow, m_body.leftHand);
        painter.drawLine(m_body.rightElbow, m_body.rightHand);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawEllipse(m_body.headCentre, m_body.headRadius, m_body.headRadius);
        painter.setBrush(background);
        painter.drawEllipse(m_body.leftEye, 2.0, 2.0);
        painter.drawEllipse(m_body.rightEye, 2.0, 2.0);
    }

private:
    StretchFigure m_figure;
    StretchBody m_body;
};

StretchFigure StretchFeature::parseFigure(const QJsonObject &figure)
{
    StretchFigure result;
    result.width = number(figure, "width", result.width);
    result.height = number(figure, "height", result.height);
    result.hip = point(figure, "hip", result.hip);
    result.hipShift = number(figure, "hipShift", result.hipShift);
    result.kneeDip = number(figure, "kneeDip", result.kneeDip);
    result.torso = number(figure, "torso", result.torso);
    result.shoulderHalf = number(figure, "shoulderHalf", result.shoulderHalf);
    result.shrug = number(figure, "shrug", result.shrug);
    result.neck = number(figure, "neck", result.neck);
    result.head = number(figure, "head", result.head);
    result.upperArm = number(figure, "upperArm", result.upperArm);
    result.forearm = number(figure, "forearm", result.forearm);
    result.hipHalf = number(figure, "hipHalf", result.hipHalf);
    result.tapLift = number(figure, "tapLift", result.tapLift);
    result.tapHz = number(figure, "tapHz", result.tapHz);
    const QJsonArray feet = figure.value(QStringLiteral("feet")).toArray();
    if (feet.size() == 2) {
        const QJsonArray left = feet.at(0).toArray();
        const QJsonArray right = feet.at(1).toArray();
        if (left.size() == 2 && right.size() == 2) {
            result.leftFoot = QPointF(left.at(0).toDouble(), left.at(1).toDouble());
            result.rightFoot = QPointF(right.at(0).toDouble(), right.at(1).toDouble());
        }
    }
    return result;
}

QHash<QString, StretchMotion> StretchFeature::parseMotions(const QJsonObject &motions, const QJsonArray &poseFields)
{
    QStringList fields;
    for (const QJsonValue &value : poseFields)
        fields << value.toString();
    QHash<QString, StretchMotion> result;
    for (auto it = motions.begin(); it != motions.end(); ++it) {
        const QJsonObject source = it.value().toObject();
        StretchMotion motion;
        motion.period = number(source, "period", 4);
        motion.tap = text(source, "tap");
        for (const QJsonValue &value : source.value(QStringLiteral("frames")).toArray()) {
            const QJsonObject frame = value.toObject();
            StretchPose pose;
            for (const auto &member : poseMembers()) {
                if (!fields.isEmpty() && !fields.contains(member.first))
                    continue;
                const QJsonValue field = frame.value(member.first);
                if (field.isDouble())
                    pose.*(member.second) = field.toDouble();
            }
            motion.times << number(frame, "t", 0);
            motion.frames << pose;
        }
        if (!motion.frames.isEmpty())
            result.insert(it.key(), motion);
    }
    return result;
}

QVector<StretchRoutine> StretchFeature::parseRoutines(const QJsonArray &routines)
{
    QVector<StretchRoutine> result;
    for (const QJsonValue &value : routines) {
        const QJsonObject source = value.toObject();
        StretchRoutine routine;
        routine.id = text(source, "id");
        routine.title = text(source, "title");
        routine.summary = text(source, "summary");
        for (const QJsonValue &item : source.value(QStringLiteral("steps")).toArray()) {
            const QJsonObject object = item.toObject();
            StretchStep step;
            step.id = text(object, "id");
            step.title = text(object, "title");
            step.say = text(object, "say");
            step.half = text(object, "half");
            step.motion = text(object, "motion");
            step.seconds = integer(object, "seconds");
            if (step.seconds > 0)
                routine.steps << step;
        }
        for (const StretchStep &step : std::as_const(routine.steps))
            routine.seconds += step.seconds;
        if (!routine.id.isEmpty() && !routine.steps.isEmpty())
            result << routine;
    }
    return result;
}

StretchPose StretchFeature::poseAt(const StretchMotion &motion, double seconds)
{
    if (motion.frames.isEmpty())
        return {};
    if (motion.frames.size() == 1)
        return motion.frames.first();
    const double period = motion.period > 0 ? motion.period : 1.0;
    double phase = std::fmod(seconds, period) / period;
    if (phase < 0)
        phase += 1.0;
    int a = int(motion.frames.size()) - 2;
    for (int index = 0; index + 1 < motion.frames.size(); ++index) {
        if (motion.times.at(index) <= phase && phase <= motion.times.at(index + 1)) {
            a = index;
            break;
        }
    }
    const int b = a + 1;
    const double span = motion.times.at(b) - motion.times.at(a);
    double u = span > 0 ? (phase - motion.times.at(a)) / span : 0.0;
    u = std::clamp(u, 0.0, 1.0);
    u = u * u * (3.0 - 2.0 * u);
    StretchPose pose;
    const StretchPose &from = motion.frames.at(a);
    const StretchPose &to = motion.frames.at(b);
    for (const auto &member : poseMembers())
        pose.*(member.second) = from.*(member.second) + (to.*(member.second) - from.*(member.second)) * u;
    return pose;
}

StretchBody StretchFeature::bodyFor(const StretchFigure &figure, const StretchPose &pose, const QString &tap,
                                    double seconds)
{
    StretchBody body;
    body.hip = QPointF(figure.hip.x() + pose.hipX * figure.hipShift, figure.hip.y() + pose.knees * figure.kneeDip);
    const double torso = figure.torso * (1.0 - 0.5 * pose.fold);
    body.chest = body.hip + rotated(pose.lean, 0, -1) * torso;
    const double halfWidth = figure.shoulderHalf * (1.0 - 0.4 * std::abs(pose.twist));
    const QPointF side = rotated(pose.lean, 1, 0);
    const QPointF lift(0, pose.shrug * figure.shrug);
    body.leftShoulder = body.chest - side * halfWidth - lift;
    body.rightShoulder = body.chest + side * halfWidth - lift;
    const double headAngle = pose.lean + pose.headTilt;
    body.headCentre = body.chest + rotated(headAngle, 0, -1) * figure.neck +
                      QPointF(pose.twist * 3.0, pose.headNod * 6.0);
    body.headRadius = figure.head;
    body.leftEye = body.headCentre + rotated(headAngle, -5.0 + pose.headTurn * 5.0, -2.0 + pose.headNod * 3.0);
    body.rightEye = body.headCentre + rotated(headAngle, 5.0 + pose.headTurn * 5.0, -2.0 + pose.headNod * 3.0);
    const auto arm = [&figure, &pose](const QPointF &shoulder, double sign, double armDegrees, double elbowDegrees,
                                      QPointF *elbow, QPointF *hand) {
        const double upper = armDegrees * kPi / 180.0;
        const double lower = (armDegrees + elbowDegrees) * kPi / 180.0;
        *elbow = shoulder + rotated(pose.lean, sign * std::sin(upper), std::cos(upper)) * figure.upperArm;
        *hand = *elbow + rotated(pose.lean, sign * std::sin(lower), std::cos(lower)) * figure.forearm;
    };
    arm(body.leftShoulder, -1.0, pose.lArm, pose.lElbow, &body.leftElbow, &body.leftHand);
    arm(body.rightShoulder, 1.0, pose.rArm, pose.rElbow, &body.rightElbow, &body.rightHand);
    const double tapLift = figure.tapLift * std::abs(std::sin(2.0 * kPi * figure.tapHz * seconds));
    if (tap == QLatin1String("left") || tap == QLatin1String("both"))
        body.leftHand.ry() -= tapLift;
    if (tap == QLatin1String("right") || tap == QLatin1String("both"))
        body.rightHand.ry() -= tapLift;
    const QPointF leftHip = body.hip - QPointF(figure.hipHalf, 0);
    const QPointF rightHip = body.hip + QPointF(figure.hipHalf, 0);
    body.leftFoot = figure.leftFoot;
    body.rightFoot = figure.rightFoot;
    body.leftKnee = (leftHip + figure.leftFoot) / 2.0 - QPointF(pose.knees * 10.0, 0);
    body.rightKnee = (rightHip + figure.rightFoot) / 2.0 + QPointF(pose.knees * 10.0, 0);
    return body;
}

QString StretchFeature::clockLabel(int seconds)
{
    const int clamped = std::max(0, seconds);
    return QStringLiteral("%1:%2").arg(clamped / 60).arg(clamped % 60, 2, 10, QLatin1Char('0'));
}

QString StretchFeature::minutesLabel(int seconds)
{
    if (seconds <= 0)
        return QStringLiteral("0 min");
    if (seconds < 60)
        return QStringLiteral("%1 s").arg(seconds);
    return QStringLiteral("%1 min").arg(qRound(seconds / 60.0));
}

QString StretchFeature::withDay(const QString &path, const QString &day)
{
    return path + (path.contains(QLatin1Char('?')) ? QStringLiteral("&day=") : QStringLiteral("?day=")) + day;
}

QString StretchFeature::errorText(const QJsonObject &payload, const QString &error)
{
    const QString code = text(payload, "error");
    if (code == QLatin1String("invalid_routine"))
        return QStringLiteral("That routine is not known.");
    if (code == QLatin1String("invalid_seconds"))
        return QStringLiteral("The session length was out of range.");
    if (code == QLatin1String("invalid_day"))
        return QStringLiteral("The session day was not accepted.");
    if (code == QLatin1String("day_full"))
        return QStringLiteral("That day already holds as many sessions as it can.");
    if (code == QLatin1String("session_not_found"))
        return QStringLiteral("It was already removed. Refreshing.");
    if (code == QLatin1String("plugin_not_installed"))
        return QStringLiteral("Stretch Break is not installed for your account.");
    return error.isEmpty() ? code : error;
}

bool StretchFeature::exportHasData(const QJsonObject &payload)
{
    return !payload.value(QStringLiteral("export")).toObject().value(QStringLiteral("sessions")).toArray().isEmpty();
}

int StretchFeature::loggedSeconds(qint64 activeMs)
{
    return int(std::clamp<qint64>((activeMs + 500) / 1000, 1, kMaxSessionSeconds));
}

bool StretchFeature::shouldLogStop(qint64 activeMs)
{
    return activeMs >= kMinLoggedStopMs;
}

StretchFeature::StretchFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_request(host.request),
      m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("stretchSection"));
    m_voice = detectVoice(&m_voiceProgram);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Stretch Break"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addWidget(mutedLabel(QStringLiteral("A guided, spoken stretch for your back and posture, and a lymph tapping routine")));
    heading->addStretch();
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    m_exportButton = new QPushButton(QStringLiteral("Export my data"));
    m_uninstallButton = new QPushButton(QStringLiteral("Uninstall…"));
    for (QPushButton *button : {refreshButton, m_exportButton, m_uninstallButton}) {
        button->setObjectName(QStringLiteral("ghostButton"));
        heading->addWidget(button);
    }
    outer->addLayout(heading);

    m_status = mutedLabel(QStringLiteral("Loading Stretch Break…"));
    outer->addWidget(m_status);

    m_installPanel = new QWidget;
    auto *installLayout = new QVBoxLayout(m_installPanel);
    installLayout->setContentsMargins(8, 36, 8, 8);
    auto *installNote = new QLabel(QStringLiteral(
        "Stretch Break is private to your ForkMesh account. Install it for a guided stretch break: a voice "
        "tells you what to do, an example person does each move with you, and a countdown shows how long is "
        "left of each step. There is a desk stretch for your back and posture and a lymph tapping routine. "
        "Finished sessions show on the web, this desktop app and the ForkMesh iPhone app, which also saves "
        "them to Apple Health. Everything is encrypted at rest with a key minted for your account alone. "
        "This is general wellness guidance, not medical advice: move gently."));
    installNote->setAlignment(Qt::AlignHCenter);
    installNote->setWordWrap(true);
    installLayout->addWidget(installNote);
    auto *installRow = new QHBoxLayout;
    installRow->addStretch();
    m_installButton = new QPushButton(QStringLiteral("Install Stretch Break"));
    m_installButton->setObjectName(QStringLiteral("primaryButton"));
    installRow->addWidget(m_installButton);
    installRow->addStretch();
    installLayout->addLayout(installRow);
    installLayout->addStretch();
    outer->addWidget(m_installPanel, 1);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_content = scroll;
    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 6, 0, 0);
    layout->setSpacing(12);

    m_pickerPanel = new QWidget;
    auto *picker = new QVBoxLayout(m_pickerPanel);
    picker->setContentsMargins(0, 0, 0, 0);
    picker->setSpacing(8);
    auto *pickerTitle = new QLabel(QStringLiteral("Start a stretch break"));
    pickerTitle->setStyleSheet(QStringLiteral("font-size: 22px; font-weight: 600;"));
    picker->addWidget(pickerTitle);
    auto *routineRow = new QHBoxLayout;
    routineRow->addWidget(new QLabel(QStringLiteral("Routine")));
    m_routine = new QComboBox;
    m_routine->setObjectName(QStringLiteral("stretchRoutine"));
    routineRow->addWidget(m_routine);
    m_startButton = new QPushButton(QStringLiteral("Start"));
    m_startButton->setObjectName(QStringLiteral("primaryButton"));
    m_startButton->setMinimumWidth(140);
    routineRow->addWidget(m_startButton);
    routineRow->addStretch();
    picker->addLayout(routineRow);
    auto *voiceRow = new QHBoxLayout;
    m_voiceCheck = new QCheckBox(QStringLiteral("Speak the cues"));
    voiceRow->addWidget(m_voiceCheck);
    m_voiceNote = mutedLabel(QString());
    voiceRow->addWidget(m_voiceNote, 1);
    picker->addLayout(voiceRow);
    m_routineSummary = mutedLabel(QString());
    picker->addWidget(m_routineSummary);
    m_routineSteps = new QLabel;
    m_routineSteps->setWordWrap(true);
    m_routineSteps->setTextInteractionFlags(Qt::TextSelectableByMouse);
    picker->addWidget(m_routineSteps);
    picker->addWidget(mutedLabel(QStringLiteral(
        "General wellness guidance, not medical advice. Move gently and stop if anything hurts.")));

    auto *stats = new QGridLayout;
    stats->setHorizontalSpacing(24);
    m_todayLabel = new QLabel;
    m_streakLabel = new QLabel;
    m_bestLabel = new QLabel;
    m_weekLabel = new QLabel;
    m_totalLabel = new QLabel;
    const QList<QPair<QString, QLabel *>> statRows = {
        {QStringLiteral("Today"), m_todayLabel},
        {QStringLiteral("Streak"), m_streakLabel},
        {QStringLiteral("Best streak"), m_bestLabel},
        {QStringLiteral("Last 7 days"), m_weekLabel},
        {QStringLiteral("All sessions"), m_totalLabel},
    };
    for (int index = 0; index < statRows.size(); ++index) {
        statRows.at(index).second->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
        stats->addWidget(mutedLabel(statRows.at(index).first), 0, index);
        stats->addWidget(statRows.at(index).second, 1, index);
    }
    stats->setColumnStretch(int(statRows.size()), 1);
    picker->addSpacing(6);
    picker->addLayout(stats);
    picker->addWidget(sectionLabel(QStringLiteral("Recent sessions")));
    m_recent = new QTableWidget(0, 6);
    m_recent->setHorizontalHeaderLabels({QStringLiteral("When"), QStringLiteral("Routine"), QStringLiteral("Length"),
                                         QStringLiteral("Steps"), QStringLiteral("From"), QString()});
    m_recent->verticalHeader()->setVisible(false);
    m_recent->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recent->setSelectionMode(QAbstractItemView::NoSelection);
    m_recent->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_recent->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    picker->addWidget(m_recent);
    m_recentEmpty = mutedLabel(QStringLiteral("No sessions yet. Pick a routine and press Start."));
    picker->addWidget(m_recentEmpty);
    layout->addWidget(m_pickerPanel);

    m_sessionPanel = new QWidget;
    auto *session = new QHBoxLayout(m_sessionPanel);
    session->setContentsMargins(0, 0, 0, 0);
    session->setSpacing(20);
    m_figure = new StretchFigureView;
    session->addWidget(m_figure, 2);
    auto *side = new QVBoxLayout;
    side->setSpacing(8);
    m_sessionRoutine = mutedLabel(QString());
    side->addWidget(m_sessionRoutine);
    m_stepTitle = new QLabel;
    m_stepTitle->setWordWrap(true);
    m_stepTitle->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: 600;"));
    side->addWidget(m_stepTitle);
    m_stepSay = new QLabel;
    m_stepSay->setWordWrap(true);
    m_stepSay->setStyleSheet(QStringLiteral("font-size: 15px;"));
    side->addWidget(m_stepSay);
    m_countdown = new QLabel(QStringLiteral("0:00"));
    m_countdown->setObjectName(QStringLiteral("stretchCountdown"));
    m_countdown->setStyleSheet(QStringLiteral("font-size: 64px; font-weight: 700;"));
    m_countdown->setAccessibleName(QStringLiteral("Time left in this step"));
    side->addWidget(m_countdown);
    m_overall = new QProgressBar;
    m_overall->setTextVisible(false);
    m_overall->setMaximumHeight(10);
    side->addWidget(m_overall);
    m_overallLabel = mutedLabel(QString());
    side->addWidget(m_overallLabel);
    m_nextLabel = new QLabel;
    m_nextLabel->setWordWrap(true);
    side->addWidget(m_nextLabel);
    side->addStretch();
    auto *controls = new QHBoxLayout;
    m_pauseButton = new QPushButton(QStringLiteral("Pause"));
    m_pauseButton->setObjectName(QStringLiteral("primaryButton"));
    m_backButton = new QPushButton(QStringLiteral("Back"));
    m_skipButton = new QPushButton(QStringLiteral("Skip"));
    m_stopButton = new QPushButton(QStringLiteral("Stop"));
    m_stopButton->setObjectName(QStringLiteral("dangerButton"));
    m_voiceButton = new QPushButton(QStringLiteral("Voice on"));
    m_voiceButton->setCheckable(true);
    for (QPushButton *button : {m_backButton, m_skipButton, m_voiceButton})
        button->setObjectName(QStringLiteral("ghostButton"));
    for (QPushButton *button : {m_pauseButton, m_backButton, m_skipButton, m_stopButton, m_voiceButton})
        controls->addWidget(button);
    controls->addStretch();
    side->addLayout(controls);
    session->addLayout(side, 3);
    m_sessionPanel->setVisible(false);
    layout->addWidget(m_sessionPanel, 1);
    layout->addStretch();
    scroll->setWidget(body);
    outer->addWidget(m_content, 1);

    m_clock = new QTimer(this);
    m_clock->setTimerType(Qt::PreciseTimer);
    connect(m_clock, &QTimer::timeout, this, [this] { tick(); });

    connect(refreshButton, &QPushButton::clicked, this, [this] { load(); });
    connect(m_installButton, &QPushButton::clicked, this, [this] { installPlugin(); });
    connect(m_exportButton, &QPushButton::clicked, this, [this] { exportMine(); });
    connect(m_uninstallButton, &QPushButton::clicked, this, [this] { uninstallPlugin(); });
    connect(m_routine, &QComboBox::currentIndexChanged, this, [this] { renderRoutineDetail(); });
    connect(m_startButton, &QPushButton::clicked, this, [this] { startSession(); });
    connect(m_voiceCheck, &QCheckBox::toggled, this, [this](bool on) { saveVoice(on); });
    connect(m_voiceButton, &QPushButton::toggled, this, [this](bool on) { saveVoice(on); });
    connect(m_pauseButton, &QPushButton::clicked, this, [this] { togglePause(); });
    connect(m_backButton, &QPushButton::clicked, this, [this] { stepBack(); });
    connect(m_skipButton, &QPushButton::clicked, this, [this] { stepSkip(); });
    connect(m_stopButton, &QPushButton::clicked, this, [this] { stopSession(); });

    setVoiceState(true);
    applyInstallState();
}

StretchFeature::~StretchFeature()
{
    if (m_speech && m_speech->state() != QProcess::NotRunning)
        m_speech->kill();
}

void StretchFeature::request(const QByteArray &method, const QString &path, const QJsonObject &body,
                             ReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("Stretch Break network access is unavailable."));
        return;
    }
    const QPointer<StretchFeature> guard(this);
    m_request(method, path, body,
              [guard, handler = std::move(handler)](bool ok, const QJsonObject &payload,
                                                    const QString &error) mutable {
        if (!guard)
            return;
        handler(ok, payload, error);
        if (guard && !ok && text(payload, "error") == QLatin1String("plugin_not_installed"))
            guard->load();
    });
}

QString StretchFeature::apiPath(const QString &suffix) const
{
    return QStringLiteral("/api/stretch") + suffix;
}

QString StretchFeature::today() const
{
    return QDate::currentDate().toString(Qt::ISODate);
}

void StretchFeature::refresh()
{
    load();
}

QList<forkmesh::market::NativePackageSearchHit> StretchFeature::searchHits(const QString &query, int limit) const
{
    const QString needle = query.trimmed().toLower();
    if (!m_installed || limit <= 0 || needle.size() < 3)
        return {};
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const bool page = QStringLiteral("stretch break posture back neck exercise").contains(needle);
    if (m_running && page)
        hits << forkmesh::market::NativePackageSearchHit{
            QStringLiteral("session"), QStringLiteral("Stretch Break: %1").arg(m_active.title),
            QStringLiteral("Session running")};
    for (const StretchRoutine &item : m_routines) {
        if (hits.size() >= limit)
            break;
        if (!page && !item.title.toLower().contains(needle) && !item.summary.toLower().contains(needle))
            continue;
        hits << forkmesh::market::NativePackageSearchHit{
            QStringLiteral("routine:") + item.id, QStringLiteral("Stretch: %1").arg(item.title),
            QStringLiteral("%1 · %2").arg(clockLabel(item.seconds),
                                          plural(int(item.steps.size()), QStringLiteral("step"), QStringLiteral("steps")))};
    }
    return hits.mid(0, limit);
}

void StretchFeature::activateSearchHit(const QString &id)
{
    if (id.startsWith(QLatin1String("routine:")) && !m_running) {
        const int index = m_routine->findData(id.mid(8));
        if (index >= 0)
            m_routine->setCurrentIndex(index);
    }
    if (!m_running)
        load();
}

void StretchFeature::applyInstallState()
{
    m_installPanel->setVisible(m_checked && !m_installed);
    m_content->setVisible(m_installed);
    m_exportButton->setVisible(m_installed);
    m_uninstallButton->setVisible(m_installed);
}

void StretchFeature::installPlugin()
{
    m_installButton->setEnabled(false);
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/install")),
            QJsonObject{{QStringLiteral("voice"), true},
                        {QStringLiteral("routine"), QStringLiteral("desk")},
                        {QStringLiteral("tzOffsetMinutes"), -QDateTime::currentDateTime().offsetFromUtc() / 60}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_installButton->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("Stretch Break was not installed: %1").arg(errorText(payload, error)), true);
        setStatus(QStringLiteral("Stretch Break installed for your account."));
        load();
    });
}

void StretchFeature::load()
{
    if (m_loading) {
        m_reloadPending = true;
        return;
    }
    m_loading = true;
    m_reloadPending = false;
    request(QByteArrayLiteral("GET"), withDay(apiPath(), today()), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        if (m_reloadPending)
            return load();
        if (!ok) {
            setStatus(QStringLiteral("Stretch Break could not load: %1").arg(errorText(payload, error)), true);
            return;
        }
        m_checked = true;
        m_installed = payload.value(QStringLiteral("installed")).toBool();
        applyLibrary(payload.value(QStringLiteral("library")).toObject());
        if (!m_installed && m_running) {
            m_clock->stop();
            silence();
            m_running = false;
            m_paused = false;
        }
        applyInstallState();
        if (!m_installed) {
            m_overview = QJsonObject();
            setStatus(QStringLiteral("Stretch Break is not installed for your account yet."));
            return;
        }
        m_overview = payload;
        render();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
    });
}

void StretchFeature::applyLibrary(const QJsonObject &library)
{
    if (library.isEmpty())
        return;
    m_figureBox = parseFigure(library.value(QStringLiteral("figure")).toObject());
    m_motions = parseMotions(library.value(QStringLiteral("motions")).toObject(),
                             library.value(QStringLiteral("poseFields")).toArray());
    m_routines = parseRoutines(library.value(QStringLiteral("routines")).toArray());
}

const StretchRoutine *StretchFeature::routine(const QString &id) const
{
    for (const StretchRoutine &item : m_routines)
        if (item.id == id)
            return &item;
    return nullptr;
}

void StretchFeature::renderRoutines()
{
    const QJsonObject profile = m_overview.value(QStringLiteral("profile")).toObject();
    const QString preferred = text(profile, "routine").isEmpty() ? QStringLiteral("desk") : text(profile, "routine");
    const QString chosen = m_routine->count() > 0 ? m_routine->currentData().toString() : preferred;
    {
        const QSignalBlocker blocker(m_routine);
        m_routine->clear();
        for (const StretchRoutine &item : std::as_const(m_routines))
            m_routine->addItem(QStringLiteral("%1 · %2").arg(item.title, clockLabel(item.seconds)), item.id);
        int index = m_routine->findData(chosen);
        if (index < 0)
            index = m_routine->findData(preferred);
        m_routine->setCurrentIndex(index < 0 ? 0 : index);
    }
    renderRoutineDetail();
}

void StretchFeature::renderRoutineDetail()
{
    const StretchRoutine *item = routine(m_routine->currentData().toString());
    m_startButton->setEnabled(item && !m_running);
    if (!item) {
        m_routineSummary->setText(QStringLiteral("The routines did not load. Press Refresh."));
        m_routineSteps->clear();
        return;
    }
    m_routineSummary->setText(QStringLiteral("%1 %2 in total, %3.")
                                  .arg(item->summary, clockLabel(item->seconds))
                                  .arg(plural(int(item->steps.size()), QStringLiteral("step"), QStringLiteral("steps"))));
    QStringList lines;
    for (int index = 0; index < item->steps.size(); ++index)
        lines << QStringLiteral("%1. %2 · %3")
                     .arg(index + 1)
                     .arg(item->steps.at(index).title, clockLabel(item->steps.at(index).seconds));
    m_routineSteps->setText(lines.join(QLatin1Char('\n')));
}

void StretchFeature::renderRecent()
{
    const QJsonArray recent = m_overview.value(QStringLiteral("recent")).toArray();
    m_recent->setRowCount(0);
    for (const QJsonValue &value : recent) {
        const QJsonObject item = value.toObject();
        const int row = m_recent->rowCount();
        m_recent->insertRow(row);
        const qint64 at = qint64(item.value(QStringLiteral("at")).toDouble());
        const qint64 logged = qint64(item.value(QStringLiteral("loggedAt")).toDouble());
        const QDateTime when = QDateTime::fromMSecsSinceEpoch(at > 0 ? at : logged);
        m_recent->setItem(row, 0, new QTableWidgetItem(QStringLiteral("%1 %2").arg(
                                      dateLabel(text(item, "day")), QLocale().toString(when.time(), QLocale::ShortFormat))));
        const StretchRoutine *known = routine(text(item, "routine"));
        m_recent->setItem(row, 1, new QTableWidgetItem(known ? known->title : text(item, "routine")));
        m_recent->setItem(row, 2, new QTableWidgetItem(clockLabel(integer(item, "seconds"))));
        const bool completed = item.value(QStringLiteral("completed")).toBool();
        m_recent->setItem(row, 3, new QTableWidgetItem(
                                      completed ? QStringLiteral("All %1").arg(integer(item, "stepsTotal"))
                                                : QStringLiteral("%1 of %2, stopped early")
                                                      .arg(integer(item, "steps"))
                                                      .arg(integer(item, "stepsTotal"))));
        QString from = sourceLabel(text(item, "source"));
        if (item.value(QStringLiteral("health")).toBool())
            from += QStringLiteral(" · Apple Health");
        m_recent->setItem(row, 4, new QTableWidgetItem(from));
        auto *remove = new QPushButton(QStringLiteral("Delete"));
        remove->setObjectName(QStringLiteral("ghostButton"));
        const QString id = text(item, "id");
        connect(remove, &QPushButton::clicked, this, [this, id] { deleteSession(id); });
        m_recent->setCellWidget(row, 5, remove);
    }
    fitTable(m_recent, 8);
    m_recent->setVisible(!recent.isEmpty());
    m_recentEmpty->setVisible(recent.isEmpty());
}

void StretchFeature::render()
{
    const QJsonObject profile = m_overview.value(QStringLiteral("profile")).toObject();
    const QJsonObject summary = m_overview.value(QStringLiteral("summary")).toObject();
    const QJsonObject todayInfo = m_overview.value(QStringLiteral("today")).toObject();
    setVoiceState(profile.value(QStringLiteral("voice")).toBool(true));
    renderRoutines();
    m_pickerPanel->setVisible(!m_running);
    m_sessionPanel->setVisible(m_running);
    const int todaySessions = integer(todayInfo, "sessions");
    m_todayLabel->setText(todaySessions == 0
                              ? QStringLiteral("Not yet")
                              : QStringLiteral("%1 · %2").arg(plural(todaySessions, QStringLiteral("session"),
                                                                     QStringLiteral("sessions")),
                                                              minutesLabel(integer(todayInfo, "seconds"))));
    m_streakLabel->setText(plural(integer(summary, "streak"), QStringLiteral("day"), QStringLiteral("days")));
    m_bestLabel->setText(plural(integer(summary, "bestStreak"), QStringLiteral("day"), QStringLiteral("days")));
    m_weekLabel->setText(QStringLiteral("%1 · %2").arg(
        plural(integer(summary, "weekSessions"), QStringLiteral("session"), QStringLiteral("sessions")),
        minutesLabel(integer(summary, "weekSeconds"))));
    m_totalLabel->setText(QString::number(integer(summary, "sessions")));
    renderRecent();
    if (!m_running)
        setStatus(todaySessions > 0
                      ? QStringLiteral("You have stretched today. Streak: %1.")
                            .arg(plural(integer(summary, "streak"), QStringLiteral("day"), QStringLiteral("days")))
                      : QStringLiteral("Stand up, pick a routine and press Start."));
    if (!m_notice.isEmpty()) {
        setStatus(m_notice);
        m_notice.clear();
    }
}

void StretchFeature::startSession()
{
    const StretchRoutine *item = routine(m_routine->currentData().toString());
    if (!item || item->steps.isEmpty())
        return setStatus(QStringLiteral("Pick a routine first."), true);
    m_active = *item;
    m_sessionAt = QDateTime::currentMSecsSinceEpoch();
    m_sessionDay = today();
    m_activeMs = 0;
    m_running = true;
    m_paused = false;
    m_pickerPanel->setVisible(false);
    m_sessionPanel->setVisible(true);
    m_pauseButton->setText(QStringLiteral("Pause"));
    m_sessionRoutine->setText(m_active.title);
    setStatus(QStringLiteral("%1 started. Follow the example person.").arg(m_active.title));
    m_lastTick.start();
    updateClockRate();
    m_clock->start();
    advanceTo(0);
}

void StretchFeature::advanceTo(int index)
{
    m_stepIndex = std::clamp(index, 0, int(m_active.steps.size()) - 1);
    m_stepMs = 0;
    m_halfSpoken = false;
    m_nextSpoken = false;
    const StretchStep &step = m_active.steps.at(m_stepIndex);
    speak(QStringLiteral("%1. %2").arg(step.title, step.say));
    renderSession();
}

void StretchFeature::tick()
{
    const qint64 delta = std::clamp<qint64>(m_lastTick.restart(), 0, kMaxTickMs);
    if (!m_running || m_active.steps.isEmpty())
        return;
    if (!m_paused) {
        m_activeMs += delta;
        m_stepMs += delta;
    }
    const StretchStep &step = m_active.steps.at(m_stepIndex);
    const qint64 stepTotal = qint64(step.seconds) * 1000;
    const bool hasNext = m_stepIndex + 1 < m_active.steps.size();
    if (m_stepMs >= stepTotal) {
        if (!hasNext)
            return finishSession();
        advanceTo(m_stepIndex + 1);
        return;
    }
    if (!m_paused && !m_halfSpoken && step.seconds >= 20 && !step.half.isEmpty() && m_stepMs * 2 >= stepTotal) {
        m_halfSpoken = true;
        speak(step.half);
    }
    if (!m_paused && !m_nextSpoken && step.seconds >= 15 && hasNext && m_stepMs >= stepTotal - 3000) {
        m_nextSpoken = true;
        speak(QStringLiteral("Next: %1").arg(m_active.steps.at(m_stepIndex + 1).title));
    }
    renderSession();
}

void StretchFeature::renderSession()
{
    if (!m_running || m_active.steps.isEmpty())
        return;
    const StretchStep &step = m_active.steps.at(m_stepIndex);
    const qint64 stepTotal = qint64(step.seconds) * 1000;
    m_stepTitle->setText(step.title);
    m_stepSay->setText(step.say);
    m_countdown->setText(clockLabel(int((std::max<qint64>(0, stepTotal - m_stepMs) + 999) / 1000)));
    qint64 doneMs = m_stepMs;
    for (int index = 0; index < m_stepIndex; ++index)
        doneMs += qint64(m_active.steps.at(index).seconds) * 1000;
    const int total = std::max(1, m_active.seconds);
    m_overall->setRange(0, total * 10);
    m_overall->setValue(int(std::clamp<qint64>(doneMs / 100, 0, qint64(total) * 10)));
    m_overallLabel->setText(QStringLiteral("Step %1 of %2 · %3 of %4%5")
                                .arg(m_stepIndex + 1)
                                .arg(m_active.steps.size())
                                .arg(clockLabel(int(doneMs / 1000)), clockLabel(m_active.seconds),
                                     m_paused ? QStringLiteral(" · paused") : QString()));
    m_nextLabel->setText(m_stepIndex + 1 < m_active.steps.size()
                             ? QStringLiteral("Next: %1").arg(m_active.steps.at(m_stepIndex + 1).title)
                             : QStringLiteral("Last step"));
    m_backButton->setEnabled(m_stepIndex > 0 || m_stepMs > 0);
    if (m_figure->isVisible()) {
        const double seconds = m_stepMs / 1000.0;
        const auto motion = m_motions.constFind(step.motion);
        if (motion == m_motions.constEnd())
            m_figure->setBody(m_figureBox, bodyFor(m_figureBox, StretchPose{}, QString(), seconds));
        else
            m_figure->setBody(m_figureBox, bodyFor(m_figureBox, poseAt(*motion, seconds), motion->tap, seconds));
    }
}

void StretchFeature::updateClockRate()
{
    m_clock->setInterval(isVisible() ? kVisibleTickMs : kHiddenTickMs);
}

void StretchFeature::showEvent(QShowEvent *event)
{
    forkmesh::market::NativePackageWidget::showEvent(event);
    updateClockRate();
    renderSession();
}

void StretchFeature::hideEvent(QHideEvent *event)
{
    forkmesh::market::NativePackageWidget::hideEvent(event);
    updateClockRate();
}

void StretchFeature::togglePause()
{
    if (!m_running)
        return;
    m_paused = !m_paused;
    m_pauseButton->setText(m_paused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    if (m_paused)
        silence();
    renderSession();
}

void StretchFeature::stepBack()
{
    if (!m_running)
        return;
    advanceTo(m_stepMs >= 3000 || m_stepIndex == 0 ? m_stepIndex : m_stepIndex - 1);
}

void StretchFeature::stepSkip()
{
    if (!m_running)
        return;
    if (m_stepIndex + 1 >= m_active.steps.size())
        return finishSession();
    advanceTo(m_stepIndex + 1);
}

void StretchFeature::finishSession()
{
    speak(QStringLiteral("Well done. That is your %1 finished.").arg(m_active.title));
    endSession(true);
}

void StretchFeature::stopSession()
{
    if (!m_running)
        return;
    silence();
    endSession(false);
}

void StretchFeature::endSession(bool completed)
{
    m_clock->stop();
    m_running = false;
    m_paused = false;
    m_sessionPanel->setVisible(false);
    m_pickerPanel->setVisible(true);
    renderRoutineDetail();
    const int stepsTotal = int(m_active.steps.size());
    if (!completed && !shouldLogStop(m_activeMs)) {
        setStatus(QStringLiteral("Stopped after %1. Sessions under 30 seconds are not logged.")
                      .arg(clockLabel(int(m_activeMs / 1000))));
        return;
    }
    logSession(QJsonObject{
        {QStringLiteral("routine"), m_active.id},
        {QStringLiteral("seconds"), loggedSeconds(m_activeMs)},
        {QStringLiteral("steps"), completed ? stepsTotal : m_stepIndex},
        {QStringLiteral("stepsTotal"), stepsTotal},
        {QStringLiteral("completed"), completed},
        {QStringLiteral("at"), double(m_sessionAt)},
        {QStringLiteral("day"), m_sessionDay},
        {QStringLiteral("source"), QStringLiteral("desktop")},
        {QStringLiteral("health"), false},
    });
}

void StretchFeature::logSession(const QJsonObject &session)
{
    const bool completed = session.value(QStringLiteral("completed")).toBool();
    const QString title = m_active.title;
    const int seconds = session.value(QStringLiteral("seconds")).toInt();
    request(QByteArrayLiteral("POST"), withDay(apiPath(QStringLiteral("/sessions")), today()), session,
            [this, completed, title, seconds](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("The session was not logged: %1").arg(errorText(payload, error)), true);
            return load();
        }
        m_notice = completed ? QStringLiteral("Well done. %1 finished and logged (%2).").arg(title, clockLabel(seconds))
                             : QStringLiteral("Stopped early. %1 logged as not completed (%2).")
                                   .arg(title, clockLabel(seconds));
        load();
    });
}

void StretchFeature::deleteSession(const QString &id)
{
    if (id.isEmpty())
        return;
    if (QMessageBox::question(this, QStringLiteral("Delete session"),
                              QStringLiteral("Delete this stretch session from your history?"),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    request(QByteArrayLiteral("DELETE"), withDay(apiPath(QStringLiteral("/sessions/") + id), today()), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            setStatus(QStringLiteral("The session was not deleted: %1").arg(errorText(payload, error)), true);
        else
            m_notice = QStringLiteral("Session deleted.");
        load();
    });
}

void StretchFeature::setVoiceState(bool on)
{
    m_voiceOn = on;
    {
        const QSignalBlocker checkBlocker(m_voiceCheck);
        const QSignalBlocker buttonBlocker(m_voiceButton);
        m_voiceCheck->setChecked(on);
        m_voiceButton->setChecked(on);
    }
    m_voiceButton->setText(on ? QStringLiteral("Voice on") : QStringLiteral("Voice off"));
    if (m_voice == Voice::None)
        m_voiceNote->setText(QStringLiteral("No system voice was found on this computer, so the cues show as text only."));
    else
        m_voiceNote->setText(on ? QStringLiteral("Spoken by %1 on this computer; nothing is sent to a speech service.")
                                      .arg(voiceName())
                                : QStringLiteral("The cues show as text only."));
    if (!on)
        silence();
}

void StretchFeature::saveVoice(bool on)
{
    setVoiceState(on);
    request(QByteArrayLiteral("PUT"), withDay(apiPath(QStringLiteral("/profile")), today()),
            QJsonObject{{QStringLiteral("voice"), on}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The voice setting was not saved: %1").arg(errorText(payload, error)), true);
        const QJsonObject profile = payload.value(QStringLiteral("profile")).toObject();
        if (!profile.isEmpty())
            m_overview.insert(QStringLiteral("profile"), profile);
    });
}

StretchFeature::Voice StretchFeature::detectVoice(QString *program)
{
    const auto find = [program](const QString &name) {
        *program = QStandardPaths::findExecutable(name);
        return !program->isEmpty();
    };
#if defined(Q_OS_MACOS)
    if (find(QStringLiteral("say")))
        return Voice::MacSay;
#elif defined(Q_OS_WIN)
    if (find(QStringLiteral("powershell")))
        return Voice::WindowsSpeech;
#else
    if (find(QStringLiteral("espeak-ng")))
        return Voice::EspeakNg;
    if (find(QStringLiteral("espeak")))
        return Voice::Espeak;
    if (find(QStringLiteral("spd-say")))
        return Voice::SpdSay;
#endif
    program->clear();
    return Voice::None;
}

QString StretchFeature::voiceName() const
{
    switch (m_voice) {
    case Voice::MacSay:
        return QStringLiteral("the macOS system voice");
    case Voice::WindowsSpeech:
        return QStringLiteral("the Windows system voice");
    case Voice::EspeakNg:
        return QStringLiteral("eSpeak NG");
    case Voice::Espeak:
        return QStringLiteral("eSpeak");
    case Voice::SpdSay:
        return QStringLiteral("Speech Dispatcher");
    case Voice::None:
        break;
    }
    return QString();
}

void StretchFeature::speak(const QString &line)
{
    silence();
    const QString spoken = line.simplified();
    if (!m_voiceOn || m_voice == Voice::None || m_voiceProgram.isEmpty() || spoken.isEmpty())
        return;
    QStringList arguments;
    QByteArray input;
    switch (m_voice) {
    case Voice::MacSay:
        input = spoken.toUtf8();
        break;
    case Voice::WindowsSpeech:
        arguments = {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-Command"),
                     QStringLiteral("[Console]::InputEncoding=[Text.Encoding]::UTF8;"
                                    "Add-Type -AssemblyName System.Speech;"
                                    "$s=New-Object System.Speech.Synthesis.SpeechSynthesizer;"
                                    "$s.Speak([Console]::In.ReadToEnd())")};
        input = spoken.toUtf8();
        break;
    case Voice::EspeakNg:
    case Voice::Espeak:
        arguments = {QStringLiteral("--stdin")};
        input = spoken.toUtf8();
        break;
    case Voice::SpdSay:
        arguments = {QStringLiteral("-w"), QStringLiteral("--"), spoken};
        break;
    case Voice::None:
        return;
    }
    auto *process = new QProcess(this);
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
#if defined(Q_OS_WIN)
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *create) {
        create->flags |= 0x08000000;
    });
#endif
    connect(process, &QProcess::finished, this, [this, process] {
        if (m_speech != process)
            return;
        m_speech = nullptr;
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_speech != process)
            return;
        m_speech = nullptr;
        process->deleteLater();
    });
    m_speech = process;
    process->start(m_voiceProgram, arguments);
    if (!input.isEmpty() && m_speech == process) {
        process->write(input);
        process->closeWriteChannel();
    }
}

void StretchFeature::silence()
{
    if (!m_speech)
        return;
    QProcess *previous = m_speech;
    m_speech = nullptr;
    if (previous->state() == QProcess::NotRunning) {
        previous->deleteLater();
        return;
    }
    connect(previous, &QProcess::finished, previous, &QObject::deleteLater);
    previous->kill();
}

bool StretchFeature::saveExportBundle(const QJsonObject &payload)
{
    const QString suggested = text(payload, "filename").isEmpty() ? QStringLiteral("forkmesh-stretch-export.json")
                                                                  : text(payload, "filename");
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save the Stretch Break export"), suggested,
                                                      QStringLiteral("JSON files (*.json)"));
    if (path.isEmpty())
        return false;
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(payload.value(QStringLiteral("export")).toObject())
                                 .toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        setStatus(QStringLiteral("The export file could not be written."), true);
        return false;
    }
    setStatus(QStringLiteral("Exported to %1.").arg(path));
    return true;
}

void StretchFeature::exportMine()
{
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/export")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The export failed: %1").arg(errorText(payload, error)), true);
        saveExportBundle(payload);
    });
}

bool StretchFeature::exportData(forkmesh::market::NativePackageReplyHandler done)
{
    if (m_checked && !m_installed)
        return false;
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/export")), {},
            [done = std::move(done)](bool ok, const QJsonObject &payload, const QString &error) {
        QJsonObject result = payload;
        result.insert(QStringLiteral("hasData"), exportHasData(payload));
        done(ok, result, error);
    });
    return true;
}

void StretchFeature::uninstallPlugin()
{
    if (QMessageBox::warning(
            this, QStringLiteral("Uninstall Stretch Break"),
            QStringLiteral("Uninstall Stretch Break from your account?\n\nYour stretch sessions are saved to a file "
                           "first, then deleted from the web dashboard, this desktop app, and the mobile app. "
                           "Apple Health keeps the workouts it already saved."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/export")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Uninstall stopped — the export failed: %1").arg(errorText(payload, error)),
                             true);
        if (exportHasData(payload) && !saveExportBundle(payload))
            return setStatus(QStringLiteral("Uninstall cancelled: the data file was not saved."), true);
        request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/uninstall")), {},
                [this](bool removed, const QJsonObject &removePayload, const QString &removeError) {
            if (!removed)
                return setStatus(QStringLiteral("Stretch Break was not uninstalled: %1")
                                     .arg(errorText(removePayload, removeError)),
                                 true);
            if (m_running) {
                m_clock->stop();
                silence();
                m_running = false;
                m_paused = false;
            }
            m_checked = true;
            m_installed = false;
            m_overview = QJsonObject();
            applyInstallState();
            setStatus(QStringLiteral("Stretch Break uninstalled from your account. Install it again at any time."));
        });
    });
}

void StretchFeature::setStatus(const QString &value, bool error)
{
    m_status->setText(value);
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

} // namespace forkmesh::stretch

namespace {
const forkmesh::market::NativePackageRegistration stretchRegistration({
    QStringLiteral("builtin:stretch"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Stretch Break"),
    QStringLiteral("person-standing"),
    QStringLiteral("/api/stretch"),
    QStringLiteral("forkmesh-stretch-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::stretch::StretchFeature(host, parent);
    },
});
} // namespace
