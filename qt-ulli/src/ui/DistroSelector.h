// ui/DistroSelector.h
//
// Distribution picker plus the ISO status of the current selection.
//
// The widget is deliberately *not* responsible for downloading: an ISO is
// only fetched after the user has reviewed and approved a disk plan (see
// MainWindow). The widget reports the state of the cached ISO and lets the
// main window drive the download / verification stage display.

#pragma once

#include "core/Catalog.h"
#include "core/Result.h"

#include <QWidget>

#include <QFile>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>

#include <filesystem>
#include <functional>
#include <memory>

class QComboBox;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QProgressBar;
class QLabel;

namespace ulli::ui {

class DistroSelector : public QWidget {
    Q_OBJECT
public:
    explicit DistroSelector(QWidget* parent = nullptr);
    ~DistroSelector() override;

    void setCatalog(const core::Catalog* catalog);
    QString selectedKey() const;
    QString customIsoPath() const;
    bool isCustom() const;

    // True when the selected ISO is present on disk. When the catalog
    // provides a digest the file has also been checksum verified.
    bool isoReady() const { return isoReady_; }

    // The distro of the current selection (null for a custom ISO).
    const core::Distro* currentDistro() const { return currentDistro_; }

    // True when the ISO of the current selection has been verified against
    // the catalog digest.
    bool isoVerified() const { return isoVerified_; }

    // Human readable state of the ISO of the current selection.
    QString isoStatusText() const;

    // Cache path of the ISO of the current selection (empty for a custom
    // ISO, which has its own path).
    std::filesystem::path isoCachePath() const;

    // Drives the download / verification stage display from the main window.
    void setIsoStage(const QString& status, int percent, bool busy);

    // Called by the main window once an ISO has been fetched and checked.
    // `verified` is true only when the content was checked against a
    // trusted digest. It is never derived from the size of the file: a
    // file can be modified without changing its size.
    void notifyIsoReady(bool verified);

    // Called by the main window when the ISO of the current selection was
    // rejected - for example because the cached file does not match the
    // expected digest. A rejected ISO is neither ready nor verified, so it
    // must not be offered for an installation any more.
    void notifyIsoInvalid();

    signals:
    void selectionChanged();
    void isoStatusChanged(const QString& distroKey, const QString& status);
    void isoReadinessChanged(bool ready);

private slots:
    void onCustomToggled(bool checked);
    void onBrowseClicked();
    void onComboIndexChanged(int index);
    void onIsoVerificationFinished();

private:
    void updateIsoStatusDisplay();
    void setIsoReady(bool ready);
    void updateCustomIsoReadiness();
    std::filesystem::path getIsoCachePath(const core::Distro* distro) const;

    QComboBox* combo_ = nullptr;
    QCheckBox* customCheck_ = nullptr;
    QLineEdit* customPath_ = nullptr;
    QPushButton* browseBtn_ = nullptr;
    QProgressBar* isoProgress_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    const core::Catalog* catalog_ = nullptr;
    core::Distro* currentDistro_ = nullptr;

    // In-flight asynchronous ISO verification, so the UI never claims an
    // ISO is "verified" without actually checking its SHA-256.
    QFutureWatcher<bool>* verifyWatcher_ = nullptr;
    QString verifyingKey_;

    // True when the selected ISO is present (and verified when the catalog
    // provides a digest).
    bool isoReady_ = false;
    bool isoVerified_ = false;
};

}  // namespace ulli::ui
