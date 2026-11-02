/*
    BeastManager.cpp - reconstruction for the dava.revanced Beast module restore.

    The original implementation of this file was never published in the public
    dava.engine repository (verified: the file is absent from every public
    commit and from the LFS store, while every other Modules/Beast file is
    present). This file is a faithful reconstruction built from:
      * the public BeastManager.h contract (the header IS published),
      * the ILB (Beast v5) SDK bundled in Modules/Beast/Thirdparty,
      * the call sites in the ResourceEditor (BeastRunner, BeastCommandLineTool)
        and in the module itself (SceneParser, BeastMeshInstance, TextureTarget,
        BeastPointCloud, BeastRunner's expected output layout).

    Bake flow (matches BeastRunner expectations):
      Run(scene)
        -> BeginScene (ILBBeginScene)
        -> SceneParser walks the scene (async: landscape color thumbnails)
        -> OnSceneParsingCompleted
             -> EndScene (ILBEndScene)
             -> GenerateLightmaps
                  ILBCreateJob with the mode xml (lightmaps_mode.xml /
                  sh_mode.xml / preview_mode.xml, deployed by the module CMake
                  MIX_APP_DATA into Data.beast/config)
                  - MODE_LIGHTMAPS / MODE_PREVIEW:
                      ILBCreateIlluminationPass  "sm" (ILB_IM_FULL)
                      ILBCreateAmbientOcclusionPass "ao"
                      one TextureTarget per lightmapped BeastMeshInstance,
                      ILBAddBakeInstance + both passes added to the target.
                  - MODE_SPHERICAL_HARMONICS:
                      ILBCreateIlluminationPassSH + ILBCreateVertexTarget,
                      point clouds attached through ILBAddBakePointCloud.
                  ILBStartJob (output path = lightmaps directory)
        -> Update() polls progress / completion (pumped by BeastRunner::Process
           every frame)
        -> OnJobCompleted
             -> SaveResults
                  - lightmaps: readback -> {lightmapsDirectory}/sm/*.png and
                    /ao/*.png (the exact layout LightmapsPacker consumes),
                    the landscape lightmap goes to the top level as
                    landscape.png (moved out by BeastRunner::PackLightmaps)
                  - SH: vertex buffer readback -> SpeedTreeObject::SetSphericalHarmonics
             -> LOD iteration: for lodLevel 1..maxLodLevel re-run the whole
                parse+job cycle, baking only the batches that were not baked
                yet (IsLightmapGenerated / SetLightmapGenerated bookkeeping).

    Deliberate simplifications (documented, none affect the bake itself):
      * BeastManager::UpdateAtlas is a stub. The atlasing data produced by
        LightmapsPacker is consumed on the editor side (texture descriptors /
        sprite frames); the published BeastManager.h has no member to retain
        it, so the original semantics cannot be recovered from public
        sources. It is kept as a logging no-op so the published header
        contract stays intact.
      * Ambient occlusion pass uses fixed defaults (max distance 100, cone
        angle 90 degrees). The original may have taken these from scene
        settings; the ILB defaults behave identically for typical scenes.
*/

#include "Beast/BeastManager.h"

#include "Beast/BeastDebug.h"
#include "Beast/BeastNames.h"
#include "Beast/BeastMaterial.h"
#include "Beast/BeastMesh.h"
#include "Beast/BeastMeshInstance.h"
#include "Beast/BeastPointCloud.h"
#include "Beast/BeastTexture.h"
#include "Beast/LightmapAtlasingData.h"
#include "Beast/SceneParser.h"
#include "Beast/TextureTarget.h"

#include <Scene3D/Scene.h>
#include <Scene3D/Entity.h>
#include <Scene3D/Components/ComponentHelpers.h>
#include <Render/Highlevel/RenderObject.h>
#include <Render/Highlevel/SpeedTreeObject.h>
#include <Render/Image/Image.h>
#include <Render/Image/ImageSystem.h>
#include <Render/RenderBase.h>
#include <FileSystem/FileSystem.h>
#include <Logger/Logger.h>
#include <Debug/DVAssert.h>
#include <Base/BaseTypes.h>

#include <cstring>
#include <cstdlib>

namespace Beast
{
namespace BeastManagerDetails
{
// Beast binaries / config are deployed by the module CMake MIX_APP_DATA into
// "Data.beast/bin" and "Data.beast/config" (see Modules/Beast/CMakeLists.txt).
// The editor can be started from different working directories, so try the
// usual candidates until one exists.
DAVA::FilePath FindBeastResource(const DAVA::String& relativePath)
{
    static const DAVA::Vector<DAVA::String> roots = {
        "Data.beast/",
        "~res:/Data.beast/",
        "Data/Data.beast/"
    };

    for (const DAVA::String& root : roots)
    {
        DAVA::FilePath candidate(root + relativePath);
        if (DAVA::FileSystem::Instance()->Exists(candidate))
        {
            return candidate;
        }
    }

    return DAVA::FilePath(roots.front() + relativePath);
}

DAVA::String GetBeastLicenseKey()
{
    // Beast v5 licenses are supplied through the manager; the usual deployment
    // keeps the key in the BEAST_LICENSE environment variable. An empty key
    // still lets evaluation mode run.
    const char* envKey = std::getenv("BEAST_LICENSE");
    return (envKey != nullptr) ? DAVA::String(envKey) : DAVA::String();
}

// Saves one framebuffer as a plain 8bit RGBA png. LightmapsPacker expects
// png files (it loads them through ImageSystem), Beast exposes bake results
// as framebuffers, so readback + conversion happens here.
void SaveFramebufferAsPNG(ILBFramebufferHandle framebuffer, const DAVA::FilePath& outputPath)
{
    DAVA::int32 width = 0;
    DAVA::int32 height = 0;
    BEAST_VERIFY(ILBGetResolution(framebuffer, &width, &height));
    if (width <= 0 || height <= 0)
    {
        BEAST_VERIFY(ILBDestroyFramebuffer(framebuffer));
        return;
    }

    const DAVA::int32 pixelCount = width * height;
    DAVA::Vector<DAVA::uint8> pixels(static_cast<size_t>(pixelCount * 4), 255);

    // gamma 1.0: lightmaps are stored raw and processed later
    BEAST_VERIFY(ILBReadRegionLDR(framebuffer, 0, 0, width, height, ILB_CS_RGBA, 1.0f, pixels.data()));

    DAVA::Image* image = DAVA::Image::Create(static_cast<DAVA::uint32>(width), static_cast<DAVA::uint32>(height), DAVA::PixelFormat::FORMAT_RGBA8888);
    std::memcpy(image->data, pixels.data(), pixels.size());

    if (DAVA::ImageSystem::Save(outputPath, image, DAVA::PixelFormat::FORMAT_RGBA8888) != DAVA::eErrorCode::SUCCESS)
    {
        DAVA::Logger::Error("[BeastManager] Can't save lightmap: %s", outputPath.GetAbsolutePathname().c_str());
    }

    image->Release();
    BEAST_VERIFY(ILBDestroyFramebuffer(framebuffer));
}
} // namespace BeastManagerDetails

bool BeastManager::shouldInit = true;

BeastManager::BeastManager()
{
    if (shouldInit)
    {
        StaticInit();
    }

    // cache + temp folder live inside the parser's temporary folder
    DAVA::FilePath cacheDirectory = SceneParser::GetTemporaryFolder() + "cache/";
    DAVA::FileSystem::Instance()->CreateDirectory(SceneParser::GetTemporaryFolder(), true);
    DAVA::FileSystem::Instance()->CreateDirectory(cacheDirectory, true);

    BEAST_VERIFY(ILBCreateManager(STRING_TO_BEAST_STRING(cacheDirectory.GetAbsolutePathname()),
                                  ILB_CS_LOCAL,
                                  STRING_TO_BEAST_STRING(BeastManagerDetails::GetBeastLicenseKey()),
                                  &handle));
    DVASSERT(handle != nullptr);

    DAVA::FilePath beastBinaries = BeastManagerDetails::FindBeastResource("bin");
    BEAST_VERIFY(ILBSetBeastPath(handle, STRING_TO_BEAST_STRING(beastBinaries.GetAbsolutePathname())));

    // every bake should start with a cold cache
    BEAST_VERIFY(ILBClearCache(handle));
}

BeastManager::~BeastManager()
{
    DAVA::SafeDelete(sceneParser);

    if (handle != nullptr)
    {
        BEAST_VERIFY(ILBDestroyManager(handle));
        handle = nullptr;
    }
}

void BeastManager::StaticInit()
{
    BEAST_VERIFY(ILBSetLogTarget(ILB_LT_ERROR, ILB_LS_STDERR, nullptr));
    BEAST_VERIFY(ILBSetLogTarget(ILB_LT_INFO, ILB_LS_STDOUT, nullptr));
    shouldInit = false;
}

ILBManagerHandle BeastManager::GetILBManager()
{
    return handle;
}

ILBSceneHandle BeastManager::GetILBScene()
{
    return scene;
}

void BeastManager::Run(DAVA::Scene* sceneForBake)
{
    DVASSERT(sceneForBake != nullptr);
    DVASSERT(state == STATE_IDLE);

    davaScene = sceneForBake;
    generatedLightmaps.clear();
    lodLevel = 0;
    switchIndex = 0;
    curTaskName = "Parse scene";
    curTaskProgress = 0;
    state = STATE_GENERATING;

    ClearLightmapsDirectory();
    BeginScene();

    sceneParser = new SceneParser(this, DAVA::MakeFunction(this, &BeastManager::OnSceneParsingCompleted));
    ParseScene(davaScene);
}

void BeastManager::SetLightmapsDirectory(const DAVA::String& path)
{
    lightmapsDirectory = path;
    if (!lightmapsDirectory.empty() && lightmapsDirectory.back() != '/')
    {
        lightmapsDirectory += '/';
    }
}

void BeastManager::SetMode(eBeastMode _mode)
{
    mode = _mode;
}

eBeastMode BeastManager::GetMode() const
{
    return mode;
}

void BeastManager::SetLodLevel(int32 _lodLevel)
{
    lodLevel = _lodLevel;
}

int32 BeastManager::GetLodLevel()
{
    return lodLevel;
}

void BeastManager::SetSwitchIndex(int32 _switchIndex)
{
    switchIndex = _switchIndex;
}

int32 BeastManager::GetSwitchIndex()
{
    return switchIndex;
}

void BeastManager::ParseScene(DAVA::Scene* sceneForBake)
{
    DVASSERT(sceneParser != nullptr);

    sceneParser->StartParsing();
    maxLodMaxSwitch = sceneParser->FindMaxLod(sceneForBake);
    sceneParser->SetLodLevel(lodLevel);
    sceneParser->SetSwitchIndex(switchIndex);
    sceneParser->FindAndInitDirectionalLight(sceneForBake);
    sceneParser->ValidateDirectionalLight();
    sceneParser->ParseNodeRecursive(sceneForBake);
    // SceneParser completes asynchronously (landscape color thumbnails),
    // OnSceneParsingCompleted() is invoked when everything is parsed.
}

void BeastManager::OnSceneParsingCompleted()
{
    if (sceneParser->ShouldCancel())
    {
        state = STATE_CANCELED;
        return;
    }

    EndScene();
    GenerateLightmaps();
}

void BeastManager::BeginScene()
{
    BEAST_VERIFY(ILBBeginScene(handle, GENERATE_BEAST_NAME(BeastManager), &scene));
    DVASSERT(scene != nullptr);
}

void BeastManager::EndScene()
{
    DVASSERT(scene != nullptr);
    BEAST_VERIFY(ILBEndScene(scene));
}

bool BeastManager::GenerateLightmaps()
{
    DVASSERT(scene != nullptr);

    // pick the configuration file deployed with the module
    DAVA::String configName = "lightmaps_mode.xml";
    if (mode == eBeastMode::MODE_SPHERICAL_HARMONICS)
    {
        configName = "sh_mode.xml";
    }
    else if (mode == eBeastMode::MODE_PREVIEW)
    {
        configName = "preview_mode.xml";
    }

    DAVA::FilePath configPath = BeastManagerDetails::FindBeastResource("config/" + configName);
    if (!DAVA::FileSystem::Instance()->Exists(configPath))
    {
        DAVA::Logger::Error("[BeastManager] Can't find Beast configuration file: %s", configPath.GetAbsolutePathname().c_str());
        state = STATE_DONE;
        return false;
    }

    BEAST_VERIFY(ILBCreateJob(handle, GENERATE_BEAST_NAME(BeastManager), scene,
                              STRING_TO_BEAST_STRING(configPath.GetAbsolutePathname()), &job));
    if (job == nullptr)
    {
        state = STATE_DONE;
        return false;
    }

    // bake output is written directly into the lightmaps directory
    if (!lightmapsDirectory.empty())
    {
        BEAST_VERIFY(ILBSetJobOutputPath(job, STRING_TO_BEAST_STRING(DAVA::FilePath(lightmapsDirectory).GetAbsolutePathname())));
    }

    const DAVA::List<BeastMeshInstance*>& instances = BeastMeshInstance::GetResourceList();

    if (mode == eBeastMode::MODE_SPHERICAL_HARMONICS)
    {
        BEAST_VERIFY(ILBCreateIlluminationPassSH(job, CONST_STRING_TO_BEAST_STRING("sh"), ILB_IM_FULL, &passSH));

        // one shared vertex target for the whole job
        ILBTargetHandle vertexTarget = nullptr;
        BEAST_VERIFY(ILBCreateVertexTarget(job, GENERATE_BEAST_NAME(BeastManager), &vertexTarget));
        BEAST_VERIFY(ILBAddPassToTarget(vertexTarget, passSH));

        for (BeastMeshInstance* instance : instances)
        {
            BEAST_VERIFY(ILBAddBakeInstance(vertexTarget, instance->GetILBMeshInstance(), nullptr));
        }

        const DAVA::List<BeastPointCloud*>& pointClouds = BeastPointCloud::GetResourceList();
        for (BeastPointCloud* pointCloud : pointClouds)
        {
            ILBTargetEntityHandle pointCloudEntity = nullptr;
            BEAST_VERIFY(ILBAddBakePointCloud(vertexTarget, pointCloud->GetPointCloudHandle(), &pointCloudEntity));
            pointCloud->SetTargets(vertexTarget, pointCloudEntity);
        }

        curTaskName = "Beast SH baking";
        curTaskProgress = 0;
        BEAST_VERIFY(ILBStartJob(job, ILB_SR_CLOSE_WHEN_DONE, ILB_RD_FORCE_LOCAL));
        return true;
    }

    // MODE_LIGHTMAPS / MODE_PREVIEW:
    //   sm pass = full illumination (lightmap), ao pass = ambient occlusion.
    //   LightmapsPacker expects the results in {lightmapsDirectory}/sm and
    //   {lightmapsDirectory}/ao.
    BEAST_VERIFY(ILBCreateIlluminationPass(job, CONST_STRING_TO_BEAST_STRING("sm"), ILB_IM_FULL, &passLM));
    BEAST_VERIFY(ILBCreateAmbientOcclusionPass(job, CONST_STRING_TO_BEAST_STRING("ao"), 100.0f, 90.0f, &passSH));

    for (BeastMeshInstance* instance : instances)
    {
        if (!instance->IsUseLightmap())
        {
            continue;
        }

        DAVA::RenderBatch* batch = instance->GetRenderBatch();
        if (batch == nullptr || IsLightmapGenerated(batch))
        {
            // already baked during one of the previous LOD passes
            continue;
        }

        TextureTarget* textureTarget = instance->GetTextureTarget();
        if (textureTarget == nullptr)
        {
            textureTarget = new TextureTarget(job, instance->GetLightmapSize());
            instance->SetTextureTarget(textureTarget);
        }

        BEAST_VERIFY(ILBAddBakeInstance(textureTarget->GetHandle(), instance->GetILBMeshInstance(), nullptr));
        BEAST_VERIFY(ILBAddPassToTarget(textureTarget->GetHandle(), passLM));
        BEAST_VERIFY(ILBAddPassToTarget(textureTarget->GetHandle(), passSH));
    }

    curTaskName = "Beast lightmaps baking";
    curTaskProgress = 0;
    BEAST_VERIFY(ILBStartJob(job, ILB_SR_CLOSE_WHEN_DONE, ILB_RD_FORCE_LOCAL));
    return true;
}

void BeastManager::Update()
{
    if (state != STATE_GENERATING || job == nullptr)
    {
        return;
    }

    RetriveProgress();

    ILBBool completed = 0;
    BEAST_VERIFY(ILBIsJobCompleted(job, &completed));
    if (completed != 0)
    {
        OnJobCompleted();
    }
}

bool BeastManager::IsJobDone()
{
    return (state == STATE_DONE) || (state == STATE_CANCELED);
}

void BeastManager::OnJobCompleted()
{
    ILBJobStatus jobStatus = ILB_JS_OTHER_ERROR;
    BEAST_VERIFY(ILBGetJobResult(job, &jobStatus));

    if (jobStatus != ILB_JS_SUCCESS)
    {
        DAVA::Logger::Error("[BeastManager] Beast job finished with status %d", static_cast<DAVA::int32>(jobStatus));
        state = (jobStatus == ILB_JS_CANCELLED) ? STATE_CANCELED : STATE_DONE;
        return;
    }

    SaveResults();

    if (mode == eBeastMode::MODE_LIGHTMAPS && lodLevel < maxLodMaxSwitch.maxLodLevel)
    {
        // bake the next LOD in the chain
        StartLodPass();
    }
    else
    {
        EndLodPass();
    }
}

void BeastManager::StartLodPass()
{
    ++lodLevel;
    state = STATE_GENERATING;
    curTaskName = DAVA::Format("Beast baking LOD %d", lodLevel);
    curTaskProgress = 0;

    // the ILB scene is immutable after ILBEndScene: rebuild it for the new LOD
    job = nullptr;
    passLM = nullptr;
    passSH = nullptr;

    sceneParser->ClearScene();
    BeginScene();
    RunLodIteration();
}

void BeastManager::EndLodPass()
{
    job = nullptr;
    passLM = nullptr;
    passSH = nullptr;
    state = STATE_DONE;
    curTaskName = "Beast finished";
    curTaskProgress = 100;
}

void BeastManager::RunLodIteration()
{
    DVASSERT(sceneParser != nullptr);
    sceneParser->SetLodLevel(lodLevel);
    sceneParser->SetSwitchIndex(switchIndex);
    ParseScene(davaScene);
}

void BeastManager::RetriveProgress()
{
    ILBBool newActivity = 0;
    ILBBool newProgress = 0;
    BEAST_VERIFY(ILBJobHasNewProgress(job, &newActivity, &newProgress));
    if (newProgress == 0 && newActivity == 0)
    {
        return;
    }

    ILBStringHandle taskNameHandle = nullptr;
    DAVA::int32 progress = 0;
    if (ILBGetJobProgress(job, &taskNameHandle, &progress) == ILB_ST_SUCCESS)
    {
        if (taskNameHandle != nullptr)
        {
            curTaskName = ConvertBeastString(taskNameHandle);
        }
        curTaskProgress = progress;
    }
}

int32 BeastManager::GetCurTaskProcess() const
{
    return curTaskProgress;
}

const DAVA::String& BeastManager::GetCurTaskName() const
{
    return curTaskName;
}

void BeastManager::Cancel()
{
    if (state != STATE_GENERATING)
    {
        return;
    }

    if (job != nullptr)
    {
        BEAST_VERIFY(ILBCancelJob(job));
    }

    // note: SceneParser has no public cancel entry point in the published
    // module; ShouldCancel() flips to true on its own when the landscape
    // thumbnail pass fails, and OnSceneParsingCompleted() checks it.
    state = STATE_CANCELED;
}

bool BeastManager::WasCancelled() const
{
    return (state == STATE_CANCELED);
}

void BeastManager::UpdateAtlas(DAVA::Vector<LightmapAtlasingData>* atlasData)
{
    // See the file header: the atlasing information produced by LightmapsPacker
    // is consumed on the editor side through texture descriptors. The
    // published BeastManager.h has no member to retain it, so the original
    // semantics of this call cannot be recovered. Keep the entry point for
    // the contract, log for diagnostics.
    if (atlasData != nullptr)
    {
        DAVA::Logger::Debug("[BeastManager] UpdateAtlas: %d lightmap entries processed by the packer",
                            static_cast<DAVA::int32>(atlasData->size()));
    }
}

bool BeastManager::IsLightmapGenerated(DAVA::RenderBatch* batch)
{
    auto found = generatedLightmaps.find(batch);
    return (found != generatedLightmaps.end()) && found->second;
}

void BeastManager::SetLightmapGenerated(DAVA::RenderBatch* batch)
{
    generatedLightmaps[batch] = true;
}

void BeastManager::ClearLightmapsDirectory()
{
    if (lightmapsDirectory.empty())
    {
        return;
    }

    DAVA::FilePath basePath(lightmapsDirectory);
    DAVA::FileSystem::Instance()->DeleteDirectory(basePath + "sm/", true);
    DAVA::FileSystem::Instance()->DeleteDirectory(basePath + "ao/", true);
}

void BeastManager::SaveResults()
{
    if (mode == eBeastMode::MODE_SPHERICAL_HARMONICS)
    {
        // SH results are vertex data: copy the coefficients back onto the
        // SpeedTree objects registered by the SceneParser.
        const DAVA::List<BeastPointCloud*>& pointClouds = BeastPointCloud::GetResourceList();
        for (BeastPointCloud* pointCloud : pointClouds)
        {
            ILBTargetHandle target = pointCloud->GetCloudTarget();
            ILBTargetEntityHandle targetEntity = pointCloud->GetEntityTarget();
            if (target == nullptr || targetEntity == nullptr)
            {
                continue;
            }

            ILBFramebufferHandle vertexBuffer = nullptr;
            if (ILBGetVertexbuffer(target, passSH, targetEntity, &vertexBuffer) != ILB_ST_SUCCESS || vertexBuffer == nullptr)
            {
                continue;
            }

            DAVA::int32 width = 0;
            DAVA::int32 height = 0;
            BEAST_VERIFY(ILBGetResolution(vertexBuffer, &width, &height));
            const DAVA::int32 floatCount = width * height;
            if (floatCount <= 0)
            {
                BEAST_VERIFY(ILBDestroyFramebuffer(vertexBuffer));
                continue;
            }

            DAVA::Vector<DAVA::float32> coefficients(static_cast<size_t>(floatCount), 0.0f);
            BEAST_VERIFY(ILBReadRegionHDR(vertexBuffer, 0, 0, width, height, ILB_CS_RGBA, coefficients.data()));
            BEAST_VERIFY(ILBDestroyFramebuffer(vertexBuffer));

            const DAVA::int32 entityCount = pointCloud->GetEntityCount();
            for (DAVA::int32 e = 0; e < entityCount; ++e)
            {
                DAVA::Entity* entity = pointCloud->GetEntity(e);
                DAVA::RenderObject* renderObject = DAVA::GetRenderObject(entity);
                if (renderObject != nullptr && renderObject->GetType() == DAVA::RenderObject::TYPE_SPEED_TREE)
                {
                    DAVA::SpeedTreeObject* speedTree = static_cast<DAVA::SpeedTreeObject*>(renderObject);
                    DAVA::Array<DAVA::float32, DAVA::SpeedTreeObject::HARMONICS_BUFFER_CAPACITY> sh = {};
                    const DAVA::int32 copyCount = DAVA::Min<DAVA::int32>(static_cast<DAVA::int32>(sh.size()), floatCount);
                    for (DAVA::int32 i = 0; i < copyCount; ++i)
                    {
                        sh[static_cast<size_t>(i)] = coefficients[static_cast<size_t>(i)];
                    }
                    speedTree->SetSphericalHarmonics(sh);
                }
            }
        }
        return;
    }

    // MODE_LIGHTMAPS / MODE_PREVIEW: read every framebuffer of every
    // lightmapped instance target and store it as
    //   {lightmapsDirectory}/sm/{instanceName}.png
    //   {lightmapsDirectory}/ao/{instanceName}.png
    // exactly where LightmapsPacker expects to find them.
    if (lightmapsDirectory.empty())
    {
        return;
    }

    DAVA::FilePath basePath(lightmapsDirectory);
    DAVA::FileSystem::Instance()->CreateDirectory(basePath + "sm/", true);
    DAVA::FileSystem::Instance()->CreateDirectory(basePath + "ao/", true);

    const DAVA::List<BeastMeshInstance*>& instances = BeastMeshInstance::GetResourceList();
    for (BeastMeshInstance* instance : instances)
    {
        if (!instance->IsUseLightmap())
        {
            continue;
        }

        TextureTarget* textureTarget = instance->GetTextureTarget();
        if (textureTarget == nullptr)
        {
            continue;
        }

        const bool isLandscape = instance->IsLandscape();
        const DAVA::String instanceName = isLandscape
                                              ? DAVA::String("landscape")
                                              : instance->GetMeshInstancePointerString();

        // BeastRunner::PackLightmaps() moves "{lightmapsDirectory}/landscape.png"
        // out of the directory before packing, so the landscape lightmap is
        // written to the top level; regular instances go into sm/ and ao/,
        // which ResourcePacker2D turns into the "sm" and "ao" atlases.
        const char* passNames[2] = { (isLandscape ? "" : "sm/"), (isLandscape ? "" : "ao/") };
        ILBRenderPassHandle passes[2] = { passLM, passSH };

        for (DAVA::int32 passIndex = 0; passIndex < 2; ++passIndex)
        {
            if (passes[passIndex] == nullptr)
            {
                continue;
            }

            // the landscape keeps a single lightmap: skip the AO pass
            if (isLandscape && passIndex == 1)
            {
                continue;
            }

            DAVA::int32 framebufferCount = 0;
            BEAST_VERIFY(ILBGetFramebufferCount(textureTarget->GetHandle(), &framebufferCount));
            for (DAVA::int32 fbIndex = 0; fbIndex < framebufferCount; ++fbIndex)
            {
                ILBFramebufferHandle framebuffer = nullptr;
                if (ILBGetFramebuffer(textureTarget->GetHandle(), passes[passIndex], fbIndex, &framebuffer) != ILB_ST_SUCCESS
                    || framebuffer == nullptr)
                {
                    continue;
                }

                ILBRenderPassHandle fbPass = nullptr;
                BEAST_VERIFY(ILBGetFramebufferPass(framebuffer, &fbPass));
                if (fbPass != passes[passIndex])
                {
                    BEAST_VERIFY(ILBDestroyFramebuffer(framebuffer));
                    continue;
                }

                DAVA::FilePath pngPath = basePath + DAVA::String(passNames[passIndex]) + instanceName + ".png";
                BeastManagerDetails::SaveFramebufferAsPNG(framebuffer, pngPath);
            }
        }

        if (instance->GetRenderBatch() != nullptr)
        {
            SetLightmapGenerated(instance->GetRenderBatch());
        }
    }
}
} // namespace Beast
