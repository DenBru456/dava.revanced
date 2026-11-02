#pragma once

#include <QDateTime>
#include <QWidget>
#include <map>

class QComboBox;
class QLabel;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace DAVA
{
class Command;
class RECommandStack;
class SceneEditor2;
} // namespace DAVA

class HistoryWidget : public QWidget
{
    Q_OBJECT

public:
    // Action classes used for the filter and the row chips.
    // All is filter-only and must stay at index 0 (combo index maps 1:1).
    enum class Category
    {
        All = 0,
        Move,
        Rotate,
        Scale,
        Transform,
        Material,
        Entity,
        Component,
        LodMesh,
        Particles,
        Landscape,
        Property,
        Sound,
        Other
    };

    explicit HistoryWidget(QWidget* parent = nullptr);
    ~HistoryWidget() override;

    // may be nullptr when no scene is open / scene was closed
    void OnSceneChanged(DAVA::SceneEditor2* scene);

private:
    // connected to RECommandStack::commandExecuted
    void OnCommandExecuted(const DAVA::Command* command, bool redo);
    // connected to RECommandStack::historyReset (Clear / RemoveCommands)
    void OnStackReset();

    void RebuildList();
    void JumpToIndex(int targetIndex);
    void OnItemClicked(QTreeWidgetItem* item, int column);
    void OnUndoAllClicked();
    void OnRedoAllClicked();
    void OnClearClicked();
    void OnFilterChanged(int index);
    void OnTimestampsToggled(bool checked);

    int TopLevelIndexForItem(QTreeWidgetItem* item) const;
    QString CommandDisplayName(const DAVA::Command* command) const;
    QString CommandTypeName(const DAVA::Command* command) const;
    Category ClassifyCommand(const DAVA::Command* command, const QString& batchName) const;

    void ApplyFilter();
    void ResetSessionData();

    DAVA::SceneEditor2* scene = nullptr;
    bool rebuildGuard = false;

    QLabel* infoLabel = nullptr;
    QLabel* sceneLabel = nullptr;
    QTreeWidget* tree = nullptr;
    QToolButton* undoAllButton = nullptr;
    QToolButton* redoAllButton = nullptr;
    QComboBox* filterCombo = nullptr;
    QToolButton* timestampButton = nullptr;
    QToolButton* clearButton = nullptr;

    // per top-level stack index, recorded when the command enters the stack
    std::map<int, QDateTime> stamps;
    int lastKnownCount = 0;
};
