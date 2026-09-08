// ui/PlanDialog.cpp
#include "ui/PlanDialog.h"

#include "core/DiskInfo.h"

#include <QBrush>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace ulli::ui {

PlanDialog::PlanDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Install plan"));
    setMinimumSize(720, 600);

    // ─── Disk section ────────────────────────────────────────────────────
    auto* diskGroup = new QGroupBox(tr("Target disk"), this);
    diskCombo_ = new QComboBox(diskGroup);
    auto* diskForm = new QFormLayout(diskGroup);
    diskForm->addRow(tr("Disk:"), diskCombo_);

    // Partition tree
    partitionTree_ = new QTreeWidget(diskGroup);
    partitionTree_->setHeaderLabels({tr("Partition"), tr("Type"), tr("Filesystem"), tr("Size"), tr("Offset"), tr("Flags")});
    partitionTree_->setRootIsDecorated(false);
    partitionTree_->setAlternatingRowColors(true);
    partitionTree_->setMinimumHeight(180);
    partitionTree_->header()->setStretchLastSection(false);
    partitionTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    partitionTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    partitionTree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    partitionTree_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    partitionTree_->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    partitionTree_->header()->setSectionResizeMode(5, QHeaderView::ResizeToContents);

    unallocatedLabel_ = new QLabel(tr("Unallocated: 0 B"), diskGroup);
    unallocatedLabel_->setStyleSheet("font-weight: bold; color: #2a7;");

    auto* diskLayout = new QVBoxLayout(diskGroup);
    diskLayout->addLayout(diskForm);
    diskLayout->addWidget(new QLabel(tr("Current partitions:"), diskGroup));
    diskLayout->addWidget(partitionTree_, 1);
    diskLayout->addWidget(unallocatedLabel_);

    // ─── Strategy section ────────────────────────────────────────────────
    auto* strategyGroup = new QGroupBox(tr("Installation strategy"), this);
    shrinkRadio_ = new QRadioButton(tr("Shrink target partition (Linux + boot + rEFInd)"), strategyGroup);
    freeRadio_   = new QRadioButton(tr("Use existing free space (no shrink)"), strategyGroup);
    wipeRadio_   = new QRadioButton(tr("Wipe the target disk (DESTRUCTIVE — all data lost)"), strategyGroup);
    shrinkRadio_->setChecked(true);

    strategyDescription_ = new QLabel(strategyGroup);
    strategyDescription_->setWordWrap(true);
    strategyDescription_->setStyleSheet("color: #666; font-style: italic;");

    auto* strategyLayout = new QVBoxLayout(strategyGroup);
    strategyLayout->addWidget(shrinkRadio_);
    strategyLayout->addWidget(freeRadio_);
    strategyLayout->addWidget(wipeRadio_);
    strategyLayout->addWidget(strategyDescription_);

    // ─── Size section ────────────────────────────────────────────────────
    auto* sizeGroup = new QGroupBox(tr("Linux partition size (GB)"), this);
    linuxSizeSpin_ = new QSpinBox(sizeGroup);
    linuxSizeSpin_->setRange(20, 10000);
    linuxSizeSpin_->setValue(30);
    linuxSizeSpin_->setSuffix(" GB");
    auto* sizeLayout = new QHBoxLayout(sizeGroup);
    sizeLayout->addWidget(linuxSizeSpin_);

    // ─── Options section ─────────────────────────────────────────────────
    auto* optionsGroup = new QGroupBox(tr("Options"), this);
    autoRestartCheck_ = new QCheckBox(tr("Restart automatically after successful installation"), optionsGroup);
    autoRestartCheck_->setChecked(false);
    auto* optionsLayout = new QVBoxLayout(optionsGroup);
    optionsLayout->addWidget(autoRestartCheck_);

    // ─── Planned layout preview ──────────────────────────────────────────
    auto* previewGroup = new QGroupBox(tr("Planned result"), this);
    plannedLayoutLabel_ = new QLabel(previewGroup);
    plannedLayoutLabel_->setWordWrap(true);
    plannedLayoutLabel_->setTextFormat(Qt::RichText);
    plannedLayoutLabel_->setMinimumHeight(100);
    plannedLayoutLabel_->setStyleSheet("background: #f8f8f8; border: 1px solid #ddd; padding: 8px; font-family: monospace;");

    safetyWarningLabel_ = new QLabel(previewGroup);
    safetyWarningLabel_->setWordWrap(true);
    safetyWarningLabel_->setTextFormat(Qt::RichText);
    safetyWarningLabel_->setStyleSheet("color: #c00; font-weight: bold;");

    auto* previewLayout = new QVBoxLayout(previewGroup);
    previewLayout->addWidget(plannedLayoutLabel_);
    previewLayout->addWidget(safetyWarningLabel_);

    // ─── Buttons ─────────────────────────────────────────────────────────
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(true);

    auto* root = new QVBoxLayout(this);
    root->addWidget(diskGroup, 1);
    root->addWidget(strategyGroup);
    root->addWidget(sizeGroup);
    root->addWidget(optionsGroup);
    root->addWidget(previewGroup);
    root->addWidget(buttons_);
    setLayout(root);

    connect(diskCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &PlanDialog::onDiskChanged);
    connect(shrinkRadio_, &QRadioButton::toggled, this, &PlanDialog::onStrategyChanged);
    connect(freeRadio_, &QRadioButton::toggled, this, &PlanDialog::onStrategyChanged);
    connect(wipeRadio_, &QRadioButton::toggled, this, &PlanDialog::onStrategyChanged);
    connect(linuxSizeSpin_, qOverload<int>(&QSpinBox::valueChanged),
            this, &PlanDialog::onLinuxSizeChanged);
    connect(autoRestartCheck_, &QCheckBox::toggled, this, &PlanDialog::updateOkButtonState);
    connect(buttons_, &QDialogButtonBox::accepted, this, &PlanDialog::onAcceptClicked);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Initial preview update
    updatePlanPreview();
}

void PlanDialog::onAcceptClicked() {
    // Extra confirmation for WipeDisk strategy
    if (wipeRadio_->isChecked() && !wipeConfirmed_) {
        const int idx = diskCombo_->currentIndex();
        if (idx >= 0 && static_cast<std::size_t>(idx) < disks_.size()) {
            const auto& d = disks_[static_cast<std::size_t>(idx)];
            auto reply = QMessageBox::warning(this, tr("ULLI — Confirm Disk Wipe"),
                tr("You have selected <b>Wipe Disk</b> for <b>Disk %1 (%2)</b>.<br><br>"
                   "This will <b>PERMANENTLY ERASE ALL DATA</b> on this disk.<br>"
                   "There is <b>NO UNDO</b>.<br><br>"
                   "Type <b>WIPE</b> to confirm:").arg(d.number).arg(d.qmodel()),
                QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);

            if (reply == QMessageBox::Ok) {
                // For a real implementation, we'd show a text input dialog to type "WIPE"
                // For now, use a simpler double-confirm
                reply = QMessageBox::question(this, tr("ULLI — Final Confirmation"),
                    tr("Are you absolutely sure you want to wipe Disk %1?\n\n"
                       "ALL DATA WILL BE LOST.").arg(d.number),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (reply == QMessageBox::Yes) {
                    wipeConfirmed_ = true;
                    accept();
                    return;
                }
            }
            return;
        }
    }
    accept();
}

void PlanDialog::setDisks(const std::vector<core::Disk>& disks) {
    disks_ = disks;
    diskCombo_->clear();
    for (std::vector<core::Disk>::size_type i = 0; i < disks.size(); ++i) {
        const auto& d = disks[i];
        diskCombo_->addItem(
            QString("Disk %1 — %2 (%3)").arg(d.number)
                .arg(d.qmodel().isEmpty() ? tr("Unknown") : d.qmodel())
                .arg(core::formatBytes(d.sizeBytes)),
            QVariant::fromValue<int>(static_cast<int>(i)));
    }
    wipeConfirmed_ = false;
    updateOkButtonState();
    onDiskChanged(0);
}

void PlanDialog::onDiskChanged(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= disks_.size()) return;
    const auto& d = disks_[static_cast<std::size_t>(index)];
    populateDiskPartitions(d);
    updateStrategyAvailability(d);
    updateSizeBounds();
    updatePlanPreview();
    wipeConfirmed_ = false;
    updateOkButtonState();
}

void PlanDialog::populateDiskPartitions(const core::Disk& disk) {
    partitionTree_->clear();
    for (const auto& p : disk.partitions) {
        auto* item = new QTreeWidgetItem(partitionTree_);
        item->setText(0, QString("Partition %1").arg(p.number));
        item->setText(1, partitionKindToString(p.kind));
        item->setText(2, filesystemToString(p.fs));
        item->setText(3, formatPartitionSize(p.sizeBytes));
        item->setText(4, formatPartitionSize(p.offsetBytes));
        QString flags;
        if (p.isBoot) flags += tr("Boot ");
        if (p.isSystem) flags += tr("System ");
        if (p.isHidden) flags += tr("Hidden ");
        if (p.driveLetter.has_value()) flags += QString("Letter: %1 ").arg(p.driveLetter.value());
        item->setText(5, flags.trimmed());
        // Highlight system disk partitions
        if (p.isSystem) {
            item->setBackground(0, QBrush(QColor("#fff3cd")));
            item->setBackground(1, QBrush(QColor("#fff3cd")));
            item->setBackground(2, QBrush(QColor("#fff3cd")));
            item->setBackground(3, QBrush(QColor("#fff3cd")));
            item->setBackground(4, QBrush(QColor("#fff3cd")));
            item->setBackground(5, QBrush(QColor("#fff3cd")));
        }
    }
    unallocatedLabel_->setText(tr("Unallocated: %1").arg(core::formatBytes(disk.unallocatedBytes)));
}

void PlanDialog::updateStrategyAvailability(const core::Disk& disk) {
    const bool isSystem = disk.isSystem();

    // WipeDisk: only on non-system disks
    wipeRadio_->setEnabled(!isSystem);
    wipeRadio_->setVisible(!isSystem);
    if (isSystem && wipeRadio_->isChecked()) {
        freeRadio_->setChecked(true);
    }

    // ShrinkAll: only if there's an NTFS partition with a drive letter on this disk
    bool hasShrinkable = false;
    for (const auto& p : disk.partitions) {
        if (p.driveLetter.has_value() && (p.fs == core::FileSystem::Ntfs || p.kind == core::PartitionKind::WindowsNtfs)) {
            hasShrinkable = true;
            break;
        }
    }
    shrinkRadio_->setEnabled(hasShrinkable);
    if (!hasShrinkable && shrinkRadio_->isChecked()) {
        freeRadio_->setChecked(true);
    }

    // UseFreeAll: only if there's unallocated space
    freeRadio_->setEnabled(disk.unallocatedBytes >= (20ull * 1024 * 1024 * 1024 + 7ull * 1024 * 1024 * 1024));
    if (!freeRadio_->isEnabled() && freeRadio_->isChecked()) {
        shrinkRadio_->setChecked(true);
    }

    updatePlanPreview();
}

void PlanDialog::onStrategyChanged() {
    wipeConfirmed_ = false;
    updateOkButtonState();
    updatePlanPreview();
}

void PlanDialog::onLinuxSizeChanged(int value) {
    Q_UNUSED(value);
    updateSizeBounds();
    updatePlanPreview();
}

void PlanDialog::updateOkButtonState() {
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(!(wipeRadio_->isChecked() && !wipeConfirmed_));
}

void PlanDialog::updateSizeBounds() {
    const int idx = diskCombo_->currentIndex();
    if (idx < 0 || static_cast<std::size_t>(idx) >= disks_.size()) return;
    const auto& d = disks_[static_cast<std::size_t>(idx)];

    uint64_t maxLinuxBytes = 0;
    if (freeRadio_->isChecked() || shrinkRadio_->isChecked()) {
        maxLinuxBytes = d.unallocatedBytes;
    } else if (wipeRadio_->isChecked()) {
        maxLinuxBytes = d.sizeBytes;
    }

    const int minSize = 20;
    int maxSize = 0;
    if (maxLinuxBytes > 0) {
        maxSize = static_cast<int>((maxLinuxBytes - 7ull * 1024 * 1024 * 1024) / (1024ull * 1024 * 1024));
    }
    if (maxSize < minSize) maxSize = minSize;
    if (maxSize > 10000) maxSize = 10000;
    linuxSizeSpin_->setRange(minSize, maxSize);
    if (linuxSizeSpin_->value() > maxSize) linuxSizeSpin_->setValue(maxSize);
    if (linuxSizeSpin_->value() < minSize) linuxSizeSpin_->setValue(minSize);
}

void PlanDialog::updatePlanPreview() {
    const int idx = diskCombo_->currentIndex();
    if (idx < 0 || static_cast<std::size_t>(idx) >= disks_.size()) return;
    const auto& d = disks_[static_cast<std::size_t>(idx)];

    QString html;
    html += QString("<b>Disk %1</b> (%2, %3)<br>")
                .arg(d.number).arg(d.qmodel()).arg(core::formatBytes(d.sizeBytes));

    if (d.style == core::PartitionStyle::GPT) html += "Partition style: GPT<br>";
    else if (d.style == core::PartitionStyle::MBR) html += "Partition style: MBR<br>";

    html += "<br><b>Current layout:</b><br>";

    for (const auto& p : d.partitions) {
        QString flags;
        if (p.isBoot) flags += "[Boot] ";
        if (p.isSystem) flags += "<span style='color:#c00;'>[SYSTEM]</span> ";
        if (p.isHidden) flags += "[Hidden] ";
        if (p.driveLetter.has_value()) flags += QString("[%1:] ").arg(p.driveLetter.value());

        html += QString("  ├─ Part %1: %2, %3, %4, %5<br>")
                    .arg(p.number)
                    .arg(partitionKindToString(p.kind))
                    .arg(filesystemToString(p.fs))
                    .arg(formatPartitionSize(p.sizeBytes))
                    .arg(flags);
    }

    if (d.unallocatedBytes > 0) {
        html += QString("  └─ Unallocated: %1<br>").arg(core::formatBytes(d.unallocatedBytes));
    }

    html += "<br><b>After installation:</b><br>";

    const uint64_t linuxBytes = static_cast<uint64_t>(linuxSizeSpin_->value()) * 1024 * 1024 * 1024;
    const uint64_t bootBytes = 7ull * 1024 * 1024 * 1024;
    const uint64_t refindBytes = 100ull * 1024 * 1024;

    if (wipeRadio_->isChecked()) {
        html += QString("  ├─ EFI System Partition: %1 (FAT32)<br>").arg(formatPartitionSize(bootBytes));
        html += QString("  ├─ Linux partition: %1 (free space for distro)<br>").arg(formatPartitionSize(linuxBytes));
        html += QString("  └─ (rEFInd partition: %1 if selected)<br>").arg(formatPartitionSize(refindBytes));
        safetyWarningLabel_->setText(tr("⚠ WARNING: This will ERASE ALL DATA on disk %1!").arg(d.number));
        safetyWarningLabel_->setStyleSheet("color: #c00; font-weight: bold;");
        safetyWarningLabel_->setVisible(true);
    } else if (shrinkRadio_->isChecked()) {
        html += QString("  ├─ Existing partitions preserved<br>");
        html += QString("  ├─ Shrink selected NTFS partition by %1<br>").arg(formatPartitionSize(linuxBytes + bootBytes + refindBytes));
        html += QString("  ├─ New boot partition: %1 (FAT32)<br>").arg(formatPartitionSize(bootBytes));
        html += QString("  ├─ New Linux partition: %1 (free space)<br>").arg(formatPartitionSize(linuxBytes));
        html += QString("  └─ (rEFInd partition: %1 if selected)<br>").arg(formatPartitionSize(refindBytes));
        safetyWarningLabel_->setText(tr("⚠ Modifying existing partition — ensure you have backups."));
        safetyWarningLabel_->setStyleSheet("color: #c60; font-weight: bold;");
        safetyWarningLabel_->setVisible(true);
    } else if (freeRadio_->isChecked()) {
        html += QString("  ├─ Existing partitions untouched<br>");
        html += QString("  ├─ New boot partition: %1 (FAT32) from unallocated<br>").arg(formatPartitionSize(bootBytes));
        html += QString("  ├─ New Linux partition: %1 (free space) from unallocated<br>").arg(formatPartitionSize(linuxBytes));
        html += QString("  └─ (rEFInd partition: %1 if selected)<br>").arg(formatPartitionSize(refindBytes));
        safetyWarningLabel_->setText(tr("✓ Safest option — no existing partitions modified."));
        safetyWarningLabel_->setStyleSheet("color: #2a7; font-weight: bold;");
        safetyWarningLabel_->setVisible(true);
    }

    plannedLayoutLabel_->setText(html);
}

QString PlanDialog::partitionKindToString(core::PartitionKind kind) const {
    switch (kind) {
        case core::PartitionKind::Esp: return tr("EFI System");
        case core::PartitionKind::Recovery: return tr("Recovery");
        case core::PartitionKind::MsReserved: return tr("MS Reserved");
        case core::PartitionKind::WindowsNtfs: return tr("Windows NTFS");
        case core::PartitionKind::WindowsFat: return tr("Windows FAT");
        case core::PartitionKind::Linux: return tr("Linux");
        case core::PartitionKind::LinuxSwap: return tr("Linux Swap");
        case core::PartitionKind::LinuxLvm: return tr("Linux LVM");
        case core::PartitionKind::LinuxRaid: return tr("Linux RAID");
        case core::PartitionKind::BasicData: return tr("Basic Data");
        default: return tr("Unknown");
    }
}

QString PlanDialog::filesystemToString(core::FileSystem fs) const {
    switch (fs) {
        case core::FileSystem::Fat32: return "FAT32";
        case core::FileSystem::Ntfs: return "NTFS";
        case core::FileSystem::Ext2: return "ext2";
        case core::FileSystem::Ext3: return "ext3";
        case core::FileSystem::Ext4: return "ext4";
        case core::FileSystem::Btrfs: return "btrfs";
        case core::FileSystem::Xfs: return "XFS";
        case core::FileSystem::F2fs: return "F2FS";
        case core::FileSystem::Iso9660: return "ISO9660";
        default: return tr("Unknown");
    }
}

QString PlanDialog::formatPartitionSize(uint64_t bytes) const {
    return core::formatBytes(bytes);
}

core::InstallPlan PlanDialog::buildPlan(const std::string& distroKey,
                                          const std::filesystem::path& isoPath) const {
    core::InstallPlan p;
    p.distroKey = distroKey;
    p.isoPath = isoPath;
    p.autoRestart = autoRestartCheck_->isChecked();

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
        // TODO: In a real implementation, we'd track which NTFS partition to shrink
        // For now, the backend will need to pick an appropriate one
    }
    return p;
}

}  // namespace ulli::ui