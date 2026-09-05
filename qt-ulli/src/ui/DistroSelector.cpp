// ui/DistroSelector.cpp
#include "ui/DistroSelector.h"

#include <QComboBox>
#include <QFileDialog>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace ulli::ui {

DistroSelector::DistroSelector(QWidget* parent) : QWidget(parent) {
    auto* label = new QLabel(tr("Distribution"), this);
    label->setStyleSheet("font-weight: bold;");

    combo_ = new QComboBox(this);
    customCheck_ = new QCheckBox(tr("Use a custom ISO file"), this);
    customPath_ = new QLineEdit(this);
    customPath_->setReadOnly(true);
    customPath_->setPlaceholderText(tr("No file selected"));
    browseBtn_ = new QPushButton(tr("Browse..."), this);
    customPath_->setEnabled(false);
    browseBtn_->setEnabled(false);

    auto* customRow = new QHBoxLayout;
    customRow->addWidget(customPath_, 1);
    customRow->addWidget(browseBtn_);

    auto* root = new QVBoxLayout(this);
    root->addWidget(label);
    root->addWidget(combo_);
    root->addWidget(customCheck_);
    root->addLayout(customRow);
    setLayout(root);

    connect(combo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &DistroSelector::selectionChanged);
    connect(customCheck_, &QCheckBox::toggled,
            this, &DistroSelector::onCustomToggled);
    connect(browseBtn_, &QPushButton::clicked,
            this, &DistroSelector::onBrowseClicked);
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
}

void DistroSelector::onCustomToggled(bool checked) {
    customPath_->setEnabled(checked);
    browseBtn_->setEnabled(checked);
    combo_->setEnabled(!checked);
    emit selectionChanged();
}

void DistroSelector::onBrowseClicked() {
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Select Linux ISO"), QString(),
        tr("ISO Files (*.iso);;All Files (*)"));
    if (f.isEmpty()) return;
    customPath_->setText(f);
    emit selectionChanged();
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
