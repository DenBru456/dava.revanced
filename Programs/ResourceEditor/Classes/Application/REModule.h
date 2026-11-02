#pragma once

#include <TArc/Core/ClientModule.h>
#include <TArc/DataProcessing/DataListener.h>
#include <TArc/DataProcessing/DataWrapper.h>

#include <Render/Material/NMaterial.h>
#include <QAction>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QRect>
#include <QSet>
#include <QString>
#include <TArc/Utils/QtConnections.h>

class QWidget;
class TextureCache;
class ResourceEditorLauncher;
class REModule : public DAVA::ClientModule, public DAVA::DataListener
{
public:
    REModule();
    ~REModule();

protected:
    void PostInit() override;

    // NOTE: the borders switch is a live setting, the inset follows it without a restart
    void OnDataChanged(const DAVA::DataWrapper& wrapper, const DAVA::Vector<DAVA::Any>& fields) override;

    // applies "FullScreen: no viewport borders" to every scene view, whatever the window state is
    void ApplyViewportBorders();

    void ShowMaterial(DAVA::NMaterial* material);

    void ToggleFullscreen();

    // applies the mode the editor was closed in, once a scene exists: same path as F11
    void ApplySavedModeAtStartup();

    void SyncRenderSurface(QWidget* window);

    DAVA_VIRTUAL_REFLECTION_IN_PLACE(REModule, DAVA::ClientModule)
    {
        DAVA::ReflectionRegistrator<REModule>::Begin()
        .ConstructorByPointer()
        .End();
    }

    DAVA::QtConnections connections;
    DAVA::DataWrapper generalSettingsWrapper;
    QByteArray windowedDockState;
    bool fullscreenActive = false;
    // set while the mode hides or shows a host, so the visibility watchers do not read it as a user decision
    bool modeHidingInProgress = false;
    // set while the startup restore runs: an already fullscreen window must enter the mode, not leave it
    bool modeEnterForced = false;

    // the layout the mode was left with: hosts the user opened in it and hosts the user closed in it
    QSet<QString> modeHostOpened;
    QSet<QString> modeHostClosed;

    // normal-mode visibility per host, captured before the mode hides anything
    QHash<QString, bool> windowedHostVisibleAtEntry;

    // actions whose shortcuts moved onto the window because their host is hidden in the mode
    QList<QAction*> fullscreenRehomedActions;

    // watchers on the hosts while a mode is active, so in-mode changes reach the mode's memory
    QList<QMetaObject::Connection> fullscreenDockConnections;
    bool windowedMenuBarVisible = true;
    bool windowedStatusBarVisible = true;
    Qt::WindowFlags windowedWindowFlags;
    QRect windowedGeometry;
    Qt::WindowStates windowedWindowState = Qt::WindowNoState;
    QString windowedStyleSheet;
    qint64 lastFullscreenToggleMs = 0;
};