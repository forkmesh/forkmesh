#pragma once

// The data half of the repository code map (adhoc #187): every file of a
// commit as a squarified treemap, sized by bytes, plus the elevations of the 3D
// view built from the same layout and the text model that lets a file's source
// be drawn inside its cell.
//
// Pure Qt Core so it is unit-testable and thread-agnostic; CodeMapView paints
// it. It mirrors app/public/assets/codemap/codemap.js, which is the website's
// version of the same picture — but the desktop reads the repository straight
// from git, so nothing here is folded or capped the way a host's /sizes reply
// is.

#include <QByteArray>
#include <QHash>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

namespace forkmesh::codemap {

// One source token: [column, length, class]. Classes index the view's code
// palette: 0 plain, 1 keyword, 2 string, 3 comment, 4 number, 5 call,
// 6 punctuation.
struct Token {
    int column = 0;
    int length = 0;
    int kind = 0;
};

// A file's text, split into capped lines, with the block-comment state at the
// start of every line so any single line can be tokenized on its own, and the
// newspaper-column fit of those lines into the file's cell.
struct Code {
    QStringList lines;
    int totalLines = 0; // before the MAX_LINES cap
    QString language;
    QVector<bool> startsInBlock;
    QString blockClose;
    int maxLength = 12; // column width in characters (97th percentile line)
    QVector<QVector<Token>> tokens; // lazily filled; empty optional per line
    QVector<bool> tokenized;

    // Fit into the owning cell (layout units, relative to the cell origin).
    QSizeF fittedFor;
    int columns = 1;
    int rows = 1;
    double lineHeight = 0;
    double columnWidth = 0;
    QRectF area;
};

// What a file's cell shows. Images (and fonts, as a type specimen) are drawn
// from their bytes; every other binary shows a card naming its type and is
// never read; everything else is source text.
enum class Preview { Code, Image, Font, Card };

struct Node {
    enum Type { Directory, File };
    Type type = Directory;
    QString name;
    QString path;
    qint64 size = 0;
    double weight = 0;
    int parent = -1;
    int depth = 0;
    QVector<int> children; // largest first; this is also the layout order
    QRectF rect;           // layout units
    double band = 0;       // label band on top of the cell, layout units
    QString language;      // a directory takes its dominant language
    Preview preview = Preview::Code;
    int hue = -1;          // categorical slot, -1 = gray "Other"
    double base = 0;       // 3D elevation of the underside
    double top = 0;        // 3D elevation of the top face
    double subtreeTop = 0; // highest top in the subtree, for culling

    // Squarify cuts the cell into strips, and every strip off from the rest
    // with one straight line, so the children form a guillotine partition.
    // That makes an exact back-to-front order cheap for the 3D painter: at
    // each cut, whichever side the camera is not on is farther away.
    struct Strip {
        bool vertical = false; // true: a column at the left, cut at x = edge
        double edge = 0;       // the line separating this strip from the rest
        int first = 0;         // range into `children`
        int count = 0;
    };
    QVector<Strip> strips;

    // Content. 0 none, 1 requested, 2 ready (text, or a picture held by the
    // view), 3 unavailable (a binary without a picture, or missing).
    int contentState = 0;
    std::shared_ptr<Code> code;
    int revision = 0; // bumps when a cached bitmap of the cell is stale
    quint64 lastSeen = 0;
};

struct Tree {
    QVector<Node> nodes; // nodes[0] is the root
    // Languages by bytes, largest first; the first eight take a hue.
    QVector<QPair<QString, qint64>> languages;
    int fileCount = 0;

    bool isEmpty() const { return nodes.size() <= 1; }
};

constexpr int kHueSlots = 8;

// Builds the tree from `git ls-tree -r -l -z <ref>` output.
Tree buildTree(const QByteArray &lsTreeOutput, const QString &rootName);

struct Entry {
    QString path;
    qint64 size = 0;
    QString language;
    Preview preview = Preview::Code;
};

Tree buildTreeFromEntries(const QVector<Entry> &entries, const QString &rootName);

void layoutTree(Tree &tree, double width, double height);

// 3D elevations. A folder is a slab standing on its parent whose thickness is
// maxHeight * sqrt(its share of the repository): a folder four times the size
// stands twice as tall, and deep nests still fit on screen.
void elevateTree(Tree &tree, double maxHeight);

// Child indexes of a node from farthest to nearest for a camera above layout
// point (cameraX, cameraY). Exact for the guillotine partition squarify makes.
QVector<int> backToFront(const Tree &tree, int nodeIndex, double cameraX, double cameraY);

// Splits `git cat-file --batch` output back into the blobs that were asked
// for, in request order. A path git could not resolve ("<name> missing") or
// that is not a blob simply has no entry, which the view reads as unavailable.
QHash<QString, QByteArray> parseCatFileBatch(const QByteArray &output, const QStringList &paths);

QString languageOf(const QString &fileName);
bool isBinaryName(const QString &fileName);
Preview previewKindOf(const QString &fileName);
// "Archive", "Video", … for a type card; "Binary" when nothing more is known.
QString fileCategoryOf(const QString &fileName);
QString extensionOf(const QString &fileName);
// The picture's rect inside a file cell (layout units, relative to the cell
// origin): the inset box the source text uses, fitted to `aspect`.
QRectF previewArea(const Node &node, double aspect);

// Pictures larger than this stay a type card; decoded ones are kept at most
// this many pixels on their longer side.
constexpr qint64 kMaxPreviewBytes = 8 * 1024 * 1024;
constexpr int kPreviewMaxSide = 768;

std::shared_ptr<Code> prepareCode(const QString &text, const QString &language);
const QVector<Token> &tokenizeLine(Code &code, int line);
// Fits the lines into the node's cell as columns, choosing the column count
// that leaves the largest line height. Returns true when the fit changed.
bool fitCode(Code &code, const Node &node);

// Level of detail: bitmap line height in device pixels for a line that is
// `devicePixelsPerLine` tall on screen. 0 means "draw live text instead".
double pickLevel(double devicePixelsPerLine);
const QVector<double> &levels();

// Characters are laid out on a fixed grid this wide, as a share of line height.
constexpr double kCharAspect = 0.5;

} // namespace forkmesh::codemap
