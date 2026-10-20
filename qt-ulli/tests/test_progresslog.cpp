// tests/test_progresslog.cpp
#include "core/ProgressLog.h"

#include <QFile>
#include <QMap>
#include <QTemporaryDir>
#include <QStringList>
#include <QtTest/QtTest>

#include <thread>
#include <vector>

class TestProgressLog : public QObject {
    Q_OBJECT
private slots:
    void singleThreadedAppendWritesToFile() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = tmp.path() + "/log.txt";

        ulli::core::ProgressLog log;
        QVERIFY(log.openFile(path));

        log.append("first");
        log.append("second");
        log.append("third");

        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QString::fromUtf8(f.readAll());
        f.close();

        QStringList lines = content.split('\n');
        if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();

        QCOMPARE(lines.size(), 3);
        QVERIFY(lines[0].startsWith("[Info] ["));
        QVERIFY(lines[0].contains("first"));
        QVERIFY(lines[1].startsWith("[Info] ["));
        QVERIFY(lines[1].contains("second"));
        QVERIFY(lines[2].startsWith("[Info] ["));
        QVERIFY(lines[2].contains("third"));
    }

    void concurrentAppendsAreAllPersisted() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = tmp.path() + "/log.txt";

        ulli::core::ProgressLog log;
        QVERIFY(log.openFile(path));

        const int numThreads = 4;
        const int msgsPerThread = 25;

        std::vector<QString> expected;
        expected.reserve(numThreads * msgsPerThread);
        for (int t = 0; t < numThreads; ++t) {
            for (int i = 0; i < msgsPerThread; ++i) {
                expected.push_back(QString("thread-%1-msg-%2").arg(t).arg(i));
            }
        }

        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (int t = 0; t < numThreads; ++t) {
            threads.emplace_back([&log, t]() {
                for (int i = 0; i < msgsPerThread; ++i) {
                    log.append(QString("thread-%1-msg-%2").arg(t).arg(i));
                }
            });
        }
        for (auto& th : threads) th.join();

        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
        f.close();
        if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();

        QCOMPARE(lines.size(), numThreads * msgsPerThread);

        int bad = 0;
        for (const QString& line : lines) {
            if (!line.startsWith("[Info] [")) { ++bad; continue; }
            bool endsWithMsg = false;
            for (const QString& m : expected) {
                if (line.endsWith(m)) { endsWithMsg = true; break; }
            }
            if (!endsWithMsg) ++bad;
        }
        QCOMPARE(bad, 0);

        QMap<QString, int> counts;
        for (const QString& line : lines) {
            for (const QString& m : expected) {
                if (line.endsWith(m)) { ++counts[m]; break; }
            }
        }
        for (const QString& m : expected) {
            QCOMPARE(counts.value(m), 1);
        }
    }

    void signalIsEmittedForEveryAppend() {
        ulli::core::ProgressLog log;
        int count = 0;
        QObject::connect(&log, &ulli::core::ProgressLog::logAppended,
                         [&count](QString, ulli::core::ProgressLog::Severity, QString) {
                             ++count;
                         });

        for (int i = 0; i < 5; ++i) {
            log.append(QString("m%1").arg(i));
        }
        QCOMPARE(count, 5);
    }
};

QTEST_GUILESS_MAIN(TestProgressLog)
#include "test_progresslog.moc"
