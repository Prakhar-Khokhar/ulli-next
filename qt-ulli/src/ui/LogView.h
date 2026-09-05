// ui/LogView.h
//
// Read-only QPlainTextEdit that mirrors the ProgressLog. Supports
// "Copy log" and "Open folder" actions for post-install debugging.

#pragma once

#include "core/ProgressLog.h"

#include <QPlainTextEdit>
#include <QPointer>

class QPushButton;

namespace ulli::ui {

class LogView : public QWidget {
    Q_OBJECT
public:
    explicit LogView(QWidget* parent = nullptr);
    ~LogView() override;

    void bindLog(core::ProgressLog* log);
    QString logFilePath() const;

private slots:
    void onAppended(QString ts, core::ProgressLog::Severity sev, QString msg);
    void onCopyClicked();
    void onOpenClicked();

private:
    QPlainTextEdit* text_ = nullptr;
    QPushButton* copyBtn_ = nullptr;
    QPushButton* openBtn_ = nullptr;
    QPointer<core::ProgressLog> log_;
};

}  // namespace ulli::ui
