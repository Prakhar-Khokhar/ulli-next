// ui/RestartCountdownDialog.cpp
#include "ui/RestartCountdownDialog.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace ulli::ui {

RestartCountdownDialog::RestartCountdownDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Restart Required"));
    setWindowModality(Qt::ApplicationModal);
    setFixedSize(400, 200);
    setWindowFlags(windowFlags() & ~Qt::WindowCloseButtonHint);

    messageLabel_ = new QLabel(
        tr("The installation was successful.\n"
           "The system will restart in %1 seconds to apply changes."),
        this);
    messageLabel_->setWordWrap(true);
    messageLabel_->setAlignment(Qt::AlignCenter);

    countdownLabel_ = new QLabel(this);
    countdownLabel_->setAlignment(Qt::AlignCenter);
    countdownLabel_->setStyleSheet("font-size: 24pt; font-weight: bold; color: #2a7;");

    cancelBtn_ = new QPushButton(tr("Cancel Restart"), this);
    cancelBtn_->setMinimumHeight(36);

    auto* layout = new QVBoxLayout(this);
    layout->addStretch(1);
    layout->addWidget(messageLabel_);
    layout->addWidget(countdownLabel_);
    layout->addStretch(1);
    layout->addWidget(cancelBtn_);
    setLayout(layout);

    timer_ = new QTimer(this);
    timer_->setInterval(1000);

    connect(timer_, &QTimer::timeout, this, &RestartCountdownDialog::onTick);
    connect(cancelBtn_, &QPushButton::clicked, this, &RestartCountdownDialog::onCancelClicked);
}

RestartCountdownDialog::~RestartCountdownDialog() {
    if (timer_) timer_->stop();
}

bool RestartCountdownDialog::startCountdown(int seconds) {
    remainingSeconds_ = seconds;
    countdownLabel_->setText(QString::number(remainingSeconds_) + tr("s"));
    timer_->start();
    return true;
}

void RestartCountdownDialog::onTick() {
    if (remainingSeconds_ <= 0) {
        timer_->stop();
        emit countdownFinished();
        accept();  // closes dialog with Accepted
        return;
    }
    --remainingSeconds_;
    countdownLabel_->setText(QString::number(remainingSeconds_) + tr("s"));
}

void RestartCountdownDialog::onCancelClicked() {
    timer_->stop();
    emit countdownCancelled();
    reject();  // closes dialog with Rejected
}

}  // namespace ulli::ui