#include "Classes/Application/REModule.h"
#include "Classes/Application/FileSystemData.h"
#include "Classes/Application/Private/SettingsConverter.h"

#include "Classes/Qt/Main/mainwindow.h"
#include "Classes/Qt/MaterialEditor/MaterialEditor.h"
#include "Classes/Qt/TextureBrowser/TextureCache.h"
#include "Classes/SceneManager/Private/SceneRenderWidget.h"

#include <REPlatform/DataNodes/Settings/RESettings.h>
#include <REPlatform/Global/GlobalOperations.h>
#include <REPlatform/Scene/Systems/EditorStatisticsSystem.h>

#include <TArc/DataProcessing/TArcDataNode.h>
#include <TArc/WindowSubSystem/Private/UIManager.h>

#include <Engine/EngineContext.h>
#include <FileSystem/LocalizationSystem.h>
#include <UI/Render/UIRenderSystem.h>
#include <UI/UIControlSystem.h>

#include <QPointer>
#include <Engine/Engine.h>
#include <Engine/Qt/RenderWidget.h>
#include <Engine/Window.h>
#include <Functional/Function.h>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFrame>
#include <QGuiApplication>
#include <QLayout>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QRect>
#include <QScreen>
#include <QShortcut>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QWindow>
#include <REPlatform/DataNodes/SceneData.h>
#include <Render/Highlevel/RenderSystem.h>
#include <Render/RHI/rhi_Type.h>
#include <Scene3D/Scene.h>
#include <TArc/Controls/SceneTabbar.h>
#include <TArc/DataProcessing/PropertiesHolder.h>
#include <TArc/WindowSubSystem/ActionUtils.h>
#include <TArc/WindowSubSystem/QtAction.h>
#include <algorithm>
#include <functional>

namespace REModuleDetail
{
class REGlobalData : public DAVA::TArcDataNode
{
public:
    REGlobalData(DAVA::UI* ui)
    {
        textureCache = new TextureCache();
        mainWindow = new QtMainWindow(ui);
    }

    ~REGlobalData()
    {
        DAVA::SafeRelease(textureCache);

        if (!mainWindow.isNull())
        {
            QtMainWindow* mainWindowPointer = mainWindow.data();
            mainWindow.clear();
            DAVA::SafeDelete(mainWindowPointer);
        }
    }

    TextureCache* textureCache = nullptr;
    QPointer<QtMainWindow> mainWindow;

    DAVA_VIRTUAL_REFLECTION_IN_PLACE(REGlobalData) {
    };
};
} // namespace REModuleDetail

REModule::REModule()
{
    DAVA_REFLECTION_REGISTER_PERMANENT_NAME(DAVA::RenderStatsSettings);
    DAVA_REFLECTION_REGISTER_PERMANENT_NAME(MaterialEditorSettings);
}

REModule::~REModule()
{
    generalSettingsWrapper.SetListener(nullptr);
    GetAccessor()->GetGlobalContext()->DeleteData<REModuleDetail::REGlobalData>();
}

void REModule::OnDataChanged(const DAVA::DataWrapper& wrapper, const DAVA::Vector<DAVA::Any>& fields)
{
    // NOTE: every general setting lands here, the inset is one bool so re-reading it is free
    ApplyViewportBorders();
}

void REModule::ApplyViewportBorders()
{
    using TData = REModuleDetail::REGlobalData;

    DAVA::DataContext* globalContext = GetAccessor()->GetGlobalContext();
    TData* globalData = globalContext->GetData<TData>();
    if (globalData == nullptr || globalData->mainWindow.isNull())
    {
        return;
    }

    DAVA::GeneralSettings* settings = globalContext->GetData<DAVA::GeneralSettings>();
    // NOTE: no settings node still means no borders
    const bool noBorders = (settings == nullptr) || settings->fullscreenNoBorders;
    const int margin = (noBorders == true) ? 0 : SceneRenderWidget::DefaultViewMargin;

    const QList<SceneRenderWidget*> views = globalData->mainWindow.data()->findChildren<SceneRenderWidget*>();
    for (SceneRenderWidget* view : views)
    {
        view->SetViewMargin(margin);
    }
}

namespace
{
// NOTE: one native window state change at a time; presses during it fold into a parity
QElapsedTimer fullscreenTransitionTimer;
bool fullscreenTransitionPending = false;
const int fullscreenTransitionMs = 220;

// NOTE: frames painted during a resize are cleared frames; block until the layout settles
class RenderFrameGuard final : public QObject
{
public:
    explicit RenderFrameGuard(DAVA::RenderWidget* renderWidget_)
        : QObject(renderWidget_)
        , renderWidget(renderWidget_)
    {
        settleTimer.setSingleShot(true);
        settleTimer.setInterval(150);
        QObject::connect(&settleTimer, &QTimer::timeout, this, [this]()
                         { Refresh(false); });

        QCoreApplication* application = QCoreApplication::instance();
        if (application != nullptr)
        {
            application->installEventFilter(this);
        }
    }

    ~RenderFrameGuard() override
    {
        QCoreApplication* application = QCoreApplication::instance();
        if (application != nullptr)
        {
            application->removeEventFilter(this);
        }

        if (renderWidget != nullptr)
        {
            renderWidget->SetFrameBlocked(false);
        }
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        const QEvent::Type type = event->type();

        if (type == QEvent::Resize)
        {
            QWidget* widget = qobject_cast<QWidget*>(object);
            QWidget* window = (renderWidget != nullptr) ? renderWidget->window() : nullptr;
            if (widget != nullptr && window != nullptr && (widget == window || window->isAncestorOf(widget) == true))
            {
                // NOTE: one timer for the whole resize storm
                settleTimer.start();
                Refresh(true);
            }

            return false;
        }

        if (type == QEvent::Show || type == QEvent::Hide)
        {
            // NOTE: menus and panels move the layout after the event; treat them the same
            if (qobject_cast<QMenu*>(object) != nullptr ||
                qobject_cast<QDockWidget*>(object) != nullptr ||
                qobject_cast<QToolBar*>(object) != nullptr)
            {
                settleTimer.start();
                Refresh(true);
            }
        }

        return false;
    }

private:
    void Refresh(bool blocked)
    {
        if (renderWidget != nullptr && blocked != isBlocked)
        {
            isBlocked = blocked;
            renderWidget->SetFrameBlocked(blocked);
        }

        if (blocked == false && renderWidget != nullptr)
        {
            // NOTE: frozen view: ask the surface for exactly one frame
            QWidget* backend = renderWidget->findChild<QWidget*>(DAVA::RenderWidget::BackendWidgetName);
            if (backend != nullptr)
            {
                const QList<QWindow*> surfaces = backend->findChildren<QWindow*>();
                for (QWindow* surface : surfaces)
                {
                    if (surface->isExposed() == true)
                    {
                        surface->requestUpdate();
                    }
                }
            }
        }
    }

    DAVA::RenderWidget* renderWidget = nullptr;
    QTimer settleTimer;
    bool isBlocked = false;
};

} // namespace

namespace
{
// NOTE: the settings say how the mode looks; whether it is on lives in a property node
const DAVA::String fullscreenModeNode = "reFullscreenMode";
const DAVA::String fullscreenModeActiveKey = "active";

bool LoadModeActive(DAVA::ContextAccessor* accessor)
{
    if (accessor == nullptr)
    {
        return false;
    }

    DAVA::PropertiesItem item = accessor->CreatePropertiesNode(fullscreenModeNode);
    return item.Get(fullscreenModeActiveKey, false);
}

void SaveModeActive(DAVA::ContextAccessor* accessor, bool active)
{
    if (accessor == nullptr)
    {
        return;
    }

    DAVA::PropertiesItem item = accessor->CreatePropertiesNode(fullscreenModeNode);
    item.Set(fullscreenModeActiveKey, active);
}

// NOTE: not from PostInit, the docks do not exist yet; waits for the first scene
class ModeRestoreOnStartup : public QObject, private DAVA::DataListener
{
public:
    ModeRestoreOnStartup(DAVA::ContextAccessor* accessor_, std::function<void()> restore_)
        : accessor(accessor_)
        , restore(std::move(restore_))
    {
        sceneWrapper = accessor->CreateWrapper(DAVA::ReflectedTypeDB::Get<DAVA::SceneData>());
        sceneWrapper.SetListener(this);
    }

    void OnDataChanged(const DAVA::DataWrapper&, const DAVA::Vector<DAVA::Any>&) override
    {
        if (sceneWrapper.HasData() == false)
        {
            return;
        }

        DAVA::DataContext* activeContext = accessor->GetActiveContext();
        DAVA::SceneData* sceneData = (activeContext != nullptr) ? activeContext->GetData<DAVA::SceneData>() : nullptr;
        if (sceneData == nullptr || sceneData->GetScene().Get() == nullptr)
        {
            return;
        }

        sceneWrapper.SetListener(nullptr);

        // NOTE: one turn later, the scene tab exists by then
        std::function<void()> restoreMode = restore;
        QTimer::singleShot(0, this, [restoreMode]()
                           { restoreMode(); });

        deleteLater();
    }

private:
    DAVA::ContextAccessor* accessor = nullptr;
    DAVA::DataWrapper sceneWrapper;
    std::function<void()> restore;
};
} // namespace

void REModule::PostInit()
{
    DAVA::ContextAccessor* accessor = GetAccessor();
    ConvertSettingsIfNeeded(accessor->GetPropertiesHolder(), accessor);

    const DAVA::EngineContext* engineContext = accessor->GetEngineContext();
    engineContext->localizationSystem->InitWithDirectory("~res:/Strings/");
    engineContext->localizationSystem->SetCurrentLocale("en");
    engineContext->uiControlSystem->GetRenderSystem()->SetClearColor(DAVA::Color(accessor->GetGlobalContext()->GetData<DAVA::GeneralSettings>()->sceneBackgroundColor));

    using TData = REModuleDetail::REGlobalData;
    DAVA::DataContext* globalContext = accessor->GetGlobalContext();
    globalContext->CreateData(std::make_unique<TData>(GetUI()));
    TData* globalData = globalContext->GetData<TData>();

    DAVA::UI* ui = GetUI();
    ui->InjectWindow(DAVA::mainWindowKey, globalData->mainWindow);
    globalData->mainWindow->AfterInjectInit();
    globalData->mainWindow->EnableGlobalTimeout(true);

    generalSettingsWrapper = accessor->CreateWrapper(DAVA::ReflectedTypeDB::Get<DAVA::GeneralSettings>());
    generalSettingsWrapper.SetListener(this);
    ApplyViewportBorders();

    RegisterOperation(DAVA::ShowMaterial.ID, this, &REModule::ShowMaterial);

    { // fullscreen with the panels out of the way. F11 itself is owned by the window
      // NOTE: the action hosts in the status bar, which Full hides; the shortcut needs another host
        DAVA::QtAction* fullscreenAction = new DAVA::QtAction(accessor, QStringLiteral("Fullscreen"));
        fullscreenAction->setObjectName("actionFullscreen");
        fullscreenAction->setCheckable(true);

        DAVA::KeyBindableActionInfo keyBindInfo;
        keyBindInfo.blockName = "Editor";
        keyBindInfo.context = Qt::ApplicationShortcut; // must also work while the window is fullscreen
        DAVA::MakeActionKeyBindable(fullscreenAction, keyBindInfo);

        connections.AddConnection(fullscreenAction, &QAction::triggered, DAVA::Bind(&REModule::ToggleFullscreen, this));

        DAVA::ActionPlacementInfo placementInfo;
        placementInfo.AddPlacementPoint(DAVA::CreateStatusbarPoint(false));
        GetUI()->AddAction(DAVA::mainWindowKey, placementInfo, fullscreenAction);

        // NOTE: window owned shortcut so it survives the status bar being hidden
        QShortcut* fullscreenShortcut = new QShortcut(QKeySequence(Qt::Key_F11), globalData->mainWindow.data());
        fullscreenShortcut->setContext(Qt::ApplicationShortcut);
        // NOTE: autoRepeat off, a held key toggled the mode 30 times a second
        fullscreenShortcut->setAutoRepeat(false);
        connections.AddConnection(fullscreenShortcut, &QShortcut::activated, DAVA::Bind(&REModule::ToggleFullscreen, this));
    }

    { // NOTE: block the frame while the layout settles, the engine paints cleared frames otherwise
        DAVA::RenderWidget* renderWidget = globalData->mainWindow->findChild<DAVA::RenderWidget*>();
        if (renderWidget != nullptr)
        {
            new RenderFrameGuard(renderWidget);
        }
    }

    { // bring back the mode the editor was closed in, once there is a scene to bring it back to
        new ModeRestoreOnStartup(accessor, [this]()
                                 { ApplySavedModeAtStartup(); });
    }

    globalContext->CreateData(std::make_unique<FileSystemData>());
}

namespace
{
// NOTE: every action under this widget
void CollectWidgetActions(QWidget* widget, QList<QAction*>& actions)
{
    if (widget == nullptr)
    {
        return;
    }

    actions.append(widget->actions());

    const QList<QWidget*> children = widget->findChildren<QWidget*>();
    for (QWidget* child : children)
    {
        actions.append(child->actions());
    }
}

// NOTE: hidden hosts disarm their actions shortcuts; re-home them on the window
void RehomeActionShortcuts(QWidget* window, const QList<QWidget*>& hiddenHosts, QList<QAction*>& added)
{
    if (window == nullptr)
    {
        return;
    }

    QSet<QAction*> seen;
    for (QWidget* host : hiddenHosts)
    {
        QList<QAction*> actions;
        CollectWidgetActions(host, actions);
        for (QAction* action : actions)
        {
            if (action == nullptr || action->shortcut().isEmpty() || seen.contains(action))
            {
                continue;
            }

            seen.insert(action);
            window->addAction(action);
            added.push_back(action);
        }
    }
}

// NOTE: stable widget id: object name, then title, then pointer
QString WidgetKey(QWidget* widget)
{
    if (widget == nullptr)
    {
        return QString();
    }

    const QString name = widget->objectName();
    if (name.isEmpty() == false)
    {
        return name;
    }

    const QString title = widget->windowTitle();
    if (title.isEmpty() == false)
    {
        return title;
    }

    return QString::number(reinterpret_cast<quintptr>(widget));
}

QScreen* ScreenForWindow(QWidget* window)
{
    if (window != nullptr && window->windowHandle() != nullptr && window->windowHandle()->screen() != nullptr)
    {
        return window->windowHandle()->screen();
    }
    return QGuiApplication::primaryScreen();
}

// NOTE: Qt caches windowState across a window recreation; compare it with the real rect
void EnsureMaximized(QWidget* window)
{
    if (window == nullptr)
    {
        return;
    }

    QScreen* screen = ScreenForWindow(window);
    const QRect area = (screen != nullptr) ? screen->availableGeometry() : QRect();
    const bool stateSaysMaximized = window->windowState().testFlag(Qt::WindowMaximized);
    const bool geometrySaysMaximized = (area.isValid() == false) || (window->geometry() == area);

    if (stateSaysMaximized == true && geometrySaysMaximized == true)
    {
        return;
    }

    if (stateSaysMaximized == true)
    {
        window->setWindowState(Qt::WindowNoState);
    }

    window->setWindowState(Qt::WindowMaximized);
}

void EnsureWindowGeometry(QWidget* window, const QRect& target)
{
    if (window == nullptr)
    {
        return;
    }

    const Qt::WindowStates state = window->windowState();
    if (state.testFlag(Qt::WindowMaximized) || state.testFlag(Qt::WindowFullScreen))
    {
        // NOTE: a maximized or fullscreen window ignores setGeometry, drop the state first
        window->setWindowState(Qt::WindowNoState);
    }

    if (target.isValid() && window->geometry() != target)
    {
        window->setGeometry(target);
    }
}

// NOTE: the D3D surface only resizes with the widget
void SyncOneWindow(QWidget* window)
{
    if (window == nullptr)
    {
        return;
    }

    if (window->layout() != nullptr)
    {
        window->layout()->activate();
    }

    // NOTE: RenderWidget::Update() is private, the surface QWindow::requestUpdate() is public
    QWidget* backend = window->findChild<QWidget*>(DAVA::RenderWidget::BackendWidgetName);
    if (backend != nullptr)
    {
        backend->updateGeometry();

        const QList<QWindow*> surfaces = backend->findChildren<QWindow*>();
        for (QWindow* surface : surfaces)
        {
            // NOTE: only an exposed surface can draw
            if (surface->isExposed())
            {
                surface->requestUpdate();
            }
        }
        return;
    }

    // NOTE: no backend by that name, fall back to any render widget
    const QList<QWidget*> children = window->findChildren<QWidget*>();
    for (QWidget* child : children)
    {
        if (QString::fromLatin1(child->metaObject()->className()).contains(QString("RenderWidget")))
        {
            child->updateGeometry();
            child->update();
        }
    }
}
} // namespace

void REModule::SyncRenderSurface(QWidget* window)
{
    // NOTE: the WM finishes the state change after the call returns; sync the surface twice
    SyncOneWindow(window);

    QPointer<QWidget> guard(window);
    QTimer::singleShot(0, window, [guard]()
                       { SyncOneWindow(guard.data()); });
    QTimer::singleShot(50, window, [guard]()
                       { SyncOneWindow(guard.data()); });
}

void REModule::ApplySavedModeAtStartup()
{
    if (fullscreenActive == true)
    {
        return;
    }

    if (LoadModeActive(GetAccessor()) == false)
    {
        // NOTE: closed with the mode off, nothing to restore
        return;
    }

    // NOTE: same path as F11; a window already fullscreen enters the mode, not exits
    modeEnterForced = true;
    ToggleFullscreen();
    modeEnterForced = false;
}

void REModule::ToggleFullscreen()
{
    using TData = REModuleDetail::REGlobalData;

    DAVA::DataContext* globalContext = GetAccessor()->GetGlobalContext();
    TData* globalData = globalContext->GetData<TData>();
    DAVA::GeneralSettings* settings = globalContext->GetData<DAVA::GeneralSettings>();

    if (globalData == nullptr || globalData->mainWindow.isNull())
    {
        return;
    }

    if (fullscreenTransitionTimer.isValid() == true && fullscreenTransitionTimer.elapsed() < fullscreenTransitionMs)
    {
        // NOTE: a press is pending, let the running toggle finish
        fullscreenTransitionPending = !fullscreenTransitionPending;
        return;
    }

    fullscreenTransitionTimer.start();

    QtMainWindow* mainWindow = globalData->mainWindow.data();
    QWidget* topLevel = mainWindow->window();

    // NOTE: the action and the window shortcut both deliver F11, ignore the duplicate
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - lastFullscreenToggleMs < 250)
    {
        return;
    }
    lastFullscreenToggleMs = nowMs;

    // NOTE: do not re-sync while restoring, the saved geometry is already fullscreen
    if (modeEnterForced == false && fullscreenActive == false && topLevel != nullptr && topLevel->isFullScreen() == true)
    {
        fullscreenActive = true;
    }

    // NOTE: no settings node still means fullscreen
    const DAVA::eFullscreenMode mode = (settings != nullptr) ? settings->fullscreenMode : DAVA::eFullscreenMode::Full;
    const bool hideDocks = (settings == nullptr) || settings->fullscreenHideDocks;
    const bool hideToolbars = (settings == nullptr) || settings->fullscreenHideToolbars;
    const bool hideMenuBar = (settings == nullptr) || settings->fullscreenHideMenuBar;
    const bool hideStatusBar = (settings == nullptr) || settings->fullscreenHideStatusBar;
    const bool hideSceneTab = (settings == nullptr) || settings->fullscreenHideSceneTab;
    const bool noBorders = (settings == nullptr) || settings->fullscreenNoBorders;

    // NOTE: Hide reads docks and toolbars, the other two read all six
    const bool windowedLayout = (mode != DAVA::eFullscreenMode::Hide);

    // NOTE: the viewport inset is a setting of its own, it is not part of the mode
    ApplyViewportBorders();

    if (fullscreenActive == false)
    {
        // NOTE: snapshot everything the toggle is about to change
        windowedDockState = mainWindow->saveState();
        windowedMenuBarVisible = (mainWindow->menuBar() != nullptr) && mainWindow->menuBar()->isVisible();
        windowedStatusBarVisible = (mainWindow->statusBar() != nullptr) && mainWindow->statusBar()->isVisible();
        if (topLevel != nullptr)
        {
            windowedWindowFlags = topLevel->windowFlags();
            windowedWindowState = topLevel->windowState();
            // NOTE: normalGeometry is the un-maximized rect
            windowedGeometry = topLevel->normalGeometry();
        }

        if (modeEnterForced == true && topLevel != nullptr && topLevel->isFullScreen() == true)
        {
            // NOTE: nothing windowed was captured, leaving the mode ends maximized
            windowedWindowState = Qt::WindowMaximized;
        }

        // NOTE: the thin frame around the viewport is layout margin
        if (noBorders == true && mode != DAVA::eFullscreenMode::Hide)
        {
            // NOTE: no style sheet here, child widgets and QMenu popups lose their style to QStyleSheetStyle

            QWidget* central = mainWindow->centralWidget();
            if (central != nullptr)
            {
                const QList<QLayout*> layouts = central->findChildren<QLayout*>();
                for (QLayout* layout : layouts)
                {
                    // NOTE: QVariant has no toMargins() in this Qt, four ints in a QRect
                    const QMargins margins = layout->contentsMargins();
                    layout->setProperty("reFullscreenMargins", QRect(margins.left(), margins.top(), margins.right(), margins.bottom()));
                    layout->setProperty("reFullscreenSpacing", layout->spacing());
                    layout->setContentsMargins(0, 0, 0, 0);
                    layout->setSpacing(0);
                }

                // NOTE: the central pane gets no style sheet either, only frames and margins
                const QList<QTabWidget*> tabWidgets = central->findChildren<QTabWidget*>();
                for (QTabWidget* tabWidget : tabWidgets)
                {
                    // NOTE: QTabWidget has no frame shape, documentMode drops the pane frame
                    tabWidget->setProperty("reFullscreenDocumentMode", tabWidget->documentMode());
                    tabWidget->setDocumentMode(true);
                    tabWidget->setContentsMargins(0, 0, 0, 0);
                    if (tabWidget->tabBar() != nullptr)
                    {
                        tabWidget->setProperty("reFullscreenTabBarVisible", tabWidget->tabBar()->isVisible());
                        tabWidget->tabBar()->hide();
                    }

                    if (tabWidget->layout() != nullptr)
                    {
                        QLayout* paneLayout = tabWidget->layout();
                        const QMargins paneMargins = paneLayout->contentsMargins();
                        paneLayout->setProperty("reFullscreenMargins", QRect(paneMargins.left(), paneMargins.top(), paneMargins.right(), paneMargins.bottom()));
                        paneLayout->setProperty("reFullscreenSpacing", paneLayout->spacing());
                        paneLayout->setContentsMargins(0, 0, 0, 0);
                        paneLayout->setSpacing(0);
                    }
                }

                // NOTE: theme frames inside the central area, one px on each side
                const QList<QFrame*> frames = central->findChildren<QFrame*>();
                for (QFrame* frame : frames)
                {
                    frame->setProperty("reFullscreenFrameShape", static_cast<int>(frame->frameShape()));
                    frame->setFrameShape(QFrame::NoFrame);
                }
            }

            // NOTE: the window layout is not a child of central, it kept its margins
            if (mainWindow->layout() != nullptr)
            {
                QLayout* windowLayout = mainWindow->layout();
                const QMargins margins = windowLayout->contentsMargins();
                windowLayout->setProperty("reFullscreenMargins", QRect(margins.left(), margins.top(), margins.right(), margins.bottom()));
                windowLayout->setProperty("reFullscreenSpacing", windowLayout->spacing());
                windowLayout->setContentsMargins(0, 0, 0, 0);
                windowLayout->setSpacing(0);
            }
        }

        // NOTE: what this session hides, so the exit can tell what the user brought back
        fullscreenHiddenByMode.clear();

        QList<QWidget*> hiddenHosts;

        auto hideUnlessKept = [this, &hiddenHosts](QWidget* widget)
        {
            const QString key = WidgetKey(widget);
            if (fullscreenKeptVisible.contains(key) == true)
            {
                // NOTE: the user opened this one in an earlier session, leave it
                return;
            }

            fullscreenHiddenByMode.insert(key);
            widget->hide();
            hiddenHosts.push_back(widget);
        };

        if (hideDocks == true)
        {
            const QList<QDockWidget*> docks = mainWindow->findChildren<QDockWidget*>();
            for (QDockWidget* dock : docks)
            {
                hideUnlessKept(dock);
            }
        }

        if (hideToolbars == true)
        {
            const QList<QToolBar*> toolbars = mainWindow->findChildren<QToolBar*>();
            for (QToolBar* toolbar : toolbars)
            {
                hideUnlessKept(toolbar);
            }
        }

        // NOTE: from here on a visibility change is the user decision
        for (const QMetaObject::Connection& connection : fullscreenDockConnections)
        {
            QObject::disconnect(connection);
        }
        fullscreenDockConnections.clear();
        fullscreenUserClosedDocks.clear();

        // NOTE: a close during the mode must not be overruled by the layout snapshot
        auto onHostVisibilityChanged = [this](const QString& key, bool visible)
        {
            if (modeHidingInProgress == true)
            {
                // NOTE: the mode is putting this host away, not the user
                return;
            }

            if (visible == false)
            {
                fullscreenUserClosedDocks.insert(key);
                fullscreenKeptVisible.remove(key);
            }
            else
            {
                fullscreenUserClosedDocks.remove(key);
            }
        };

        const QList<QDockWidget*> watchedDocks = mainWindow->findChildren<QDockWidget*>();
        for (QDockWidget* dock : watchedDocks)
        {
            const QString key = WidgetKey(dock);
            QMetaObject::Connection connection = QObject::connect(dock, &QDockWidget::visibilityChanged, mainWindow, [onHostVisibilityChanged, key](bool visible)
                                                                  { onHostVisibilityChanged(key, visible); });
            fullscreenDockConnections.push_back(connection);
        }

        const QList<QToolBar*> watchedToolbars = mainWindow->findChildren<QToolBar*>();
        for (QToolBar* toolbar : watchedToolbars)
        {
            const QString key = WidgetKey(toolbar);
            QMetaObject::Connection connection = QObject::connect(toolbar, &QToolBar::visibilityChanged, mainWindow, [onHostVisibilityChanged, key](bool visible)
                                                                  { onHostVisibilityChanged(key, visible); });
            fullscreenDockConnections.push_back(connection);
        }

        if (windowedLayout == true)
        {
            if (hideMenuBar == true && mainWindow->menuBar() != nullptr)
            {
                mainWindow->menuBar()->hide();
                hiddenHosts.push_back(mainWindow->menuBar());
            }

            if (hideStatusBar == true && mainWindow->statusBar() != nullptr)
            {
                mainWindow->statusBar()->hide();
                hiddenHosts.push_back(mainWindow->statusBar());
            }

            if (hideSceneTab == true)
            {
                // NOTE: the scene tab strip is part of the border
                const QList<DAVA::SceneTabbar*> sceneTabbars = mainWindow->findChildren<DAVA::SceneTabbar*>();
                for (DAVA::SceneTabbar* tabbar : sceneTabbars)
                {
                    tabbar->hide();
                    hiddenHosts.push_back(tabbar);
                }
            }
        }

        if (topLevel != nullptr)
        {
            if (mode == DAVA::eFullscreenMode::Full && topLevel->isFullScreen() == false)
            {
                // NOTE: one state call only, a following setGeometry recreates the surface
                topLevel->setWindowState(Qt::WindowFullScreen);
            }
            // NOTE: only Full touches the window

            topLevel->raise();
            topLevel->activateWindow();
        }

        mainWindow->setFocus();

        // NOTE: move shortcuts of hidden hosts onto the window
        fullscreenRehomedActions.clear();
        RehomeActionShortcuts(mainWindow, hiddenHosts, fullscreenRehomedActions);

        fullscreenActive = true;
        SaveModeActive(GetAccessor(), true);
    }
    else
    {
        if (topLevel != nullptr)
        {
            // NOTE: never hide the window, that flashes the desktop through
            const bool leftFullScreen = topLevel->windowState().testFlag(Qt::WindowFullScreen);
            if (mode != DAVA::eFullscreenMode::Full && leftFullScreen == false)
            {
                // NOTE: only Full has to undo itself
            }
            else if (windowedWindowState.testFlag(Qt::WindowMaximized) || windowedGeometry.isValid() == false)
            {
                // NOTE: no windowed geometry captured, end maximized
                EnsureMaximized(topLevel);
            }
            else
            {
                EnsureWindowGeometry(topLevel, windowedGeometry);
            }

            topLevel->raise();
            topLevel->activateWindow();
        }

        // NOTE: the mode is over, a visibility change is not a decision of the mode
        for (const QMetaObject::Connection& connection : fullscreenDockConnections)
        {
            QObject::disconnect(connection);
        }
        fullscreenDockConnections.clear();

        // NOTE: what the user re-opened stays open in the next session
        auto rememberKept = [this](QWidget* widget)
        {
            const QString key = WidgetKey(widget);
            if (fullscreenHiddenByMode.contains(key) == false)
            {
                return;
            }

            if (widget->isVisible() == true)
            {
                fullscreenKeptVisible.insert(key);
            }
            else
            {
                fullscreenKeptVisible.remove(key);
            }
        };

        const QList<QToolBar*> exitToolbars = mainWindow->findChildren<QToolBar*>();
        for (QToolBar* toolbar : exitToolbars)
        {
            rememberKept(toolbar);
        }

        const QList<QDockWidget*> exitDocks = mainWindow->findChildren<QDockWidget*>();
        for (QDockWidget* dock : exitDocks)
        {
            rememberKept(dock);
        }

        // NOTE: the shortcuts belong to their real hosts again
        for (QAction* action : fullscreenRehomedActions)
        {
            mainWindow->removeAction(action);
        }
        fullscreenRehomedActions.clear();

        bool restored = false;
        if (windowedDockState.isEmpty() == false)
        {
            restored = mainWindow->restoreState(windowedDockState);
            windowedDockState.clear();
        }

        if (restored == false)
        {
            // NOTE: plain show() so panels and toolbars always come back
            const QList<QDockWidget*> docks = mainWindow->findChildren<QDockWidget*>();
            for (QDockWidget* dock : docks)
            {
                dock->show();
            }

            const QList<QToolBar*> toolbars = mainWindow->findChildren<QToolBar*>();
            for (QToolBar* toolbar : toolbars)
            {
                toolbar->show();
            }
        }

        const QList<DAVA::SceneTabbar*> sceneTabbars = mainWindow->findChildren<DAVA::SceneTabbar*>();
        for (DAVA::SceneTabbar* tabbar : sceneTabbars)
        {
            tabbar->show();
        }

        // NOTE: not part of the restored layout, put it back explicitly
        const QList<QDockWidget*> keptDocks = mainWindow->findChildren<QDockWidget*>();
        for (QDockWidget* dock : keptDocks)
        {
            if (fullscreenKeptVisible.contains(WidgetKey(dock)) == true)
            {
                dock->show();
            }
        }

        // NOTE: restoreState brought back what the user closed, undo it
        modeHidingInProgress = true;

        auto applyUserChoice = [this](QWidget* widget)
        {
            if (fullscreenUserClosedDocks.contains(WidgetKey(widget)) == true)
            {
                widget->hide();
            }
        };

        const QList<QToolBar*> chosenToolbars = mainWindow->findChildren<QToolBar*>();
        for (QToolBar* toolbar : chosenToolbars)
        {
            applyUserChoice(toolbar);
        }

        const QList<QDockWidget*> chosenDocks = mainWindow->findChildren<QDockWidget*>();
        for (QDockWidget* dock : chosenDocks)
        {
            applyUserChoice(dock);
        }

        modeHidingInProgress = false;

        // NOTE: menu and status bar visibility are not part of the saved state
        if (mainWindow->menuBar() != nullptr)
        {
            mainWindow->menuBar()->setVisible(windowedMenuBarVisible);
        }

        if (mainWindow->statusBar() != nullptr)
        {
            mainWindow->statusBar()->setVisible(windowedStatusBarVisible);
        }

        // NOTE: close it again, or it returns on every entry
        const QList<QDockWidget*> closedDocks = mainWindow->findChildren<QDockWidget*>();
        for (QDockWidget* dock : closedDocks)
        {
            if (fullscreenUserClosedDocks.contains(WidgetKey(dock)) == true)
            {
                dock->hide();
            }
        }

        if (topLevel != nullptr)
        {
            topLevel->raise();
        }
        fullscreenActive = false;
        SaveModeActive(GetAccessor(), false);
    }

    // NOTE: frames first, the margin pass restores them
    const QList<QFrame*> frames = mainWindow->findChildren<QFrame*>();
    for (QFrame* frame : frames)
    {
        const QVariant shape = frame->property("reFullscreenFrameShape");
        if (shape.isValid() == true)
        {
            frame->setFrameShape(static_cast<QFrame::Shape>(shape.toInt()));
            frame->setProperty("reFullscreenFrameShape", QVariant());
        }
    }

    if (QWidget* central = mainWindow->centralWidget())
    {
        // NOTE: the window layout carries the property too, so walk the whole window
        const QList<QLayout*> layouts = mainWindow->findChildren<QLayout*>();
        for (QLayout* layout : layouts)
        {
            const QVariant margins = layout->property("reFullscreenMargins");
            if (margins.isValid() == false)
            {
                continue;
            }

            const QRect saved = margins.toRect();
            layout->setContentsMargins(saved.left(), saved.top(), saved.width(), saved.height());
            layout->setSpacing(layout->property("reFullscreenSpacing").toInt());
            layout->setProperty("reFullscreenMargins", QVariant());
            layout->setProperty("reFullscreenSpacing", QVariant());
        }

        const QList<QTabWidget*> tabWidgets = central->findChildren<QTabWidget*>();
        for (QTabWidget* tabWidget : tabWidgets)
        {
            const QVariant documentMode = tabWidget->property("reFullscreenDocumentMode");
            if (documentMode.isValid() == false)
            {
                continue;
            }

            tabWidget->setDocumentMode(documentMode.toBool());
            tabWidget->setProperty("reFullscreenDocumentMode", QVariant());

            const QVariant barVisible = tabWidget->property("reFullscreenTabBarVisible");
            if (barVisible.isValid() == true && tabWidget->tabBar() != nullptr)
            {
                tabWidget->tabBar()->setVisible(barVisible.toBool());
                tabWidget->setProperty("reFullscreenTabBarVisible", QVariant());
            }
        }
    }

    // NOTE: one surface sync at the end of the toggle
    SyncRenderSurface(topLevel);

    // NOTE: the action indicator follows the mode
    if (QAction* action = mainWindow->findChild<QAction*>(QString("actionFullscreen")))
    {
        action->setChecked(fullscreenActive);
    }

    if (fullscreenTransitionPending == true)
    {
        // NOTE: odd press while moving, run one more toggle when it stops
        fullscreenTransitionPending = false;
        QTimer::singleShot(fullscreenTransitionMs, mainWindow, [this]()
                           { ToggleFullscreen(); });
    }
}

void REModule::ShowMaterial(DAVA::NMaterial* material)
{
    using TData = REModuleDetail::REGlobalData;
    DAVA::DataContext* globalContext = GetAccessor()->GetGlobalContext();
    TData* globalData = globalContext->GetData<TData>();
    globalData->mainWindow->OnMaterialEditor(material);
}
