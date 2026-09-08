// ui/DistroSelector.h
//
// ComboBox that lists distros from the catalog + a "Custom ISO" entry
// with a file picker. Also supports downloading and verifying ISOs
// from the distro catalog metadata.

#pragma once

#include "core/Catalog.h"
#include "core/Result.h"

#include <QWidget>

#include <filesystem>
#include <functional>
#include <memory>

class QComboBox;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QProgressBar;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;

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

    // Set a callback for downloading ISOs. The callback should perform
    // the download and verification, reporting progress via the progress
    // callback. This allows the backend to handle the actual download.
    using DownloadCallback = std::function<void(
        const core::Distro& distro,
        const std::filesystem::path& destPath,
        std::function<void(int percent, const QString& status)> progressCallback,
        std::function<void(core::Result<std::filesystem::path>)> finishedCallback)>;

    void setDownloadCallback(DownloadCallback cb);

signals:
    void selectionChanged();
    void isoStatusChanged(const QString& distroKey, const QString& status);

private slots:
    void onCustomToggled(bool checked);
    void onBrowseClicked();
    void onDownloadClicked();
    void onComboIndexChanged(int index);

private:
    void updateIsoStatusDisplay();
    QString getIsoStatus(const core::Distro* distro) const;
    std::filesystem::path getIsoCachePath(const core::Distro* distro) const;

    QComboBox* combo_ = nullptr;
    QCheckBox* customCheck_ = nullptr;
    QLineEdit* customPath_ = nullptr;
    QPushButton* browseBtn_ = nullptr;
    QPushButton* downloadBtn_ = nullptr;
    QProgressBar* downloadProgress_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    const core::Catalog* catalog_ = nullptr;
    DownloadCallback downloadCallback_;
    core::Distro* currentDistro_ = nullptr;
    bool downloading_ = false;
};

}  // namespace ulli::ui