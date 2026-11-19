#include "Classes/History/HistoryModule.h"
#include "Classes/History/Private/HistoryWidget.h"

#include "REPlatform/DataNodes/SceneData.h"
#include "REPlatform/Scene/SceneEditor2.h"

#include <TArc/Core/ContextAccessor.h>
#include <TArc/Utils/ModuleCollection.h>
#include <TArc/WindowSubSystem/UI.h>

#include <Reflection/ReflectedTypeDB.h>
#include <Reflection/ReflectionRegistrator.h>

#include <QDockWidget>

HistoryModule::HistoryModule()
{
}

void HistoryModule::PostInit()
{
    using namespace DAVA;

    ContextAccessor* accessor = GetAccessor();
    binder.reset(new FieldBinder(accessor));

    FieldDescriptor sceneField;
    sceneField.type = ReflectedTypeDB::Get<SceneData>();
    sceneField.fieldName = FastName(SceneData::scenePropertyName);
    binder->BindField(sceneField, [this](const Any& v)
                      {
        SceneEditor2* scene = nullptr;
        if (v.CanCast<SceneData::TSceneType>())
        {
            scene = v.Cast<SceneData::TSceneType>().Get();
        }
        if (widget != nullptr)
        {
            widget->OnSceneChanged(scene);
        } });

    widget = new HistoryWidget();

    UI* ui = GetUI();

    DockPanelInfo info;
    info.title = QString("History");
    info.area = Qt::RightDockWidgetArea;
    info.tabbed = true;
    info.ensureVisible = false;

    PanelKey key("History", info);
    ui->AddView(DAVA::mainWindowKey, key, widget.data());
}

void HistoryModule::OnWindowClosed(const DAVA::WindowKey& /*key*/)
{
    if (widget != nullptr)
    {
        widget->DetachFromScene();
    }
}

DAVA_VIRTUAL_REFLECTION_IMPL(HistoryModule)
{
    DAVA::ReflectionRegistrator<HistoryModule>::Begin()
    .ConstructorByPointer()
    .End();
}

DECL_TARC_MODULE(HistoryModule);
    