// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "filterparams.h"
#include "treemapsettings.h"
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
class QFrame;
class QLineEdit;
class QPushButton;
class QTimer;

class SearchFilterPanel : public QWidget {
    Q_OBJECT
public:
    explicit SearchFilterPanel(QWidget* parent = nullptr);

    // Re-populate the file-type combo from updated settings.
    void setSettings(const TreemapSettings& settings);

    // Returns the currently active filter, built from UI state.
    FilterParams currentParams() const;

    // Resets all controls to their defaults and emits filterParamsChanged with empty params.
    void clearAll();

    void focusNameField();

    void setChromeBorderColor(const QColor& color);

signals:
    void filterParamsChanged(const FilterParams& params);

private slots:
    void onNameTextChanged();
    void onAnyFilterChanged();

protected:
    void changeEvent(QEvent* event) override;

private:
    void buildUi();
    void refreshChromeStyles();
    static qint64 computeSizeBytes(double value, int unitIndex);

    QFrame* m_mainFrame = nullptr;
    QColor m_chromeBorderColor;
    // Row 1
    QLineEdit*      m_nameEdit       = nullptr;
    QDoubleSpinBox* m_sizeMinSpin    = nullptr;
    QComboBox*      m_sizeMinUnit    = nullptr;
    QDoubleSpinBox* m_sizeMaxSpin    = nullptr;
    QComboBox*      m_sizeMaxUnit    = nullptr;
    QCheckBox*      m_dateFromCheck  = nullptr;
    QDateEdit*      m_dateFromEdit   = nullptr;
    QCheckBox*      m_dateToCheck    = nullptr;
    QDateEdit*      m_dateToEdit     = nullptr;

    // Row 2
    QComboBox*      m_typeCombo      = nullptr;
    QButtonGroup*   m_modeGroup      = nullptr;
    QPushButton*    m_markButtons[6] = {};    // ColorRed…ColorPurple
    QPushButton*    m_iconMarkButtons[11] = {}; // CatGames…CatVideo
    QCheckBox*      m_hideCheck      = nullptr;
    QPushButton*    m_clearButton    = nullptr;

    QTimer*         m_nameDebounce   = nullptr;
};
