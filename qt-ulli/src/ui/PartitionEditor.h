// ui/PartitionEditor.h
//
// Advanced (Phase 2) manual partition editor. Phase 1 shows a
// placeholder; the real editor with a QTreeView / QTableView comes in
// Phase 2.

#pragma once

#include <QWidget>

namespace ulli::ui {

class PartitionEditor : public QWidget {
    Q_OBJECT
public:
    explicit PartitionEditor(QWidget* parent = nullptr);
};

}  // namespace ulli::ui
