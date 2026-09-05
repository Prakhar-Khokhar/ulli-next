// tests/test_catalog.cpp
#include "core/Catalog.h"

#include <QString>
#include <QtTest/QtTest>

class TestCatalog : public QObject {
    Q_OBJECT
private slots:
    void fallbackNotEmpty() {
        bool fb = false;
        auto c = ulli::core::Catalog::load(&fb);
        QVERIFY(!c.empty());
        QVERIFY(c.find("mint") != nullptr);
        QVERIFY(c.find("ubuntu") != nullptr);
        QVERIFY(c.find("cachyos") != nullptr);
        QCOMPARE(c.find("mint")->label(),
                 std::string("Linux Mint 22.3"));
        QVERIFY(!c.find("mint")->mirrors().empty());
    }
    void keysOrdered() {
        auto c = ulli::core::Catalog::load();
        QVERIFY(!c.keys().empty());
    }
    void fromDistros() {
        std::vector<ulli::core::Distro> v;
        v.emplace_back("test", "Test", "test.iso",
                       "0000000000000000000000000000000000000000000000000000000000000000",
                       std::vector<std::string>{"http://x/y.test.iso"});
        auto c = ulli::core::Catalog::fromDistros(std::move(v));
        QVERIFY(!c.empty());
        QCOMPARE(c.find("test")->sha256(),
                 std::string("0000000000000000000000000000000000000000000000000000000000000000"));
    }
};

QTEST_GUILESS_MAIN(TestCatalog)
#include "test_catalog.moc"
