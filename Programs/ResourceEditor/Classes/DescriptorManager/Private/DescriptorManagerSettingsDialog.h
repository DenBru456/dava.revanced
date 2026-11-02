#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QListWidget;
class QPushButton;
class QShowEvent;
class QStackedWidget;

class DescriptorManagerSettings;

// Tools -> Descriptor Manager -> Settings
// Priorities: one list, three pages (Format | GPU | Time) picked with buttons above it
class DescriptorManagerSettingsDialog : public QDialog
{
public:
    // settings are read on open and written back on accept
    DescriptorManagerSettingsDialog(DescriptorManagerSettings* settings, QWidget* parent = nullptr);

private:
    enum ePriorityPage
    {
        PAGE_FORMAT = 0,
        PAGE_GPU = 1,
        PAGE_TIME = 2
    };

    void showEvent(QShowEvent* event) override;
    void FitToContent();
    void ShowPage(int page);
    void UpdateArrowState();
    void OnMoveUp();
    void OnMoveDown();
    void OnDefaults();
    void OnAccepted();

    QListWidget* ActiveList() const;

    DescriptorManagerSettings* settings = nullptr;

    QStackedWidget* pages = nullptr;
    QPushButton* formatPageButton = nullptr;
    QPushButton* gpuPageButton = nullptr;
    QPushButton* timePageButton = nullptr;
    QListWidget* formatList = nullptr;
    QListWidget* familyList = nullptr;
    QListWidget* timeList = nullptr;
    QPushButton* upButton = nullptr;
    QPushButton* downButton = nullptr;

    QGroupBox* priorityGroup = nullptr;
    QGroupBox* miscGroup = nullptr;
    QLabel* missingLabel = nullptr;
    QLabel* extensionLabel = nullptr;
    QLabel* startupLabel = nullptr;

    QComboBox* sourceBox = nullptr;
    QCheckBox* overwriteBox = nullptr;
    QComboBox* defaultExtensionBox = nullptr;

    QCheckBox* sceneOpenBox = nullptr;
    QCheckBox* reactBox = nullptr;
    QCheckBox* ignoreHealthyBox = nullptr;
    QComboBox* startupBox = nullptr;
    QPushButton* defaultsButton = nullptr;
    QPushButton* okButton = nullptr;
    QPushButton* cancelButton = nullptr;
};
