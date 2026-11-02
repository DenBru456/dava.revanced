#include "Classes/DescriptorManager/Private/DescriptorManagerSettingsDialog.h"

#include "Classes/DescriptorManager/Private/DescriptorManagerSettings.h"
#include <Debug/DVAssert.h>

#include <Render/TextureDescriptor.h>

#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTimer>

#include <algorithm>

namespace
{
const int OUTER_MARGIN = 4;
const int GROUP_MARGIN = 6;
const int GROUP_GAP = 10;
const int ROW_GAP = 6;
const int LABEL_GAP = 6;
const int ARROW_SIZE = 24;
const int BUTTON_HEIGHT = 26;

int ClampTimeRow(int priority)
{
    return (priority >= 0 && priority <= 2) ? priority : 0;
}

QString FormatTooltip(const QString& extension)
{
    if (extension == QString(".dds"))
    {
        return QString("DDS: also a compressed container (DXT/BC). Usable as a source - "
                       "the engine decodes it on the CPU.");
    }
    if (extension == QString(".pvr"))
    {
        return QString("PVR: compressed container, not a plain image format. Usable as a source for "
                       "raw 4444/5551/565 payloads; PVRTC has no CPU decoder, so CPU-side consumers "
                       "will not read it.");
    }
    return QString("Plain image format.");
}

void FillPriorityList(QListWidget* list, const DAVA::Vector<DAVA::String>& entries, bool formatTips)
{
    list->clear();
    for (const DAVA::String& entry : entries)
    {
        const QString text = QString::fromStdString(entry);
        QListWidgetItem* item = new QListWidgetItem(text, list);
        item->setToolTip(formatTips ? FormatTooltip(text) : QString("Preferred over the families below it when one texture "
                                                                    "has several family-prefixed variants."));
    }
}

// height needed to show every row, chrome included: the largest of three measurements wins
int ListFullHeight(QListWidget* list)
{
    const int chrome = std::max(2, list->height() - list->viewport()->height());
    if (list->count() <= 0)
    {
        return chrome;
    }

    const QRect firstRow = list->visualItemRect(list->item(0));
    int rowHeight = std::max(list->sizeHintForRow(0), firstRow.height());
    if (rowHeight <= 0)
    {
        rowHeight = list->fontMetrics().height() + 8;
    }

    const QRect lastRow = list->visualItemRect(list->item(list->count() - 1));
    const int byRows = list->count() * rowHeight;
    const int byLastRow = lastRow.bottom() + 1;

    return std::max(byRows, byLastRow) + chrome;
}

void ConfigureList(QListWidget* list)
{
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setUniformItemSizes(true);
    // no horizontal scrollbar: it would reserve about 17 px under the last row
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // grows horizontally with the window, stays as tall as its rows
    list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    list->setMinimumWidth(120);
    if (list->count() > 0)
    {
        list->setCurrentRow(0);
    }
    // provisional height, FitToContent sets the real one after polish
    list->setFixedHeight(ListFullHeight(list));
}

void MoveRow(QListWidget* list, int delta)
{
    if (list == nullptr)
    {
        return;
    }

    const int row = list->currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= list->count())
    {
        return;
    }

    QListWidgetItem* item = list->takeItem(row);
    list->insertItem(target, item);
    list->setCurrentRow(target);
}

QPushButton* MakeArrowButton(const QString& glyph, const QString& tooltip)
{
    QPushButton* button = new QPushButton(glyph);
    button->setFixedSize(ARROW_SIZE, ARROW_SIZE);
    button->setToolTip(tooltip);
    return button;
}

QPushButton* MakePageButton(const QString& text, QWidget* parent)
{
    QPushButton* button = new QPushButton(text, parent);
    button->setCheckable(true);
    button->setMinimumHeight(BUTTON_HEIGHT);
    return button;
}

// width is the text plus the theme padding, never a fixed number
QPushButton* MakeActionButton(const QString& text)
{
    QPushButton* button = new QPushButton(text);
    button->setFixedHeight(BUTTON_HEIGHT);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return button;
}

} // namespace

DescriptorManagerSettingsDialog::DescriptorManagerSettingsDialog(DescriptorManagerSettings* settings_, QWidget* parent)
    : QDialog(parent)
    , settings(settings_)
{
    DVASSERT(settings != nullptr);
    setWindowTitle(tr("Descriptor Manager Settings"));
    setModal(true);
    setSizeGripEnabled(false);

    formatList = new QListWidget(this);
    FillPriorityList(formatList, settings->formatPriority, true);
    ConfigureList(formatList);

    familyList = new QListWidget(this);
    FillPriorityList(familyList, settings->familyPriority, false);
    ConfigureList(familyList);

    timeList = new QListWidget(this);
    {
        QListWidgetItem* noneItem = new QListWidgetItem(tr("None"), timeList);
        noneItem->setData(Qt::UserRole, DescriptorManagerSettings::TIME_NONE);
        noneItem->setToolTip(tr("File age is ignored. A family is resolved by its declared container, "
                                "then .pvr, then .dds."));

        QListWidgetItem* currentItem = new QListWidgetItem(tr("Current"), timeList);
        currentItem->setData(Qt::UserRole, DescriptorManagerSettings::TIME_CURRENT);
        currentItem->setToolTip(tr("When one family holds several files, keep the one that was already "
                                   "there. A generated copy never outranks the asset it came from."));

        QListWidgetItem* newItem = new QListWidgetItem(tr("New"), timeList);
        newItem->setData(Qt::UserRole, DescriptorManagerSettings::TIME_NEW);
        newItem->setToolTip(tr("When one family holds several files, take the most recently written one."));
    }
    ConfigureList(timeList);
    timeList->setCurrentRow(ClampTimeRow(settings->timePriority));

    pages = new QStackedWidget(this);
    pages->addWidget(formatList);
    pages->addWidget(familyList);
    pages->addWidget(timeList);

    formatPageButton = MakePageButton(tr("Format"), this);
    formatPageButton->setToolTip(tr("Source file extension order (.png, .tga, ...)."));
    gpuPageButton = MakePageButton(tr("GPU"), this);
    gpuPageButton->setToolTip(tr("GPU family order for family-prefixed variants."));
    timePageButton = MakePageButton(tr("Time"), this);
    timePageButton->setToolTip(tr("Which file wins when one family holds several."));

    QHBoxLayout* pageRow = new QHBoxLayout();
    pageRow->setContentsMargins(0, 0, 0, 0);
    pageRow->setSpacing(ROW_GAP);
    pageRow->addWidget(formatPageButton);
    pageRow->addWidget(gpuPageButton);
    pageRow->addWidget(timePageButton);
    pageRow->addStretch(1);

    upButton = MakeArrowButton(QString::fromUtf8("\xE2\x96\xB2"), tr("Move up (higher priority)"));
    downButton = MakeArrowButton(QString::fromUtf8("\xE2\x96\xBC"), tr("Move down (lower priority)"));

    QVBoxLayout* arrowColumn = new QVBoxLayout();
    arrowColumn->setContentsMargins(0, 0, 0, 0);
    arrowColumn->setSpacing(ROW_GAP);
    arrowColumn->addWidget(upButton);
    arrowColumn->addWidget(downButton);
    arrowColumn->addStretch(1);

    QHBoxLayout* listRow = new QHBoxLayout();
    listRow->setContentsMargins(0, 0, 0, 0);
    listRow->setSpacing(ROW_GAP);
    listRow->addWidget(pages, 1);
    listRow->addLayout(arrowColumn);

    sourceBox = new QComboBox(this);
    sourceBox->addItem(tr("Existing"), DescriptorManagerSettings::SOURCE_EXISTING);
    sourceBox->addItem(tr("original"), DescriptorManagerSettings::SOURCE_DEFAULT);
    sourceBox->addItem(tr("Off"), DescriptorManagerSettings::SOURCE_OFF);
    sourceBox->setItemData(0, tr("Use only sources already present under the plain name. Nothing is ever created."),
                           Qt::ToolTipRole);
    sourceBox->setItemData(1, tr("If no plain source exists, recreate the original from the best family variant."),
                           Qt::ToolTipRole);
    sourceBox->setItemData(2, tr("Never read or create source files. Descriptors are created with their "
                                 "defaults and the runtime fallbacks serve the pixels."),
                           Qt::ToolTipRole);
    {
        const int stored = sourceBox->findData(settings->sourceCreate);
        sourceBox->setCurrentIndex(stored >= 0 ? stored : 1);
    }
    sourceBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    sourceBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    overwriteBox = new QCheckBox(tr("Overwrite existing source textures"), this);
    overwriteBox->setChecked(settings->overwriteExistingSource);
    overwriteBox->setToolTip(tr("A plain-named source that already exists is replaced instead of kept."));

    defaultExtensionBox = new QComboBox(this);
    defaultExtensionBox->addItem(QString(".png"));
    defaultExtensionBox->addItem(QString(".tga"));
    defaultExtensionBox->setToolTip(tr("Extension of a source file the module has to create. Same as the "
                                       "variant's extension: byte copy; different: decoded and re-encoded."));
    {
        const QString storedExtension = QString::fromStdString(settings->defaultSourceExtension);
        const int extensionIndex = defaultExtensionBox->findText(storedExtension);
        defaultExtensionBox->setCurrentIndex(extensionIndex >= 0 ? extensionIndex : 0);
    }
    defaultExtensionBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    defaultExtensionBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    sceneOpenBox = new QCheckBox(tr("Create missing descriptors when a scene opens"), this);
    sceneOpenBox->setChecked(settings->generateMissingOnSceneOpen);
    sceneOpenBox->setToolTip(tr("A scene whose textures have no .tex normally shows pink placeholders. "
                                "On: the descriptors are created from the source images when the scene opens "
                                "and the placeholders are reloaded in place."));

    reactBox = new QCheckBox(tr("React to texture original missing errors"), this);
    reactBox->setChecked(settings->reactToMissingSource);
    reactBox->setToolTip(tr("On the engine's 'Cannot create ... from <path>' error, resolve the source immediately."));

    ignoreHealthyBox = new QCheckBox(tr("Update ignores healthy descriptors"), this);
    ignoreHealthyBox->setChecked(settings->updateIgnoresHealthy);
    ignoreHealthyBox->setToolTip(tr("Off: every descriptor is resaved and reconverted, healthy ones included."));

    startupBox = new QComboBox(this);
    startupBox->addItem(tr("Nothing"), DescriptorManagerSettings::STARTUP_NOTHING);
    startupBox->addItem(tr("Generate"), DescriptorManagerSettings::STARTUP_GENERATE);
    startupBox->addItem(tr("Update"), DescriptorManagerSettings::STARTUP_UPDATE);
    startupBox->setItemData(0, tr("Do nothing when the project opens."), Qt::ToolTipRole);
    startupBox->setItemData(1, tr("Create missing descriptors on open."), Qt::ToolTipRole);
    startupBox->setItemData(2, tr("Rebuild and relink existing descriptors on open."), Qt::ToolTipRole);
    {
        const int stored = startupBox->findData(settings->startupAction);
        startupBox->setCurrentIndex(stored >= 0 ? stored : 0);
    }
    startupBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    startupBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    priorityGroup = new QGroupBox(tr("Priorities"), this);
    QVBoxLayout* priorityLayout = new QVBoxLayout(priorityGroup);
    priorityLayout->setContentsMargins(GROUP_MARGIN, GROUP_MARGIN, GROUP_MARGIN, GROUP_MARGIN);
    priorityLayout->setSpacing(ROW_GAP);
    priorityLayout->addLayout(pageRow);
    priorityLayout->addLayout(listRow);

    // Labels in column 0 at natural width,
    // dropdowns in column 1 stretched to the box edge, checkboxes spanning both
    miscGroup = new QGroupBox(tr("MISC"), this);
    QGridLayout* miscLayout = new QGridLayout(miscGroup);
    miscLayout->setContentsMargins(GROUP_MARGIN, GROUP_MARGIN, GROUP_MARGIN, GROUP_MARGIN);
    miscLayout->setHorizontalSpacing(LABEL_GAP);
    miscLayout->setVerticalSpacing(ROW_GAP);

    missingLabel = new QLabel(tr("Missing source:"), miscGroup);
    extensionLabel = new QLabel(tr("Default source extension:"), miscGroup);
    startupLabel = new QLabel(tr("On project startup:"), miscGroup);
    QLabel* labels[] = { missingLabel, extensionLabel, startupLabel };
    for (QLabel* label : labels)
    {
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }

    miscLayout->addWidget(missingLabel, 0, 0);
    miscLayout->addWidget(sourceBox, 0, 1);
    miscLayout->addWidget(overwriteBox, 1, 0, 1, 2);
    miscLayout->addWidget(extensionLabel, 2, 0);
    miscLayout->addWidget(defaultExtensionBox, 2, 1);
    miscLayout->addWidget(sceneOpenBox, 3, 0, 1, 2);
    miscLayout->addWidget(reactBox, 4, 0, 1, 2);
    miscLayout->addWidget(ignoreHealthyBox, 5, 0, 1, 2);
    miscLayout->addWidget(startupLabel, 6, 0);
    miscLayout->addWidget(startupBox, 6, 1);
    miscLayout->setColumnStretch(1, 1);

    defaultsButton = MakeActionButton(tr("Defaults"));
    defaultsButton->setToolTip(tr("Put every option back to its shipped value. Nothing is written until OK."));
    okButton = MakeActionButton(tr("OK"));
    cancelButton = MakeActionButton(tr("Cancel"));
    okButton->setDefault(true);

    connect(defaultsButton, &QPushButton::clicked, this, &DescriptorManagerSettingsDialog::OnDefaults);
    connect(okButton, &QPushButton::clicked, this, &DescriptorManagerSettingsDialog::OnAccepted);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    QHBoxLayout* buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(ROW_GAP);
    buttonRow->addWidget(defaultsButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(okButton);
    buttonRow->addWidget(cancelButton);

    connect(formatPageButton, &QPushButton::clicked, this, [this]()
            { ShowPage(PAGE_FORMAT); });
    connect(gpuPageButton, &QPushButton::clicked, this, [this]()
            { ShowPage(PAGE_GPU); });
    connect(timePageButton, &QPushButton::clicked, this, [this]()
            { ShowPage(PAGE_TIME); });
    connect(upButton, &QPushButton::clicked, this, &DescriptorManagerSettingsDialog::OnMoveUp);
    connect(downButton, &QPushButton::clicked, this, &DescriptorManagerSettingsDialog::OnMoveDown);
    connect(formatList, &QListWidget::itemSelectionChanged, this, &DescriptorManagerSettingsDialog::UpdateArrowState);
    connect(familyList, &QListWidget::itemSelectionChanged, this, &DescriptorManagerSettingsDialog::UpdateArrowState);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(OUTER_MARGIN, OUTER_MARGIN, OUTER_MARGIN, OUTER_MARGIN);
    layout->setSpacing(GROUP_GAP);
    layout->addWidget(priorityGroup);
    layout->addWidget(miscGroup);
    layout->addLayout(buttonRow);

    ShowPage(PAGE_FORMAT);

    // sizing runs in FitToContent from showEvent: only then do the theme font and stylesheet paddings exist
    layout->activate();
}

void DescriptorManagerSettingsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);

    // run twice: the first pass on the polished theme, the second queued behind the first paint
    FitToContent();
    QTimer::singleShot(0, this, [this]()
                       { FitToContent(); });
}

void DescriptorManagerSettingsDialog::FitToContent()
{
    // heights only; height() keeps reporting the old geometry until the parent lays out
    QListWidget* lists[] = { formatList, familyList, timeList };
    int listHeight = 0;
    for (QListWidget* list : lists)
    {
        const int height = ListFullHeight(list);
        list->setFixedHeight(height);
        listHeight = std::max(listHeight, height);
    }
    pages->setFixedHeight(listHeight);

    // widths are the layout's job; only a floor is set so no dropdown collapses
    QComboBox* combos[] = { sourceBox, defaultExtensionBox, startupBox };
    for (QComboBox* combo : combos)
    {
        combo->setMinimumWidth(combo->sizeHint().width());
    }
    // the window ends at the widest fixed line plus the group frame and the dialog padding
    layout()->activate();
    setFixedSize(layout()->minimumSize());
}

QListWidget* DescriptorManagerSettingsDialog::ActiveList() const
{
    const int page = pages->currentIndex();
    if (page == PAGE_FORMAT)
    {
        return formatList;
    }
    if (page == PAGE_GPU)
    {
        return familyList;
    }
    return nullptr;
}

void DescriptorManagerSettingsDialog::ShowPage(int page)
{
    pages->setCurrentIndex(page);
    formatPageButton->setChecked(page == PAGE_FORMAT);
    gpuPageButton->setChecked(page == PAGE_GPU);
    timePageButton->setChecked(page == PAGE_TIME);
    UpdateArrowState();
}

void DescriptorManagerSettingsDialog::UpdateArrowState()
{
    QListWidget* list = ActiveList();
    const bool reorderable = (list != nullptr);
    upButton->setEnabled(reorderable);
    downButton->setEnabled(reorderable);
    upButton->setVisible(reorderable);
    downButton->setVisible(reorderable);
}

void DescriptorManagerSettingsDialog::OnMoveUp()
{
    MoveRow(ActiveList(), -1);
    UpdateArrowState();
}

void DescriptorManagerSettingsDialog::OnMoveDown()
{
    MoveRow(ActiveList(), 1);
    UpdateArrowState();
}

void DescriptorManagerSettingsDialog::OnDefaults()
{
    FillPriorityList(formatList, DescriptorManagerSettings::AllSourceFormats(), true);
    FillPriorityList(familyList, DescriptorManagerSettings::AllGPUFamilies(), false);
    formatList->setCurrentRow(0);
    familyList->setCurrentRow(0);

    timeList->setCurrentRow(0);
    sourceBox->setCurrentIndex(std::max(0, sourceBox->findData(int(DescriptorManagerSettings::SOURCE_DEFAULT))));
    startupBox->setCurrentIndex(std::max(0, startupBox->findData(int(DescriptorManagerSettings::STARTUP_NOTHING))));

    overwriteBox->setChecked(false);
    sceneOpenBox->setChecked(true);
    reactBox->setChecked(false);
    ignoreHealthyBox->setChecked(true);
    defaultExtensionBox->setCurrentIndex(std::max(0, defaultExtensionBox->findText(QString(".png"))));

    UpdateArrowState();
}

void DescriptorManagerSettingsDialog::OnAccepted()
{
    settings->formatPriority.clear();
    for (int i = 0; i < formatList->count(); ++i)
    {
        settings->formatPriority.push_back(formatList->item(i)->text().toStdString());
    }

    settings->familyPriority.clear();
    for (int i = 0; i < familyList->count(); ++i)
    {
        settings->familyPriority.push_back(familyList->item(i)->text().toStdString());
    }

    {
        QListWidgetItem* timeItem = timeList->currentItem();
        settings->timePriority = (timeItem != nullptr) ? timeItem->data(Qt::UserRole).toInt() : int(DescriptorManagerSettings::TIME_NONE);
    }
    settings->generateMissingOnSceneOpen = sceneOpenBox->isChecked();
    settings->reactToMissingSource = reactBox->isChecked();
    settings->updateIgnoresHealthy = ignoreHealthyBox->isChecked();
    settings->overwriteExistingSource = overwriteBox->isChecked();
    settings->defaultSourceExtension = defaultExtensionBox->currentText().toStdString();
    settings->sourceCreate = sourceBox->currentData().toInt();
    settings->startupAction = startupBox->currentData().toInt();

    // applies immediately: the next resolved texture uses the new rule
    DAVA::TextureDescriptor::SetTimePriority(settings->timePriority);

    accept();
}
