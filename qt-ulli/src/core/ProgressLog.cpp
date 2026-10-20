// core/ProgressLog.cpp
#include "core/ProgressLog.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <iostream>

namespace ulli::core {

ProgressLog::ProgressLog(QObject* parent) : QObject(parent) {}

ProgressLog::~ProgressLog() {
    QMutexLocker lock(&mutex_);
    if (stream_) stream_->flush();
    if (file_) file_->close();
}

bool ProgressLog::openFile(const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());

    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        std::cerr << "WARN: cannot open log file " << path.toStdString() << '\n';
        return false;
    }
    auto stream = std::make_unique<QTextStream>(file.get());

    QMutexLocker lock(&mutex_);
    logFilePath_ = path;
    stream_ = std::move(stream);
    file_ = std::move(file);
    return true;
}

void ProgressLog::append(const QString& message, Severity severity) {
    const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    const QString sevStr = severity == Severity::Error
                               ? "Error"
                               : (severity == Severity::Warn ? "Warn" : "Info");
    {
        // Only the stream write is serialized; the signal is emitted after
        // the lock is released so re-entrant appends cannot deadlock.
        QMutexLocker lock(&mutex_);
        if (stream_) {
            *stream_ << '[' << sevStr << "] [" << ts << "] " << message << '\n';
            stream_->flush();
        }
    }
    emit logAppended(ts, severity, message);
}

}  // namespace ulli::core
