// ui/PlanDialog.h
//
// The plan dialog: pick target disk, strategy, and Linux size. Validates
// inputs before allowing OK. The output is a complete InstallPlan
// (minus ISO path / distro which come from the main window).

#pragma once

#include "core/DiskInfo.h"
#include "core/InstallPlan.h"

#include <QDialog>
class QComboBox;
class QRadioButton;
class QSpinBox;

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
    void updateSizeBounds();

private:
    std::vector<core::Disk> disks_;
    QComboBox* diskCombo_ = nullptr;
    QComboBox* shrinkLetterCombo_ = nullptr;
    QRadioButton* shrinkRadio_ = nullptr;
    QRadioButton* freeRadio_ = nullptr;
    QRadioButton* wipeRadio_ = nullptr;
    QSpinBox* linuxSizeSpin_ = nullptr;
};

}  // namespace ulli::ui
