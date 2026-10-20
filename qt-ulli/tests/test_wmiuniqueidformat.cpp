// tests/test_wmiuniqueidformat.cpp
//
// Focused test for the PowerShell serialization of the unique id format
// in platform/windows/Wmi.cpp.
//
// The WMI script is embedded in the C++ source, so the test takes the
// expression that is actually deployed out of the source file and
// evaluates it with PowerShell. That covers the serialization of the
// numeric zero, which a plain "if ($value)" test would swallow: in
// PowerShell the number 0 is falsy, and 0 is a valid descriptor type
// ("Vendor Specific").
//
// The WMI query itself (Get-Disk) needs real storage hardware and an
// elevated shell, so it cannot be exercised here. The test is skipped
// when no PowerShell is installed.

#include <QtTest/QtTest>

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

// Extracts the expression the WMI script uses for the unique id format.
QString uniqueIdFormatExpression(const QString& source) {
    const QString marker = QStringLiteral("UniqueIdFormat=");
    for (const QString& line : source.split(QLatin1Char('\n'))) {
        const qsizetype pos = line.indexOf(marker);
        if (pos < 0) continue;
        return line.mid(pos + marker.size()).trimmed();
    }
    return QString();
}

}  // namespace

class TestWmiUniqueIdFormat : public QObject {
    Q_OBJECT

private slots:
    void numericZeroIsSerializedAsPresent();
};

void TestWmiUniqueIdFormat::numericZeroIsSerializedAsPresent() {
    const QString pwsh = QStandardPaths::findExecutable("pwsh");
    if (pwsh.isEmpty()) {
        QSKIP("PowerShell (pwsh) is not installed");
    }

    const QString sourcePath =
        QStringLiteral(ULLI_SOURCE_ROOT "/src/platform/windows/Wmi.cpp");
    QFile source(sourcePath);
    QVERIFY2(source.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable("cannot read " + sourcePath));
    QTextStream in(&source);
    const QString expression = uniqueIdFormatExpression(in.readAll());
    QVERIFY2(!expression.isEmpty(),
             "the unique id format expression was not found in Wmi.cpp");

    // Evaluates the shipped expression with the given property value and
    // returns what the script would serialize.
    const auto serialize = [&](const QString& propertyValue) {
        QTemporaryDir dir;
        if (!dir.isValid()) return QStringLiteral("<no temporary directory>");
        const QString scriptPath = dir.filePath("uniqueidformat.ps1");
        {
            QFile script(scriptPath);
            if (!script.open(QIODevice::WriteOnly | QIODevice::Text)) {
                return QStringLiteral("<cannot write the script>");
            }
            QTextStream out(&script);
            out << "$d = [pscustomobject]@{ UniqueIdFormat = " << propertyValue
                << " }\n"
                << "$value = " << expression << "\n"
                << "[Console]::Out.Write($value)\n";
        }

        QProcess process;
        process.start(pwsh, {QStringLiteral("-NoProfile"),
                             QStringLiteral("-NonInteractive"),
                             QStringLiteral("-File"), scriptPath});
        if (!process.waitForStarted(15000)) {
            return QStringLiteral("<pwsh did not start>");
        }
        if (!process.waitForFinished(30000)) {
            return QStringLiteral("<pwsh did not finish>");
        }
        if (process.exitCode() != 0) {
            return QStringLiteral("<pwsh failed: ") +
                   QString::fromUtf8(process.readAllStandardError()).trimmed() + ">";
        }
        return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    };

    // 0 is a valid descriptor type ("Vendor Specific") and has to survive
    // the serialization as a non-empty value.
    QCOMPARE(serialize(QStringLiteral("0")), QStringLiteral("0"));

    // A property that is not set stays empty.
    QCOMPARE(serialize(QStringLiteral("$null")), QString());
}

QTEST_MAIN(TestWmiUniqueIdFormat)
#include "test_wmiuniqueidformat.moc"
