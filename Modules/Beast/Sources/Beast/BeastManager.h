#pragma once

#include "Beast/BeastTypes.h"
#include "Beast/BeastNames.h"
#include "Beast/BeastConstants.h"
#include "Beast/SceneParser.h"

#include <Base/BaseTypes.h>
#include <Math/Matrix4.h>

namespace DAVA
{
class Scene;
class RenderBatch;
class NMaterial;
class Texture;
}

namespace Beast
{
class BeastMeshInstance;
struct LightmapAtlasingData;

class BeastMaterial;
class BeastManager
{
public:
    DECLARE_BEAST_NAME(BeastManager);

    BeastManager();
    virtual ~BeastManager();

    // Readiness is checked once at construction: the Beast manager, the beast
    // binary path and the manager itself are what every later ILB call needs.
    // When this is false the bake is refused cleanly instead of asserting on a
    // null ILB scene handle.
    bool IsReady() const;
    const DAVA::String& GetInitializationError() const;

    void Run(DAVA::Scene* davaScene);

    void Update();
    bool IsJobDone();
    void UpdateAtlas(DAVA::Vector<LightmapAtlasingData>* atlasData);

    void SetLightmapsDirectory(const DAVA::String& path);

    // Resume. A lightmap target whose sm/ page already sits in the lightmaps directory is
    // left unbaked on the next run; the geometry still goes into the scene, so shadows and
    // bounce from already finished objects stay correct. Switched on by a "resume.flag"
    // file in the lightmaps directory (that file is not touched by the per-pass clean).
    bool IsResumingExistingBakes() const;
    // The rectangle a batch actually occupies inside its own UV set, and which set that is.
    // A tile is only worth its texels when the bake and the material agree on this rectangle.
    struct LightmapUVRect
    {
        DAVA::Vector2 offset{ 0.0f, 0.0f };
        DAVA::Vector2 size{ 1.0f, 1.0f };
        DAVA::int32 uvSet = 1;
    };
    void RegisterLightmapTarget(const DAVA::String& instanceKey, const DAVA::String& stableKey, DAVA::RenderBatch* batch);

    // Which part of which UV set this batch occupies, measured by the parser. Used twice: to fit the
    // shape into its tile when baking (ILBSetUVTransform) and to write the same mapping into the
    // material when the atlas is bound.
    void RegisterLightmapUVRect(DAVA::RenderBatch* batch, const DAVA::Vector2& uvOffset, const DAVA::Vector2& uvSize, DAVA::int32 uvSet);

    // Writes the final atlas UVs into the batch's lightmap UV set. The engine's lightmap shader only
    // honours uvScale/uvOffset when the material lacks the SETUP_LIGHTMAP flag, and these maps carry
    // it, so a material transform is silently ignored and any model whose lightmap UVs are not in
    // [0,1] (the rocks span -1..1) samples outside its tile and comes back black.
    void RewriteLightmapUVs(DAVA::RenderBatch* batch, const LightmapAtlasingData& entry);
    DAVA::String GetLightmapStableKey(const DAVA::String& instanceKey) const;
    bool HasBakedResult(const DAVA::String& stableKey) const;
    bool HasBakedLandscapeResult() const;
    void ParseScene(DAVA::Scene* davaScene);
    bool GenerateLightmaps();
    void SetMode(eBeastMode mode);
    eBeastMode GetMode() const;

    void SetLodLevel(int32 lodLevel);
    int32 GetLodLevel();
    void SetSwitchIndex(int32 switchIndex);
    int32 GetSwitchIndex();

    ILBManagerHandle GetILBManager();
    ILBSceneHandle GetILBScene();

    void OnJobCompleted();

    int32 GetCurTaskProcess() const;
    const DAVA::String& GetCurTaskName() const;

    void Cancel();
    bool WasCancelled() const;

    // true when the job stopped without a usable result. Callers must keep the
    // render output in that case: it is the only evidence of the failure.
    bool HasFailed() const;

    // true once the lightmap passes actually wrote something, so the caller can
    // skip packing an empty render directory
    bool HasProducedOutput() const;

    // releases Beast's own log file handles (its log lands next to the render output)
    void DisableLogTargets();

    // Skip decoding every albedo for Beast. Only useful for measuring start-up.
    void SetDumpTextures(bool value);
    // Longest side of the albedo handed to Beast; 0 keeps the shipped size.
    void SetTextureMaxSize(DAVA::int32 size);

    // When on, a batch is given a lightmap only if its material says so
    // (FLAG_ILLUMINATION_USED) and the per-material lightmapSize is used as is.
    // Shipped Blitz scenes strip those flags on export, so this is off by default
    // and every batch with geometry is baked instead.
    // Bake cost presets. Beast's own defaults are fgRays 600 with fgInterpolationPoints 1
    // (interpolation off), MinSamples 20 / MaxSamples 32768 and a variance limit of 0.001,
    // which means one bad texel can be traced thirty thousand times. Only the sample cap,
    // the variance and the GI depth are reachable from the API; fgRays, the interpolation
    // points and the AA contrast are derived by Beast and cannot be set.
    enum class eBakeQuality : DAVA::int32
    {
        BeastDefault, // touch nothing
        Test, // MaxSamples 256, variance 0.05, 1 GI depth - layout and UV checks only
        Fast, // MaxSamples 512, variance 0.02, 1 GI depth
        Balanced, // MaxSamples 2048, variance 0.005, 2 GI depths
        Clean // MaxSamples 8192, variance 0.001, 3 GI depths
    };
    void SetBakeQuality(eBakeQuality quality);
    eBakeQuality GetBakeQuality() const;

    // Which mesh UV layer Beast bakes into a target. The mesh upload names its layers after
    // the DAVA texture coord index ("0" = diffuse, "1" = the lightmap set), and Beast defaults
    // to the first one it finds, which is why every tile so far had its chart as a sliver in a
    // corner: the pixels were spent on the diffuse layout, while the material samples the atlas
    // through the lightmap UVs. "1" is the correct default; "0" stays reachable for meshes that
    // carry a single UV set.
    void SetBakeUVSetName(const DAVA::String& uvName);
    DAVA::String GetBakeUVSetName() const;

    // Which batches become lightmap targets, and where their size comes from. This
    // replaces the old "use per-material illumination" checkbox, which could only
    // express two of the three useful policies.
    enum class eLightmapSelection : DAVA::int32
    {
        All, // every batch with geometry, sized from the map's own rect when it has one
        AutoOnly, // every batch with geometry, stored rect ignored, size recalculated
        Flagged // only materials whose illumination flag is set
    };
    void SetLightmapSelection(eLightmapSelection selection);
    eLightmapSelection GetLightmapSelection() const;
    // False in AutoOnly, where the size must come from geometry instead of the scene.
    bool UseStoredLightmapRects() const;

    // Kept for the existing call sites: Flagged on, All off.
    void SetUseMaterialIllumination(bool use);
    bool UseMaterialIllumination() const;

private:
    static bool shouldInit;

    // Targets submitted across every LOD pass of the current bake. A later pass
    // that finds nothing new is the end of the chain, not a failure.
    DAVA::int32 bakedTargetsTotal = 0;

    eLightmapSelection lightmapSelection = eLightmapSelection::All;
    // Fast by default: 32768 samples at variance 0.001 lets one bad texel per tile decide the
    // whole bake, which is what turned a map into a 45 minute job. Test/Fast/Balanced/Clean stay
    // available through SetBakeQuality.
    eBakeQuality bakeQuality = eBakeQuality::Fast;
    DAVA::String bakeUVSetName = "1";
    DAVA::Vector<ILBTargetEntityHandle> bakeTargetEntities; // kept between ILBAddBakeInstance and ILBStartJob
    DAVA::Vector<BeastMeshInstance*> bakeTargetInstances; // parallel to bakeTargetEntities
    DAVA::Map<DAVA::RenderBatch*, LightmapUVRect> lightmapUVRects;
    DAVA::Set<DAVA::RenderBatch*> uvRewritten;
    DAVA::int32 uvRewrittenCount = 0;
    DAVA::int32 zeroSizedTargets = 0;
    DAVA::int32 uvRectLogged = 0;
    bool sampleCapRefused = false;
    void ApplyBakeRenderSettings(ILBJobHandle bakeJob);
    bool ConfigureTargetEntity(ILBTargetEntityHandle targetEntity, BeastMeshInstance* targetInstance);
    void ReportBakeTargetConfiguration();
    DAVA::int32 bakeUVSetApplied = 0;
    DAVA::int32 bakeUVSetRefused = 0;
    DAVA::int32 bakeUVRectFitted = 0;
    DAVA::int32 bakeUVRectRefused = 0;
    DAVA::int64 parseStartTime = 0;
    bool dumpTextures = true;
    DAVA::int32 textureMaxSize = 512; // targets are picked from the camera frustum, nearest first

    static void StaticInit();

    bool BeginScene();
    void EndScene();
    void RetriveProgress();
    void SaveResults();
    void ClearLightmapsDirectory();
    void StartLodPass();
    void EndLodPass();
    void RunLodIteration();
    bool IsLightmapGenerated(DAVA::RenderBatch* batch);
    void SetLightmapGenerated(DAVA::RenderBatch* batch);

    void OnSceneParsingCompleted();

    enum eState : DAVA::uint32
    {
        STATE_IDLE = 0,
        STATE_GENERATING,
        STATE_DONE,
        STATE_CANCELED,
        // The job stopped without ever reporting a usable result: crash, dead
        // handle, or a result query that never answered. Kept separate from
        // STATE_DONE so the runner preserves the render output instead of
        // packing an empty directory and deleting the evidence.
        STATE_FAILED
    };

private:
    ILBManagerHandle handle = nullptr;
    ILBSceneHandle scene = nullptr;
    ILBJobHandle job = nullptr;
    ILBRenderPassHandle passLM = nullptr;
    ILBRenderPassHandle passSH = nullptr;

    MaxLodMaxSwitch maxLodMaxSwitch;
    eBeastMode mode = eBeastMode::MODE_LIGHTMAPS;
    SceneParser* sceneParser = nullptr;

    eState state = STATE_IDLE;

    bool ready = false;
    DAVA::String initializationError;

    DAVA::Scene* davaScene = nullptr;
    DAVA::Map<DAVA::RenderBatch*, bool> generatedLightmaps;
    // UpdateAtlas gives every lightmapped batch its own material instance carrying the
    // atlas tile transform; these keep the instances and the loaded pages alive for the
    // lifetime of the session (one page entry per atlas file, not per instance)
    bool resumeExistingBakes = false;
    // mutable: the count is bookkeeping inside the const HasBakedResult() query, which the
    // parser calls per instance
    mutable DAVA::int32 resumeSkippedTargets = 0;
    // descriptor name (stable) -> batch, and the session key (pointer string) -> descriptor
    // name, so a packed atlas can be resolved without pointer strings
    DAVA::UnorderedMap<DAVA::String, DAVA::String> lightmapStableKeys;
    DAVA::UnorderedMap<DAVA::String, DAVA::RenderBatch*> lightmapKeyToBatch;
    DAVA::Vector<DAVA::NMaterial*> appliedMaterialInstances;
    DAVA::UnorderedMap<DAVA::String, DAVA::Texture*> atlasTextures;
    DAVA::String curTaskName;
    DAVA::String lightmapsDirectory;
    DAVA::int32 lodLevel = 0;
    DAVA::int32 switchIndex = 0;
    DAVA::int32 curTaskProgress = 0;
    DAVA::int32 lastLoggedProgress = -1;
    DAVA::int32 notRunningTicks = 0;
    // set once per pass when ILBIsJobCompleted flips, so the long "baked, still
    // collecting" stretch is explained once instead of silently looked stuck
    bool completionLogged = false;
    // deadline for retrying ILBGetJobResult after the completion flag flipped
    DAVA::int64 resultQueryDeadlineMs = 0;
    bool producedOutput = false;
    bool logTargetsActive = false;
    DAVA::String beastErrorLogPath;
    DAVA::String beastInfoLogPath;
};
}
