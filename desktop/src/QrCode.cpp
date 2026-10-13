#include "QrCode.h"

#include <QPainter>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>

namespace {

using std::vector;

const int8_t ECC_CODEWORDS_PER_BLOCK[4][41] = {
    {-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30,
     28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30,
     30, 30, 30},
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26,
     26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
     28, 28, 28},
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28,
     26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30,
     30, 30, 30},
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28,
     26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30,
     30, 30, 30}};

const int8_t NUM_EC_BLOCKS[4][41] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8,
     8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16,
     17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
    {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20,
     23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},
    {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25,
     25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81}};

int eclIndex(QrCode::Ecl e)
{
    switch (e) {
    case QrCode::Ecl::Low: return 0;
    case QrCode::Ecl::Medium: return 1;
    case QrCode::Ecl::Quartile: return 2;
    case QrCode::Ecl::High: return 3;
    }
    return 1;
}

int eclFormatBits(QrCode::Ecl e)
{
    switch (e) {
    case QrCode::Ecl::Low: return 1;
    case QrCode::Ecl::Medium: return 0;
    case QrCode::Ecl::Quartile: return 3;
    case QrCode::Ecl::High: return 2;
    }
    return 0;
}

bool getBit(long x, int i) { return ((x >> i) & 1) != 0; }

int getNumRawDataModules(int ver)
{
    int result = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int numAlign = ver / 7 + 2;
        result -= (25 * numAlign - 10) * numAlign - 55;
        if (ver >= 7)
            result -= 36;
    }
    return result;
}

int getNumDataCodewords(int ver, int ecl)
{
    return getNumRawDataModules(ver) / 8
        - ECC_CODEWORDS_PER_BLOCK[ecl][ver] * NUM_EC_BLOCKS[ecl][ver];
}


uint8_t rsMultiply(uint8_t x, uint8_t y)
{
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return static_cast<uint8_t>(z);
}

vector<uint8_t> rsComputeDivisor(int degree)
{
    vector<uint8_t> result(degree, 0);
    result[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (size_t j = 0; j < result.size(); j++) {
            result[j] = rsMultiply(result[j], root);
            if (j + 1 < result.size())
                result[j] ^= result[j + 1];
        }
        root = rsMultiply(root, 0x02);
    }
    return result;
}

vector<uint8_t> rsComputeRemainder(const vector<uint8_t> &data,
                                   const vector<uint8_t> &divisor)
{
    vector<uint8_t> result(divisor.size(), 0);
    for (uint8_t b : data) {
        uint8_t factor = b ^ result.front();
        result.erase(result.begin());
        result.push_back(0);
        for (size_t i = 0; i < result.size(); i++)
            result[i] ^= rsMultiply(divisor[i], factor);
    }
    return result;
}

vector<int> alignmentPositions(int ver)
{
    if (ver == 1)
        return {};
    int numAlign = ver / 7 + 2;
    int step = (ver == 32)
                   ? 26
                   : (ver * 4 + numAlign * 2 + 1) / (numAlign * 2 - 2) * 2;
    int size = ver * 4 + 17;
    vector<int> result;
    for (int i = 0, pos = size - 7; i < numAlign - 1; i++, pos -= step)
        result.insert(result.begin(), pos);
    result.insert(result.begin(), 6);
    return result;
}

vector<uint8_t> addEccAndInterleave(const vector<uint8_t> &data, int ver, int ecl)
{
    int numBlocks = NUM_EC_BLOCKS[ecl][ver];
    int blockEccLen = ECC_CODEWORDS_PER_BLOCK[ecl][ver];
    int rawCodewords = getNumRawDataModules(ver) / 8;
    int numShortBlocks = numBlocks - rawCodewords % numBlocks;
    int shortBlockLen = rawCodewords / numBlocks;

    vector<vector<uint8_t>> blocks;
    const vector<uint8_t> divisor = rsComputeDivisor(blockEccLen);
    for (int i = 0, k = 0; i < numBlocks; i++) {
        int datLen = shortBlockLen - blockEccLen + (i < numShortBlocks ? 0 : 1);
        vector<uint8_t> dat(data.begin() + k, data.begin() + k + datLen);
        k += datLen;
        const vector<uint8_t> ecc = rsComputeRemainder(dat, divisor);
        if (i < numShortBlocks)
            dat.push_back(0); // placeholder column for interleaving alignment
        dat.insert(dat.end(), ecc.begin(), ecc.end());
        blocks.push_back(std::move(dat));
    }

    vector<uint8_t> result;
    for (size_t i = 0; i < blocks[0].size(); i++) {
        for (size_t j = 0; j < blocks.size(); j++) {
            if (i != static_cast<size_t>(shortBlockLen - blockEccLen) ||
                j >= static_cast<size_t>(numShortBlocks))
                result.push_back(blocks[j][i]);
        }
    }
    return result;
}


struct Matrix {
    int ver;
    int size;
    vector<vector<bool>> modules;
    vector<vector<bool>> isFunction;

    explicit Matrix(int v)
        : ver(v), size(v * 4 + 17),
          modules(size, vector<bool>(size, false)),
          isFunction(size, vector<bool>(size, false)) {}

    void setFunction(int x, int y, bool dark)
    {
        modules[y][x] = dark;
        isFunction[y][x] = true;
    }

    void drawFinder(int x, int y)
    {
        for (int dy = -4; dy <= 4; dy++) {
            for (int dx = -4; dx <= 4; dx++) {
                int dist = std::max(std::abs(dx), std::abs(dy));
                int xx = x + dx, yy = y + dy;
                if (0 <= xx && xx < size && 0 <= yy && yy < size)
                    setFunction(xx, yy, dist != 2 && dist != 4);
            }
        }
    }

    void drawAlignment(int x, int y)
    {
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                setFunction(x + dx, y + dy,
                            std::max(std::abs(dx), std::abs(dy)) != 1);
    }

    void drawFormatBits(int eclFmt, int mask)
    {
        int data = eclFmt << 3 | mask;
        int rem = data;
        for (int i = 0; i < 10; i++)
            rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        int bits = (data << 10 | rem) ^ 0x5412;

        for (int i = 0; i <= 5; i++)
            setFunction(8, i, getBit(bits, i));
        setFunction(8, 7, getBit(bits, 6));
        setFunction(8, 8, getBit(bits, 7));
        setFunction(7, 8, getBit(bits, 8));
        for (int i = 9; i < 15; i++)
            setFunction(14 - i, 8, getBit(bits, i));

        for (int i = 0; i < 8; i++)
            setFunction(size - 1 - i, 8, getBit(bits, i));
        for (int i = 8; i < 15; i++)
            setFunction(8, size - 15 + i, getBit(bits, i));
        setFunction(8, size - 8, true); // always-dark module
    }

    void drawVersion()
    {
        if (ver < 7)
            return;
        int rem = ver;
        for (int i = 0; i < 12; i++)
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        long bits = static_cast<long>(ver) << 12 | rem;
        for (int i = 0; i < 18; i++) {
            bool bit = getBit(bits, i);
            int a = size - 11 + i % 3, b = i / 3;
            setFunction(a, b, bit);
            setFunction(b, a, bit);
        }
    }

    void drawFunctionPatterns(int eclFmt)
    {
        for (int i = 0; i < size; i++) {
            setFunction(6, i, i % 2 == 0);
            setFunction(i, 6, i % 2 == 0);
        }
        drawFinder(3, 3);
        drawFinder(size - 4, 3);
        drawFinder(3, size - 4);

        const vector<int> align = alignmentPositions(ver);
        int n = static_cast<int>(align.size());
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                if ((i == 0 && j == 0) || (i == 0 && j == n - 1) ||
                    (i == n - 1 && j == 0))
                    continue; // overlaps the finder patterns
                drawAlignment(align[i], align[j]);
            }
        }
        drawFormatBits(eclFmt, 0); // placeholder mask, redrawn later
        drawVersion();
    }

    void drawCodewords(const vector<uint8_t> &data)
    {
        size_t i = 0; // bit index into data
        for (int right = size - 1; right >= 1; right -= 2) {
            if (right == 6)
                right = 5;
            for (int vert = 0; vert < size; vert++) {
                for (int j = 0; j < 2; j++) {
                    int x = right - j;
                    bool upward = ((right + 1) & 2) == 0;
                    int y = upward ? size - 1 - vert : vert;
                    if (!isFunction[y][x] && i < data.size() * 8) {
                        modules[y][x] = getBit(data[i >> 3], 7 - (i & 7));
                        i++;
                    }
                }
            }
        }
    }

    void applyMask(int mask)
    {
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                bool invert = false;
                switch (mask) {
                case 0: invert = (x + y) % 2 == 0; break;
                case 1: invert = y % 2 == 0; break;
                case 2: invert = x % 3 == 0; break;
                case 3: invert = (x + y) % 3 == 0; break;
                case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                case 7: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (!isFunction[y][x] && invert)
                    modules[y][x] = !modules[y][x];
            }
        }
    }

    void addHistory(int run, std::array<int, 7> &hist) const
    {
        if (hist[0] == 0)
            run += size; // light border before the first run
        for (int i = 6; i > 0; i--)
            hist[i] = hist[i - 1];
        hist[0] = run;
    }

    int countPatterns(const std::array<int, 7> &h) const
    {
        int n = h[1];
        bool core = n > 0 && h[2] == n && h[3] == n * 3 && h[4] == n && h[5] == n;
        return (core && h[0] >= n * 4 && h[6] >= n ? 1 : 0) +
               (core && h[6] >= n * 4 && h[0] >= n ? 1 : 0);
    }

    int terminateAndCount(bool runColor, int run, std::array<int, 7> &hist) const
    {
        if (runColor) {
            addHistory(run, hist);
            run = 0;
        }
        run += size; // light border after the final run
        addHistory(run, hist);
        return countPatterns(hist);
    }

    long penaltyScore() const
    {
        const int N1 = 3, N2 = 3, N3 = 40, N4 = 10;
        long result = 0;

        for (int y = 0; y < size; y++) {
            bool runColor = false;
            int run = 0;
            std::array<int, 7> hist = {};
            for (int x = 0; x < size; x++) {
                if (modules[y][x] == runColor) {
                    run++;
                    if (run == 5)
                        result += N1;
                    else if (run > 5)
                        result++;
                } else {
                    addHistory(run, hist);
                    if (!runColor)
                        result += countPatterns(hist) * N3;
                    runColor = modules[y][x];
                    run = 1;
                }
            }
            result += terminateAndCount(runColor, run, hist) * N3;
        }
        for (int x = 0; x < size; x++) {
            bool runColor = false;
            int run = 0;
            std::array<int, 7> hist = {};
            for (int y = 0; y < size; y++) {
                if (modules[y][x] == runColor) {
                    run++;
                    if (run == 5)
                        result += N1;
                    else if (run > 5)
                        result++;
                } else {
                    addHistory(run, hist);
                    if (!runColor)
                        result += countPatterns(hist) * N3;
                    runColor = modules[y][x];
                    run = 1;
                }
            }
            result += terminateAndCount(runColor, run, hist) * N3;
        }
        for (int y = 0; y < size - 1; y++) {
            for (int x = 0; x < size - 1; x++) {
                bool c = modules[y][x];
                if (c == modules[y][x + 1] && c == modules[y + 1][x] &&
                    c == modules[y + 1][x + 1])
                    result += N2;
            }
        }
        int dark = 0;
        for (const auto &row : modules)
            for (bool v : row)
                if (v)
                    dark++;
        int total = size * size;
        int k = static_cast<int>((std::abs(dark * 20L - total * 10L) + total - 1) /
                                 total) - 1;
        result += static_cast<long>(k) * N4;
        return result;
    }
};

void appendBits(vector<bool> &bb, uint32_t val, int n)
{
    for (int i = n - 1; i >= 0; i--)
        bb.push_back(((val >> i) & 1) != 0);
}

} // namespace

std::vector<std::vector<bool>> QrCode::encode(const QByteArray &data, Ecl ecl)
{
    const int ecl_i = eclIndex(ecl);

    int ver = 0;
    for (int v = 1; v <= 40; v++) {
        int capacityBits = getNumDataCodewords(v, ecl_i) * 8;
        int ccBits = (v <= 9) ? 8 : 16;
        long needed = 4 + ccBits + 8L * data.size();
        if (needed <= capacityBits) {
            ver = v;
            break;
        }
    }
    if (ver == 0)
        return {}; // does not fit in any version

    const int ccBits = (ver <= 9) ? 8 : 16;
    vector<bool> bb;
    appendBits(bb, 0x4, 4); // byte mode indicator
    appendBits(bb, static_cast<uint32_t>(data.size()), ccBits);
    for (unsigned char c : data)
        appendBits(bb, c, 8);

    const int capacityBits = getNumDataCodewords(ver, ecl_i) * 8;
    appendBits(bb, 0, std::min(4, capacityBits - static_cast<int>(bb.size())));
    appendBits(bb, 0, (8 - static_cast<int>(bb.size()) % 8) % 8);
    for (uint8_t pad = 0xEC; static_cast<int>(bb.size()) < capacityBits;
         pad = static_cast<uint8_t>(pad ^ 0xEC ^ 0x11))
        appendBits(bb, pad, 8);

    vector<uint8_t> dataCodewords(bb.size() / 8, 0);
    for (size_t i = 0; i < bb.size(); i++)
        if (bb[i])
            dataCodewords[i >> 3] |= 1 << (7 - (i & 7));

    const vector<uint8_t> allCodewords =
        addEccAndInterleave(dataCodewords, ver, ecl_i);

    Matrix m(ver);
    const int eclFmt = eclFormatBits(ecl);
    m.drawFunctionPatterns(eclFmt);
    m.drawCodewords(allCodewords);

    int bestMask = 0;
    long minPenalty = LONG_MAX;
    for (int mask = 0; mask < 8; mask++) {
        m.applyMask(mask);
        m.drawFormatBits(eclFmt, mask);
        long p = m.penaltyScore();
        if (p < minPenalty) {
            minPenalty = p;
            bestMask = mask;
        }
        m.applyMask(mask); // undo (XOR is its own inverse)
    }
    m.applyMask(bestMask);
    m.drawFormatBits(eclFmt, bestMask);
    return m.modules;
}

QImage QrCode::encodeToImage(const QString &text, int scale, int margin, Ecl ecl)
{
    const auto modules = encode(text.toUtf8(), ecl);
    if (modules.empty())
        return QImage();
    const int n = static_cast<int>(modules.size());
    const int dim = (n + margin * 2) * scale;
    QImage img(dim, dim, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (modules[y][x])
                p.drawRect((x + margin) * scale, (y + margin) * scale, scale,
                           scale);
    return img;
}
