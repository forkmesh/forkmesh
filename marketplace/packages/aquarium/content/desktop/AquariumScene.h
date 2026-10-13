#pragma once

#include <QColor>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QRandomGenerator>
#include <QRectF>
#include <QRegion>
#include <QSizeF>
#include <QString>

#include <optional>

class QPainter;

namespace forkmesh::aquarium {

enum class Spot { Button, Heading, Label, Input, Toggle, Picture, Progress, Row, Tab, Document };

struct Landmark {
    Spot kind = Spot::Label;
    QRectF rect;
    QString text;
};

enum class Mood { Working, Waiting, Queued, Done, Merged, Failed, Stopped };

struct Swimmer {
    int id = 0;
    QString name;
    QString kind;
    Mood mood = Mood::Working;
    qint64 work = 0;
    int files = -1;
    int lines = -1;
    bool conflicted = false;
};

struct Pet {
    bool present = false;
    QRectF frame;
    QRectF body;
    QImage image;
    bool walking = false;
    bool airborne = false;
};

enum class Cue { Wave, Cheer, Nod, Eureka, Giggle, Surprise, Dizzy, Dance, Walk, Hop, Look };

struct PetCue {
    Cue cue = Cue::Look;
    QPointF at;
};

struct Note {
    int fishId = 0;
    QString fish;
    QString text;
};

enum class Trick {
    Peek,
    Boop,
    Read,
    Underline,
    HideAndPeek,
    Polish,
    Nibble,
    Mirror,
    Type,
    Checkmark,
    Surf,
    TabHop,
    RowPatrol,
    Perch,
    Lantern,
    Measure,
    Trace,
    BlowKiss,
    Count,
    Startle,
    Fetch,
    BubbleRing,
    Highlight,
    Shout,
    Quote,
    Carry,
    Wobble,
    Magnify,
    Squeegee,
    Trampoline,
    Hello,
    NoseBoop,
    Tag,
    Orbit,
    Crown,
    DanceParty,
    Launch,
    FeatherTickle,
    HideBehind,
    HighFive,
    Race,
    Pearl,
    Serenade,
    Shadow,
    BubbleBath,
    Peekaboo,
    Ride,
    Nap,
    Kiss,
    PlayFetch,
    Juggle,
    BarrelRoll,
    InkCloud,
    LegCount,
    Photo,
    LeapFrog,
    SilkSwing,
    CopyDance,
    Spotlight,
    Balloon,
};

constexpr int kElementTricks = 30;
constexpr int kPetTricks = 30;
constexpr int kTricks = kElementTricks + kPetTricks;

class Scene
{
public:
    explicit Scene(quint32 seed = 0);

    void setSize(const QSizeF &size);
    QSizeF size() const { return m_size; }
    void setBackdrop(const QImage &image);
    void setLandmarks(const QList<Landmark> &landmarks);
    const QList<Landmark> &landmarks() const { return m_landmarks; }
    void setSwimmers(const QList<Swimmer> &swimmers);
    void setPet(const Pet &pet);
    void setFade(double fade) { m_fade = fade; }
    void setPixelRatio(qreal ratio);
    void step(double dt);
    void paint(QPainter &painter) const;
    QRegion dirtyRegion();
    QList<PetCue> takeCues();
    QList<Note> takeNotes();
    bool perform(int fishId, Trick trick);
    bool viable(Trick trick) const;

    struct FishInfo {
        int id = 0;
        QString name;
        QPointF pos;
        bool busy = false;
        bool leaving = false;
        std::optional<Trick> trick;
        Mood mood = Mood::Working;
        bool conflicted = false;
        double length = 0;
    };
    QList<FishInfo> fish() const;
    int fishAt(const QPointF &point) const;
    int particleCount() const { return int(m_particles.size()); }

    static QString trickName(Trick trick);
    static bool petTrick(Trick trick) { return int(trick) >= kElementTricks; }
    static QString moodName(Mood mood);
    static QColor moodColor(Mood mood);
    static QImage portrait(const Swimmer &swimmer, int size, qreal ratio = 1);

private:
    enum class Puff {
        Bubble,
        Sparkle,
        Heart,
        Note,
        Glyph,
        Zzz,
        Ring,
        Flash,
        Crumb,
        Star,
        Ink,
        Pearl,
        Text,
        Streak,
        Sand,
        Photo,
    };

    struct Particle {
        Puff kind = Puff::Bubble;
        QPointF pos;
        QPointF vel;
        double age = 0;
        double life = 2;
        double size = 4;
        double rise = 0;
        QColor color;
        QString text;
        QRectF rect;
        QImage image;
    };

    struct Act {
        Trick trick = Trick::Peek;
        Landmark mark;
        QList<Landmark> marks;
        double t = 0;
        double length = 4;
        bool performing = false;
        double approach = 0;
        unsigned fired = 0;
        double seed = 0;
        QPointF memo;
        int step = -1;
        double side = 1;
        QImage piece;
    };

    struct Fish {
        int id = 0;
        QString name;
        int species = 0;
        quint32 hash = 0;
        QColor body;
        QColor fin;
        QColor accent;
        QColor iris;
        double length = 40;
        double finScale = 1;
        double presence = 1;
        Mood mood = Mood::Working;
        int files = -1;
        int lines = -1;
        bool conflicted = false;
        QImage skin;
        QRectF skinRect;
        QImage tailSkin;
        QRectF tailRect;
        mutable QImage tag;
        mutable QString tagKey;
        double nextMurmur = 0;
        QPointF pos;
        QPointF vel;
        QPointF goal;
        double speed = 110;
        bool snap = false;
        double facing = 1;
        double heading = 1;
        double tilt = 0;
        double roll = 0;
        double tail = 0;
        double wag = 1;
        QPointF gaze;
        bool hasGaze = false;
        double mouth = 0;
        double puffed = 0;
        double alpha = 1;
        double scale = 1;
        bool behind = false;
        bool lantern = false;
        bool sleepy = false;
        bool pucker = false;
        qint64 work = 0;
        bool entering = true;
        bool leaving = false;
        QString say;
        double sayUntil = 0;
        double nextTrick = 0;
        double wanderUntil = 0;
        std::optional<Act> act;
    };

    void spawn(const Swimmer &swimmer);
    void dress(Fish &fish) const;
    void moodChanged(Fish &fish, Mood was, bool wasConflicted);
    static bool playful(const Fish &fish);
    QPointF lineTop(const Fish &fish) const;
    bool shadowed(const Fish &fish) const;
    void think(Fish &fish, double dt);
    void swim(Fish &fish, double dt);
    void begin(Fish &fish, Trick trick, const Landmark &mark, const QList<Landmark> &marks);
    bool pick(Fish &fish);
    bool landmarkFor(Trick trick, Landmark *mark, QList<Landmark> *marks) const;
    QPointF approachPoint(const Fish &fish) const;
    void perform(Fish &fish, double dt);
    void performElement(Fish &fish, Act &act, double u, double dt);
    void performPet(Fish &fish, Act &act, double u, double dt);
    void finish(Fish &fish);
    QString describe(const Act &act) const;
    bool once(Act &act, int bit);
    void cue(Cue cue, const QPointF &at = QPointF());
    Particle &puff(Puff kind, const QPointF &at, double life, double size = 4);
    void bubbles(const QPointF &at, int count, double spread = 6);
    void say(Fish &fish, const QString &text, double seconds);
    QPointF mouthOf(const Fish &fish) const;
    QPointF petHead() const;
    QPointF petFoot() const;
    double floorY() const;
    QRectF tank() const;
    double rnd(double lo, double hi);
    QImage slice(const QRectF &rect) const;
    void ambient(double dt);
    void buildComposite();

    void paintWater(QPainter &p) const;
    void paintWeeds(QPainter &p) const;
    void paintActBehind(QPainter &p, const Fish &fish) const;
    void paintActFront(QPainter &p, const Fish &fish) const;
    void paintFish(QPainter &p, const Fish &fish) const;
    void paintFins(QPainter &p, const Fish &fish, double bodyW, double H, double amp, bool far) const;
    void paintLabel(QPainter &p, const Fish &fish) const;
    void paintParticle(QPainter &p, const Particle &particle) const;
    void paintPet(QPainter &p) const;
    void paintGhostFish(QPainter &p, const Fish &fish, const QPointF &at, double facing,
                        double alpha) const;

    mutable QRandomGenerator m_random;
    QSizeF m_size;
    QImage m_backdrop;
    QImage m_composite;
    QList<Landmark> m_landmarks;
    QList<Fish> m_fish;
    QList<Particle> m_particles;
    QList<PetCue> m_cues;
    QList<QRectF> m_lastDirty;
    QList<Note> m_notes;
    QList<int> m_elementBag;
    QList<int> m_petBag;
    Pet m_pet;
    double m_time = 0;
    double m_fade = 1;
    qreal m_ratio = 1;
    double m_petFree = 0;
    double m_nextBubble = 0;
    double m_lookAt = 0;
    int m_petFish = -1;
};

} // namespace forkmesh::aquarium
