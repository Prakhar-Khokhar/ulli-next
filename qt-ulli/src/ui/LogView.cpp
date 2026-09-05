// ui/LogView.cpp
#include "ui/LogView.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTextStream>
#include <QTimer>

namespace ulli::ui {

LogView::LogView(QWidget* parent) : QWidget(parent) {
    text_ = new QPlainTextEdit(this);
    text_->setReadOnly(true);
    text_->setMaximumBlockCount(5000);
    text_->setFont(QFont("Consolas", 9));

    copyBtn_ = new QPushButton(tr("Copy log"), this);
    openBtn_ = new QPushButton(tr("Open folder"), this);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    btnRow->addWidget(copyBtn_);
    btnRow->addWidget(openBtn_);

    auto* root = new QVBoxLayout(this);
    root->addWidget(text_);
    root->addLayout(btnRow);
    setLayout(root);

    connect(copyBtn_, &QPushButton::clicked, this, &LogView::onCopyClicked);
    connect(openBtn_, &QPushButton::clicked, this, &LogView::onOpenClicked);
}

LogView::~LogView() = default;

void LogView::bindLog(core::ProgressLog* log) {
    if (log_) disconnect(log_, nullptr, this, nullptr);
    log_ = log;
    if (log_) {
        connect(log_, &core::ProgressLog::logAppended,
                this, &LogView::onAppended);
    }
}

QString LogView::logFilePath() const {
    return log_ ? log_->logFilePath() : QString();
}

void LogView::onAppended(QString ts, core::ProgressLog::Severity sev, QString msg) {
    QString prefix;
    switch (sev) {
        case core::ProgressLog::Severity::Error: prefix = "[ERROR] "; break;
        case core::ProgressLog::Severity::Warn:  prefix = "[WARN]  "; break;
        case core::ProgressLog::Severity::Info:  prefix = "[INFO]  "; break;
    }
    text_->appendPlainText(QString("[%1] %2%3").arg(ts, prefix, msg));
}

void LogView::onCopyClicked() {
    const QString path = logFilePath();
    if (path.isEmpty() || !QFile::exists(path)) {
        QMessageBox::information(this, tr("ULLI"),
            tr("Log file not available yet."));
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("ULLI"),
            tr("Cannot read log file: %1").arg(f.errorString()));
        return;
    }
    QGuiApplication::clipboard()->setText(QString::fromLocal8Bit(f.readAll()));
    copyBtn_->setText(tr("Copied!"));
    QTimer::singleShot(1500, this, [this] { copyBtn_->setText(tr("Copy log")); });
}

void LogView::onOpenClicked() {
    const QString path = logFilePath();
    if (path.isEmpty()) return;
    if (!QFile::exists(path)) {
        // Make sure the file exists so Explorer highlights it.
        QFile f(path);
        f.open(QIODevice::WriteOnly | QIODevice::Text);
    }
#if defined(Q_OS_WIN)
    QProcess::startDetached("explorer.exe", {"/select," + QDir::toNativeSeparators(path)});
#else
    QProcess::startDetached("xdg-open", {QFileInfo(path).absolutePath()});
#endif
}

}  // namespace ulli::ui
