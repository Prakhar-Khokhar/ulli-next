// core/ProgressLog.h
//
// Thread-safe log buffer + Qt signals. The install engine runs on a
// QThread and appends messages; the UI binds to logAppended() and
// severityChanged() to render them. Also persists to a file in the
// per-user cache directory for post-install debugging.

#pragma once

#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QString>
#include <QTextStream>

#include <memory>

namespace ulli::core {

class ProgressLog : public QObject {
    Q_OBJECT
public:
    enum class Severity { Info, Warn, Error };

    explicit ProgressLog(QObject* parent = nullptr);
    ~ProgressLog() override;

    // Open a log file. Safe to call once at startup. Errors are
    // reported via the append() callback but never throw.
    bool openFile(const QString& path);

    // Append a log message. Always emits logAppended so the UI can
    // render; also writes to the file if one was opened successfully.
    void append(const QString& message,
                Severity severity = Severity::Info);

    QString logFilePath() const { return logFilePath_; }

signals:
    void logAppended(QString timestamp, ulli::core::ProgressLog::Severity severity,
                     QString message);

private:
    QString logFilePath_;
    std::unique_ptr<QFile> file_;
    std::unique_ptr<QTextStream> stream_;
};

}  // namespace ulli::core

Q_DECLARE_METATYPE(ulli::core::ProgressLog::Severity)
