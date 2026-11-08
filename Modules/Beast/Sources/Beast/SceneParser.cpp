#include "Beast/SceneParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include "Beast/BeastMaterial.h"
#include "Beast/BeastTexture.h"
#include "Beast/BeastMesh.h"
#include "Beast/BeastMeshInstance.h"
#include "Beast/LandscapeGeometry.h"
#include "Beast/BeastLight.h"
#include "Beast/BeastDebug.h"
#include "Beast/BeastNames.h"
#include "Beast/BeastPointCloud.h"

#include <Render/Highlevel/Landscape.h>
#include <Render/Material/NMaterialNames.h>
#include <Scene3D/Components/ComponentHelpers.h>
#include <Scene3D/Components/RenderComponent.h>
#include <Scene3D/Components/LightComponent.h>
#include <Scene3D/Entity.h>
#include <Render/Highlevel/RenderObject.h>
#include <Render/Image/ImageSystem.h>
#include <Render/Image/ImageConvert.h>
#include <Render/TextureDescriptor.h>
#include <FileSystem/FileSystem.h>
#include <Logger/Logger.h>
#include <Scene3D/Components/TransformComponent.h>
#include <Math/TransformUtils.h>

// A shipped .sc2 has illumination.lightmapSize stripped (Scene::OptimizeBeforeExport),
// so NMaterial hands back DEFAULT_LIGHTMAP_SIZE (16) and every lightmap target came out
// 16x16: no light gradient, no shadow, one flat wash per object. The lightmaps that ship
// with the real Rudniki map (rudniki.sc2_lightmaps: 2 x 2048 + 1 x 1024 DXT1 pages for
// ~1016 lightmapped objects) work out to roughly 96x96 per object, and the terrain
// lightmap ships as landscape/wwLm at 512x512. These are the floors we bake with when
// the material does not say anything.
static const DAVA::int32 BEAST_DEFAULT_MESH_LIGHTMAP_SIZE = 64;
static const DAVA::int32 BEAST_DEFAULT_LANDSCAPE_LIGHTMAP_SIZE = 512;

// Sizing from geometry, the way the shipped atlas is actually organised: the real Rudniki
// pages hand 512 px to the biggest objects and 16 px to barrell-sized props. One fixed size
// for everything is what made a 30 m wall and a crate share 64 px, which is mush on the wall
// and pure waste on the crate. Pick the rung of the ladder that keeps texel density roughly
// constant instead, and let an explicit material value win over it.
static const DAVA::int32 BEAST_LIGHTMAP_SIZE_LADDER[] = { 16, 32, 64, 128, 256, 512 };
// 9 texels per metre. Measured against the shipped atlas after a full bake at 12: ours came out
// 512 x22 / 256 x35 / 128 x230 / 64 x472 / 32 x98 / 16 x0 = 13.86M texels over 4 pages, while
// the map ships 512 x12 / 256 x37 / 128 x90 / 64 x421 / 32 x293 / 16 x159 = 9.11M over 3. The
// surplus is all in the middle of the ladder - 140 extra 128 px tiles and not one 16 px tile -
// which is a density that is too high, not a packer that is too loose (page fill was 96/94/94).
// 9 texels per metre brings 13.86M down to about 7.8M and lets small props reach 16 px.
static const DAVA::float32 BEAST_LIGHTMAP_TEXELS_PER_METER = 9.0f;

// How hot the bake runs. Beast reads the light level from the "editor.intensity" custom property
// (BeastLight falls back to 1.0), NOT from Light::GetIntensity, so the 300 in the scene is not what
// gets baked - 1.0 is, and a sun-facing wall then bakes to pure white. The engine's material
// doubles the lightmap when it samples it (materials-fp.sl), so a fully lit surface is supposed to
// land near 0.5, and the shipped Blitz pages do. With 1.0 the last run clipped: p95 255 and 3.8
// percent of the atlas above 250, which is exactly the "one side is blinding" look.
static const DAVA::float32 BEAST_BAKE_SUN_INTENSITY = 0.5f;
// shadows are filled by the sky light (DAVA type 3). If it is baked at the same level as the sun
// there is no fill at all and every surface facing away goes black, which is what the rocks do.
static const DAVA::float32 BEAST_BAKE_SKY_INTENSITY = 0.75f;

// Authoring channel for the two things a shipped scene cannot express. The exporter clears
// flag BITS rather than removing them (all 2854 material records in the Rudniki scene carry a
// "flags" word, and the whole population collapses to 0x20 and 0x08), and NMaterial synthesises
// PARAM_LIGHTMAP_SIZE = 16 whenever the illumination bit is set. So "artist picked 16" and
// "nothing was ever set" are the same value on both channels. A plain custom material property
// is not touched by OptimizeBeforeExport, so it is the only place an explicit answer survives:
//   beast.lightmapSize  float, present means use it verbatim (16 allowed), absent means auto
//   beast.illumination  float, present means 1 bakes and 0 does not, absent means auto
// constructed at call time on purpose: a file-scope FastName runs before the global name
// table is ready, which is a startup-order trap the engine avoids the same way.
static DAVA::FastName GetArtistLightmapSizeProp()
{
    return DAVA::FastName("beast.lightmapSize");
}

static DAVA::FastName GetArtistIlluminationProp()
{
    return DAVA::FastName("beast.illumination");
}

static DAVA::int32 SnapToLightmapLadder(DAVA::int32 size)
{
    const DAVA::int32 ladderSize = static_cast<DAVA::int32>(sizeof(BEAST_LIGHTMAP_SIZE_LADDER) / sizeof(BEAST_LIGHTMAP_SIZE_LADDER[0]));
    for (DAVA::int32 i = 0; i < ladderSize; ++i)
    {
        if (size <= BEAST_LIGHTMAP_SIZE_LADDER[i])
        {
            return BEAST_LIGHTMAP_SIZE_LADDER[i];
        }
    }
    return BEAST_LIGHTMAP_SIZE_LADDER[ladderSize - 1];
}

// The scene carries the original author's lightmap rect and I had been ignoring it: uvScale is
// tileSize/pageWidth and uvOffset is rectOrigin/pageWidth, both present on every one of the 1017
// materials in the shipped Rudniki scene. Decoded against a 2048 page they give 0.015625 .. 0.25,
// i.e. 32 .. 512 texels with the distribution 159/607/200/38/12, which is the same histogram the
// packed atlas shows. That is the exact size the map was baked at, per material, no guessing.
static const DAVA::float32 BEAST_SHIPPED_LIGHTMAP_PAGE = 2048.0f;

// uvScale is tileSize/pageWidth and uvOffset is rectOrigin/pageWidth, so the page width is half
// of the answer, and the scene stores it: every lightmapped material in the shipped Rudniki scene
// points its "lightmap" slot at one of three shipped atlas pages
// (rudniki.sc2_lightmaps/texture1..3.tex, referenced by 28 / 426 / 562 materials). Reading the
// width off that texture removes the 2048 assumption and gets the smaller page right.
static DAVA::int32 ShippedLightmapPageWidth(DAVA::NMaterial* material)
{
    DAVA::Texture* page = material->GetEffectiveTexture(DAVA::NMaterialTextureName::TEXTURE_LIGHTMAP);
    if ((page != nullptr) && (page->GetWidth() > 0))
    {
        return page->GetWidth();
    }
    return static_cast<DAVA::int32>(BEAST_SHIPPED_LIGHTMAP_PAGE);
}

static DAVA::int32 ResolveShippedLightmapSize(DAVA::NMaterial* material)
{
    const DAVA::float32* uvScale = material->GetEffectivePropValue(DAVA::FastName("uvScale"));
    if (uvScale == nullptr || uvScale[0] <= 0.0f)
    {
        return 0;
    }

    const DAVA::float32 texels = uvScale[0] * static_cast<DAVA::float32>(ShippedLightmapPageWidth(material));

    // Only trusted when it lands on the ladder. A different page width, or a uvScale we wrote
    // ourselves on an earlier pass, produces something that is not a rung, and that is the
    // signal to fall through to the next source instead of baking a wrong size.
    const DAVA::int32 snapped = SnapToLightmapLadder(static_cast<DAVA::int32>(texels + 0.5f));
    if (std::fabs(static_cast<DAVA::float32>(snapped) - texels) > (texels * 0.02f))
    {
        return 0;
    }
    return snapped;
}

// A tile is only worth its texels when the bake and the material agree on which part of the UV
// set the batch occupies. A shipped model keeps its lightmap islands inside a sub-rectangle of
// [0,1], and some models go past 1.0 entirely, so baking the raw coordinates spends most of the
// target on nothing and lets the surface sample outside the painted area: that is what the hard
// edged black patches on walls, rocks and the lighthouse were. Beast can place a shape anywhere
// in the target (ILBSetUVTransform), so the rectangle is measured once here and used on both
// sides, bake and binding.
static bool ComputeLightmapUVRect(DAVA::RenderBatch* batch, DAVA::Vector2& uvMinOut, DAVA::Vector2& uvSizeOut)
{
    if (batch == nullptr)
    {
        return false;
    }

    DAVA::PolygonGroup* pg = batch->GetPolygonGroup();
    if ((pg == nullptr) || (pg->vertexCount <= 0))
    {
        return false;
    }

    // The lightmap shader samples texCoord1 unconditionally, so a mesh with a single UV set cannot
    // be lightmapped at all in this generation. Baking its only set would put light somewhere the
    // shader never reads, which is worse than leaving it out: skip it and say how many.
    if (pg->textureCoordCount < 2)
    {
        return false;
    }
    const DAVA::int32 uvSet = 1;

    DAVA::Vector2 uv;
    DAVA::Vector2 uvMin(0.0f, 0.0f);
    DAVA::Vector2 uvMax(0.0f, 0.0f);
    for (DAVA::int32 i = 0; i < pg->vertexCount; ++i)
    {
        pg->GetTexcoord(uvSet, i, uv);
        if (i == 0)
        {
            uvMin = uv;
            uvMax = uv;
            continue;
        }
        uvMin.x = std::min(uvMin.x, uv.x);
        uvMin.y = std::min(uvMin.y, uv.y);
        uvMax.x = std::max(uvMax.x, uv.x);
        uvMax.y = std::max(uvMax.y, uv.y);
    }

    const DAVA::Vector2 size(uvMax.x - uvMin.x, uvMax.y - uvMin.y);
    // a collapsed rectangle has no island to bake into, and a huge one is a world projected set
    // (landscape and water live in their own path), so neither is a texture target candidate
    if ((size.x <= 1e-5f) || (size.y <= 1e-5f) || (size.x > 64.0f) || (size.y > 64.0f))
    {
        return false;
    }

    // An atlas tile is small and sits inside [0,1]: that is what a scene looks like after a bake
    // has written atlas space into the mesh and the editor has saved it. Baking such a batch again
    // would fit its own tile into a new tile, so leave it alone instead of corrupting it.
    const bool looksLikeAtlasTile = (size.x <= 0.3f) && (size.y <= 0.3f) && (uvMin.x >= -0.01f) && (uvMin.y >= -0.01f) && (uvMax.x <= 1.01f) && (uvMax.y <= 1.01f);
    if (looksLikeAtlasTile)
    {
        DAVA::Logger::Info("[SceneParser] lightmap UVs of this batch are already atlas space (rect %.3f x %.3f at %.3f,%.3f), skipping",
                            size.x, size.y, uvMin.x, uvMin.y);
        return false;
    }

    uvMinOut = uvMin;
    uvSizeOut = size;
    return true;
}

static DAVA::int32 PickLightmapSizeFromGeometry(DAVA::RenderBatch* batch, const DAVA::Matrix4& worldTransform)
{
    if (batch == nullptr)
    {
        return BEAST_DEFAULT_MESH_LIGHTMAP_SIZE;
    }

    DAVA::AABBox3 worldBox = DAVA::AABBox3();
    batch->GetBoundingBox().GetTransformedBox(worldTransform, worldBox);

    DAVA::float32 extents[3] = { worldBox.max.x - worldBox.min.x, worldBox.max.y - worldBox.min.y, worldBox.max.z - worldBox.min.z };

    // the two largest extents describe the face the lightmap texels are mostly spent on
    for (DAVA::int32 i = 0; i < 2; ++i)
    {
        for (DAVA::int32 j = i + 1; j < 3; ++j)
        {
            if (extents[j] > extents[i])
            {
                const DAVA::float32 swapValue = extents[i];
                extents[i] = extents[j];
                extents[j] = swapValue;
            }
        }
    }

    if (extents[1] <= 0.0f)
    {
        return BEAST_DEFAULT_MESH_LIGHTMAP_SIZE;
    }

    const DAVA::float32 linearSize = std::sqrt(extents[0] * extents[1]);
    return SnapToLightmapLadder(static_cast<DAVA::int32>(std::ceil(linearSize * BEAST_LIGHTMAP_TEXELS_PER_METER)));
}

// dava.revanced compatibility: entity transforms live on TransformComponent
// (this file was written against dava.engine master's Entity::GetWorldTransform)
static DAVA::Matrix4 GetEntityWorldTransform(DAVA::Entity* node)
{
    if (node == nullptr)
        return DAVA::Matrix4::IDENTITY;
    const DAVA::TransformComponent* transformComponent = node->GetComponent<DAVA::TransformComponent>();
    if (transformComponent == nullptr)
        return DAVA::Matrix4::IDENTITY;
    return DAVA::TransformUtils::ToMatrix(transformComponent->GetWorldTransform());
}


namespace Beast
{
namespace
{
// A lightmap page has to survive the process that wrote it. Pointer strings do not: after a
// restart the batch is at a new address and a finished page can never be matched to it, so a
// power cut in hour two threw away hour one. This key is built from things that do not move
// between runs - the entity's position in the node tree, the switch, the LOD and the batch
// part - and folded to 64 bits so it is safe as a file name on any filesystem.
DAVA::String BuildStableInstanceKey(DAVA::Entity* entity, DAVA::int32 switchIndex, DAVA::int32 lodLevel, DAVA::int32 partIndex)
{
    // Entity::Save writes "id" into the sc2 (Entity.cpp:477) and Entity::Load reads it back
    // (Entity.cpp:523), so the id the Properties panel shows is the same number on every load
    // of the same map. That is the identity to key a lightmap page on, not a heap address.
    //
    // The node path is folded in as a guard: if an id ever drifts, because an entity was added
    // or the scene was re-saved by a different editor, the key changes and the target gets
    // re-baked. Runs the other way round are what a wrong page would look like, so a mismatch
    // has to be the failure mode, never a match on the wrong object.
    DAVA::String path;
    for (DAVA::Entity* node = entity; node != nullptr; node = node->GetParent())
    {
        path = DAVA::String(node->GetName().c_str()) + "/" + path;
    }

    DAVA::uint32 hash = 2166136261u;
    for (char c : path)
    {
        hash ^= static_cast<DAVA::uint8>(c);
        hash *= 16777619u;
    }

    // readable on purpose: sm/e142_s0_l1_p0_9f2c1ab4.png says which entity, switch, LOD and
    // part without opening the editor
    return DAVA::Format("e%u_s%d_l%d_p%d_%08x",
                        (entity == nullptr) ? 0u : entity->GetID(),
                        switchIndex, lodLevel, partIndex, hash);
}
} // namespace

namespace
{
bool NameLooksLikeWater(const DAVA::String& name)
{
    DAVA::String lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("water") != DAVA::String::npos || lower.find("sea_surface") != DAVA::String::npos
    || lower.find("seasurface") != DAVA::String::npos || lower.find("_sea") != DAVA::String::npos;
}
}

namespace
{
// Bilinear sample of an RGBA8888 image with wrap-around addressing.
void SampleRgba(const DAVA::Image* image, float u, float v, DAVA::float32 out[4])
{
    u = u - std::floor(u);
    v = v - std::floor(v);
    const DAVA::int32 w = static_cast<DAVA::int32>(image->width);
    const DAVA::int32 h = static_cast<DAVA::int32>(image->height);
    const DAVA::float32 fx = u * w - 0.5f;
    const DAVA::float32 fy = v * h - 0.5f;
    const DAVA::int32 x0 = static_cast<DAVA::int32>(std::floor(fx));
    const DAVA::int32 y0 = static_cast<DAVA::int32>(std::floor(fy));
    const DAVA::float32 tx = fx - x0;
    const DAVA::float32 ty = fy - y0;
    const DAVA::uint8* data = static_cast<const DAVA::uint8*>(image->data);

    for (DAVA::int32 c = 0; c < 4; ++c)
    {
        DAVA::float32 acc = 0.0f;
        for (DAVA::int32 dy = 0; dy <= 1; ++dy)
        {
            for (DAVA::int32 dx = 0; dx <= 1; ++dx)
            {
                const DAVA::int32 sx = ((x0 + dx) % w + w) % w;
                const DAVA::int32 sy = ((y0 + dy) % h + h) % h;
                const DAVA::float32 weight = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty);
                acc += weight * (data[(static_cast<size_t>(sy) * w + sx) * 4 + c] / 255.0f);
            }
        }
        out[c] = acc;
    }
}
}

SceneParser::SceneParser(BeastManager* _beastManager, DAVA::Function<void()> parsingCompletedCallback)
    : beastManager(_beastManager)
    , sceneParsingCompleteCallback(parsingCompletedCallback)
{
}

SceneParser::~SceneParser()
{
    ClearScene();

    if (thumbnailsRequestId != DAVA::LandscapeThumbnails::InvalidID)
    {
        DAVA::LandscapeThumbnails::CancelRequest(thumbnailsRequestId);
    }
}

MaxLodMaxSwitch SceneParser::FindMaxLod(DAVA::Entity* node)
{
    maxLodMaxSwitch.maxLodLevel = 0;
    maxLodMaxSwitch.maxSwitchIndex = 0;

    if (beastManager->GetMode() == eBeastMode::MODE_LIGHTMAPS)
    {
        DAVA::ScopedPtr<DAVA::Texture> pinkTexture(DAVA::Texture::CreatePink());
        FindMaxLodRecursive(node, pinkTexture);
    }

    return maxLodMaxSwitch;
}

void SceneParser::FindMaxLodRecursive(DAVA::Entity* node, DAVA::Texture* pinkTexture)
{
    DAVA::int32 nodesCount = node->GetChildrenCount();
    for (DAVA::int32 nodeIndex = 0; nodeIndex < nodesCount; ++nodeIndex)
    {
        DAVA::Entity* child = node->GetChild(nodeIndex);
        DAVA::RenderObject* ro = GetRenderObject(child);
        if (ro)
        {
            DAVA::uint32 count = ro->GetRenderBatchCount();
            for (DAVA::uint32 ri = 0; ri < count; ++ri)
            {
                DAVA::NMaterial* material = ro->GetRenderBatch(ri)->GetMaterial();
                if (IsMaterialTemplateContainString(material, "TextureLightmap"))
                {
                    if (material->HasLocalTexture(DAVA::NMaterialTextureName::TEXTURE_LIGHTMAP))
                        material->SetTexture(DAVA::NMaterialTextureName::TEXTURE_LIGHTMAP, pinkTexture);
                    else
                        material->AddTexture(DAVA::NMaterialTextureName::TEXTURE_LIGHTMAP, pinkTexture);
                }
            }

            maxLodMaxSwitch.maxLodLevel = DAVA::Max(maxLodMaxSwitch.maxLodLevel, ro->GetMaxLodIndex());
            maxLodMaxSwitch.maxSwitchIndex = DAVA::Max(maxLodMaxSwitch.maxSwitchIndex, ro->GetMaxSwitchIndex());
        }

        FindMaxLodRecursive(child, pinkTexture);
    }
}

void SceneParser::SetEventPump(DAVA::Function<void()> pump)
{
    eventPump = pump;
}

void SceneParser::StartParsing()
{
    shouldCancel = false;
    entitiesParsed = false;
    landscapeTextureProcessed = false;
    dumpedTextureCount = 0;
    skippedWaterBatches = 0;
    landscapeColorReady = false;
    directionalLightEntity = nullptr;

    // per-LOD counters: what the batch loop accepted and why it rejected the rest
    skippedBatchCount = 0;
    loggedSkipCount = 0;
    acceptedBatchCount = 0;
    loggedUnlitBatchCount = 0;
    loggedLightCount = 0;
}

void SceneParser::FindAndInitDirectionalLight(DAVA::Entity* node)
{
    DAVA::LightComponent* lightComponent = GetLightComponent(node);
    if (lightComponent != nullptr)
    {
        ParseLightComponent(lightComponent);
    }
    else
    {
        DAVA::int32 nodesCount = node->GetChildrenCount();
        for (DAVA::int32 nodeIndex = 0; nodeIndex < nodesCount; ++nodeIndex)
        {
            FindAndInitDirectionalLight(node->GetChild(nodeIndex));
        }
    }
}

void SceneParser::ValidateDirectionalLight()
{
    if (directionalLightEntity == nullptr)
    {
        DAVA::Logger::Warning("No directional lights found on scene, default light direction will be used");
        mainLightDirection = DAVA::Vector3(0.0f, 0.0f, 1.0f);
    }
}

void SceneParser::ParseNodeRecursive(DAVA::Entity* node, DAVA::uint32 depth)
{
    if (eventPump && ++nodesSinceEventPump >= 64)
    {
        nodesSinceEventPump = 0;
        eventPump();
    }

    DAVA::RenderComponent* renderComponent = GetRenderComponent(node);

    // Live preview radius: skip what the camera cannot see. Children are still
    // walked because entities are not guaranteed to be grouped spatially.
    bool culled = false;
    if ((renderComponent != nullptr) && (liveCullRadius > 0.0f))
    {
        const DAVA::Vector3 position = GetEntityWorldTransform(node).GetTranslationVector();
        culled = (position - liveCullCenter).SquareLength() > (liveCullRadius * liveCullRadius);
    }

    if (renderComponent && !culled)
    {
        DAVA::RenderObject* rObject = renderComponent->GetRenderObject();
        if (rObject && (rObject->GetType() == DAVA::RenderObject::TYPE_MESH || rObject->GetType() == DAVA::RenderObject::TYPE_SPEED_TREE))
        {
            ParseNodeMaterials(renderComponent);
            ParseNodeMeshes(renderComponent);
        }
    }

    auto landscape = GetLandscape(node);
    if (nullptr != landscape)
    {
        landscapeNode = node;
        ParseLandscapeMaterial(landscape);
    }

    // don't go deeper into lightNode
    if (GetLightComponent(node) == nullptr)
    {
        DAVA::int32 nodesCount = node->GetChildrenCount();
        for (DAVA::int32 nodeIndex = 0; nodeIndex < nodesCount; ++nodeIndex)
        {
            DAVA::Entity* child = node->GetChild(nodeIndex);
            ParseNodeRecursive(child, depth + 1);
        }
    }

    if (depth == 0)
    {
        entitiesParsed = true;
        ReportSkippedWater();
        landscapeTextureProcessed = landscapeTextureProcessed || (landscapeNode == nullptr) || (landscapeMaterial == nullptr);
        CheckIfParsingCompleted();
    }
}

void SceneParser::ReportSkippedWater()
{
    if (skippedWaterBatches > 0)
    {
        DAVA::Logger::Info("[SceneParser] skipped %u water surface batch(es): Beast has no transparent pass, they drew as checkerboard planes",
                           skippedWaterBatches);
    }
}

void SceneParser::FinishParsingLandscape()
{
    landscapeTextureProcessed = true;
    CheckIfParsingCompleted();
}

void SceneParser::CheckIfParsingCompleted()
{
    if (entitiesParsed && landscapeTextureProcessed)
    {
        LogSkipSummary();
        sceneParsingCompleteCallback();
    }
}


bool SceneParser::IsWaterLikeMaterial(DAVA::NMaterial* material, const DAVA::Entity* entity)
{
    if (material == nullptr)
    {
        return false;
    }
    if (NameLooksLikeWater(material->GetMaterialName().c_str()))
    {
        return true;
    }
    // The template the material is based on carries the shader name ("seaSurface"
    // and friends even when the instance is renamed).
    if (material->GetEffectiveTexture(DAVA::NMaterialTextureName::TEXTURE_ALBEDO) == nullptr)
    {
        // GetEffectiveTexture and GetParent are non-const, so this takes NMaterial*.
        DAVA::NMaterial* parent = material->GetParent();
        for (DAVA::int32 depth = 0; parent != nullptr && depth < 4; ++depth, parent = parent->GetParent())
        {
            if (NameLooksLikeWater(parent->GetMaterialName().c_str()))
            {
                return true;
            }
        }
    }
    if (entity != nullptr && NameLooksLikeWater(entity->GetName().c_str()))
    {
        return true;
    }
    return false;
}

void SceneParser::ParseNodeMaterials(DAVA::RenderComponent* renderComponent)
{
    DAVA::Entity* entity = renderComponent->GetEntity();
    DAVA::RenderObject* ro = renderComponent->GetRenderObject();
    if ((ro == nullptr) || (ro->GetType() == DAVA::RenderObject::TYPE_LANDSCAPE))
    {
        return;
    }

    DAVA::uint32 count = ro->GetRenderBatchCount();
    for (DAVA::uint32 ri = 0; ri < count; ++ri)
    {
        DAVA::NMaterial* material = ro->GetRenderBatch(ri)->GetMaterial();
        if (material == nullptr)
            continue;

        // Water and sea surfaces are translucent sheets with no albedo: Beast has no
        // transparency for a bake, so they came out as the big checkerboard plane.
        if (IsWaterLikeMaterial(material, entity))
        {
            ++skippedWaterBatches;
            continue;
        }

        BeastTexture* beastTexture = nullptr;
        DAVA::Texture* diffuseTexture = material->GetEffectiveTexture(DAVA::NMaterialTextureName::TEXTURE_ALBEDO);
        if (textureMaxSize < 0)
        {
            // "nothing" in the texture combo: no albedo goes to Beast at all. Fastest
            // start, materials draw with their flat colour.
            diffuseTexture = nullptr;
            ++materialsWithoutAlbedo;
        }
        if (diffuseTexture && diffuseTexture->GetDescriptor()->GetSourceTexturePathname().Exists())
        {
            beastTexture = BeastTexture::CreateWithName(BeastTexture::PointerToString(diffuseTexture), beastManager);
            beastTexture->InitWithTexture(diffuseTexture);
        }
        else if (diffuseTexture && dumpTextures)
        {
            BeastTexture* uploadedTexture = BeastTexture::CreateWithName(BeastTexture::PointerToString(diffuseTexture), beastManager);
            // Set only on success: a texture that failed here must reach the material
            // as nullptr, otherwise Beast receives a null handle.
            beastTexture = (uploadedTexture->GetILBTexture() != nullptr) ? uploadedTexture : nullptr;
            if (uploadedTexture->GetILBTexture() == nullptr)
            {
                // Shipped .tex files are export form: the descriptor points at a
                // source .dds/.pvr that was never shipped. The engine can still
                // decode the payload, so the pixels are handed to Beast directly.
                // No PNG is written: the game ships no PNGs, and any file Beast had
                // to read back was one we had just encoded ourselves (its own log
                // spent 42.73 s loading those files).
                DAVA::ScopedPtr<DAVA::Image> image(diffuseTexture->CreateImageFromMemory());
                const bool imageIsSane = image && image->data != nullptr && image->width > 0 && image->height > 0
                && image->width <= 8192 && image->height <= 8192;

                if (imageIsSane)
                {
                    DAVA::ScopedPtr<DAVA::Image> rgba;
                    DAVA::Image* toUpload = image;
                    if (image->format != DAVA::FORMAT_RGBA8888)
                    {
                        rgba = DAVA::Image::Create(image->width, image->height, DAVA::FORMAT_RGBA8888);
                        if (rgba && DAVA::ImageConvert::ConvertImage(image, rgba))
                        {
                            toUpload = rgba;
                        }
                        else
                        {
                            ++materialsWithoutAlbedo;
                            DAVA::Logger::Warning("[SceneParser] texture format %d cannot be converted to RGBA8888, skipping the albedo",
                                                  static_cast<DAVA::int32>(image->format));
                            toUpload = nullptr;
                        }
                    }

                    if (toUpload != nullptr)
                    {
                        // A preview does not need a 2048 px albedo: less to convert,
                        // less to copy into Beast, less Beast memory per material.
                        const DAVA::uint32 maxSize = static_cast<DAVA::uint32>(textureMaxSize);
                        if ((maxSize > 0) && (toUpload->width > maxSize || toUpload->height > maxSize))
                        {
                            const DAVA::uint32 longest = DAVA::Max(toUpload->width, toUpload->height);
                            const DAVA::uint32 scaledW = DAVA::Max<DAVA::uint32>(1, toUpload->width * maxSize / longest);
                            const DAVA::uint32 scaledH = DAVA::Max<DAVA::uint32>(1, toUpload->height * maxSize / longest);
                            if (!toUpload->ResizeImage(scaledW, scaledH))
                            {
                                DAVA::Logger::Warning("[SceneParser] texture resize %ux%u failed, uploading it as is",
                                                      toUpload->width, toUpload->height);
                            }
                        }

                        const bool uploaded = uploadedTexture->InitWithPixels(toUpload);
                        if (dumpedTextureCount < 5)
                        {
                            ++dumpedTextureCount;
                            DAVA::Logger::Info("[SceneParser] texture upload: %ux%u from format %d -> %s",
                                               static_cast<DAVA::int32>(toUpload->width), static_cast<DAVA::int32>(toUpload->height),
                                               static_cast<DAVA::int32>(image->format), uploaded ? "ok" : "REFUSED");
                        }
                        // An empty BeastTexture object is worse than none: Beast gets a
                        // null handle and logs "Invalid BeastTexture Handle" for every
                        // material that uses it. Vertex colours are the honest fallback.
                        beastTexture = uploaded ? uploadedTexture : nullptr;
                    }
                }
                else
                {
                    if (dumpedTextureCount < 5)
                    {
                        ++dumpedTextureCount;
                        DAVA::Logger::Warning("[SceneParser] texture could not be decoded for Beast, vertex colours instead: '%s'",
                                              diffuseTexture->GetDescriptor()->GetSourceTexturePathname().GetStringValue().c_str());
                        beastTexture = nullptr;
                    }
                }
            }
        }

        BeastMaterial* beastMaterial = BeastMaterial::CreateWithName(BeastMaterial::PointerToString(material), beastManager);
        beastMaterial->InitWithTextureAndDavaMaterial(beastTexture, material);
        uploadedMaterials.insert(material);

        // Decoding and uploading albedos is the slow part of the parse; pumping only
        // between entities left the window frozen for the whole material pass.
        if (eventPump && ++materialsSinceEventPump >= 8)
        {
            materialsSinceEventPump = 0;
            eventPump();
        }

#ifdef BEAST_ENABLED_NORMALMAPS
        DAVA::Texture* normalTexture = material->GetTexture(DAVA::NMaterial::TEXTURE_NORMAL);
        if (!normalTexture && material->GetParent())
            normalTexture = material->GetParent()->GetTexture(DAVA::NMaterial::TEXTURE_NORMAL);

        if (normalTexture)
        {
            BeastTexture* beastNormalTexture = BeastTexture::CreateWithName(BeastTexture::PointerToString(normalTexture), beastManager);
            beastNormalTexture->InitWithTexture(normalTexture);
            beastMaterial->AttachNormalMap(beastNormalTexture);
        }
#endif
    }
}

DAVA::Matrix4 SceneParser::BuildLeafTransformMatrix(const DAVA::Matrix4& entityInverseTransform)
{
    DAVA::Vector3 localLightDirection = DAVA::MultiplyVectorMat3x3(mainLightDirection, entityInverseTransform);
    localLightDirection.Normalize();

    DAVA::Vector3 initialOrientation(0.0f, 0.0f, 1.0f);
    DAVA::Vector3 c = initialOrientation.CrossProduct(localLightDirection);
    DAVA::float32 e = initialOrientation.DotProduct(localLightDirection);
    DAVA::float32 h = 1.0f / (1.0f + e);

    DAVA::Matrix4 leafTransformMatrix = DAVA::Matrix4::IDENTITY;
    leafTransformMatrix._data[0][0] = -(e + h * c.x * c.x);
    leafTransformMatrix._data[0][1] = -(h * c.x * c.y - c.z);
    leafTransformMatrix._data[0][2] = h * c.x * c.z + c.y;
    leafTransformMatrix._data[1][0] = -(h * c.x * c.y + c.z);
    leafTransformMatrix._data[1][1] = -(e + h * c.y * c.y);
    leafTransformMatrix._data[1][2] = h * c.y * c.z - c.x;
    leafTransformMatrix._data[2][0] = -(h * c.x * c.z - c.y);
    leafTransformMatrix._data[2][1] = -(h * c.y * c.z + c.x);
    leafTransformMatrix._data[2][2] = e + h * c.z * c.z;
    return leafTransformMatrix;
}

DAVA::int32 SceneParser::FindDetailedSpeedTreeLOD(DAVA::RenderObject* ro) const
{
    DVASSERT(ro->GetType() == DAVA::RenderObject::TYPE_SPEED_TREE);

    DAVA::int32 maxLod = 0;
    DAVA::int32 maxVertexCount = 0;
    DAVA::uint32 totalBatches = ro->GetRenderBatchCount();
    for (DAVA::uint32 b = 0; b < totalBatches; ++b)
    {
        DAVA::int32 batchLod = 0;
        DAVA::int32 batchSwitch = 0;
        auto batch = ro->GetRenderBatch(b, batchLod, batchSwitch);
        auto vertices = batch->GetPolygonGroup()->GetVertexCount();
        if (vertices > maxVertexCount)
        {
            maxVertexCount = vertices;
            maxLod = batchLod;
        }
    }
    return maxLod;
}

void SceneParser::ParseNodeMeshes(DAVA::RenderComponent* renderComponent)
{
    DAVA::RenderObject* ro = renderComponent->GetRenderObject();
    if ((ro == nullptr) || (ro->GetType() == DAVA::RenderObject::TYPE_LANDSCAPE))
    {
        return;
    }

    DAVA::Entity* entity = renderComponent->GetEntity();
    DAVA::Matrix4 entityInverseTransform;
    DAVA::Matrix4 entityTransform = GetEntityWorldTransform(entity);
    entityTransform.GetInverse(entityInverseTransform);

    bool lightmapModeUsed = (beastManager->GetMode() == eBeastMode::MODE_LIGHTMAPS) || (beastManager->GetMode() == eBeastMode::MODE_PREVIEW)
                            || (beastManager->GetMode() == eBeastMode::MODE_LIVE);
    bool shModeUsed = (beastManager->GetMode() == eBeastMode::MODE_SPHERICAL_HARMONICS);

    bool isSpeedTree = ro->GetType() == DAVA::RenderObject::TYPE_SPEED_TREE;
    if (isSpeedTree && shModeUsed)
    {
        // One BeastPointCloud for scene
        auto name = BeastPointCloud::PointerToString(renderComponent->GetEntity()->GetScene());
        BeastPointCloud* beastPointCloud = BeastPointCloud::CreateWithName(name, beastManager);
        beastPointCloud->AddBakeEntity(renderComponent->GetEntity());
    }
    else
    {
        DAVA::uint32 renderBatchesCount = ro->GetRenderBatchCount();
        DAVA::int32 lodForCurrentSwitch = -1;
        bool useLightMap = false;
        if (isSpeedTree)
        {
            lodForCurrentSwitch = FindDetailedSpeedTreeLOD(ro);
        }
        else
        {
            for (DAVA::uint32 ri = 0; ri < renderBatchesCount; ++ri)
            {
                // find max lod level for selected switch index
                DAVA::int32 rbLodIndex = 0;
                DAVA::int32 rbSwitchIndex = 0;
                DAVA::RenderBatch* batch = ro->GetRenderBatch(ri, rbLodIndex, rbSwitchIndex);

                if ((rbSwitchIndex == -1) || (rbSwitchIndex == switchIndex))
                {
                    lodForCurrentSwitch = DAVA::Max(lodForCurrentSwitch, rbLodIndex);
                }
            }

            useLightMap = (lodLevel <= lodForCurrentSwitch) || (lodForCurrentSwitch == -1);
            if (useLightMap)
            {
                // detect real lod level for current switch.
                // we select last lod if current lodLevel for parcing is greater than last lod for ro
                lodForCurrentSwitch = lodLevel;
            }
        }

        for (DAVA::uint32 ri = 0; ri < renderBatchesCount; ++ri)
        {
            DAVA::int32 rbLodIndex = 0;
            DAVA::int32 rbSwitchIndex = 0;
            DAVA::RenderBatch* batch = ro->GetRenderBatch(ri, rbLodIndex, rbSwitchIndex);
            DAVA::NMaterial* material = batch->GetMaterial();

            if (!DAVA::IsPointerToExactClass<DAVA::RenderBatch>(batch) || (nullptr == material))
            {
                LogBatchSkipped(static_cast<DAVA::int32>(ri), "not a RenderBatch or no material", material);
                continue;
            }
            // NOT a gate. Scene::OptimizeBeforeExport() strips ILLUMINATION_USED,
            // ILLUMINATION_SHADOW_CASTER/RECEIVER and lightmapSize from every
            // material before the scene is saved, so a shipped .sc2 always reports
            // 0 here. Gating on it skipped all 2755 batches of a real map.
            if (material->GetEffectiveFlagValue(DAVA::NMaterialFlagName::FLAG_ILLUMINATION_USED) == 0)
            {
                if (loggedUnlitBatchCount < 5)
                {
                    DAVA::Logger::Info("[SceneParser] batch %d has FLAG_ILLUMINATION_USED off, baking it anyway (material '%s')",
                                       static_cast<DAVA::int32>(ri),
                                       DAVA::String(material->GetMaterialName().c_str()).c_str());
                }
                ++loggedUnlitBatchCount;
            }

            DAVA::String fxNameString(material->GetEffectiveFXName().c_str());
            std::transform(fxNameString.begin(), fxNameString.end(), fxNameString.begin(), ::tolower);

            bool skyMaterial = fxNameString.find("skyobject") != DAVA::String::npos;
            bool leafMaterial = fxNameString.find("speedtreeleaf") != DAVA::String::npos;
            bool lightmapMaterial = fxNameString.find("lightmap") != DAVA::String::npos;

            if (skyMaterial)
            {
                LogBatchSkipped(static_cast<DAVA::int32>(ri), "sky material", material);
                continue;
            }
            // The same filters as ParseNodeMaterials, in the same order. A batch whose
            // material never reached Beast must not become a mesh: its material group
            // would name an undefined material and Beast would refuse the whole scene.
            if (IsWaterLikeMaterial(material, entity))
            {
                LogBatchSkipped(static_cast<DAVA::int32>(ri), "water material", material);
                continue;
            }
            if (uploadedMaterials.find(material) == uploadedMaterials.end())
            {
                ++skippedWithoutMaterial;
                LogBatchSkipped(static_cast<DAVA::int32>(ri), "material was never uploaded to Beast", material);
                continue;
            }
            if ((rbSwitchIndex != -1) && (rbSwitchIndex != switchIndex))
            {
                LogBatchSkipped(static_cast<DAVA::int32>(ri), "other switch index", material);
                continue;
            }

            if ((rbLodIndex != -1) && (rbLodIndex != lodForCurrentSwitch))
            {
                LogBatchSkipped(static_cast<DAVA::int32>(ri), "other LOD level", material);
                continue;
            }

            ++acceptedBatchCount;

            DAVA::String name = ConstructMeshInstancePartName(batch, ri);

            BeastMesh* beastMesh = BeastMesh::CreateWithName(name, beastManager);
            if (leafMaterial)
            {
                beastMesh->InitWithSpeedTreeLeaf(batch, ri, BuildLeafTransformMatrix(entityInverseTransform));
            }
            else
            {
                beastMesh->InitWithMeshInstancePart(batch, ri);
            }

            BeastMeshInstance* beastMeshInstance = BeastMeshInstance::CreateWithName(name, beastManager);
            beastMeshInstance->InitWithRenderBatchAndTransform(batch, ri, entityTransform);

            // registered before the target decision so the readback, the packer and a later
            // resumed run all agree on what this instance is called
            const DAVA::String stableKey = BuildStableInstanceKey(entity, switchIndex, lodLevel, static_cast<DAVA::int32>(ri));
            DAVA::Vector2 lightmapUVOffset(0.0f, 0.0f);
            DAVA::Vector2 lightmapUVSize(1.0f, 1.0f);
            const bool hasLightmapUVRect = ComputeLightmapUVRect(batch, lightmapUVOffset, lightmapUVSize);
            if (beastMeshInstance != nullptr)
            {
                beastManager->RegisterLightmapTarget(beastMeshInstance->GetMeshInstancePointerString(), stableKey, batch);
                if (hasLightmapUVRect)
                {
                    beastManager->RegisterLightmapUVRect(batch, lightmapUVOffset, lightmapUVSize, 1);
                }
            }
            if ((beastMeshInstance != nullptr) && (beastManager->GetMode() == eBeastMode::MODE_LIVE))
            {
                // The scene handed to Beast is a snapshot: without this, moving an
                // object in the editor changes nothing in the live view.
                beastManager->RegisterLiveInstance(entity, beastMeshInstance->GetILBMeshInstance());
            }
            beastMeshInstance->SetLodLevel(lodLevel);
            // The FX name test is a heuristic for the Blitz-era material set. With
            // "use per-material illumination settings" on, the material's own flag
            // decides instead, which is what an authored scene expects.
            bool bakesLightmap = lightmapMaterial;
            switch (beastManager->GetLightmapSelection())
            {
            case BeastManager::eLightmapSelection::All:
            case BeastManager::eLightmapSelection::AutoOnly:
                bakesLightmap = true;
                break;
            case BeastManager::eLightmapSelection::Flagged:
                bakesLightmap = material->GetEffectiveFlagValue(DAVA::NMaterialFlagName::FLAG_ILLUMINATION_USED) != 0;
                break;
            }
            // ...unless the material states it outright, which beats every inference above
            const DAVA::float32* artistIllumination = material->GetEffectivePropValue(GetArtistIlluminationProp());
            if (artistIllumination != nullptr)
            {
                bakesLightmap = *artistIllumination > 0.5f;
            }

            if (!hasLightmapUVRect)
            {
                // no second UV set and no usable first one: this batch cannot be lightmapped at
                // all in this shader generation, it samples texCoord1 unconditionally. Baking it
                // anyway only produced a tile nobody can read, and garbage where it was read.
                bakesLightmap = false;
            }

            {
                // One line per batch, at info level so it survives into a release log: this is what
                // says whether an object entered the bake and what its UV layout looks like.
                DAVA::Logger::Info("[SceneParser] batch '%s': uv1 %.3f x %.3f at %.3f,%.3f, lightmap material %s, bakes lightmap %s",
                                   entity ? entity->GetName().c_str() : "?", lightmapUVSize.x, lightmapUVSize.y,
                                   lightmapUVOffset.x, lightmapUVOffset.y,
                                   lightmapMaterial ? "yes" : "no", bakesLightmap ? "yes" : "no");
            }

            if ((beastMeshInstance != nullptr) && useLightMap && bakesLightmap && lightmapModeUsed)
            {
                // Size, in precedence order: our own authoring property, then the engine
                // material value when it says something, then the object's geometry. 16 is
                // deliberately NOT taken from PARAM_LIGHTMAP_SIZE even with the illumination
                // bit set, because the engine synthesises exactly that value when the bit is
                // set and no size was authored. 16 has to come through beast.lightmapSize.
                const DAVA::float32* artistLightmapSize = material->GetEffectivePropValue(GetArtistLightmapSizeProp());
                const DAVA::float32* lightmapSize = material->GetEffectivePropValue(DAVA::NMaterialParamName::PARAM_LIGHTMAP_SIZE);
                DAVA::float32 targetLightmapSize = 0.0f;
                // AutoOnly means "ignore what the scene stored, recalculate": that is the mode
                // to compare our geometry-derived sizes against the shipped ones.
                const DAVA::int32 shippedLightmapSize = beastManager->UseStoredLightmapRects() ? ResolveShippedLightmapSize(material) : 0;
                if (artistLightmapSize != nullptr)
                {
                    targetLightmapSize = *artistLightmapSize;
                }
                else if (shippedLightmapSize > 0)
                {
                    // the map's own baked rect: this is the size the level was authored at
                    targetLightmapSize = static_cast<DAVA::float32>(shippedLightmapSize);
                }
                else if ((lightmapSize != nullptr) && (*lightmapSize > static_cast<DAVA::float32>(DAVA::NMaterial::DEFAULT_LIGHTMAP_SIZE)))
                {
                    targetLightmapSize = *lightmapSize;
                }
                else
                {
                    targetLightmapSize = static_cast<DAVA::float32>(PickLightmapSizeFromGeometry(batch, entityTransform));
                }
                // the ladder is what the shader's seam dither assumes, so never bake off it
                targetLightmapSize = static_cast<DAVA::float32>(SnapToLightmapLadder(static_cast<DAVA::int32>(targetLightmapSize)));
                if (beastManager->IsResumingExistingBakes() && beastManager->HasBakedResult(stableKey))
                {
                    // Already baked in an earlier run: it stays in the scene as geometry, so it
                    // still blocks light and bounces it, but it is not a target again.
                    DAVA::Logger::Debug("[SceneParser] '%s' already baked, skipped", stableKey.c_str());
                }
                else
                {
                    beastMeshInstance->SetLightmapSize(static_cast<DAVA::int32>(targetLightmapSize));
                    beastMeshInstance->UseLightMap();
                }
            }
        }
    }
}

DAVA::String SceneParser::ConstructMeshInstancePartName(DAVA::RenderBatch* batch, DAVA::int32 index)
{
    return DAVA::Format("%s_%d", BeastMesh::PointerToString(batch).c_str(), index);
}

void SceneParser::ParseLandscapeNode(DAVA::Entity* node, DAVA::Landscape* landscape)
{
}

void SceneParser::LogBatchSkipped(DAVA::int32 batchIndex, const char* reason, DAVA::NMaterial* material)
{
    ++skippedBatchCount;
    if (loggedSkipCount >= 40)
    {
        return;
    }
    ++loggedSkipCount;

    if (material == nullptr)
    {
        DAVA::Logger::Info("[SceneParser] batch %d skipped: %s (no material)", batchIndex, reason);
        return;
    }

    // material name and shader path are what identify the material era: a Blitz
    // 3.2.3 project answers these from its own DataSource/Materials, RE's own
    // Data/Materials only fills the gaps
    DAVA::Logger::Info("[SceneParser] batch %d skipped: %s (material '%s', shader '%s', illuminationUsed %d)",
                       batchIndex, reason,
                       DAVA::String(material->GetMaterialName().c_str()).c_str(),
                       DAVA::String(material->GetEffectiveFXName().c_str()).c_str(),
                       material->GetEffectiveFlagValue(DAVA::NMaterialFlagName::FLAG_ILLUMINATION_USED));
}

void SceneParser::LogSkipSummary()
{
    // Which materials reached Beast without a diffuse: the answer to "why is this
    // model checkered" instead of another guess.
    DAVA::Logger::Info("[SceneParser] %u of %u uploaded material(s) have no albedo and draw with a flat colour",
                       materialsWithoutAlbedo, static_cast<DAVA::uint32>(uploadedMaterials.size()));

    if (skippedBatchCount == 0)
    {
        DAVA::Logger::Info("[SceneParser] %d render batch(es) accepted, none skipped", acceptedBatchCount);
        return;
    }

    DAVA::Logger::Info("[SceneParser] %d render batch(es) accepted, %d skipped (%d listed above, the rest share these reasons)",
                       acceptedBatchCount, skippedBatchCount, loggedSkipCount);
}

DAVA::int32 SceneParser::GetLightsCreated() const
{
    return lightsCreated;
}

void SceneParser::ParseLightComponent(DAVA::LightComponent* lightComponent)
{
    bool staticLightEnabled = true;

    DAVA::KeyedArchive* props = DAVA::GetCustomPropertiesArchieve(lightComponent->GetEntity());
    const bool hasExplicitSetting = (props != nullptr) && props->IsKeyExists("editor.staticlight.enable");
    if (props)
    {
        staticLightEnabled = props->GetBool("editor.staticlight.enable", true);
    }

    if (!staticLightEnabled)
    {
        // A shipped scene says this explicitly: rudniki enables ONE directional light (intensity
        // 0.5) and disables the other sun and the sky light. Baking the disabled ones anyway puts
        // three lights' worth of energy into the map where the artist intended one.
        DAVA::Logger::Info("[SceneParser] light '%s' is excluded from the bake: editor.staticlight.enable = false",
                           lightComponent->GetEntity()->GetName().c_str());
        return;
    }

    if (staticLightEnabled)
    {
        auto light = lightComponent->GetLightObject();

        // The scene is authoritative, and it says all three lights belong in the bake. Dumping the
        // custom properties of BOTH scenes settles it - there is no "with and without" here, every
        // light in both files carries the full editor.staticlight.* set:
        //
        //   OGTOWERONLYrudniki.sc2   Light1 type 0 enable TRUE intensity 1.00
        //                            Light  type 3 enable TRUE intensity 1.00
        //                            Light1 type 0 enable TRUE intensity 1.00
        //   rudnikiv56.sc2           Light1 type 0 enable TRUE intensity 0.50
        //                            Light  type 3 enable TRUE intensity 0.75
        //                            Light1 type 0 enable TRUE intensity 0.50
        //
        // So this map is baked from TWO directional lights plus a sky light, all enabled, at the
        // authored intensities. The filter that kept only one sun was mine, it contradicted the
        // data, and it is gone.

        // Set the level Beast will actually bake with, unless the artist authored one. This is the
        // only channel that reaches BeastLight's intensity, and the engine expects a lightmap that
        // is roughly half brightness, so the defaults are below 1.0 (see the constants above).
        // Overwrite unconditionally: the shipped scene already carries editor.intensity = 1.0, so a
        // "only if absent" rule never fired and the bake kept clipping (the log showed 1.000 on
        // every light while the constants below said otherwise).
        // Do not overwrite it. BeastLight reads "editor.intensity" and this is the number the scene
        // authored: 1.00 in the shipped map, 0.50/0.75/0.50 in the rig copy. Forcing 0.5 on every
        // light made the bake carry a third of the energy the target was baked with, which is why
        // the shipped highlights sit at 179/255 while ours sat at 97.
        DAVA::KeyedArchive* lightProps = DAVA::GetCustomPropertiesArchieve(lightComponent->GetEntity());
        // Say what Beast is about to bake with. BeastLight reads "editor.intensity", so if the
        // atlas still comes back clipped this line is the difference between "the value never
        // arrived" and "it arrived and something else is scaling the result up".
        if (lightProps != nullptr)
        {
            DAVA::Logger::Info("[SceneParser] light '%s' bake intensity: editor.intensity = %.3f",
                               lightComponent->GetEntity()->GetName().c_str(),
                               lightProps->GetFloat("editor.intensity", 1.0f));
        }

        if (light->GetType() == DAVA::Light::TYPE_DIRECTIONAL)
        {
            if (directionalLightEntity == nullptr)
            {
                mainLightDirection = light->GetDirection();
                mainLightDirection.Normalize();
                directionalLightEntity = lightComponent->GetEntity();
            }
            else
            {
                // Every light is still baked: BeastLight is created below for all
                // of them. The first directional light is only the one whose
                // direction orients SpeedTree leaf cards, so a second sun is a
                // note, not a failure.
                DAVA::Logger::Warning("Several static directional lights found; '%s' is used to orient SpeedTree leaf geometry. "
                                      "All lights are still baked.",
                                      directionalLightEntity->GetName().c_str());
            }
        }

        // A light's position and direction come from the ENTITY's world matrix
        // (LightUpdateSystem::RecalcLight -> SetPositionDirectionFromMatrix), not
        // from the LightComponent's own params, so an entity with an identity
        // transform is a light at the world origin: under the terrain. Print what
        // Beast is actually given instead of leaving that to guesswork.
        if (loggedLightCount < 24)
        {
            ++loggedLightCount;
            const DAVA::Vector3& lightPosition = light->GetPosition();
            const DAVA::Vector3& lightDirection = light->GetDirection();
            // the colour is what makes a lightmap coloured or grey: the baked illumination is
            // light colour x albedo bounce, and albedo never leaves Beast unless the textures arrive
            const DAVA::Color diffuseColor = light->GetDiffuseColor();
            const DAVA::Color ambientColor = light->GetAmbientColor();
            DAVA::Logger::Info("[SceneParser] light '%s' type %d at (%.2f, %.2f, %.2f) direction (%.2f, %.2f, %.2f) intensity %.2f diffuse (%.3f, %.3f, %.3f, %.3f) ambient (%.3f, %.3f, %.3f, %.3f) staticlight archive %d, enable %d",
                               lightComponent->GetEntity()->GetName().c_str(), static_cast<DAVA::int32>(light->GetType()),
                               lightPosition.x, lightPosition.y, lightPosition.z,
                               lightDirection.x, lightDirection.y, lightDirection.z, light->GetIntensity(),
                               diffuseColor.r, diffuseColor.g, diffuseColor.b, diffuseColor.a,
                               ambientColor.r, ambientColor.g, ambientColor.b, ambientColor.a,
                               static_cast<DAVA::int32>(props != nullptr), static_cast<DAVA::int32>(staticLightEnabled));

            // A light with no authored static-light properties still bakes: BeastLight falls
            // back to intensity 1.0 and shadows on. Say so, because "the scene has no
            // editor.staticlight.* keys" is the normal shape of a shipped .sc2 and it explains
            // both the light level and the lack of shadow-angle tuning in the result.
            if (beastManager->GetMode() != eBeastMode::MODE_LIVE)
            {
                DAVA::KeyedArchive* properties = DAVA::GetCustomPropertiesArchieve(lightComponent->GetEntity());
                if (properties == nullptr || !properties->IsKeyExists("editor.staticlight.enable"))
                {
                    DAVA::Logger::Warning("[SceneParser] light '%s' carries no editor.staticlight.* properties: Beast runs it at intensity 1.0 with shadows enabled",
                                          lightComponent->GetEntity()->GetName().c_str());
                }
            }

            if (light->GetType() != DAVA::Light::TYPE_DIRECTIONAL && std::fabs(lightPosition.x) < 1.0e-3f && std::fabs(lightPosition.y) < 1.0e-3f && std::fabs(lightPosition.z) < 1.0e-3f)
            {
                DAVA::Logger::Warning("[SceneParser] light '%s' sits at the world origin (its entity transform is identity), which is under the landscape: give it a transform, position and direction are read from the entity matrix",
                                      lightComponent->GetEntity()->GetName().c_str());
            }
        }

        BeastLight* beastLight = BeastLight::CreateWithName(BeastLight::PointerToString(light), beastManager);
        beastLight->InitWithLight(lightComponent->GetEntity(), light);
        ++lightsCreated;

        // DAVA's renderer adds every light's ambColor to each surface as an ambient term. The two
        // suns in rudniki carry ambColor (0.96, 0.84, 0.36) and (1.0, 0.87, 0.65) and the sky light
        // carries none; Beast was never told about any of it, so the sides facing away from the sun
        // got nothing at all and came out neutral black instead of the map's warm fill. One ambient
        // light per non-black ambColor restores exactly that contribution.
        const DAVA::Color ambientColor = light->GetAmbientColor();
        const bool authoredAmbient = (ambientColor.r + ambientColor.g + ambientColor.b) > 1.0e-3f;
        if (!authoredAmbient && (lightsCreated == 1))
        {
            // No light in this scene carries an ambient colour, so the bake had no fill at all and
            // every surface facing away from the sun went to near black. The shipped map does have
            // fill and it is COOL where the sun is warm: fitting a v10.60 bake per channel gives the
            // additive terms 16.0 / 22.0 / 25.9 out of 255, i.e. (0.617, 0.849, 1.000) normalised.
            // That is a blue-white sky fill, the classic warm-sun/cool-sky pair. Created once, from
            // the first baked light, and only when the scene authors no ambient of its own.
            DAVA::Matrix4 identity;
            identity.Identity();
            const ILBMatrix4x4 fillMatrix = ConvertDavaMatrix(identity);
            const ILBLinearRGB fillRGB(0.617f, 0.849f, 1.000f);
            const DAVA::float32 fillIntensity = (props != nullptr)
            ? props->GetFloat("editor.staticlight.ambientintensity", 0.05f)
            : 0.05f;
            ILBLightHandle fillHandle = nullptr;
            if (fillIntensity <= 1.0e-4f)
            {
                // Off by default now: the scene bakes its own sky light (type 3, enable TRUE), so
                // that hemisphere IS the fill. Adding a synthetic ambient on top would count it twice.
                DAVA::Logger::Info("[SceneParser] sky fill stays off; the scene bakes its own sky light");
                return;
            }
            const ILBStatus fillStatus = ILBCreateAmbientLight(beastManager->GetILBScene(), STRING_TO_BEAST_STRING("beast_sky_fill"), &fillMatrix, &fillRGB, &fillHandle);
            if ((fillStatus == ILB_ST_SUCCESS) && (fillHandle != nullptr))
            {
                ILBSetLightIntensity(fillHandle, fillIntensity);
                DAVA::Logger::Info("[SceneParser] sky fill 0.617,0.849,1.000 at intensity %.3f (no ambient authored by the scene)", fillIntensity);
            }
            else
            {
                DAVA::Logger::Warning("[SceneParser] ILBCreateAmbientLight for the sky fill failed with status %d", static_cast<DAVA::int32>(fillStatus));
            }
        }
        if (authoredAmbient)
        {
            DAVA::Matrix4 identity;
            identity.Identity();
            const ILBMatrix4x4 ambientMatrix = ConvertDavaMatrix(identity);
            const ILBLinearRGB ambientRGB(ambientColor.r, ambientColor.g, ambientColor.b);
            const DAVA::float32 ambientIntensity = (props != nullptr)
            ? props->GetFloat("editor.staticlight.ambientintensity", 0.05f)
            : 0.05f;
            const DAVA::String ambientName = DAVA::Format("%s_ambient", lightComponent->GetEntity()->GetName().c_str());
            ILBLightHandle ambientHandle = nullptr;
            if (ambientIntensity <= 1.0e-4f)
            {
                DAVA::Logger::Info("[SceneParser] ambient %.3f,%.3f,%.3f from light '%s' stays off (editor.staticlight.ambientintensity 0)",
                                   ambientColor.r, ambientColor.g, ambientColor.b, lightComponent->GetEntity()->GetName().c_str());
            }
            else
            {
                const ILBStatus ambientStatus = ILBCreateAmbientLight(beastManager->GetILBScene(), STRING_TO_BEAST_STRING(ambientName), &ambientMatrix, &ambientRGB, &ambientHandle);
                if ((ambientStatus == ILB_ST_SUCCESS) && (ambientHandle != nullptr))
                {
                    const ILBStatus intensityStatus = ILBSetLightIntensity(ambientHandle, ambientIntensity);
                    DAVA::Logger::Info("[SceneParser] ambient %.3f,%.3f,%.3f at intensity %.2f from light '%s' (status %d)",
                                       ambientColor.r, ambientColor.g, ambientColor.b, ambientIntensity,
                                       lightComponent->GetEntity()->GetName().c_str(), static_cast<DAVA::int32>(intensityStatus));
                }
                else
                {
                    DAVA::Logger::Warning("[SceneParser] ILBCreateAmbientLight for light '%s' failed with status %d",
                                          lightComponent->GetEntity()->GetName().c_str(), static_cast<DAVA::int32>(ambientStatus));
                }
            }
        }
    }
}

void SceneParser::ParseLandscapeMaterial(DAVA::Landscape* landscape)
{
    DAVA::SafeRelease(landscapeMaterial);

    auto texture = landscape->GetMaterial()->GetEffectiveTexture(DAVA::Landscape::TEXTURE_COLOR);
    if (texture == nullptr)
    {
        DAVA::Logger::Error("Beast: Can't parse Landscape color texture!");
        landscapeNode = nullptr;
        shouldCancel = true;
        FinishParsingLandscape();
        return;
    }

    landscapeMaterial = BeastMaterial::CreateWithName(BeastTexture::PointerToString(landscape), beastManager);

    // The colour map is a landscape texture, not a render target. Asking
    // LandscapeThumbnails for a captured copy left the terrain with no texture at
    // all whenever that callback did not arrive (it did not in this build), so the
    // texture is uploaded straight away and the terrain is parsed without waiting.
    if (UploadLandscapeColorMap(landscape))
    {
        ContinueParsingLandscape(landscapeNode, landscape);
        return;
    }

    // Fallback: the texture could not be decoded in process, so let the editor
    // render the thumbnail and continue from that callback as before.
    thumbnailsRequestId = DAVA::LandscapeThumbnails::Create(landscape, DAVA::MakeFunction(this, &SceneParser::OnLandscapeImageCaptured));
}

DAVA::Image* SceneParser::CompositeLandscapeAlbedo(DAVA::Landscape* landscape, const DAVA::Image* colorMap)
{
    DAVA::NMaterial* material = landscape->GetMaterial();
    if (material == nullptr || colorMap == nullptr)
    {
        DAVA::Logger::Warning("[SceneParser] landscape: no material or no colour map, tiles cannot be composed");
        return nullptr;
    }
    if (colorMap->format != DAVA::FORMAT_RGBA8888)
    {
        // Should not happen any more: the caller converts first.
        DAVA::Logger::Warning("[SceneParser] landscape: colour map format %d is not RGBA8888, tiles skipped",
                              static_cast<DAVA::int32>(colorMap->format));
        return nullptr;
    }

    // tilemask-fp.sl branches on #ifdef TILEMASK, so the material's own flag decides
    // which path the ground uses; guessing from "are both textures present" is not the
    // same question and can pick the wrong branch.
    const bool tileMaskBranch = material->GetEffectiveFlagValue(DAVA::FastName("TILEMASK")) != 0;

    DAVA::Texture* tileTexture = material->GetEffectiveTexture(DAVA::Landscape::TEXTURE_TILE);
    DAVA::Texture* tileMaskTexture = material->GetEffectiveTexture(DAVA::Landscape::TEXTURE_TILEMASK);
    if (!tileMaskBranch || tileTexture == nullptr || tileMaskTexture == nullptr)
    {
        // The other branch of the same shader: "#else uniform sampler2D fullTiledTexture;
        // ... tex2D(fullTiledTexture, texCoord)" - a single packed, already coloured
        // tiled texture, with no colour-map multiply. Blitz-era maps use this one.
        DAVA::Texture* fullTiled = material->GetEffectiveTexture(DAVA::FastName("fullTiledTexture"));
        if (fullTiled == nullptr)
        {
            DAVA::Logger::Info("[SceneParser] landscape: tile %s, mask %s, fullTiledTexture missing - colour map only",
                               tileTexture ? "ok" : "missing", tileMaskTexture ? "ok" : "missing");
            return nullptr;
        }

        DAVA::ScopedPtr<DAVA::Image> full(fullTiled->CreateImageFromMemory());
        if (!full)
        {
            DAVA::Logger::Warning("[SceneParser] landscape: fullTiledTexture %s could not be decoded, colour map only",
                                  fullTiled->texDescriptor->pathname.GetAbsolutePathname().c_str());
            return nullptr;
        }
        if (full->format != DAVA::FORMAT_RGBA8888)
        {
            DAVA::Image* converted = DAVA::Image::Create(full->width, full->height, DAVA::FORMAT_RGBA8888);
            if (converted != nullptr && DAVA::ImageConvert::ConvertImage(full.get(), converted))
            {
                full.reset(converted);
            }
            else
            {
                DAVA::SafeRelease(converted);
                return nullptr;
            }
        }
        DAVA::Logger::Info("[SceneParser] landscape: FULLTILED path (%ux%u), no tile mask in this material",
                           full->width, full->height);
        // ScopedPtr has no release(): hand the caller its own reference.
        DAVA::Image* composed = full.get();
        DAVA::SafeRetain(composed);
        return composed;
    }

    DAVA::ScopedPtr<DAVA::Image> tiles(tileTexture->CreateImageFromMemory());
    DAVA::ScopedPtr<DAVA::Image> mask(tileMaskTexture->CreateImageFromMemory());
    if (!tiles || !mask)
    {
        DAVA::Logger::Warning("[SceneParser] landscape: tile texture %s, tile mask %s could not be decoded, colour map only",
                              tiles ? "ok" : "missing", mask ? "ok" : "missing");
        return nullptr;
    }

    // Shipped .tex are PVR/DXT, not RGBA8888, and refusing them here is what made the
    // live terrain a single smeared colour with no tile detail at all.
    // ScopedPtr has no swap(): the converted image is created, converted into, then
    // handed to the scoped pointer with release().
    if (tiles->format != DAVA::FORMAT_RGBA8888)
    {
        DAVA::Image* converted = DAVA::Image::Create(tiles->width, tiles->height, DAVA::FORMAT_RGBA8888);
        if (converted != nullptr && DAVA::ImageConvert::ConvertImage(tiles.get(), converted))
        {
            tiles.reset(converted);
            DAVA::Logger::Info("[SceneParser] landscape: tile texture converted to RGBA8888");
        }
        else
        {
            DAVA::SafeRelease(converted);
        }
    }
    if (mask->format != DAVA::FORMAT_RGBA8888)
    {
        DAVA::Image* converted = DAVA::Image::Create(mask->width, mask->height, DAVA::FORMAT_RGBA8888);
        if (converted != nullptr && DAVA::ImageConvert::ConvertImage(mask.get(), converted))
        {
            mask.reset(converted);
        }
        else
        {
            DAVA::SafeRelease(converted);
        }
    }
    if (tiles->format != DAVA::FORMAT_RGBA8888 || mask->format != DAVA::FORMAT_RGBA8888)
    {
        DAVA::Logger::Warning("[SceneParser] landscape: tile %d / mask %d could not be converted, colour map only",
                              static_cast<DAVA::int32>(tiles->format), static_cast<DAVA::int32>(mask->format));
        return nullptr;
    }

    // Layer colours and the tiling factor live on the material, not in the textures.
    DAVA::float32 layerColor[4][3] = { { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f } };
    const DAVA::FastName colorParams[4] = { DAVA::Landscape::PARAM_TILE_COLOR0, DAVA::Landscape::PARAM_TILE_COLOR1,
                                            DAVA::Landscape::PARAM_TILE_COLOR2, DAVA::Landscape::PARAM_TILE_COLOR3 };
    for (DAVA::int32 layer = 0; layer < 4; ++layer)
    {
        const DAVA::float32* value = material->GetEffectivePropValue(colorParams[layer]);
        if (value != nullptr)
        {
            layerColor[layer][0] = value[0];
            layerColor[layer][1] = value[1];
            layerColor[layer][2] = value[2];
        }
    }

    // landscape-editor-vp.sl: "[material][instance] property float2 textureTiling =
    // float2(50,50);" and "output.texCoordTiled = output.texCoord * textureTiling.xy".
    // That is the WHOLE tiling rule: tile UV = map UV * textureTiling. The default when
    // the material does not override it is 50, not 1.
    DAVA::float32 tilingX = 50.0f;
    DAVA::float32 tilingY = 50.0f;
    const DAVA::float32* tiling = material->GetEffectivePropValue(DAVA::NMaterialParamName::PARAM_LANDSCAPE_TEXTURE_TILING);
    if (tiling != nullptr)
    {
        tilingX = (tiling[0] > 0.0f) ? tiling[0] : 1.0f;
        tilingY = (tiling[1] > 0.0f) ? tiling[1] : 1.0f;
    }

    const DAVA::int32 width = static_cast<DAVA::int32>(colorMap->width);
    const DAVA::int32 height = static_cast<DAVA::int32>(colorMap->height);
    DAVA::Image* result = DAVA::Image::Create(width, height, DAVA::FORMAT_RGBA8888);
    if (result == nullptr)
    {
        return nullptr;
    }

    // tilemask-fp.sl / landscape-editor-fp.sl sample tileTexture0 with texCoordTiled,
    // which the geometry generates as uv * textureTiling ("tiling_0"). Scaling by the
    // colour-map/tile size ratio as well made the pattern repeat twice too often.
    const DAVA::float32 tileRepeatX = tilingX;
    const DAVA::float32 tileRepeatY = tilingY;
    DAVA::Logger::Info("[SceneParser] landscape tiles: tile %ux%u, mask %ux%u, colour %dx%d, tiling %.3f x %.3f",
                       tiles->width, tiles->height, mask->width, mask->height, width, height, tilingX, tilingY);

    const DAVA::uint8* colorData = static_cast<const DAVA::uint8*>(colorMap->data);
    DAVA::uint8* out = static_cast<DAVA::uint8*>(result->data);

    for (DAVA::int32 y = 0; y < height; ++y)
    {
        const DAVA::float32 v = (y + 0.5f) / height;
        for (DAVA::int32 x = 0; x < width; ++x)
        {
            const DAVA::float32 u = (x + 0.5f) / width;
            const size_t pixel = static_cast<size_t>(y) * width + x;

            DAVA::float32 maskPixel[4] = { 0, 0, 0, 0 };
            SampleRgba(mask, u, v, maskPixel);

            DAVA::float32 tilePixel[4] = { 0, 0, 0, 0 };
            SampleRgba(tiles, u * tileRepeatX, v * tileRepeatY, tilePixel);

            for (DAVA::int32 c = 0; c < 3; ++c)
            {
                DAVA::float32 layer = 0.0f;
                for (DAVA::int32 l = 0; l < 4; ++l)
                {
                    layer += tilePixel[l] * maskPixel[l] * layerColor[l][c];
                }
                // DAVA multiplies the composite by the colour map and doubles it.
                const DAVA::float32 base = colorData[pixel * 4 + c] / 255.0f;
                const DAVA::float32 value = std::min(1.0f, base * layer * 2.0f);
                out[pixel * 4 + c] = static_cast<DAVA::uint8>(value * 255.0f + 0.5f);
            }
            out[pixel * 4 + 3] = 255;
        }
    }

    DAVA::Logger::Info("[SceneParser] landscape: composited 4 tile layers into a %dx%d albedo (tiling %.2f x %.2f)",
                       width, height, tilingX, tilingY);
    return result;
}

bool SceneParser::UploadLandscapeColorMap(DAVA::Landscape* landscape)
{

    if (landscapeColorReady)
    {
        return true;
    }

    auto texture = landscape->GetMaterial()->GetEffectiveTexture(DAVA::Landscape::TEXTURE_COLOR);
    if (texture == nullptr)
    {
        return false;
    }

    DAVA::ScopedPtr<DAVA::Image> image(texture->CreateImageFromMemory());
    if (!image || image->data == nullptr || image->width == 0 || image->height == 0)
    {
        DAVA::Logger::Warning("[SceneParser] landscape colour map could not be decoded in process");
        return false;
    }

    // Beast gets ONE texture for the whole terrain, while DAVA composites four tile
    // layers in the shader. Without this the ground is only the low-res colour map:
    // no gravel, no sand stripes, no grass patches.
    if (compositeLandscapeTiles)
    {
        // Shipped colour maps are PVR/DXT (the log said format 12, not RGBA8888 = 1), and
        // CompositeLandscapeAlbedo refused anything else at its first gate: that silent
        // nullptr is why the ground never had tile detail.
        if (image->format != DAVA::FORMAT_RGBA8888)
        {
            DAVA::Image* converted = DAVA::Image::Create(image->width, image->height, DAVA::FORMAT_RGBA8888);
            if (converted != nullptr && DAVA::ImageConvert::ConvertImage(image.get(), converted))
            {
                image.reset(converted);
            }
            else
            {
                DAVA::SafeRelease(converted);
                DAVA::Logger::Warning("[SceneParser] landscape: colour map format %d could not be converted",
                                      static_cast<DAVA::int32>(image->format));
            }
        }

        if (image->format == DAVA::FORMAT_RGBA8888)
        {
            DAVA::Image* composite = CompositeLandscapeAlbedo(landscape, image.get());
            if (composite != nullptr)
            {
                image.reset(composite);
            }
        }
    }

    DAVA::Logger::Info("[SceneParser] landscape colour map: %ux%u format %d",
                       static_cast<DAVA::int32>(image->width), static_cast<DAVA::int32>(image->height),
                       static_cast<DAVA::int32>(image->format));

    DAVA::Image* toUpload = image;
    DAVA::ScopedPtr<DAVA::Image> rgba;
    if (image->format != DAVA::FORMAT_RGBA8888)
    {
        rgba = DAVA::Image::Create(image->width, image->height, DAVA::FORMAT_RGBA8888);
        if (rgba && DAVA::ImageConvert::ConvertImage(image, rgba))
        {
            toUpload = rgba;
        }
        else
        {
            DAVA::Logger::Warning("[SceneParser] landscape colour map format %d has no RGBA8888 conversion",
                                  static_cast<DAVA::int32>(image->format));
            return false;
        }
    }

    BeastTexture* beastTexture = BeastTexture::CreateWithName(BeastTexture::PointerToString(texture), beastManager);
    if (!beastTexture->InitWithPixels(toUpload))
    {
        DAVA::Logger::Warning("[SceneParser] landscape colour map was refused by Beast");
        return false;
    }

    landscapeMaterial->InitWithTextureAndDavaMaterial(beastTexture, nullptr);
    landscapeColorReady = true;
    return true;
}

void SceneParser::OnLandscapeImageCaptured(DAVA::Landscape* landscape, DAVA::Texture* landscapeTexture)
{
    if (landscapeColorReady)
    {
        // Already uploaded directly; this is the late thumbnail for the same slot.
        ContinueParsingLandscape(landscapeNode, landscape);
        return;
    }

    // The landscape colour map is one texture but it is the one every landscape
    // batch samples, so it goes through the same in-memory upload as the props:
    // no PNG on disk, nothing for Beast to decode.
    DAVA::ScopedPtr<DAVA::Image> image(landscapeTexture->CreateImageFromMemory());
    BeastTexture* beastTexture = BeastTexture::CreateWithName(BeastTexture::PointerToString(landscapeTexture), beastManager);

    DAVA::Image* toUpload = image;
    DAVA::ScopedPtr<DAVA::Image> rgba;
    if (image && image->data != nullptr && image->format != DAVA::FORMAT_RGBA8888)
    {
        rgba = DAVA::Image::Create(image->width, image->height, DAVA::FORMAT_RGBA8888);
        if (rgba && DAVA::ImageConvert::ConvertImage(image, rgba))
        {
            toUpload = rgba;
        }
    }

    if (image)
    {
        DAVA::Logger::Info("[SceneParser] landscape colour map: %ux%u format %d",
                           static_cast<DAVA::int32>(image->width), static_cast<DAVA::int32>(image->height),
                           static_cast<DAVA::int32>(image->format));
    }
    if (toUpload != nullptr && !beastTexture->InitWithPixels(toUpload))
    {
        DAVA::Logger::Warning("[SceneParser] landscape colour map was refused by Beast, the landscape stays untextured");
        beastTexture = nullptr;
    }
    landscapeMaterial->InitWithTextureAndDavaMaterial(beastTexture, nullptr);
    landscapeColorReady = true;

    ContinueParsingLandscape(landscapeNode, landscape);
}

void SceneParser::ContinueParsingLandscape(DAVA::Entity* node, DAVA::Landscape* landscape)
{
    // No FLAG_ILLUMINATION_USED gate here: shipped scenes strip the illumination
    // flags on export, so the old check rejected every landscape and Beast got no
    // terrain at all. Whether the terrain carries a lightmap is decided further
    // down, where the flag is only consulted if the user asked for it.

    if (landscape == nullptr || landscape->GetRenderBatchCount() == 0)
    {
        DAVA::Logger::Warning("[SceneParser] landscape: no render batch yet, skipping the terrain");
        FinishParsingLandscape();
        return;
    }
    DAVA::RenderBatch* landscapeBatch = landscape->GetRenderBatch(0);
    DAVA::NMaterial* davaLandscapeMaterial = (landscapeBatch != nullptr) ? landscapeBatch->GetMaterial() : nullptr;
    if (davaLandscapeMaterial == nullptr)
    {
        DAVA::Logger::Warning("[SceneParser] landscape: render batch 0 has no material, skipping the terrain");
        FinishParsingLandscape();
        return;
    }

    DAVA::Logger::Info("[SceneParser] landscape: asking for the level 0 geometry");
    LandscapeGeometry geometry;
    const bool gotGeometry = landscape->GetLevel0Geometry(geometry.vertices, geometry.indices);
    DAVA::Logger::Info("[SceneParser] landscape: level 0 geometry returned %d (%d vertices, %d indices)",
                       static_cast<DAVA::int32>(gotGeometry),
                       static_cast<DAVA::int32>(geometry.vertices.size()), static_cast<DAVA::int32>(geometry.indices.size()));
    if (!gotGeometry || geometry.vertices.empty() || geometry.indices.empty())
    {
        FinishParsingLandscape();
        return;
    }
    geometry.ComputeNormals();

    // Is the terrain actually three dimensional? A flat looking landscape in the
    // preview is either a heightmap that did not resolve or a viewer convention,
    // and one line settles which.
    {
        const DAVA::float32 big = std::numeric_limits<DAVA::float32>::max();
        DAVA::Vector3 minimum = DAVA::Vector3(big, big, big);
        DAVA::Vector3 maximum = DAVA::Vector3(-big, -big, -big);
        for (const auto& vertex : geometry.vertices)
        {
            minimum.x = DAVA::Min(minimum.x, vertex.position.x);
            minimum.y = DAVA::Min(minimum.y, vertex.position.y);
            minimum.z = DAVA::Min(minimum.z, vertex.position.z);
            maximum.x = DAVA::Max(maximum.x, vertex.position.x);
            maximum.y = DAVA::Max(maximum.y, vertex.position.y);
            maximum.z = DAVA::Max(maximum.z, vertex.position.z);
        }
        DAVA::Logger::Info("[SceneParser] landscape geometry: %d vertices, %d indices, x %.1f..%.1f, y %.2f..%.2f (relief %.2f), z %.1f..%.1f",
                           static_cast<DAVA::int32>(geometry.vertices.size()), static_cast<DAVA::int32>(geometry.indices.size()),
                           minimum.x, maximum.x, minimum.y, maximum.y, maximum.y - minimum.y, minimum.z, maximum.z);
    }

    auto name = BeastMeshInstance::PointerToString(landscape);
    BeastMesh* beastMesh = BeastMesh::CreateWithName(name, beastManager);
    beastMesh->InitWithLandscape(&geometry, landscapeMaterial);

    BeastMeshInstance* beastMeshInstance = BeastMeshInstance::CreateWithName(name, beastManager);
    beastMeshInstance->InitWithLandscape(landscape, beastMesh);
    beastMeshInstance->SetLodLevel(0);

    bool landscapeLightmap = true;
    if (beastManager->GetLightmapSelection() == BeastManager::eLightmapSelection::Flagged)
    {
        landscapeLightmap = davaLandscapeMaterial->GetEffectiveFlagValue(DAVA::NMaterialFlagName::FLAG_ILLUMINATION_USED) != 0;
    }

    if (landscapeLightmap && (beastManager->GetMode() == eBeastMode::MODE_LIGHTMAPS || beastManager->GetMode() == eBeastMode::MODE_PREVIEW || beastManager->GetMode() == eBeastMode::MODE_LIVE))
    {
        const DAVA::float32* lightmapSize = davaLandscapeMaterial->GetEffectivePropValue(DAVA::NMaterialParamName::PARAM_LIGHTMAP_SIZE);
        // terrain is one surface spanning the whole map: the shipped maps carry a single
        // 512x512 world-projected terrain lightmap (landscape/wwLm), never a per-tile target
        DAVA::float32 targetLightmapSize = (lightmapSize == nullptr) ? static_cast<DAVA::float32>(BEAST_DEFAULT_LANDSCAPE_LIGHTMAP_SIZE) : *lightmapSize;
        if (targetLightmapSize <= static_cast<DAVA::float32>(DAVA::NMaterial::DEFAULT_LIGHTMAP_SIZE))
        {
            targetLightmapSize = static_cast<DAVA::float32>(BEAST_DEFAULT_LANDSCAPE_LIGHTMAP_SIZE);
        }
        beastMeshInstance->SetLightmapSize(static_cast<DAVA::int32>(targetLightmapSize));
        if (beastManager->IsResumingExistingBakes() && beastManager->HasBakedLandscapeResult())
        {
            DAVA::Logger::Info("[SceneParser] landscape already baked, skipped");
        }
        else
        {
            beastMeshInstance->UseLightMap();
        }
    }

    FinishParsingLandscape();
}

void SceneParser::ClearScene()
{
    BeastTexture::ReleaseResources();
    BeastMaterial::ReleaseResources();
    BeastMesh::ReleaseResources();
    BeastMeshInstance::ReleaseResources();
    BeastLight::ReleaseResources();
    BeastPointCloud::ReleaseResources();

    shouldCancel = false;
    entitiesParsed = false;
    landscapeTextureProcessed = false;
    dumpedTextureCount = 0;
    landscapeMaterial = nullptr;
    landscapeNode = nullptr;
}

void SceneParser::SetLodLevel(DAVA::int32 _lodLevel)
{
    lodLevel = _lodLevel;
}

void SceneParser::SetSwitchIndex(DAVA::int32 _switchIndex)
{
    switchIndex = _switchIndex;
}

bool SceneParser::ShouldCancel() const
{
    return shouldCancel;
}

bool SceneParser::IsMaterialTemplateContainString(DAVA::NMaterial* material, const DAVA::String& _str)
{
    if (material)
    {
        DAVA::String str(_str);
        DAVA::String fxNameString(material->GetEffectiveFXName().c_str());
        std::transform(str.begin(), str.end(), str.begin(), ::tolower);
        std::transform(fxNameString.begin(), fxNameString.end(), fxNameString.begin(), ::tolower);
        return fxNameString.find(str) != DAVA::String::npos;
    }
    return false;
}

DAVA::FilePath SceneParser::GetTemporaryFolder()
{
    return DAVA::FileSystem::Instance()->GetCurrentWorkingDirectory() + "temp_beast/";
}

DAVA::FilePath SceneParser::GetLandscapeTemporaryFileName()
{
    return GetTemporaryFolder() + "temp_landscape_color.png";
}
}