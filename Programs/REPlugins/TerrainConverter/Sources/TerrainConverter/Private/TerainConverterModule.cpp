// rpcndr.h (pulled in via the COM/OLE chain) defines `interface` as a macro,
// which corrupts TArc's ClientModule API below. Neutralize before ANY include.
#ifdef interface
#undef interface
#endif

#include "TerainConverterModule.h"
#include "Converter.h"

#include <REPlatform/DataNodes/SceneData.h>
#include <REPlatform/Global/REFileOperationsIntarface.h>

#include <TArc/Core/ContextAccessor.h>
#include <TArc/DataProcessing/Common.h>
#include <TArc/DataProcessing/SettingsNode.h>
#include <TArc/PluginsManager/TArcPlugin.h>
#include <TArc/Qt/QtIcon.h>
#include <TArc/Qt/QtString.h>
#include <TArc/WindowSubSystem/ActionUtils.h>
#include <TArc/WindowSubSystem/QtAction.h>
#include <TArc/WindowSubSystem/UI.h>

#include <Base/BaseTypes.h>
#include <Base/FastName.h>
#include <Functional/Signal.h>
#include <Reflection/ReflectedTypeDB.h>
#include <Reflection/ReflectionRegistrator.h>
#ifdef interface
#undef interface
#endif
namespace TerrainConverterGUIModuleDetail
{
class ConvertTerrainOperation : public DAVA::REFileOperation
{
public:
    QIcon GetIcon() const override
    {
        return QIcon();
    }

    QString GetName() const override
    {
        return QStringLiteral("Convert for WoT Blitz");
    }

    eOperationType GetType() const override
    {
        return REFileOperation::eOperationType::EXPORT;
    }

    QString GetTargetFileFilter() const override
    {
        return "Scene File (*.sc2)";
    }

    void Apply(const DAVA::FilePath& filePath) const override
    {
        convertScene.Emit(filePath);
    }

    mutable DAVA::Signal<const DAVA::FilePath&> convertScene;
};

class TerrainConverterSettings : public DAVA::SettingsNode
{
public:
    // String, not FilePath: the settings framework has no Serializer<FilePath> specialization, so
    // FilePath fields are not written to the settings storage and come back empty on the next start
    DAVA::String templatesPath;
    DAVA::String clientDataSource;

    DAVA_VIRTUAL_REFLECTION_IN_PLACE(TerrainConverterSettings, DAVA::SettingsNode)
    {
        DAVA::ReflectionRegistrator<TerrainConverterSettings>::Begin()[DAVA::M::DisplayName("Terrain Converter")]
        .ConstructorByPointer()
        .Field("Templates Path", &TerrainConverterSettings::templatesPath)
        .Field("WoT Blitz DataSource", &TerrainConverterSettings::clientDataSource)
        .End();
    }
};
} // namespace TerrainConverterGUIModuleDetail

TerrainConverterGUIModule::TerrainConverterGUIModule()
{
    using namespace TerrainConverterGUIModuleDetail;
    DAVA_REFLECTION_REGISTER_PERMANENT_NAME(TerrainConverterSettings);
}

void TerrainConverterGUIModule::PostInit()
{
    using namespace DAVA;
    using namespace TerrainConverterGUIModuleDetail;

    ContextAccessor* accessor = GetAccessor();
    UI* ui = GetUI();

    QtAction* action = new QtAction(accessor, "Convert Map");
    ConvertTerrainOperation* operation = new ConvertTerrainOperation();
    operation->convertScene.Connect(this, &TerrainConverterGUIModule::OnConvertScene);
    convertOperation.reset(operation);

    const Type* sceneDataType = Type::Instance<SceneData>();
    FieldDescriptor descr;
    descr.type = ReflectedTypeDB::GetByType(sceneDataType);
    descr.fieldName = FastName(SceneData::scenePropertyName);
    action->SetStateUpdationFunction(QtAction::Enabled, descr, [](const Any& v)
                                     { return v.IsEmpty() == false; });

    QUrl menuPoint = CreateMenuPoint(QList<QString>() << "Terrain Converter");
    ActionPlacementInfo info(menuPoint);
    ui->AddAction(DAVA::mainWindowKey, info, action);
}

void TerrainConverterGUIModule::OnInterfaceRegistered(const DAVA::Type* interfaceType)
{
    using namespace TerrainConverterGUIModuleDetail;
    if (interfaceType == DAVA::Type::Instance<DAVA::REFileOperationsInterface>())
    {
        DAVA::REFileOperationsInterface* fileOperations = QueryInterface<DAVA::REFileOperationsInterface>();
        fileOperations->RegisterFileOperation(convertOperation);
    }
}

void TerrainConverterGUIModule::OnBeforeInterfaceUnregistered(const DAVA::Type* interfaceType)
{
    using namespace TerrainConverterGUIModuleDetail;
    if (interfaceType == DAVA::Type::Instance<DAVA::REFileOperationsInterface>())
    {
        DAVA::REFileOperationsInterface* fileOperations = QueryInterface<DAVA::REFileOperationsInterface>();
        fileOperations->UnregisterFileOperation(convertOperation);
    }
}

void TerrainConverterGUIModule::OnConvertScene(const DAVA::FilePath& scenePath)
{
    using namespace TerrainConverterGUIModuleDetail;
    TerrainConverterSettings* settings = GetAccessor()->GetGlobalContext()->GetData<TerrainConverterSettings>();
    Converter con(DAVA::FilePath(settings->templatesPath), DAVA::FilePath(settings->clientDataSource));
    con.Do(scenePath, DAVA::Vector<DAVA::String>());
}

DAVA_VIRTUAL_REFLECTION_IMPL(TerrainConverterGUIModule)
{
    DAVA::ReflectionRegistrator<TerrainConverterGUIModule>::Begin()
    .ConstructorByPointer()
    .End();
}

TerrainConverterConsoleModule::TerrainConverterConsoleModule(const DAVA::Vector<DAVA::String>& commandLine)
    : CommandLineModule(commandLine, "-mapConvert")
{
    using namespace DAVA;

    options.AddOption("-file", VariantType(String("")), "Target map for convert");
    options.AddOption("-project", VariantType(String("")), "Wot Bltz project path");
    options.AddOption("-templates", VariantType(String("")), "Terrain converter tamplates");
}

bool TerrainConverterConsoleModule::PostInitInternal()
{
    mapPath = DAVA::FilePath(options.GetOption("-file").AsString());
    projectPath = options.GetOption("-project").AsString();
    templatesPath = options.GetOption("-templates").AsString();

    if (mapPath.IsEmpty() == true || projectPath.empty() == true || templatesPath.empty() == true)
    {
        DAVA::Logger::Error("Incorrect input args");
        return false;
    }

    return true;
}

DAVA::ConsoleModule::eFrameResult TerrainConverterConsoleModule::OnFrameInternal()
{
    Converter con(templatesPath, projectPath);
    con.Do(mapPath, DAVA::Vector<DAVA::String>());

    return eFrameResult::FINISHED;
}

void TerrainConverterConsoleModule::ShowHelpInternal()
{
    CommandLineModule::ShowHelpInternal();
}

DAVA_VIRTUAL_REFLECTION_IMPL(TerrainConverterConsoleModule)
{
    DAVA::ReflectionRegistrator<TerrainConverterConsoleModule>::Begin()[DAVA::M::CommandName("-mapConvert")]
    .ConstructorByPointer<DAVA::Vector<DAVA::String>>()
    .End();
}

// The plugin entry block must only exist when building TerrainConverter as a
// real PLUGIN DLL. END_PLUGIN_DECLARATION() defines DAVAMain() - and so does
// ResourceEditor.exe - so in the STATIC build (DAVA_IMPLEMENT_PLUGIN_MODULE
// not defined) this block MUST be compiled out or the exe link fails with
// LNK2005 (duplicate DAVAMain). Registration in the static build happens via
// StaticModuleRegistrar.cpp (DECL_TARC_MODULE) instead.
#if defined(DAVA_IMPLEMENT_PLUGIN_MODULE)
START_PLUGIN_DECLARATION();
DECLARE_PLUGIN(TerrainConverterGUIModule, DAVA::TArcPlugin::PluginDescriptor("ResourceEditor", "TerrainConverter", "Terrain Converter for Blitz", "Terrain Converter for Blitz", 0, 1));
DECLARE_PLUGIN(TerrainConverterConsoleModule, DAVA::TArcPlugin::PluginDescriptor("ResourceEditor", "TerrainConverterCmd", "Terrain Converter for Blitz", "Terrain Converter for Blitz", 0, 1));
END_PLUGIN_DECLARATION();
#endif // DAVA_IMPLEMENT_PLUGIN_MODULE
