// ui/DistroSelector.cpp
#include "ui/DistroSelector.h"

#include "core/Catalog.h"
#include "core/Distro.h"
#include "core/Sha256.h"
#include "platform/Platform.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFile>
#include <QCheckBox>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <filesystem>

namespace ulli::ui {

DistroSelector::DistroSelector(QWidget* parent) : QWidget(parent) {
    auto* label = new QLabel(tr("Distribution"), this);
    label->setStyleSheet("font-weight: bold;");

    combo_ = new QComboBox(this);
    combo_->setMinimumWidth(400);

    customCheck_ = new QCheckBox(tr("Use a custom ISO file"), this);
    customPath_ = new QLineEdit(this);
    customPath_->setReadOnly(true);
    customPath_->setPlaceholderText(tr("No file selected"));
    browseBtn_ = new QPushButton(tr("Browse..."), this);
    customPath_->setEnabled(false);
    browseBtn_->setEnabled(false);

    isoProgress_ = new QProgressBar(this);
    isoProgress_->setRange(0, 100);
    isoProgress_->setValue(0);
    isoProgress_->setVisible(false);
    isoProgress_->setTextVisible(true);
    isoProgress_->setFormat(tr("%p%"));

    statusLabel_ = new QLabel(this);
    statusLabel_->setStyleSheet("color: #666; font-size: 11px;");
    statusLabel_->setWordWrap(true);

    auto* customRow = new QHBoxLayout;
    customRow->addWidget(customPath_, 1);
    customRow->addWidget(browseBtn_);

    auto* root = new QVBoxLayout(this);
    root->addWidget(label);
    root->addWidget(combo_);
    root->addWidget(statusLabel_);
    root->addWidget(isoProgress_);
    root->addWidget(customCheck_);
    root->addLayout(customRow);
    setLayout(root);

    connect(combo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &DistroSelector::onComboIndexChanged);
    connect(customCheck_, &QCheckBox::toggled,
            this, &DistroSelector::onCustomToggled);
    connect(browseBtn_, &QPushButton::clicked,
            this, &DistroSelector::onBrowseClicked);
}

DistroSelector::~DistroSelector() {
    if (verifyWatcher_) {
        verifyWatcher_->disconnect(this);
        verifyWatcher_->deleteLater();
    }
}

void DistroSelector::setCatalog(const core::Catalog* catalog) {
    catalog_ = catalog;
    combo_->clear();
    if (!catalog) return;
    for (const auto& key : catalog->keys()) {
        const auto* d = catalog->find(key);
        if (!d) continue;
        combo_->addItem(d->qlabel(), QString::fromStdString(d->key()));
    }
    onComboIndexChanged(0);
}

void DistroSelector::onCustomToggled(bool checked) {
    customPath_->setEnabled(checked);
    browseBtn_->setEnabled(checked);
    combo_->setEnabled(!checked);
    isoProgress_->setVisible(false);
    statusLabel_->clear();
    if (checked) {
        currentDistro_ = nullptr;
        updateCustomIsoReadiness();
    } else {
        onComboIndexChanged(combo_->currentIndex());
    }
    emit selectionChanged();
}

void DistroSelector::onComboIndexChanged(int index) {
    if (index < 0 || !catalog_) {
        currentDistro_ = nullptr;
        isoProgress_->setVisible(false);
        statusLabel_->clear();
        setIsoReady(false);
        return;
    }
    const QString key = combo_->currentData().toString();
    const auto* distro = catalog_->find(key.toStdString());
    currentDistro_ = const_cast<core::Distro*>(distro);
    updateIsoStatusDisplay();
    // A different distribution invalidates an approved plan.
    emit selectionChanged();
}

void DistroSelector::updateIsoStatusDisplay() {
    if (!currentDistro_) {
        isoProgress_->setVisible(false);
        statusLabel_->clear();
        setIsoReady(false);
        return;
    }

    // Cancel any in-flight verification for a previously selected distro.
    if (verifyWatcher_) {
        verifyWatcher_->disconnect(this);
        verifyWatcher_->deleteLater();
        verifyWatcher_ = nullptr;
        verifyingKey_.clear();
    }

    const auto isoPath = getIsoCachePath(currentDistro_);
    const bool exists = std::filesystem::exists(isoPath);

    if (!exists) {
        statusLabel_->setText(tr("ISO not downloaded yet — it will be fetched "
                                 "after the install plan is approved."));
        statusLabel_->setStyleSheet("color: #666; font-size: 11px;");
        isoProgress_->setVisible(false);
        isoProgress_->setValue(0);
        setIsoReady(false);
        emit isoStatusChanged(combo_->currentData().toString(), statusLabel_->text());
        return;
    }

    // File exists — verify its SHA-256 against the catalog before claiming
    // it is valid. Run off the UI thread so a multi-GB ISO does not freeze
    // the UI; never show "verified" without actually checking the hash.
    // Until the check completes the ISO is *not* ready to install.
    const QString key = QString::fromStdString(currentDistro_->key());
    verifyingKey_ = key;
    statusLabel_->setText(tr("Verifying ISO checksum…"));
    statusLabel_->setStyleSheet("color: #0066cc; font-size: 11px;");
    isoProgress_->setVisible(false);
    setIsoReady(false);

    verifyWatcher_ = new QFutureWatcher<bool>(this);
    connect(verifyWatcher_, &QFutureWatcher<bool>::finished,
            this, &DistroSelector::onIsoVerificationFinished);
    // Capture the digest by value: the worker must not touch the catalog
    // after the window has been destroyed.
    const std::string expectedSha = currentDistro_->sha256();
    const bool hasChecksum = currentDistro_->hasChecksum();
    verifyWatcher_->setFuture(QtConcurrent::run(
        [expectedSha, hasChecksum, isoPath]() -> bool {
            if (!hasChecksum) {
                // No trusted digest: the file is usable but it cannot be
                // claimed to be cryptographically verified.
                return true;
            }
            return static_cast<bool>(
                core::Sha256::verifyFile(isoPath, expectedSha));
        }));
}

void DistroSelector::onIsoVerificationFinished() {
    if (!verifyWatcher_) return;

    // Retrieve the result BEFORE releasing the watcher; accessing it
    // afterwards is a use-after-free (the original crash).
    const bool ok = verifyWatcher_->result();

    // Capture the key this verification was started for, then clear state.
    const QString key = verifyingKey_;

    QFutureWatcher<bool>* watcher = verifyWatcher_;
    verifyWatcher_ = nullptr;
    verifyingKey_.clear();
    watcher->deleteLater();

    // Ignore stale results if the user switched distros while verifying.
    if (!currentDistro_ || key.isEmpty() ||
        QString::fromStdString(currentDistro_->key()) != key) {
        return;
    }

    const auto isoPath = getIsoCachePath(currentDistro_);
    if (ok) {
        const bool verified = currentDistro_->hasChecksum();
        if (verified) {
            statusLabel_->setText(tr("✓ ISO verified: %1")
                                      .arg(QString::fromStdString(isoPath.string())));
            statusLabel_->setStyleSheet("color: #2a7; font-size: 11px;");
        } else {
            statusLabel_->setText(tr("⚠ ISO present but not verified — the "
                                     "catalog has no checksum for it: %1")
                                      .arg(QString::fromStdString(isoPath.string())));
            statusLabel_->setStyleSheet("color: #c60; font-size: 11px;");
        }
        notifyIsoReady(verified);
    } else {
        statusLabel_->setText(tr("⚠ ISO found but verification failed: %1")
                                  .arg(QString::fromStdString(isoPath.string())));
        statusLabel_->setStyleSheet("color: #c00; font-size: 11px;");
        setIsoReady(false);
    }
    isoProgress_->setVisible(false);
    isoProgress_->setValue(0);
    emit isoStatusChanged(combo_->currentData().toString(), statusLabel_->text());
}

std::filesystem::path DistroSelector::getIsoCachePath(const core::Distro* distro) const {
    const std::filesystem::path cacheDir = ulli::platform::cacheDir();
    return cacheDir / distro->isoFilename();
}

std::filesystem::path DistroSelector::isoCachePath() const {
    if (!currentDistro_) return {};
    return getIsoCachePath(currentDistro_);
}

QString DistroSelector::isoStatusText() const {
    return statusLabel_->text();
}

void DistroSelector::onBrowseClicked() {
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Select Linux ISO"), QString(),
        tr("ISO Files (*.iso);;All Files (*)"));
    if (f.isEmpty()) return;
    customPath_->setText(f);
    updateCustomIsoReadiness();
    emit selectionChanged();
}

void DistroSelector::setIsoReady(bool ready) {
    if (isoReady_ == ready) {
        return;
    }
    isoReady_ = ready;
    if (!ready) {
        isoVerified_ = false;
    }
    emit isoReadinessChanged(isoReady_);
}

void DistroSelector::notifyIsoReady(bool verified) {
    isoVerified_ = verified;
    setIsoReady(true);
}

void DistroSelector::notifyIsoInvalid() {
    // The ISO was rejected: it is not ready and not verified any more.
    // The status text is set by the caller (setIsoStage).
    setIsoReady(false);
}

void DistroSelector::updateCustomIsoReadiness() {
    // A custom ISO has no catalog checksum, so readiness only means the
    // file is present. The engine re-validates the file before it touches
    // any disk.
    const QString path = customPath_->text();
    if (path.isEmpty()) {
        setIsoReady(false);
        return;
    }
    std::error_code ec;
    const bool isFile = std::filesystem::is_regular_file(
        std::filesystem::path(path.toStdString()), ec) && !ec;
    setIsoReady(isFile);
    if (isFile) {
        notifyIsoReady(false);
        statusLabel_->setText(tr("Custom ISO: %1").arg(path));
        statusLabel_->setStyleSheet("color: #666; font-size: 11px;");
    }
}

QString DistroSelector::selectedKey() const {
    if (isCustom()) return QString();
    return combo_->currentData().toString();
}

QString DistroSelector::customIsoPath() const {
    return customPath_->text();
}

bool DistroSelector::isCustom() const {
    return customCheck_->isChecked();
}

void DistroSelector::setIsoStage(const QString& status, int percent, bool busy) {
    if (!status.isEmpty()) {
        statusLabel_->setText(status);
        statusLabel_->setStyleSheet("color: #0066cc; font-size: 11px;");
    }
    isoProgress_->setVisible(busy);
    if (busy) {
        isoProgress_->setValue(percent);
    }
}

}  // namespace ulli::ui
