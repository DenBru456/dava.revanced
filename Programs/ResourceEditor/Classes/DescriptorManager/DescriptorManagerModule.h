#pragma once

#include <TArc/Core/ClientModule.h>
#include <TArc/Core/FieldBinder.h>
#include <TArc/Utils/QtConnections.h>

#include <Base/BaseTypes.h>

#include <memory>

namespace DAVA
{
class RECommandNotificationObject;
class SceneEditor2;
} // namespace DAVA

class DescriptorManagerSettings;
class DescriptorManagerReactor;
class QTimer;

class DescriptorManagerModule : public DAVA::ClientModule
{
public:
    DescriptorManagerModule();
    ~DescriptorManagerModule() override;

private:
    void PostInit() override;

    // scope: 0 = whole project, 1 = opened scene, 2 = selected entities
    enum class Scope : DAVA::int32
    {
        All = 0,
        Scene = 1,
        Selected = 2
    };

    static const char* ScopeName(Scope scope);

    void OnProjectOpened();
    // scene opened: create missing descriptors of the scene's textures and reload them
    void OnSceneOpened(DAVA::SceneEditor2* scene);

    // Add Scene merges into the current scene through StructureSystem::Add instead of opening a second one
    void OnSceneStructureChanged(DAVA::SceneEditor2* scene);

    // every command reports itself here as well; this path reaches Add Scene
    void OnSceneCommandExecuted(DAVA::SceneEditor2* scene, const DAVA::RECommandNotificationObject& commandNotification);
    void WatchSceneStructure(DAVA::SceneEditor2* scene);

    // one pass per command burst, on the event loop after the frame that ran it
    void ScheduleSceneTexturePass();
    void RunScheduledSceneTexturePass();

    // creates the scene's missing descriptors and reloads what it replaced
    void RunSceneTexturePass(DAVA::SceneEditor2* scene, const char* reason);
    void ShowSettingsDialog();
    void RunGenerate(Scope scope);
    void RunUpdate(Scope scope);

    // incremental time-sliced project scan: Generate/Update All and the startup mode run through it
    void StartScanJob(bool update);
    void StopScanJob(const char* reason);
    void ProcessScanSlice();

    std::unique_ptr<DAVA::FieldBinder> binder;
    std::unique_ptr<DescriptorManagerSettings> settings;
    std::unique_ptr<DescriptorManagerReactor> reactor;
    DAVA::QtConnections connections;

    // defined in the cpp; the declaration must sit inside the class
    struct ScanJobData;

    QTimer* scanTimer = nullptr; // ClientModule is not a QObject: manual ownership
    QTimer* scenePassTimer = nullptr; // one-shot, coalesces structure changes into one pass
    std::unique_ptr<ScanJobData> scanJob;

    // scene connection state and verified descriptors, defined in the cpp next to ScanJobData
    struct SceneStructureWatcher;
    std::unique_ptr<SceneStructureWatcher> structureWatcher;

    DAVA_VIRTUAL_REFLECTION(DescriptorManagerModule, DAVA::ClientModule);
};
