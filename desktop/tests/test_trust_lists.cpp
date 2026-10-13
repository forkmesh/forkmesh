#include "TrustLists.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <algorithm>

// The desktop trust-list resolver must reproduce every case in the shared
// conformance/trust-lists/v1/vectors.json byte-for-byte (compared as canonical
// JSON), and the result must not depend on the order documents, follows, or
// overrides arrive in.
namespace {

QJsonArray reversed(const QJsonArray &array)
{
    QJsonArray out;
    for (qsizetype i = array.size() - 1; i >= 0; --i)
        out.append(array.at(i));
    return out;
}

} // namespace

class TestTrustLists : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void conformance_data();
    void conformance();
    void canonicalEncoding();
    void listIdentifier();
    void parserEdges_data();
    void parserEdges();

private:
    QJsonObject m_vectors;
};

void TestTrustLists::initTestCase()
{
    QFile file(QStringLiteral(FORKMESH_TRUST_LIST_VECTORS));
    QVERIFY2(file.open(QIODevice::ReadOnly), "the shared trust-list vectors are readable");
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    m_vectors = document.object();
    QCOMPARE(m_vectors.value(QStringLiteral("schema")).toString(),
             QStringLiteral("forkmesh-trust-list-vectors-v1"));
    QVERIFY(!m_vectors.value(QStringLiteral("cases")).toArray().isEmpty());
}

void TestTrustLists::conformance_data()
{
    QTest::addColumn<QJsonObject>("vector");
    QTest::addColumn<bool>("reverseInput");
    for (const QJsonValue &value : m_vectors.value(QStringLiteral("cases")).toArray()) {
        const QJsonObject vector = value.toObject();
        const QString id = vector.value(QStringLiteral("id")).toString();
        QTest::newRow(qPrintable(id)) << vector << false;
        QTest::newRow(qPrintable(id + QStringLiteral(" (reversed input)"))) << vector << true;
    }
}

void TestTrustLists::conformance()
{
    QFETCH(QJsonObject, vector);
    QFETCH(bool, reverseInput);
    const QJsonObject input = vector.value(QStringLiteral("input")).toObject();
    QJsonArray documents = input.value(QStringLiteral("documents")).toArray();
    QJsonArray follows = input.value(QStringLiteral("follows")).toArray();
    QJsonArray overrides = input.value(QStringLiteral("overrides")).toArray();
    if (reverseInput) {
        documents = reversed(documents);
        follows = reversed(follows);
        overrides = reversed(overrides);
    }
    QStringList raw;
    for (const QJsonValue &document : std::as_const(documents))
        raw.append(document.toString());

    const QJsonObject got = forkmesh::trust::resolve(
        raw, input.value(QStringLiteral("now")).toInteger(), follows, overrides);

    bool gotOk = false;
    bool wantOk = false;
    const QString gotText = forkmesh::trust::canonicalJson(got, &gotOk);
    const QString wantText =
        forkmesh::trust::canonicalJson(vector.value(QStringLiteral("expect")), &wantOk);
    QVERIFY2(gotOk, "the resolution is canonically encodable");
    QVERIFY2(wantOk, "the expected resolution is canonically encodable");
    QCOMPARE(gotText, wantText);
}

void TestTrustLists::canonicalEncoding()
{
    using forkmesh::trust::canonicalJson;
    const QJsonObject object{
        {QStringLiteral("b"), 1},
        {QStringLiteral("a"), QJsonArray{true, QJsonValue(QJsonValue::Null), QStringLiteral("x")}},
        {QStringLiteral("B"), qint64(-9007199254740991)},
    };
    bool ok = false;
    QCOMPARE(canonicalJson(object, &ok),
             QStringLiteral("{\"B\":-9007199254740991,\"a\":[true,null,\"x\"],\"b\":1}"));
    QVERIFY(ok);

    canonicalJson(QJsonValue(1.5), &ok);
    QVERIFY2(!ok, "non-integers have no canonical encoding");
    canonicalJson(QJsonValue(qint64(9007199254740992)), &ok);
    QVERIFY2(!ok, "integers past 2^53 - 1 have no canonical encoding");
    canonicalJson(QJsonValue(QStringLiteral("quote\"")), &ok);
    QVERIFY2(!ok, "strings needing escapes have no canonical encoding");
    canonicalJson(QJsonValue(QStringLiteral("caf\u00e9")), &ok);
    QVERIFY2(!ok, "non-ASCII strings have no canonical encoding");
}

void TestTrustLists::listIdentifier()
{
    QCOMPARE(forkmesh::trust::listId(QStringLiteral("zXRx1TfXDzTDatGMgjbGuirVh2N6ghkRQY6At9nbmW0"),
                                     QStringLiteral("security")),
             QStringLiteral("urn:forkmesh:trust-list:sha256:"
                            "0884df378f1a4efcf017d8d667d3c8a9587398d6a18b84395dd68c373079cddc"));
}

// Where Python's json.loads and QJsonDocument disagree, the reference decides
// the rejection code; these expectations were taken from app/src/trust_lists.py.
void TestTrustLists::parserEdges_data()
{
    QTest::addColumn<QString>("raw");
    QTest::addColumn<QString>("code");
    QTest::newRow("scalar top level") << QStringLiteral("1") << QStringLiteral("unsupported_schema");
    QTest::newRow("list schema") << QStringLiteral("{\"schema\":[]}")
                                 << QStringLiteral("unsupported_schema");
    QTest::newRow("leading whitespace") << QStringLiteral(" {}") << QStringLiteral("noncanonical");
    QTest::newRow("overflowing float") << QStringLiteral("1e400") << QStringLiteral("noncanonical");
    QTest::newRow("too many digits") << QString(5000, QLatin1Char('1'))
                                     << QStringLiteral("invalid_json");
    QTest::newRow("duplicate key") << QStringLiteral("{\"a\":1,\"a\":2}")
                                   << QStringLiteral("noncanonical");
    QTest::newRow("NaN") << QStringLiteral("NaN") << QStringLiteral("invalid_json");
    QTest::newRow("escaped surrogate") << QStringLiteral("\"\\ud800\"")
                                       << QStringLiteral("noncanonical");
    QTest::newRow("brackets inside a string") << QStringLiteral("[\"]]]]]]]]]]\"]")
                                              << QStringLiteral("unsupported_schema");
    QTest::newRow("escaped quote before brackets")
        << QStringLiteral("{\"x\":\"\\\"[[[[[[[[[[\"}") << QStringLiteral("noncanonical");
    QTest::newRow("depth eight") << QStringLiteral("[[[[[[[[1]]]]]]]]")
                                 << QStringLiteral("unsupported_schema");
}

void TestTrustLists::parserEdges()
{
    QFETCH(QString, raw);
    QFETCH(QString, code);
    QCOMPARE(forkmesh::trust::verifyDocument(raw), code);
}

QTEST_GUILESS_MAIN(TestTrustLists)

#include "test_trust_lists.moc"
