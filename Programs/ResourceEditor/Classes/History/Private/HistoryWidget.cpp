#include "Classes/History/Private/HistoryWidget.h"

#include "REPlatform/Commands/Private/RECommandStack.h"
#include "REPlatform/Commands/TransformCommand.h"
#include "REPlatform/Scene/SceneEditor2.h"

#include <Base/Type.h>
#include <Command/Command.h>
#include <Command/CommandBatch.h>
#include <FileSystem/FilePath.h>
#include <Reflection/ReflectedObject.h>
#include <Reflection/ReflectedType.h>
#include <Scene3D/Entity.h>

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

using namespace DAVA;

namespace HistoryWidgetDetail
{
const int TopIndexRole = Qt::UserRole + 1;
const int CategoryRole = Qt::UserRole + 2;

QString PrettyTypeName(const QString& rawType)
{
    QString s = rawType;
    for (const QString& prefix : { QString("class "), QString("struct ") })
    {
        while (s.startsWith(prefix) == true)
        {
            s.remove(0, prefix.length());
        }
    }
    return s;
}

QColor ColorForCategory(HistoryWidget::Category category)
{
    switch (category)
    {
    case HistoryWidget::Category::Move:
        return QColor(0xd9, 0x8e, 0x32);
    case HistoryWidget::Category::Rotate:
        return QColor(0x4a, 0xa3, 0xd8);
    case HistoryWidget::Category::Scale:
        return QColor(0xb0, 0x6f, 0xd8);
    case HistoryWidget::Category::Transform:
        return QColor(0xb8, 0xa9, 0x4e);
    case HistoryWidget::Category::Material:
        return QColor(0x58, 0xb3, 0x68);
    case HistoryWidget::Category::Entity:
        return QColor(0x4a, 0x90, 0xd9);
    case HistoryWidget::Category::Component:
        return QColor(0x3a, 0xa6, 0x96);
    case HistoryWidget::Category::LodMesh:
        return QColor(0xd8, 0xb5, 0x3a);
    case HistoryWidget::Category::Particles:
        return QColor(0xd8, 0x6a, 0xa0);
    case HistoryWidget::Category::Landscape:
        return QColor(0x9a, 0x7d, 0x6b);
    case HistoryWidget::Category::Property:
        return QColor(0x8a, 0x9a, 0xa8);
    case HistoryWidget::Category::Sound:
        return QColor(0x7d, 0x84, 0xc0);
    default:
        return QColor();
    }
}

QPixmap MakeChip(const QColor& color)
{
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor pen = color.darker(140);
    p.setPen(QPen(pen, 1));
    p.setBrush(color);
    p.drawRoundedRect(1, 1, 10, 10, 3, 3);
    return pm;
}

QPixmap MakeClockIcon(const QColor& color)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(color, 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(3.5, 3.5, 9.0, 9.0);
    p.drawLine(8.0, 8.0, 8.0, 5.2);
    p.drawLine(8.0, 8.0, 10.4, 9.4);
    return pm;
}

QPixmap MakeTrashIcon(const QColor& color)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 1.4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    // bin body
    p.drawLine(5.0, 6.5, 5.6, 13.0);
    p.drawLine(11.0, 6.5, 10.4, 13.0);
    p.drawLine(5.6, 13.0, 10.4, 13.0);
    // lid
    p.drawLine(3.8, 6.5, 12.2, 6.5);
    p.drawLine(7.0, 4.0, 9.0, 4.0);
    p.drawLine(7.0, 4.0, 7.0, 6.5);
    p.drawLine(9.0, 4.0, 9.0, 6.5);
    return pm;
}
} // namespace HistoryWidgetDetail

HistoryWidget::HistoryWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("HistoryWidget");

    infoLabel = new QLabel(this);
    sceneLabel = new QLabel(this);

    QHBoxLayout* statusLine = new QHBoxLayout();
    statusLine->setSpacing(4);
    statusLine->setContentsMargins(0, 0, 0, 0);
    statusLine->addWidget(infoLabel);
    statusLine->addStretch(1);
    statusLine->addWidget(sceneLabel);

    tree = new QTreeWidget(this);
    tree->setColumnCount(2);
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(true);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    tree->setColumnWidth(1, 66);

    // console-style square icon buttons
    undoAllButton = new QToolButton(this);
    undoAllButton->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    undoAllButton->setToolTip("Undo All (jump to the beginning of history)");
    undoAllButton->setAutoRaise(true);

    redoAllButton = new QToolButton(this);
    redoAllButton->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    redoAllButton->setToolTip("Redo All (jump to the end of history)");
    redoAllButton->setAutoRaise(true);

    filterCombo = new QComboBox(this);
    for (const char* label :
         {
         "All", "Move", "Rotate", "Scale", "Transform", "Material", "Entity",
         "Component", "LOD / Mesh", "Particles", "Landscape", "Property", "Sound", "Other" })
    {
        filterCombo->addItem(QString::fromUtf8(label));
    }
    filterCombo->setToolTip("Filter history by action class");
    filterCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);

    // kill the extra vertical padding some themes add to the popup
    filterCombo->setStyleSheet("QComboBox QAbstractItemView { padding: 0px; margin: 0px; }");

    timestampButton = new QToolButton(this);
    timestampButton->setIcon(QIcon(HistoryWidgetDetail::MakeClockIcon(palette().color(QPalette::Active, QPalette::Text))));
    timestampButton->setToolTip("Toggle timestamps");
    timestampButton->setCheckable(true);
    timestampButton->setChecked(true);
    timestampButton->setAutoRaise(true);

    clearButton = new QToolButton(this);
    clearButton->setIcon(QIcon(HistoryWidgetDetail::MakeTrashIcon(palette().color(QPalette::Active, QPalette::Text))));
    clearButton->setToolTip("Clear History (discards all undo information)");
    clearButton->setAutoRaise(true);

    QHBoxLayout* buttonsLayout = new QHBoxLayout();
    buttonsLayout->setSpacing(4);
    buttonsLayout->addWidget(undoAllButton);
    buttonsLayout->addWidget(redoAllButton);
    buttonsLayout->addWidget(filterCombo);
    buttonsLayout->addStretch(1);
    buttonsLayout->addWidget(timestampButton);
    buttonsLayout->addWidget(clearButton);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setSpacing(4);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(statusLine);
    layout->addWidget(tree);
    layout->addLayout(buttonsLayout);

    connect(tree, &QTreeWidget::itemClicked, this, &HistoryWidget::OnItemClicked);
    connect(undoAllButton, &QToolButton::clicked, this, &HistoryWidget::OnUndoAllClicked);
    connect(redoAllButton, &QToolButton::clicked, this, &HistoryWidget::OnRedoAllClicked);
    connect(clearButton, &QToolButton::clicked, this, &HistoryWidget::OnClearClicked);
    connect(filterCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &HistoryWidget::OnFilterChanged);
    connect(timestampButton, &QToolButton::toggled, this, &HistoryWidget::OnTimestampsToggled);

    RebuildList();
}

HistoryWidget::~HistoryWidget() = default;

void HistoryWidget::OnSceneChanged(SceneEditor2* newScene)
{
    if (scene == newScene)
    {
        RebuildList();
        return;
    }

    if (scene != nullptr)
    {
        RECommandStack* oldStack = scene->GetCommandStack();
        oldStack->commandExecuted.Disconnect(this);
        oldStack->historyReset.Disconnect(this);
    }

    scene = newScene;
    ResetSessionData();

    if (scene != nullptr)
    {
        RECommandStack* stack = scene->GetCommandStack();
        stack->commandExecuted.Connect(this, &HistoryWidget::OnCommandExecuted);
        stack->historyReset.Connect(this, &HistoryWidget::OnStackReset);
    }

    RebuildList();
}

void HistoryWidget::ResetSessionData()
{
    stamps.clear();
    lastKnownCount = 0;
}

void HistoryWidget::OnCommandExecuted(const Command* /*command*/, bool /*redo*/)
{
    if (rebuildGuard)
    {
        return;
    }
    RebuildList();
}

void HistoryWidget::OnStackReset()
{
    if (rebuildGuard)
    {
        return;
    }
    ResetSessionData();
    RebuildList();
}

QString HistoryWidget::CommandDisplayName(const Command* command) const
{
    if (command == nullptr)
    {
        return QString("(null)");
    }

    const String& description = command->GetDescription();
    if (description.empty() == false)
    {
        return QString::fromStdString(description);
    }
    return QString("(unnamed command)");
}

QString HistoryWidget::CommandTypeName(const Command* command) const
{
    if (command == nullptr)
    {
        return QString();
    }

    // Type::GetName() returns the raw std::type_info name; classification is
    // substring-based so both MSVC ("class DAVA::TransformCommand") and
    // GCC-mangled ("...16TransformCommandE") forms match.
    const ReflectedType* reflectedType = ReflectedObject(command).GetReflectedType();
    if (reflectedType != nullptr && reflectedType->GetType() != nullptr)
    {
        const char* rawName = reflectedType->GetType()->GetName();
        if (rawName != nullptr)
        {
            return QString::fromLatin1(rawName);
        }
    }
    return QString();
}

HistoryWidget::Category HistoryWidget::ClassifyCommand(const Command* command, const QString& batchName) const
{
    const QString type = CommandTypeName(command);
    if (type.isEmpty() == true)
    {
        return Category::Other;
    }

    // with the 0005 ModifSystem patch the gizmo batches are named
    // "Move" / "Rotate" / "Scale"; legacy stacks keep "Multiple transform"
    if (type.contains("TransformCommand") == true)
    {
        if (batchName == "Move")
        {
            return Category::Move;
        }
        if (batchName == "Rotate")
        {
            return Category::Rotate;
        }
        if (batchName == "Scale")
        {
            return Category::Scale;
        }
        return Category::Transform;
    }
    if (type.contains("Material") == true || type.contains("MaterialPreset") == true || type.contains("DeleteRenderBatch") == true)
    {
        return Category::Material;
    }
    if (type.contains("LOD") == true || type.contains("Billboard") == true || type.contains("Shadow") == true ||
        type.contains("TangentSpace") == true || type.contains("CloneLastBatch") == true)
    {
        return Category::LodMesh;
    }
    if (type.contains("Particle") == true)
    {
        return Category::Particles;
    }
    if (type.contains("Heightmap") == true || type.contains("HeightMap") == true || type.contains("CustomColors") == true ||
        type.contains("Landscape") == true || type.contains("Tilemask") == true)
    {
        return Category::Landscape;
    }
    if (type.contains("Sound") == true)
    {
        return Category::Sound;
    }
    if (type.contains("Entity") == true)
    {
        return Category::Entity;
    }
    if (type.contains("Component") == true || type.contains("Slot") == true)
    {
        return Category::Component;
    }
    if (type.contains("Transform") == true || type.contains("Pivot") == true)
    {
        return Category::Transform;
    }
    if (type.contains("Insp") == true || type.contains("KeyedArchive") == true || type.contains("SetFieldValue") == true ||
        type.contains("MetaObj") == true || type.contains("WayEdit") == true)
    {
        return Category::Property;
    }
    return Category::Other;
}

void HistoryWidget::RebuildList()
{
    tree->blockSignals(true);
    tree->clear();

    if (scene == nullptr)
    {
        infoLabel->setText("No scene is open.");
        sceneLabel->setText(QString());
        undoAllButton->setEnabled(false);
        redoAllButton->setEnabled(false);
        clearButton->setEnabled(false);
        tree->blockSignals(false);
        return;
    }

    const RECommandStack* stack = scene->GetCommandStack();
    const int32 currentIndex = stack->GetCurrentCommandIndex();
    const int32 cleanIndex = stack->GetCleanCommandIndex();
    const uint32 commandCount = stack->GetCommandCount();

    // timestamp bookkeeping: stamp newly seen commands, drop stamps of a
    // truncated redo tail
    if (static_cast<int>(commandCount) > lastKnownCount)
    {
        const QDateTime now = QDateTime::currentDateTime();
        for (int i = lastKnownCount; i < static_cast<int>(commandCount); ++i)
        {
            stamps[i] = now;
        }
    }
    else if (static_cast<int>(commandCount) < lastKnownCount)
    {
        stamps.erase(stamps.lower_bound(static_cast<int>(commandCount)), stamps.end());
    }
    lastKnownCount = static_cast<int>(commandCount);

    const QPalette palette = tree->palette();
    const QColor appliedColor = palette.color(QPalette::Active, QPalette::Text);
    const QColor undoneColor = palette.color(QPalette::Disabled, QPalette::Text);
    const QColor savePointColor = palette.color(QPalette::Active, QPalette::Link);

    QFont appliedFont = tree->font();
    QFont undoneFont = appliedFont;
    undoneFont.setItalic(true);
    QFont currentFont = appliedFont;
    currentFont.setBold(true);

    int32 appliedCount = 0;
    int32 undoneCount = 0;

    for (uint32 i = 0; i < commandCount; ++i)
    {
        const Command* command = stack->GetCommand(i);
        const QString displayName = CommandDisplayName(command);
        const QString typeName = HistoryWidgetDetail::PrettyTypeName(CommandTypeName(command));

        QTreeWidgetItem* item = new QTreeWidgetItem(tree);

        const bool applied = (static_cast<int32>(i) <= currentIndex);
        if (applied)
        {
            ++appliedCount;
        }
        else
        {
            ++undoneCount;
        }

        const QString timeText = (stamps.count(static_cast<int>(i)) != 0) ?
        stamps[static_cast<int>(i)].toString("HH:mm:ss") :
        QString();

        const CommandBatch* batch = (command != nullptr) ? command->Cast<CommandBatch>() : nullptr;
        if (batch != nullptr && batch->Size() == 1)
        {
            // single command: no expansion list, just the action with its object
            const Command* only = batch->GetCommand(0);
            const QString batchName = displayName;
            QString rowName = (batchName == QString("Multiple transform")) ?
            CommandDisplayName(only) :
            batchName;
            const TransformCommand* transform = (only != nullptr) ? only->Cast<TransformCommand>() : nullptr;
            if (transform != nullptr && transform->GetEntity() != nullptr)
            {
                rowName += QString(" - ") + transform->GetEntity()->GetName().c_str();
            }
            item->setText(0, rowName);
            item->setText(1, timeText);

            const Category commandCategory = ClassifyCommand(only, batchName);
            item->setData(0, HistoryWidgetDetail::CategoryRole, static_cast<int>(commandCategory));
            const QColor commandColor = HistoryWidgetDetail::ColorForCategory(commandCategory);
            if (commandColor.isValid() == true)
            {
                item->setIcon(0, QIcon(HistoryWidgetDetail::MakeChip(commandColor)));
            }
        }
        else if (batch != nullptr && batch->Size() > 1)
        {
            const QString batchName = displayName;
            item->setText(0, QString("%1 (%2)").arg(batchName).arg(batch->Size()));
            item->setText(1, timeText);

            // batch takes the category of its first child
            const Category batchCategory = ClassifyCommand(batch->GetCommand(0), batchName);
            item->setData(0, HistoryWidgetDetail::CategoryRole, static_cast<int>(batchCategory));
            const QColor batchColor = HistoryWidgetDetail::ColorForCategory(batchCategory);
            if (batchColor.isValid() == true)
            {
                item->setIcon(0, QIcon(HistoryWidgetDetail::MakeChip(batchColor)));
            }

            // children list the affected objects only, without timestamps
            for (uint32 k = 0; k < batch->Size(); ++k)
            {
                const Command* childCommand = batch->GetCommand(k);
                QTreeWidgetItem* child = new QTreeWidgetItem(item);

                QString childName;
                const TransformCommand* transform = (childCommand != nullptr) ? childCommand->Cast<TransformCommand>() : nullptr;
                if (transform != nullptr && transform->GetEntity() != nullptr)
                {
                    childName = transform->GetEntity()->GetName().c_str();
                }
                else
                {
                    childName = CommandDisplayName(childCommand);
                }
                child->setText(0, childName);

                const Category childCategory = ClassifyCommand(childCommand, batchName);
                child->setData(0, HistoryWidgetDetail::CategoryRole, static_cast<int>(childCategory));
                const QColor childColor = HistoryWidgetDetail::ColorForCategory(childCategory);
                if (childColor.isValid() == true)
                {
                    child->setIcon(0, QIcon(HistoryWidgetDetail::MakeChip(childColor)));
                }

                child->setFont(0, applied ? appliedFont : undoneFont);
                child->setForeground(0, applied ? QBrush(appliedColor) : QBrush(undoneColor));
                child->setToolTip(0, QString("%1\nClass: %2").arg(CommandDisplayName(childCommand)).arg(HistoryWidgetDetail::PrettyTypeName(CommandTypeName(childCommand))));
            }
        }
        else
        {
            QString rowName = displayName;
            const TransformCommand* transform = (command != nullptr) ? command->Cast<TransformCommand>() : nullptr;
            if (transform != nullptr && transform->GetEntity() != nullptr)
            {
                rowName += QString(" - ") + transform->GetEntity()->GetName().c_str();
            }
            item->setText(0, rowName);
            item->setText(1, timeText);

            const Category commandCategory = ClassifyCommand(command, QString());
            item->setData(0, HistoryWidgetDetail::CategoryRole, static_cast<int>(commandCategory));
            const QColor commandColor = HistoryWidgetDetail::ColorForCategory(commandCategory);
            if (commandColor.isValid() == true)
            {
                item->setIcon(0, QIcon(HistoryWidgetDetail::MakeChip(commandColor)));
            }
        }

        if (applied == false)
        {
            item->setFont(0, undoneFont);
            item->setForeground(0, QBrush(undoneColor));
        }
        else if (static_cast<int32>(i) == currentIndex)
        {
            // current position: bold + subtle tint, no text arrow
            item->setFont(0, currentFont);
            item->setForeground(0, QBrush(appliedColor));
            QColor tint = palette.color(QPalette::Active, QPalette::Highlight);
            tint.setAlpha(50);
            item->setBackground(0, QBrush(tint));
            item->setToolTip(0, "Current position: the scene state matches this command.");
        }

        if (static_cast<int32>(i) == cleanIndex)
        {
            item->setForeground(0, QBrush(savePointColor));
            QString tip = item->toolTip(0);
            if (tip.isEmpty() == false)
            {
                tip += QString("\n");
            }
            tip += QString("Save point: the scene on disk matches the state after this command.");
            item->setToolTip(0, tip);
        }
        else if (applied == false)
        {
            item->setToolTip(0, "Undone: will be discarded when a new command is executed.");
        }

        item->setData(0, HistoryWidgetDetail::TopIndexRole, static_cast<int>(i));
    }

    const bool hasCommands = (commandCount > 0);
    undoAllButton->setEnabled(hasCommands && scene->CanUndo());
    redoAllButton->setEnabled(hasCommands && scene->CanRedo());
    clearButton->setEnabled(hasCommands);

    const FilePath& scenePath = scene->GetScenePath();
    if (scenePath.IsEmpty() == false)
    {
        sceneLabel->setText(QString::fromStdString(scenePath.GetBasename()));
    }
    else
    {
        sceneLabel->setText(QString());
    }

    QString info = QString("%1 applied, %2 undone").arg(appliedCount).arg(undoneCount);
    if (cleanIndex >= 0)
    {
        info += QString(" | saved after step %1").arg(cleanIndex + 1);
    }
    else
    {
        info += QString(" | not saved");
    }
    infoLabel->setText(info);

    ApplyFilter();

    tree->blockSignals(false);
}

void HistoryWidget::ApplyFilter()
{
    const int filterIndex = filterCombo->currentIndex();
    const bool showAll = (filterIndex <= 0);
    const int wanted = filterIndex;

    // "Transform" covers all gizmo operations: Move, Rotate and Scale
    const auto categoryMatches = [showAll, wanted](int category) -> bool
    {
        if (showAll == true)
        {
            return true;
        }
        if (category == wanted)
        {
            return true;
        }
        if (wanted == static_cast<int>(Category::Transform))
        {
            return category == static_cast<int>(Category::Move) ||
            category == static_cast<int>(Category::Rotate) ||
            category == static_cast<int>(Category::Scale);
        }
        return false;
    };

    for (int i = 0; i < tree->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* top = tree->topLevelItem(i);
        const int topCategory = top->data(0, HistoryWidgetDetail::CategoryRole).toInt();
        const bool topMatches = categoryMatches(topCategory);
        bool anyChildMatches = false;

        for (int k = 0; k < top->childCount(); ++k)
        {
            QTreeWidgetItem* child = top->child(k);
            const int childCategory = child->data(0, HistoryWidgetDetail::CategoryRole).toInt();
            const bool childMatches = categoryMatches(childCategory);
            child->setHidden(!childMatches);
            anyChildMatches = anyChildMatches || childMatches;
        }

        top->setHidden(!(topMatches || anyChildMatches));
    }
}

void HistoryWidget::OnFilterChanged(int /*index*/)
{
    ApplyFilter();
}

void HistoryWidget::OnTimestampsToggled(bool checked)
{
    tree->setColumnHidden(1, !checked);
}

int HistoryWidget::TopLevelIndexForItem(QTreeWidgetItem* item) const
{
    if (item == nullptr)
    {
        return -1;
    }

    QTreeWidgetItem* top = item;
    while (top->parent() != nullptr)
    {
        top = top->parent();
    }
    bool ok = false;
    const int index = top->data(0, HistoryWidgetDetail::TopIndexRole).toInt(&ok);
    return ok ? index : -1;
}

void HistoryWidget::OnItemClicked(QTreeWidgetItem* item, int /*column*/)
{
    const int target = TopLevelIndexForItem(item);
    if (target >= 0)
    {
        JumpToIndex(target);
    }
}

void HistoryWidget::JumpToIndex(int targetIndex)
{
    if (scene == nullptr)
    {
        return;
    }

    const RECommandStack* stack = scene->GetCommandStack();
    const uint32 commandCount = stack->GetCommandCount();
    targetIndex = std::min(targetIndex, static_cast<int>(commandCount) - 1);
    targetIndex = std::max(targetIndex, -1);

    // the loop calls scene->Undo()/scene->Redo() which emit commandExecuted;
    // guard rebuilds and do a single rebuild at the end
    rebuildGuard = true;

    uint32 guard = 0;
    const uint32 maxSteps = commandCount + 2;
    while (scene->CanUndo() && stack->GetCurrentCommandIndex() > targetIndex && guard < maxSteps)
    {
        scene->Undo();
        ++guard;
    }
    while (scene->CanRedo() && stack->GetCurrentCommandIndex() < targetIndex && guard < maxSteps)
    {
        scene->Redo();
        ++guard;
    }

    rebuildGuard = false;
    RebuildList();
}

void HistoryWidget::OnUndoAllClicked()
{
    JumpToIndex(-1);
}

void HistoryWidget::OnRedoAllClicked()
{
    if (scene == nullptr)
    {
        return;
    }
    JumpToIndex(static_cast<int>(scene->GetCommandStack()->GetCommandCount()) - 1);
}

void HistoryWidget::OnClearClicked()
{
    // console-style: no confirmation dialog, immediate clear
    if (scene == nullptr)
    {
        return;
    }
    scene->ClearAllCommands();
    // historyReset is emitted by RECommandStack::Clear, rebuilding the list
}
