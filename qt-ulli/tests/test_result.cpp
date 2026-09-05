// tests/test_result.cpp
#include "core/Result.h"

#include <QString>
#include <QtTest/QtTest>

using ulli::core::Error;
using ulli::core::Result;
using ulli::core::makeOk;
using ulli::core::makeError;

class TestResult : public QObject {
    Q_OBJECT
private slots:
    void okHoldsValue() {
        auto r = makeOk(42);
        QVERIFY(r.isOk());
        QVERIFY(!r.isError());
        QVERIFY(static_cast<bool>(r));
        QCOMPARE(r.value(), 42);
    }
    void errHoldsMessage() {
        auto r = makeError<int>(Error::Kind::Platform, "boom");
        QVERIFY(r.isError());
        QVERIFY(!r.isOk());
        QVERIFY(!static_cast<bool>(r));
        QCOMPARE(r.error().kind(), Error::Kind::Platform);
        QCOMPARE(r.error().message(), std::string("boom"));
    }
    void mapTransformsSuccess() {
        auto r = makeOk(10).map([](int x) { return x * 2; });
        QVERIFY(r.isOk());
        QCOMPARE(r.value(), 20);
    }
    void mapPreservesError() {
        auto r = makeError<int>(Error::Kind::InvalidInput, "bad")
                     .map([](int) { return 1; });
        QVERIFY(r.isError());
        QCOMPARE(r.error().kind(), Error::Kind::InvalidInput);
    }
    void voidOkAndError() {
        auto ok = makeOk();
        QVERIFY(ok.isOk());
        auto err = makeError(Error::Kind::Cancelled, "stop");
        QVERIFY(err.isError());
        QCOMPARE(err.error().kind(), Error::Kind::Cancelled);
    }
    void onErrorCanSwallow() {
        int seen = 0;
        auto r = makeError(Error::Kind::Platform, "x").onError(
            [&](const Error&) { seen = 1; });
        QVERIFY(r.isError());
        QCOMPARE(seen, 1);
    }
};

QTEST_GUILESS_MAIN(TestResult)
#include "test_result.moc"
