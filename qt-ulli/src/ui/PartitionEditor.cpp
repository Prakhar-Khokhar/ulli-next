// ui/PartitionEditor.cpp — Phase 1 placeholder.
#include "ui/PartitionEditor.h"

#include <QLabel>
#include <QVBoxLayout>

namespace ulli::ui {

PartitionEditor::PartitionEditor(QWidget* parent) : QWidget(parent) {
    auto* label = new QLabel(
        tr("Manual partition editor (advanced) — coming in Phase 2."), this);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    auto* root = new QVBoxLayout(this);
    root->addStretch(1);
    root->addWidget(label);
    root->addStretch(1);
    setLayout(root);
}

}  // namespace ulli::ui
