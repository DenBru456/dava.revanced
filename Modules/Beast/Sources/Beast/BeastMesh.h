#pragma once

#include "Beast/BeastTypes.h"
#include "Beast/BeastResource.h"
#include "Beast/BeastManager.h"

#include <Base/BaseTypes.h>

namespace DAVA
{
class NMaterial;
class RenderBatch;
class PolygonGroup;
}

namespace Beast
{
class LandscapeGeometry;
class BeastMesh : public BeastResource<BeastMesh>
{
public:
    void InitWithMeshInstancePart(DAVA::RenderBatch* batch, DAVA::int32 partIndex);
    void InitWithSpeedTreeLeaf(DAVA::RenderBatch* batch, DAVA::int32 partIndex, const DAVA::Matrix4& leafTransform);

    void InitWithLandscape(LandscapeGeometry* geometry, BeastMaterial* material);

    ILBMeshHandle GetILBMesh() const;
    DAVA::int32 GetTextureCoordCount() const;

private:
    friend class BeastResource<BeastMesh>;
    BeastMesh(const DAVA::String& name, BeastManager* manager);

    bool CheckVertexFormat(DAVA::PolygonGroup* polygonGroup);
    bool HasTangents(DAVA::PolygonGroup* polygonGroup);

    // Beast keeps meshes for the whole manager (ILBBeginMesh takes the manager,
    // not the scene) and rejects a second mesh with identical content. Naming a
    // mesh after a hash of its content makes the manager cache hit instead:
    // a rebuilt LOD pass reuses the mesh it uploaded before, and two entities
    // that share geometry share one mesh, which is what Beast wants.
    //
    // The result has to be three states, not a bool: "Created" means the caller
    // must fill the mesh, "Reused" means it is already finalised in Beast and any
    // further ILBAddVertexData is answered with "Mesh already Finalized".
    enum class eBeginResult
    {
        Created,
        Reused,
        Failed
    };
    eBeginResult BeginBeastMesh(DAVA::uint64 contentHash);
    DAVA::uint64 ComputeContentHash(DAVA::PolygonGroup* polygonGroup, DAVA::NMaterial* material) const;
    DAVA::uint64 ComputeContentHash(LandscapeGeometry* geometry, BeastMaterial* material) const;

    void AddVertices(DAVA::PolygonGroup* polygonGroup);
    void AddTranfromedVertices(DAVA::PolygonGroup* polygonGroup, const DAVA::Matrix4&);
    void AddIndices(DAVA::PolygonGroup* polygonGroup, DAVA::NMaterial* material);
    void AddUV(DAVA::PolygonGroup* polygonGroup, DAVA::int32 textureCoordIndex);
    void AddVertices(LandscapeGeometry* geometry);
    void AddTangents(DAVA::PolygonGroup* polygonGroup);
    void AddIndices(LandscapeGeometry* geometry, BeastMaterial* material);
    void AddUV(LandscapeGeometry* geometry);

private:
    ILBMeshHandle mesh = nullptr;
    DAVA::int32 indecesCount = 0;
    DAVA::int32 textureCoordCount = 0;
};
}