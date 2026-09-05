// ui/DistroSelector.h
//
// ComboBox that lists distros from the catalog + a "Custom ISO" entry
// with a file picker.

#pragma once

#include "core/Catalog.h"

#include <QWidget>

class QComboBox;
class QLineEdit;
class QPushButton;
class QCheckBox;

namespace ulli::ui {

class DistroSelector : public QWidget {
    Q_OBJECT
public:
    explicit DistroSelector(QWidget* parent = nullptr);

    void setCatalog(const core::Catalog* catalog);
    QString selectedKey() const;
    QString customIsoPath() const;
    bool isCustom() const;

signals:
    void selectionChanged();

private slots:
    void onCustomToggled(bool checked);
    void onBrowseClicked();

private:
    QComboBox* combo_ = nullptr;
    QCheckBox* customCheck_ = nullptr;
    QLineEdit* customPath_ = nullptr;
    QPushButton* browseBtn_ = nullptr;
    const core::Catalog* catalog_ = nullptr;
};

}  // namespace ulli::ui
