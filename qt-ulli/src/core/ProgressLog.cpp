// core/ProgressLog.cpp
#include "core/ProgressLog.h"

#include <QDir>
#include <QStandardPaths>

#include <iostream>

namespace ulli::core {

ProgressLog::ProgressLog(QObject* parent) : QObject(parent) {}

ProgressLog::~ProgressLog() {
    if (stream_) stream_->flush();
    if (file_) file_->close();
}

bool ProgressLog::openFile(const QString& path) {
    logFilePath_ = path;
    QFileInfo info(path);
    QDir().mkpath(info.absolutePath());

    file_ = std::make_unique<QFile>(path);
    if (!file_->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        std::cerr << "WARN: cannot open log file " << path.toStdString() << '\n';
        file_.reset();
        return false;
    }
    stream_ = std::make_unique<QTextStream>(file_.get());
    return true;
}

void ProgressLog::append(const QString& message, Severity severity) {
    const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    const QString sevStr = severity == Severity::Error
                               ? "Error"
                               : (severity == Severity::Warn ? "Warn" : "Info");
    if (stream_) {
        *stream_ << '[' << sevStr << "] [" << ts << "] " << message << '\n';
        stream_->flush();
    }
    emit logAppended(ts, severity, message);
}

}  // namespace ulli::core
