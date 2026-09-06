// ui/RestartCountdownDialog.h
//
// Modal dialog that shows a 30-second countdown before restarting
// the system. The user can cancel at any time. Does not block the
// UI thread - uses a QTimer with a 1-second interval.

#pragma once

#include <QDialog>
#include <QTimer>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace ulli::ui {

class RestartCountdownDialog : public QDialog {
    Q_OBJECT
public:
    explicit RestartCountdownDialog(QWidget* parent = nullptr);
    ~RestartCountdownDialog() override;

    // Starts the countdown. Returns true if the dialog should be shown.
    // The caller should call exec() after this.
    bool startCountdown(int seconds = 30);

signals:
    void countdownFinished();     // emitted when countdown reaches 0
    void countdownCancelled();    // emitted when user clicks Cancel

private slots:
    void onTick();
    void onCancelClicked();

private:
    QLabel* messageLabel_ = nullptr;
    QLabel* countdownLabel_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QTimer* timer_ = nullptr;
    int remainingSeconds_ = 30;
};

}  // namespace ulli::ui