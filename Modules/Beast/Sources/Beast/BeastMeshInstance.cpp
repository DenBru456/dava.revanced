#include "Beast/BeastMeshInstance.h"
#include "Beast/BeastDebug.h"
#include "Beast/BeastManager.h"
#include "Beast/BeastMesh.h"
#include "Beast/SceneParser.h"

#include <Render/Highlevel/Landscape.h>
#include <Render/Material/NMaterial.h>
#include <Render/Material/NMaterialNames.h>
#include <beastapi/beastinstance.h>
#include <Logger/Logger.h>

namespace Beast
{
BeastMeshInstance::BeastMeshInstance(const DAVA::String& name, BeastManager* manager)
    : BeastResource(name, manager)
{
}

void BeastMeshInstance::InitWithRenderBatchAndTransform(DAVA::RenderBatch* batch, DAVA::int32 _partIndex, const DAVA::Matrix4& transform)
{
    if (nullptr == meshInstanceHandle)
    {
        renderBatch = batch;
        partIndex = _partIndex;
        buddyMesh = BeastMesh::FindResourceByName(resourceName);

        DVASSERT(buddyMesh);
        if (buddyMesh == nullptr)
        {
            // the mesh upload failed earlier in this pass; creating an instance on a null
            // mesh handle fails deep in Beast and buries the real message, so stop here
            DAVA::Logger::Error("[BeastMeshInstance] '%s' has no Beast mesh, instance skipped", resourceName.c_str());
            return;
        }

        ILBMatrix4x4 beastMatrix = ConvertDavaMatrix(transform);
        BEAST_VERIFY(ILBCreateInstance(manager->GetILBScene(), buddyMesh->GetILBMesh(), STRING_TO_BEAST_STRING(resourceName), &beastMatrix, &meshInstanceHandle));
        BEAST_VERIFY(ILBSetRenderStats(meshInstanceHandle, ILB_RS_DOUBLE_SIDED, ILB_RSOP_ENABLE));
        GetCastReceiveShadowOptions(renderBatch);
    }
}

void BeastMeshInstance::InitWithLandscape(DAVA::Landscape* landscape, BeastMesh* beastMesh)
{
    if (nullptr == meshInstanceHandle)
    {
        isLandscape = true;
        renderBatch = landscape->GetRenderBatch(0);
        buddyMesh = beastMesh;
        if (buddyMesh == nullptr)
        {
            DAVA::Logger::Error("[BeastMeshInstance] landscape has no Beast mesh, instance skipped");
            isLandscape = false;
            return;
        }

        ILBMatrix4x4 beastMatrix = ConvertDavaMatrix(DAVA::Matrix4::IDENTITY);
        BEAST_VERIFY(ILBCreateInstance(manager->GetILBScene(), beastMesh->GetILBMesh(), STRING_TO_BEAST_STRING(resourceName), &beastMatrix, &meshInstanceHandle));
        BEAST_VERIFY(ILBSetRenderStats(meshInstanceHandle, ILB_RS_DOUBLE_SIDED, ILB_RSOP_ENABLE));
        GetCastReceiveShadowOptions(renderBatch);
    }
}

namespace
{
// The illumination flags live in the material only while a scene is being authored.
// Scene::OptimizeBeforeExport() strips them on save, so a shipped .sc2 carries none
// of them at all and GetEffectiveFlagValue() answers 0 for every material. The stock
// implementation read that 0 as "the artist turned it off" and disabled casting,
// receiving and GI visibility on every single instance, which is why a bake of game
// content had no shadows, no occlusion and no contrast (a uniform bright wash).
// Correct reading of the data:
//   flag present and 0 -> the author disabled it
//   flag absent         -> keep Beast's default, which is enabled
bool IsIlluminationFlagExplicitlyOff(DAVA::NMaterial* material, const DAVA::FastName& flagName)
{
    for (DAVA::NMaterial* level = material; level != nullptr; level = level->GetParent())
    {
        if (level->HasLocalFlag(flagName))
        {
            return level->GetLocalFlagValue(flagName) == 0;
        }
    }

    return false;
}
} // namespace

void BeastMeshInstance::GetCastReceiveShadowOptions(DAVA::RenderBatch* batch)
{
    DAVA::NMaterial* material = batch->GetMaterial();
    if (material == nullptr)
    {
        return;
    }

    int32 flags = 0;
    if (IsIlluminationFlagExplicitlyOff(material, DAVA::NMaterialFlagName::FLAG_ILLUMINATION_SHADOW_CASTER))
    {
        flags |= ILB_RS_CAST_SHADOWS | ILB_RS_VISIBLE_FOR_GI;
    }

    if (IsIlluminationFlagExplicitlyOff(material, DAVA::NMaterialFlagName::FLAG_ILLUMINATION_SHADOW_RECEIVER))
    {
        flags |= ILB_RS_RECEIVE_SHADOWS;
    }

    if (flags > 0)
    {
        BEAST_VERIFY(ILBSetRenderStats(meshInstanceHandle, flags, ILB_RSOP_DISABLE));
    }
}

ILBInstanceHandle BeastMeshInstance::GetILBMeshInstance()
{
    return meshInstanceHandle;
}

DAVA::int32 BeastMeshInstance::GetTextureCoordCount()
{
    return (buddyMesh == nullptr) ? 1 : buddyMesh->GetTextureCoordCount();
}

TextureTarget* BeastMeshInstance::GetTextureTarget()
{
    return textureTarget;
}

void BeastMeshInstance::SetTextureTarget(TextureTarget* val)
{
    textureTarget = val;
}

BeastMeshInstance::~BeastMeshInstance()
{
    DAVA::SafeDelete(textureTarget);
}

BeastMesh* BeastMeshInstance::GetBuddyMesh()
{
    return buddyMesh;
}

int32 BeastMeshInstance::GetLightmapSize()
{
    return lightmapSize;
}

void BeastMeshInstance::SetLightmapSize(int32 size)
{
    lightmapSize = size;
}

void BeastMeshInstance::SetLodLevel(int32 _lodLevel)
{
    lodLevel = _lodLevel;
}

int32 BeastMeshInstance::GetLodLevel()
{
    return lodLevel;
}

void BeastMeshInstance::UseLightMap()
{
    useLightmap = true;
}

bool BeastMeshInstance::IsUseLightmap()
{
    return useLightmap;
}

DAVA::String BeastMeshInstance::GetMeshInstancePointerString()
{
    return DAVA::String(DAVA::Format("%s", BeastMesh::PointerToString(renderBatch).c_str()));
}

DAVA::RenderBatch* BeastMeshInstance::GetRenderBatch()
{
    return renderBatch;
}
}