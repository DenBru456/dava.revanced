#pragma once

#include <Base/BaseTypes.h>
#include <Base/Set.h>
#include <Functional/Function.h>
#include <FileSystem/FilePath.h>
#include <Math/Vector.h>
#include <Math/Matrix4.h>
#include <Render/Highlevel/LandscapeThumbnails.h>

namespace DAVA
{
class Entity;
class RenderBatch;
class RenderObject;
class RenderComponent;
class NMaterial;
class Landscape;
class Texture;
class Image;
class LightComponent;
}

namespace Beast
{
struct MaxLodMaxSwitch
{
    DAVA::int32 maxLodLevel = 0;
    DAVA::int32 maxSwitchIndex = 0;
};

class BeastMaterial;
class BeastManager;
class SceneParser
{
public:
    // Decoding a shipped texture and writing it out again costs real time on a big
    // map, so it can be switched off while diagnosing something else.
    // 0 keeps the shipped resolution. 512 quarters the PNG that Beast then has to
    // decode on every session start.
    void SetTextureMaxSize(DAVA::int32 size)
    {
        textureMaxSize = size;
    }

    void SetDumpTextures(bool value)
    {
        dumpTextures = value;
    }

    SceneParser(BeastManager* beastManager, DAVA::Function<void()> parsingCompletedCallback);
    ~SceneParser();

    // How many light sources reached Beast; 0 means the session would bake black.
    DAVA::int32 GetLightsCreated() const;

    MaxLodMaxSwitch FindMaxLod(DAVA::Entity* node);

    void StartParsing();

    // Called periodically from the node walk. The editor installs a Qt event pump here
    // so the window keeps painting instead of being flagged as not responding.
    void SetEventPump(DAVA::Function<void()> pump);

    void FindAndInitDirectionalLight(DAVA::Entity* node);
    void ValidateDirectionalLight();
    void ParseNodeRecursive(DAVA::Entity* node, DAVA::uint32 depth = 0);
    void ClearScene();
    void SetLodLevel(DAVA::int32 _lodLevel);
    void SetSwitchIndex(DAVA::int32 _switchIndex);

    bool ShouldCancel() const;

    static DAVA::String ConstructMeshInstancePartName(DAVA::RenderBatch* batch, DAVA::int32 index);
    static bool IsMaterialTemplateContainString(DAVA::NMaterial* material, const DAVA::String& str);
    static DAVA::FilePath GetTemporaryFolder();
    static DAVA::FilePath GetLandscapeTemporaryFileName();
    bool UploadLandscapeColorMap(DAVA::Landscape* landscape);
    DAVA::Image* CompositeLandscapeAlbedo(DAVA::Landscape* landscape, const DAVA::Image* colorMap);
    static bool IsWaterLikeMaterial(DAVA::NMaterial* material, const DAVA::Entity* entity);
    void ReportSkippedWater();

private:
    void ParseNodeMaterials(DAVA::RenderComponent* renderComponent);
    void ParseNodeMeshes(DAVA::RenderComponent* renderComponent);
    void ParseLandscapeNode(DAVA::Entity* node, DAVA::Landscape* landscape);
    void ParseLightComponent(DAVA::LightComponent* lightComponent);
    void FindMaxLodRecursive(DAVA::Entity* node, DAVA::Texture* pinkTexture);

    void ParseLandscapeMaterial(DAVA::Landscape* node);
    void OnLandscapeImageCaptured(DAVA::Landscape* landscape, DAVA::Texture* texture);
    void ContinueParsingLandscape(DAVA::Entity* node, DAVA::Landscape* landscape);

    void FinishParsingLandscape();
    void CheckIfParsingCompleted();

    // Why a render batch did not become a Beast mesh instance. Bounded: a big map
    // has thousands of batches and the point is to explain, not to flood.
    void LogBatchSkipped(DAVA::int32 batchIndex, const char* reason, DAVA::NMaterial* material);
    void LogSkipSummary();

    DAVA::int32 FindDetailedSpeedTreeLOD(DAVA::RenderObject*) const;
    DAVA::Matrix4 BuildLeafTransformMatrix(const DAVA::Matrix4& entityInverseTransform);

private:
    BeastManager* beastManager = nullptr;
    BeastMaterial* landscapeMaterial = nullptr;
    MaxLodMaxSwitch maxLodMaxSwitch;
    DAVA::Entity* landscapeNode = nullptr;
    DAVA::Entity* directionalLightEntity = nullptr;
    DAVA::Function<void()> sceneParsingCompleteCallback;
    DAVA::Function<void()> eventPump;
    DAVA::uint32 nodesSinceEventPump = 0;
    DAVA::int32 lodLevel = 0;
    DAVA::int32 switchIndex = 0;
    DAVA::Vector3 mainLightDirection;
    bool landscapeTextureProcessed = false;
    DAVA::int32 dumpedTextureCount = 0;
    bool dumpTextures = true;
    bool landscapeColorReady = false;
    bool compositeLandscapeTiles = true;
    DAVA::uint32 skippedWaterBatches = 0;

    // Every NMaterial that actually reached Beast. A mesh whose material group names
    // a material that was never defined makes Beast abandon the whole scene: "Material
    // <hash> not defined when loading shape ...", then "Failed to load scene meshes!".
    // One water sheet did exactly that to 1000 meshes, so the mesh walk now asks.
    DAVA::Set<DAVA::NMaterial*> uploadedMaterials;
    DAVA::uint32 materialsSinceEventPump = 0;
    DAVA::uint32 materialsWithoutAlbedo = 0;
    DAVA::uint32 skippedWithoutMaterial = 0;
    DAVA::int32 textureMaxSize = 512;
    bool entitiesParsed = false;
    bool shouldCancel = false;
    DAVA::int32 skippedBatchCount = 0;
    DAVA::int32 loggedSkipCount = 0;
    DAVA::int32 acceptedBatchCount = 0;
    DAVA::int32 loggedUnlitBatchCount = 0;
    DAVA::int32 loggedLightCount = 0;
    DAVA::int32 lightsCreated = 0;

    DAVA::LandscapeThumbnails::RequestID thumbnailsRequestId = DAVA::LandscapeThumbnails::InvalidID;
};
}