// ui/DistroSelector.cpp
#include "ui/DistroSelector.h"

#include "core/Catalog.h"
#include "core/Distro.h"
#include "platform/Platform.h"

#include <QComboBox>
#include <QFileDialog>
#include <QCheckBox>
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

    downloadBtn_ = new QPushButton(tr("Download ISO"), this);
    downloadBtn_->setEnabled(false);
    downloadBtn_->setMinimumHeight(32);

    downloadProgress_ = new QProgressBar(this);
    downloadProgress_->setRange(0, 100);
    downloadProgress_->setValue(0);
    downloadProgress_->setVisible(false);
    downloadProgress_->setTextVisible(true);
    downloadProgress_->setFormat(tr("Downloading: %p%"));

    statusLabel_ = new QLabel(this);
    statusLabel_->setStyleSheet("color: #666; font-size: 11px;");
    statusLabel_->setWordWrap(true);

    auto* customRow = new QHBoxLayout;
    customRow->addWidget(customPath_, 1);
    customRow->addWidget(browseBtn_);

    auto* downloadRow = new QHBoxLayout;
    downloadRow->addWidget(downloadBtn_);
    downloadRow->addWidget(downloadProgress_, 1);

    auto* root = new QVBoxLayout(this);
    root->addWidget(label);
    root->addWidget(combo_);
    root->addWidget(statusLabel_);
    root->addLayout(downloadRow);
    root->addWidget(customCheck_);
    root->addLayout(customRow);
    setLayout(root);

    connect(combo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &DistroSelector::onComboIndexChanged);
    connect(customCheck_, &QCheckBox::toggled,
            this, &DistroSelector::onCustomToggled);
    connect(browseBtn_, &QPushButton::clicked,
            this, &DistroSelector::onBrowseClicked);
    connect(downloadBtn_, &QPushButton::clicked,
            this, &DistroSelector::onDownloadClicked);
}

DistroSelector::~DistroSelector() = default;

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

void DistroSelector::setDownloadCallback(DownloadCallback cb) {
    downloadCallback_ = std::move(cb);
}

void DistroSelector::onCustomToggled(bool checked) {
    customPath_->setEnabled(checked);
    browseBtn_->setEnabled(checked);
    combo_->setEnabled(!checked);
    downloadBtn_->setEnabled(false);
    downloadProgress_->setVisible(false);
    statusLabel_->clear();
    if (checked) {
        currentDistro_ = nullptr;
    } else {
        onComboIndexChanged(combo_->currentIndex());
    }
    emit selectionChanged();
}

void DistroSelector::onComboIndexChanged(int index) {
    if (index < 0 || !catalog_) {
        currentDistro_ = nullptr;
        downloadBtn_->setEnabled(false);
        downloadProgress_->setVisible(false);
        statusLabel_->clear();
        return;
    }
    const QString key = combo_->currentData().toString();
    const auto* distro = catalog_->find(key.toStdString());
    currentDistro_ = const_cast<core::Distro*>(distro);
    updateIsoStatusDisplay();
}

void DistroSelector::updateIsoStatusDisplay() {
    if (!currentDistro_) {
        downloadBtn_->setEnabled(false);
        downloadProgress_->setVisible(false);
        statusLabel_->clear();
        return;
    }

    const auto isoPath = getIsoCachePath(currentDistro_);
    const bool exists = std::filesystem::exists(isoPath);
    const bool verified = exists && !currentDistro_->sha256().empty();

    if (verified) {
        statusLabel_->setText(tr("✓ ISO verified: %1").arg(QString::fromStdString(isoPath.string())));
        statusLabel_->setStyleSheet("color: #2a7; font-size: 11px;");
        downloadBtn_->setEnabled(false);
        downloadBtn_->setText(tr("ISO Ready"));
    } else if (exists) {
        statusLabel_->setText(tr("⚠ ISO found but not verified: %1").arg(QString::fromStdString(isoPath.string())));
        statusLabel_->setStyleSheet("color: #c60; font-size: 11px;");
        downloadBtn_->setEnabled(true);
        downloadBtn_->setText(tr("Verify / Re-download"));
    } else {
        statusLabel_->setText(tr("ISO not downloaded. Size: ~%1").arg(QString::fromStdString(currentDistro_->isoFilename())));
        statusLabel_->setStyleSheet("color: #666; font-size: 11px;");
        downloadBtn_->setEnabled(true);
        downloadBtn_->setText(tr("Download ISO"));
    }
    downloadProgress_->setVisible(false);
    downloadProgress_->setValue(0);
    emit isoStatusChanged(combo_->currentData().toString(), statusLabel_->text());
}

std::filesystem::path DistroSelector::getIsoCachePath(const core::Distro* distro) const {
    const std::filesystem::path cacheDir = ulli::platform::cacheDir();
    return cacheDir / distro->isoFilename();
}

void DistroSelector::onBrowseClicked() {
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Select Linux ISO"), QString(),
        tr("ISO Files (*.iso);;All Files (*)"));
    if (f.isEmpty()) return;
    customPath_->setText(f);
    emit selectionChanged();
}

void DistroSelector::onDownloadClicked() {
    if (!currentDistro_ || !downloadCallback_ || downloading_) return;

    downloading_ = true;
    downloadBtn_->setEnabled(false);
    downloadProgress_->setVisible(true);
    downloadProgress_->setValue(0);
    statusLabel_->setText(tr("Starting download..."));
    statusLabel_->setStyleSheet("color: #0066cc; font-size: 11px;");

    const auto destPath = getIsoCachePath(currentDistro_);

    downloadCallback_(*currentDistro_, destPath,
        [this](int percent, const QString& status) {
            QMetaObject::invokeMethod(this, [this, percent, status]() {
                downloadProgress_->setValue(percent);
                if (!status.isEmpty()) {
                    statusLabel_->setText(status);
                }
            }, Qt::QueuedConnection);
        },
        [this](core::Result<std::filesystem::path> result) {
            QMetaObject::invokeMethod(this, [this, result]() {
                downloading_ = false;
                downloadProgress_->setVisible(false);
                if (result) {
                    statusLabel_->setText(tr("✓ Download complete and verified"));
                    statusLabel_->setStyleSheet("color: #2a7; font-size: 11px;");
                    downloadBtn_->setEnabled(false);
                    downloadBtn_->setText(tr("ISO Ready"));
                } else {
                    statusLabel_->setText(tr("✗ Download failed: %1").arg(result.error().qmessage()));
                    statusLabel_->setStyleSheet("color: #c00; font-size: 11px;");
                    downloadBtn_->setEnabled(true);
                    downloadBtn_->setText(tr("Retry Download"));
                }
                updateIsoStatusDisplay();
                emit selectionChanged();
            }, Qt::QueuedConnection);
        });
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

}  // namespace ulli::ui