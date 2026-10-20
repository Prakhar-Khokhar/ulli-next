// tests/test_progresslog.cpp
#include "core/ProgressLog.h"

#include <QFile>
#include <QHash>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest/QtTest>

#include <thread>
#include <vector>

namespace {

// The log file is opened in text mode, so the file itself may use CRLF
// line endings. QTextStream normalises them to '\n' while reading.
QStringList readLines(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QTextStream in(&f);
    const QString content = in.readAll();
    f.close();
    QStringList lines = content.split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    return lines;
}

}  // namespace

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

        const QStringList lines = readLines(path);
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

        QStringList expected;
        expected.reserve(numThreads * msgsPerThread);
        for (int t = 0; t < numThreads; ++t) {
            for (int i = 0; i < msgsPerThread; ++i) {
                expected.push_back(QString("thread-%1-msg-%2").arg(t).arg(i));
            }
        }

        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (int t = 0; t < numThreads; ++t) {
            threads.emplace_back([&log, t, msgsPerThread]() {
                for (int i = 0; i < msgsPerThread; ++i) {
                    log.append(QString("thread-%1-msg-%2").arg(t).arg(i));
                }
            });
        }
        for (auto& th : threads) th.join();

        const QStringList lines = readLines(path);
        QCOMPARE(lines.size(), numThreads * msgsPerThread);

        // Every line must be complete: a corrupted interleaving would show
        // up as a line that does not end with one of the messages.
        int bad = 0;
        QHash<QString, int> counts;
        for (const QString& line : lines) {
            if (!line.startsWith("[Info] [")) { ++bad; continue; }
            bool matched = false;
            for (const QString& m : expected) {
                if (line.endsWith(m)) {
                    ++counts[m];
                    matched = true;
                    break;
                }
            }
            if (!matched) ++bad;
        }
        QCOMPARE(bad, 0);
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
