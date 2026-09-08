// ui/PlanDialog.h
//
// The plan dialog: pick target disk, strategy, and Linux size. Validates
// inputs before allowing OK. Shows detailed disk partition layout,
// unallocated space, and planned result. The output is a complete
// InstallPlan (minus ISO path / distro which come from the main window).

#pragma once

#include "core/DiskInfo.h"
#include "core/InstallPlan.h"

#include <QDialog>

class QComboBox;
class QRadioButton;
class QSpinBox;
class QTreeWidget;
class QLabel;
class QPushButton;
class QDialogButtonBox;
class QCheckBox;

namespace ulli::ui {

class PlanDialog : public QDialog {
    Q_OBJECT
public:
    explicit PlanDialog(QWidget* parent = nullptr);

    void setDisks(const std::vector<core::Disk>& disks);
    core::InstallPlan buildPlan(const std::string& distroKey,
                                const std::filesystem::path& isoPath) const;

private slots:
    void onDiskChanged(int index);
    void onStrategyChanged();
    void onLinuxSizeChanged(int value);
    void updatePlanPreview();
    void updateOkButtonState();
    void onAcceptClicked();

private:
    void populateDiskPartitions(const core::Disk& disk);
    void updateStrategyAvailability(const core::Disk& disk);
    void updateSizeBounds();
    void updatePlanPreviewLabels();
    QString partitionKindToString(core::PartitionKind kind) const;
    QString filesystemToString(core::FileSystem fs) const;
    QString formatPartitionSize(uint64_t bytes) const;

    std::vector<core::Disk> disks_;
    QComboBox* diskCombo_ = nullptr;
    QTreeWidget* partitionTree_ = nullptr;
    QLabel* unallocatedLabel_ = nullptr;
    QRadioButton* shrinkRadio_ = nullptr;
    QRadioButton* freeRadio_ = nullptr;
    QRadioButton* wipeRadio_ = nullptr;
    QSpinBox* linuxSizeSpin_ = nullptr;
    QCheckBox* autoRestartCheck_ = nullptr;
    QLabel* strategyDescription_ = nullptr;
    QLabel* plannedLayoutLabel_ = nullptr;
    QLabel* safetyWarningLabel_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    bool wipeConfirmed_ = false;
};

}  // namespace ulli::ui