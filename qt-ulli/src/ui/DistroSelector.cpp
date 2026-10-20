// ui/DistroSelector.cpp
#include "ui/DistroSelector.h"

#include "core/Catalog.h"
#include "core/Distro.h"
#include "platform/Platform.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFile>
#include <QByteArrayView>
#include <QCheckBox>
#include <QCryptographicHash>
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
        updateCustomIsoReadiness();
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
        setIsoReady(false);
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
        statusLabel_->setText(tr("ISO not downloaded. Size: ~%1")
                                  .arg(QString::fromStdString(currentDistro_->isoFilename())));
        statusLabel_->setStyleSheet("color: #666; font-size: 11px;");
        downloadBtn_->setEnabled(true);
        downloadBtn_->setText(tr("Download ISO"));
        downloadProgress_->setVisible(false);
        downloadProgress_->setValue(0);
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
    downloadBtn_->setEnabled(false);
    downloadProgress_->setVisible(false);
    setIsoReady(false);

    verifyWatcher_ = new QFutureWatcher<bool>(this);
    connect(verifyWatcher_, &QFutureWatcher<bool>::finished,
            this, &DistroSelector::onIsoVerificationFinished);
    verifyWatcher_->setFuture(QtConcurrent::run(&DistroSelector::verifyIsoHash,
                                                currentDistro_, isoPath));
}

bool DistroSelector::verifyIsoHash(const core::Distro* distro,
                                   const std::filesystem::path& isoPath) {
    if (!distro || distro->sha256().empty()) return false;

    QFile f(QString::fromStdString(isoPath.string()));
    if (!f.open(QIODevice::ReadOnly)) return false;

    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr std::size_t kBufferSize = 64 * 1024;
    char buffer[kBufferSize];
    while (!f.atEnd()) {
        qint64 bytesRead = f.read(buffer, kBufferSize);
        if (bytesRead < 0) {
            f.close();
            return false;
        }
        hash.addData(QByteArrayView(buffer, static_cast<qsizetype>(bytesRead)));
    }
    f.close();

    QString actualHex = QString(hash.result().toHex()).toLower();
    QString expected = QString::fromStdString(distro->sha256()).toLower().trimmed();
    return actualHex == expected;
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
        statusLabel_->setText(tr("✓ ISO verified: %1")
                                  .arg(QString::fromStdString(isoPath.string())));
        statusLabel_->setStyleSheet("color: #2a7; font-size: 11px;");
        downloadBtn_->setEnabled(false);
        downloadBtn_->setText(tr("ISO Ready"));
        setIsoReady(true);
    } else {
        statusLabel_->setText(tr("⚠ ISO found but verification failed: %1")
                                  .arg(QString::fromStdString(isoPath.string())));
        statusLabel_->setStyleSheet("color: #c00; font-size: 11px;");
        downloadBtn_->setEnabled(true);
        downloadBtn_->setText(tr("Verify / Re-download"));
        setIsoReady(false);
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
    updateCustomIsoReadiness();
    emit selectionChanged();
}

void DistroSelector::setIsoReady(bool ready) {
    if (isoReady_ == ready) return;
    isoReady_ = ready;
    emit isoReadinessChanged(isoReady_);
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
    setIsoReady(std::filesystem::is_regular_file(
        std::filesystem::path(path.toStdString()), ec) && !ec);
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