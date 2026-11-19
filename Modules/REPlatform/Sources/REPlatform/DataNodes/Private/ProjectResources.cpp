#include "REPlatform/DataNodes/ProjectResources.h"
#include "REPlatform/DataNodes/ProjectManagerData.h"

#include <TArc/Core/ContextAccessor.h>

#include <Engine/Engine.h>
#include <FileSystem/FilePath.h>
#include <FileSystem/FileSystem.h>
#include <FileSystem/YamlNode.h>
#include <FileSystem/YamlParser.h>
#include <Logger/Logger.h>
#include <Scene3D/Systems/QualitySettingsSystem.h>
#include <Sound/SoundSystem.h>

namespace DAVA
{
namespace ProjectResourcesDetails
{
// quality.yaml used to be read only from <project>/DataSource/quality.yaml. A project without a
// DataSource folder (game content that only has Data/) therefore loaded nothing, and
// QualitySettingsSystem::Load() returns silently when the file is missing, which left the quality
// dialog with empty Textures / Materials / Sounds lists. Data/ is the TOP resource folder, so it
// is tried first, exactly like ~res:/ resolves; FileSystem::Exists() and the YAML reader both
// accept a packed quality.yaml.dvpl, so either form is found.
FilePath FindQualityConfig(const ProjectManagerData* data)
{
    const FilePath candidates[] = {
        data->GetDataPath() + "quality.yaml",
        data->GetDataSourcePath() + "quality.yaml",
    };

    FileSystem* fileSystem = GetEngineContext()->fileSystem;
    for (const FilePath& candidate : candidates)
    {
        if (fileSystem->Exists(candidate))
        {
            return candidate;
        }
    }
    return FilePath();
}

Vector<String> LoadMaterialQualities(FilePath fxPath)
{
    Vector<String> qualities;
    RefPtr<YamlParser> parser(YamlParser::Create(fxPath));
    if (parser)
    {
        YamlNode* rootNode = parser->GetRootNode();
        if (rootNode)
        {
            const YamlNode* materialTemplateNode = rootNode->Get("MaterialTemplate");
            if (materialTemplateNode)
            {
                static const char* QUALITIES[] = { "LOW", "MEDIUM", "HIGH", "ULTRA_HIGH" };
                for (const char* quality : QUALITIES)
                {
                    const YamlNode* qualityNode = materialTemplateNode->Get(quality);
                    if (qualityNode)
                    {
                        qualities.push_back(quality);
                    }
                }
            }
        }
    }

    return qualities;
}

static const FilePath PATH_TO_ASSIGNABLE_YAML = "~res:/Materials/assignable.yaml"; // file that contains info about available materials

void LoadMaterialTemplatesInfo(Vector<MaterialTemplateInfo>& templates)
{
    templates.clear();

    if (FileSystem::Instance()->Exists(PATH_TO_ASSIGNABLE_YAML))
    {
        RefPtr<YamlParser> parser(YamlParser::Create(PATH_TO_ASSIGNABLE_YAML));
        YamlNode* rootNode = parser->GetRootNode();

        if (nullptr != rootNode)
        {
            FilePath materialsListDir = PATH_TO_ASSIGNABLE_YAML.GetDirectory();

            for (uint32 i = 0; i < rootNode->GetCount(); ++i)
            {
                const YamlNode* templateNode = rootNode->Get(i);
                if (nullptr != templateNode)
                {
                    const YamlNode* name = templateNode->Get("name");
                    const YamlNode* path = templateNode->Get("path");

                    if (nullptr != name && nullptr != path &&
                        name->GetType() == YamlNode::TYPE_STRING &&
                        path->GetType() == YamlNode::TYPE_STRING)
                    {
                        const FilePath templatePath = materialsListDir + path->AsString();
                        if (FileSystem::Instance()->Exists(templatePath))
                        {
                            MaterialTemplateInfo info;

                            info.name = name->AsString().c_str();
                            info.path = templatePath.GetFrameworkPath().c_str();
                            info.qualities = ProjectResourcesDetails::LoadMaterialQualities(templatePath);

                            templates.push_back(info);
                        }
                    }
                }
            }
        }
    }
}
}

ProjectResources::ProjectResources(ContextAccessor* accessor)
    : accessor(accessor)
{
    DataContext* globalContext = accessor->GetGlobalContext();
    globalContext->CreateData(std::make_unique<ProjectManagerData>());
}

ProjectResources::~ProjectResources()
{
    UnloadProject();
    DataContext* globalContext = accessor->GetGlobalContext();
    globalContext->DeleteData<ProjectManagerData>();
}

ProjectManagerData* ProjectResources::GetProjectManagerData()
{
    DataContext* ctx = accessor->GetGlobalContext();
    return ctx->GetData<ProjectManagerData>();
}

void ProjectResources::LoadProject(const FilePath& incomePath)
{
    ProjectManagerData* data = GetProjectManagerData();

    if (incomePath.IsDirectoryPathname() && incomePath != data->projectPath)
    {
        UnloadProject();

        data->projectPath = incomePath;
        FilePath::AddResourcesFolder(data->GetDataSourcePath());
        FilePath::AddTopResourcesFolder(data->GetDataPath());

        ProjectResourcesDetails::LoadMaterialTemplatesInfo(data->materialTemplatesInfo);

        const FilePath qualityConfig = ProjectResourcesDetails::FindQualityConfig(data);
        if (qualityConfig.IsEmpty())
        {
            Logger::Warning("[Project] quality.yaml not found in %s or %s, the quality lists stay empty",
                            data->GetDataPath().GetAbsolutePathname().c_str(),
                            data->GetDataSourcePath().GetAbsolutePathname().c_str());
        }
        else
        {
            Logger::Info("[Project] quality config: %s", qualityConfig.GetAbsolutePathname().c_str());
            QualitySettingsSystem::Instance()->Load(qualityConfig);
        }

        const EngineContext* engineCtx = accessor->GetEngineContext();
        engineCtx->soundSystem->InitFromQualitySettings();
    }
}

void ProjectResources::UnloadProject()
{
    ProjectManagerData* data = GetProjectManagerData();

    if (!data->projectPath.IsEmpty())
    {
        const EngineContext* engineCtx = accessor->GetEngineContext();
        engineCtx->soundSystem->UnloadFMODProjects();

        FilePath::RemoveResourcesFolder(data->GetDataPath());
        FilePath::RemoveResourcesFolder(data->GetDataSourcePath());
        data->projectPath = "";
    }
}
} // namespace DAVA
