// The code map's model (adhoc #187). Everything a reader trusts in the picture
// comes from here, so it is checked directly: areas proportional to bytes,
// children inside parents, no overlaps, folder height following folder size,
// an exact back-to-front order for the 3D painter, and a tokenizer that keeps
// each language's comment and string rules apart.
#include "CodeMapModel.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>

using namespace forkmesh::codemap;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

QByteArray record(const char *type, qint64 size, const char *path)
{
    const QByteArray sizeField = size < 0 ? QByteArray("-") : QByteArray::number(size);
    return QByteArray("100644 ") + type + " 0123456789abcdef0123456789abcdef01234567 "
        + sizeField.rightJustified(7) + '\t' + path + '\0';
}

Tree fixture()
{
    QByteArray listing;
    listing += record("blob", 40000, "src/big.py");
    listing += record("blob", 10000, "src/mid.py");
    listing += record("blob", 5000, "src/deep/a.js");
    listing += record("blob", 5000, "src/deep/b.js");
    listing += record("blob", 0, "src/deep/empty.txt");
    listing += record("blob", 20000, "docs/guide.md");
    listing += record("blob", 2000, "README.md");
    listing += record("commit", -1, "vendor/submodule");
    return buildTree(listing, QStringLiteral("fixture"));
}

int find(const Tree &tree, const char *path)
{
    for (int i = 0; i < tree.nodes.size(); ++i) {
        if (tree.nodes.at(i).path == QLatin1String(path))
            return i;
    }
    return -1;
}

bool near(double a, double b, double tolerance = 1e-6)
{
    return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b));
}

bool hasToken(const QVector<Token> &tokens, int column, int length, int kind)
{
    for (const Token &token : tokens) {
        if (token.column == column && token.length == length && token.kind == kind)
            return true;
    }
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    Tree tree = fixture();
    check(tree.fileCount == 7, "every blob is a file; a submodule commit is not");
    check(find(tree, "vendor") < 0, "a submodule alone does not invent a directory");
    check(tree.nodes.at(0).size == 82000, "the root carries the repository's bytes");
    check(tree.nodes.at(find(tree, "src")).size == 60000, "a directory sums its subtree");
    check(tree.nodes.at(find(tree, "src")).language == QLatin1String("Python"),
          "a directory takes its dominant language");
    check(tree.languages.size() >= 3 && tree.languages.at(0).first == QLatin1String("Python")
              && tree.languages.at(1).first == QLatin1String("Markdown"),
          "languages are ranked by bytes for the fixed colour order");
    check(tree.nodes.at(find(tree, "src/big.py")).hue == 0
              && tree.nodes.at(find(tree, "docs/guide.md")).hue == 1,
          "hue slots follow the language ranking");

    layoutTree(tree, 1600, 1000);
    const QVector<Node> &nodes = tree.nodes;
    check(nodes.at(0).rect == QRectF(0, 0, 1600, 1000), "the root fills the world");

    // Areas proportional to weight among siblings; inside the parent, below its
    // label band; no sibling overlap.
    bool proportional = true, contained = true, disjoint = true;
    for (int i = 0; i < nodes.size(); ++i) {
        const Node &parent = nodes.at(i);
        for (int a : parent.children) {
            const QRectF ra = nodes.at(a).rect;
            if (ra.left() < parent.rect.left() - 1e-6 || ra.right() > parent.rect.right() + 1e-6
                || ra.top() < parent.rect.top() + parent.band - 1e-6
                || ra.bottom() > parent.rect.bottom() + 1e-6)
                contained = false;
            for (int b : parent.children) {
                if (a >= b)
                    continue;
                const QRectF rb = nodes.at(b).rect;
                if (!near((ra.width() * ra.height()) / (rb.width() * rb.height()),
                          nodes.at(a).weight / nodes.at(b).weight))
                    proportional = false;
                const QRectF overlap = ra.intersected(rb);
                if (overlap.width() > 1e-6 && overlap.height() > 1e-6)
                    disjoint = false;
            }
        }
    }
    check(proportional, "sibling areas are proportional to their bytes");
    check(contained, "every child sits inside its parent, below the label band");
    check(disjoint, "siblings never overlap");
    const QRectF empty = nodes.at(find(tree, "src/deep/empty.txt")).rect;
    check(empty.width() > 0 && empty.height() > 0, "an empty file stays a visible speck");

    // 3D: thickness is the square root of the folder's share; children stack.
    elevateTree(tree, 20);
    const Node &src = nodes.at(find(tree, "src"));
    const Node &docs = nodes.at(find(tree, "docs"));
    const Node &deep = nodes.at(find(tree, "src/deep"));
    check(src.top - src.base > docs.top - docs.base && docs.top - docs.base > deep.top - deep.base,
          "a bigger folder stands taller");
    check(near((src.top - src.base) / (docs.top - docs.base), std::sqrt(60000.0 / 20000.0)),
          "folder height follows the square root of folder size");
    bool stacked = true;
    for (int i = 1; i < nodes.size(); ++i) {
        if (!near(nodes.at(i).base, nodes.at(nodes.at(i).parent).top) || nodes.at(i).top <= nodes.at(i).base)
            stacked = false;
    }
    check(stacked, "every child stands on its parent's top face");
    check(near(nodes.at(0).subtreeTop, std::max({deep.subtreeTop, docs.subtreeTop, src.subtreeTop})),
          "the root knows its tallest descendant, for culling");

    // Painter's order: from any camera, a child drawn later is never hidden
    // behind one drawn earlier — i.e. no earlier child lies strictly between
    // the camera and a later one along an axis that separates them.
    {
        QByteArray wide;
        for (int i = 0; i < 40; ++i)
            wide += record("blob", 1000 + i * 137 % 5000, QByteArray("f") + QByteArray::number(i) + ".c");
        Tree flat = buildTree(wide, QStringLiteral("flat"));
        layoutTree(flat, 1600, 1000);
        bool ordered = true;
        const QPointF cameras[] = {{-500, -500}, {2100, -400}, {800, 1500}, {-300, 500}, {800, 500}, {10, 990}};
        for (const QPointF &cam : cameras) {
            const QVector<int> order = backToFront(flat, 0, cam.x(), cam.y());
            if (order.size() != flat.nodes.at(0).children.size())
                ordered = false;
            for (int i = 0; i < order.size(); ++i) {
                for (int j = i + 1; j < order.size(); ++j) {
                    const QRectF early = flat.nodes.at(order.at(i)).rect;
                    const QRectF late = flat.nodes.at(order.at(j)).rect;
                    // `early` would wrongly cover `late` only if it were nearer on
                    // a separating axis.
                    const bool nearerX = (early.right() <= late.left() + 1e-9 && cam.x() <= early.right())
                        || (late.right() <= early.left() + 1e-9 && cam.x() >= early.left());
                    const bool nearerY = (early.bottom() <= late.top() + 1e-9 && cam.y() <= early.bottom())
                        || (late.bottom() <= early.top() + 1e-9 && cam.y() >= early.top());
                    const bool separatedX = early.right() <= late.left() + 1e-9 || late.right() <= early.left() + 1e-9;
                    const bool separatedY = early.bottom() <= late.top() + 1e-9 || late.bottom() <= early.top() + 1e-9;
                    // Nearer on every axis that separates the pair means it
                    // really is in front, and must not have been drawn first.
                    if ((separatedX ? nearerX : true) && (separatedY ? nearerY : true)
                        && (separatedX || separatedY))
                        ordered = false;
                }
            }
        }
        check(ordered, "backToFront never draws a nearer sibling before a farther one");
    }

    // Tokenizer.
    {
        auto js = prepareCode(QStringLiteral("const n = call(1); // note\n/* open\nstill */ let s = \"a//b\";"),
                              QStringLiteral("JavaScript"));
        const QVector<Token> first = tokenizeLine(*js, 0);
        check(hasToken(first, 0, 5, 1), "const is a keyword");
        check(hasToken(first, 10, 4, 5), "an identifier before ( is a call");
        check(hasToken(first, 15, 1, 4), "digits are a number");
        check(!first.isEmpty() && first.last().column == 19 && first.last().kind == 3, "// starts a comment");
        const QVector<Token> second = tokenizeLine(*js, 1);
        check(second.size() == 1 && second.at(0).kind == 3, "an unterminated block comment runs to the line end");
        const QVector<Token> third = tokenizeLine(*js, 2);
        check(!third.isEmpty() && third.at(0).column == 0 && third.at(0).length == 8 && third.at(0).kind == 3,
              "a block comment that closes mid-line ends there");
        check(hasToken(third, 9, 3, 1), "code after the block close is tokenized normally");
        check(hasToken(third, 17, 6, 2), "// inside a string is part of the string");
    }
    {
        auto py = prepareCode(QStringLiteral("x = 7 // 2  # floor\n\"\"\"Doc\nbody\"\"\"\nreturn x"), QStringLiteral("Python"));
        const QVector<Token> first = tokenizeLine(*py, 0);
        check(hasToken(first, 6, 1, 6) && hasToken(first, 7, 1, 6), "Python's // is an operator, not a comment");
        check(!first.isEmpty() && first.last().column == 12 && first.last().kind == 3, "# starts a Python comment");
        check(tokenizeLine(*py, 1).size() == 1 && tokenizeLine(*py, 1).at(0).kind == 3, "a docstring opener reads as a comment");
        check(tokenizeLine(*py, 2).size() == 1 && tokenizeLine(*py, 2).at(0).kind == 3, "the docstring body line is inside the block");
        check(hasToken(tokenizeLine(*py, 3), 0, 6, 1), "code after the docstring is code again");
    }
    {
        auto md = prepareCode(QStringLiteral("# Title don't"), QStringLiteral("Markdown"));
        bool plainOnly = true;
        for (const Token &token : tokenizeLine(*md, 0))
            if (token.kind != 0 && token.kind != 6)
                plainOnly = false;
        check(plainOnly, "prose has no keywords, strings or hash comments");
    }
    {
        QString big;
        for (int i = 0; i < 30000; ++i)
            big += QStringLiteral("line %1\n").arg(i);
        auto code = prepareCode(big, QStringLiteral("Text"));
        check(code->lines.size() == 20000 && code->totalLines == 30000,
              "a huge file is capped but reports its real length");
    }

    // Fitting lines into a cell, and the level-of-detail ladder.
    {
        Node cell;
        cell.type = Node::File;
        cell.rect = QRectF(0, 0, 300, 100);
        cell.band = 6;
        QString text;
        for (int i = 0; i < 400; ++i)
            text += QStringLiteral("int value_%1 = %1;\n").arg(i);
        auto code = prepareCode(text, QStringLiteral("C++"));
        check(fitCode(*code, cell), "the first fit reports a change");
        check(!fitCode(*code, cell), "an unchanged cell is not refitted");
        check(code->columns > 1, "a wide cell lays a long file out in several columns");
        check(code->rows * code->columns >= 400, "every line has a slot");
        check(code->rows * code->lineHeight <= code->area.height() + 1e-9
                  && code->columns * code->columnWidth <= code->area.width() + 1e-9,
              "the fitted text stays inside the cell");
    }
    check(pickLevel(0.01) == 0.125 && pickLevel(0.9) == 1 && pickLevel(3) == 4 && pickLevel(14) == 16,
          "levels round up the ladder");
    check(pickLevel(40) == 0, "past the ladder the flat view draws live text");
    check(languageOf(QStringLiteral("a/CMakeLists.txt")) == QLatin1String("CMake")
              && languageOf(QStringLiteral("x.weird")) == QLatin1String(".weird")
              && languageOf(QStringLiteral("noext")) == QLatin1String("Other"),
          "language names");
    check(isBinaryName(QStringLiteral("logo.PNG")) && !isBinaryName(QStringLiteral("main.cpp")), "binary names");
    check(previewKindOf(QStringLiteral("logo.PNG")) == Preview::Image
              && previewKindOf(QStringLiteral("icon.svg")) == Preview::Image
              && previewKindOf(QStringLiteral("Inter.woff2")) == Preview::Font
              && previewKindOf(QStringLiteral("pkg.zip")) == Preview::Card
              && previewKindOf(QStringLiteral("main.py")) == Preview::Code,
          "images and fonts are pictured, other binaries carded, the rest is source");
    check(fileCategoryOf(QStringLiteral("clip.mp4")) == QLatin1String("Video")
              && fileCategoryOf(QStringLiteral("model.glb")) == QLatin1String("3D model")
              && fileCategoryOf(QStringLiteral("blob.xyz")) == QLatin1String("Binary"),
          "type card categories");
    {
        Node cell;
        cell.type = Node::File;
        cell.rect = QRectF(0, 0, 200, 100);
        cell.band = 5;
        const double inset = 100 * 0.035;
        const QRectF wide = previewArea(cell, 4);
        const QRectF tall = previewArea(cell, 0.5);
        check(std::abs(wide.width() - (200 - inset * 2)) < 1e-9 && std::abs(wide.width() / wide.height() - 4) < 1e-9,
              "a wide picture fills the content box's width");
        check(std::abs(tall.height() - (100 - 5 - inset * 2)) < 1e-9
                  && std::abs(tall.center().x() - 100) < 1e-9,
              "a tall picture fills its height, centred");
    }

    // `git cat-file --batch` replies, in request order.
    {
        const QStringList paths = {QStringLiteral("a.txt"), QStringLiteral("dir with space/gone.txt"),
                                   QStringLiteral("sub"), QStringLiteral("b.txt")};
        QByteArray output;
        output += "1111111111111111111111111111111111111111 blob 6\nhello\n\n";
        output += "abc123:dir with space/gone.txt missing\n";
        output += "2222222222222222222222222222222222222222 tree 3\nxyz\n";
        output += "3333333333333333333333333333333333333333 blob 0\n\n";
        const QHash<QString, QByteArray> blobs = parseCatFileBatch(output, paths);
        check(blobs.value(QStringLiteral("a.txt")) == "hello\n", "a blob keeps its exact bytes, newline included");
        check(!blobs.contains(QStringLiteral("dir with space/gone.txt")), "a missing path has no entry");
        check(!blobs.contains(QStringLiteral("sub")), "a tree is not file content");
        check(blobs.contains(QStringLiteral("b.txt")) && blobs.value(QStringLiteral("b.txt")).isEmpty(),
              "an empty blob after a missing one still lines up with its path");
        check(parseCatFileBatch("1111111111111111111111111111111111111111 blob 99\nshort", paths).isEmpty(),
              "a truncated reply yields nothing rather than misaligned text");
    }

    {
        const QVector<Entry> entries = {
            {QStringLiteral("Heap/[heap]"), 6000, QStringLiteral("Heap"), Preview::Code},
            {QStringLiteral("Shared libraries/libQt6Gui.so.6"), 3000, QStringLiteral("Shared libraries"),
             Preview::Code},
            {QStringLiteral("Shared libraries/libc.so.6"), 1000, QStringLiteral("Shared libraries"),
             Preview::Code},
        };
        const Tree sized = buildTreeFromEntries(entries, QStringLiteral("memory"));
        check(sized.fileCount == 3, "every entry becomes a leaf");
        check(sized.nodes.at(0).name == QLatin1String("memory") && sized.nodes.at(0).size == 10000,
              "the root sums every entry");
        const int libraries = find(sized, "Shared libraries");
        check(libraries > 0 && sized.nodes.at(libraries).size == 4000
                  && sized.nodes.at(libraries).children.size() == 2,
              "a path prefix becomes a folder holding its entries");
        const int so = find(sized, "Shared libraries/libQt6Gui.so.6");
        check(so > 0 && sized.nodes.at(so).language == QLatin1String("Shared libraries")
                  && sized.nodes.at(so).preview == Preview::Code,
              "an entry keeps the category and preview it was given, not ones guessed from its name");
        check(sized.languages.size() == 2 && sized.languages.at(0).first == QLatin1String("Heap"),
              "categories are ranked by size like languages");
    }

    if (failures == 0)
        std::fprintf(stdout, "test_code_map_model: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
