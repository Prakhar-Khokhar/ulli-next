// ui/PlanDialog.cpp
#include "ui/PlanDialog.h"

#include "core/DiskInfo.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace ulli::ui {

PlanDialog::PlanDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Install plan"));
    setMinimumSize(640, 480);

    // ─── Disk section ────────────────────────────────────────────────────
    auto* diskGroup = new QGroupBox(tr("Target disk"), this);
    diskCombo_ = new QComboBox(diskGroup);
    auto* diskForm = new QFormLayout(diskGroup);
    diskForm->addRow(tr("Disk:"), diskCombo_);

    auto* strategyGroup = new QGroupBox(tr("Strategy"), this);
    shrinkRadio_ = new QRadioButton(tr("Shrink target by Linux + boot"), strategyGroup);
    freeRadio_   = new QRadioButton(tr("Use existing free space (no shrink)"), strategyGroup);
    wipeRadio_   = new QRadioButton(tr("Wipe the target disk (DESTRUCTIVE)"), strategyGroup);
    shrinkRadio_->setChecked(true);
    auto* strategyLayout = new QVBoxLayout(strategyGroup);
    strategyLayout->addWidget(shrinkRadio_);
    strategyLayout->addWidget(freeRadio_);
    strategyLayout->addWidget(wipeRadio_);

    auto* shrinkLetterGroup = new QGroupBox(tr("Shrink volume"), this);
    shrinkLetterCombo_ = new QComboBox(shrinkLetterGroup);
    auto* slForm = new QFormLayout(shrinkLetterGroup);
    slForm->addRow(tr("Letter:"), shrinkLetterCombo_);

    // ─── Size section ────────────────────────────────────────────────────
    auto* sizeGroup = new QGroupBox(tr("Linux partition size (GB)"), this);
    linuxSizeSpin_ = new QSpinBox(sizeGroup);
    linuxSizeSpin_->setRange(20, 10000);
    linuxSizeSpin_->setValue(30);
    linuxSizeSpin_->setSuffix(" GB");
    auto* sizeLayout = new QHBoxLayout(sizeGroup);
    sizeLayout->addWidget(linuxSizeSpin_);

    // ─── Buttons ─────────────────────────────────────────────────────────
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                        this);

    auto* root = new QVBoxLayout(this);
    root->addWidget(diskGroup);
    root->addWidget(strategyGroup);
    root->addWidget(shrinkLetterGroup);
    root->addWidget(sizeGroup);
    root->addStretch(1);
    root->addWidget(buttons);
    setLayout(root);

    connect(diskCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &PlanDialog::onDiskChanged);
    connect(linuxSizeSpin_, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int) { updateSizeBounds(); });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void PlanDialog::setDisks(const std::vector<core::Disk>& disks) {
    disks_ = disks;
    diskCombo_->clear();
    shrinkLetterCombo_->clear();
    for (std::vector<core::Disk>::size_type i = 0; i < disks.size(); ++i) {
        const auto& d = disks[i];
        diskCombo_->addItem(
            QString("Disk %1 — %2 (%3)").arg(d.number)
                .arg(d.qmodel().isEmpty() ? "Unknown" : d.qmodel())
                .arg(core::formatBytes(d.sizeBytes)),
            QVariant::fromValue<int>(static_cast<int>(i)));
        for (const auto& p : d.partitions) {
            if (p.driveLetter.has_value()) {
                const double sizeGiB = static_cast<double>(p.sizeBytes) / (1024.0 * 1024.0 * 1024.0);
                shrinkLetterCombo_->addItem(
                    QString("%1: — %2 GB").arg(p.driveLetter.value())
                        .arg(sizeGiB, 0, 'f', 1));
            }
        }
    }
    onDiskChanged(0);
}

void PlanDialog::onDiskChanged(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= disks_.size()) return;
    const auto& d = disks_[static_cast<std::size_t>(index)];
    wipeRadio_->setEnabled(!d.isSystem());
    wipeRadio_->setVisible(!d.isSystem());
    updateSizeBounds();
}

void PlanDialog::updateSizeBounds() {
    const int idx = diskCombo_->currentIndex();
    if (idx < 0 || static_cast<std::size_t>(idx) >= disks_.size()) return;
    const auto& d = disks_[static_cast<std::size_t>(idx)];
    const std::uint64_t free = d.unallocatedBytes;
    const int minSize = 20;
    int maxSize = static_cast<int>(free / (1024ull * 1024 * 1024)) - 7;
    if (maxSize < minSize) maxSize = minSize;
    if (maxSize > 10000) maxSize = 10000;
    linuxSizeSpin_->setRange(minSize, maxSize);
    if (linuxSizeSpin_->value() > maxSize) linuxSizeSpin_->setValue(maxSize);
    if (linuxSizeSpin_->value() < minSize) linuxSizeSpin_->setValue(minSize);
}

core::InstallPlan PlanDialog::buildPlan(const std::string& distroKey,
                                          const std::filesystem::path& isoPath) const {
    core::InstallPlan p;
    p.distroKey = distroKey;
    p.isoPath = isoPath;

    const int idx = diskCombo_->currentIndex();
    if (idx >= 0 && idx < static_cast<int>(disks_.size())) {
        p.targetDiskNumber = disks_[static_cast<std::size_t>(idx)].number;
    }
    p.linuxSizeBytes = static_cast<std::uint64_t>(linuxSizeSpin_->value()) * 1024 * 1024 * 1024;

    if (wipeRadio_->isChecked()) {
        p.strategy = core::Strategy::WipeDisk;
    } else if (freeRadio_->isChecked()) {
        p.strategy = core::Strategy::UseFreeAll;
    } else {
        p.strategy = core::Strategy::ShrinkAll;
        if (shrinkLetterCombo_->currentIndex() >= 0) {
            const QString txt = shrinkLetterCombo_->currentText();
            if (!txt.isEmpty()) {
                p.shrinkDriveLetter = txt.at(0).toLatin1();
                p.shrinkAmountBytes = p.linuxSizeBytes + p.bootSizeBytes + p.refindSizeBytes;
            }
        }
    }
    return p;
}

}  // namespace ulli::ui
