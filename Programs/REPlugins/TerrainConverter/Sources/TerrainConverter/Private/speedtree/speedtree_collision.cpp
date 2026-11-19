/******************************************************************************
BigWorld Technology
Copyright BigWorld Pty, Ltd.
All Rights Reserved. Commercial in confidence.

RECONSTRUCTED FILE — the original definition of createBSPTree() was stripped
when the TerrainConverter plugin was open-sourced (the published
speedtree_collision.hpp declares it and ServerSPT.cpp calls it, but no file in
the public tree defines it, hence the LNK2001 at plugin link time). The
semantics follow the BigWorld ancestor lib/speedtree/speedtree_collision.cpp:
collect collision triangles, build the BSPTree, then save a standalone cached
"<name>.bsp2" file next to the model.
******************************************************************************/

/**
 *	@file
 */

#include "speedtree_collision.hpp"

#include "../physics2/DataSection.hpp"
#include "../physics2/bsp.hpp"
#include "../physics2/worldtri.hpp"

#include <Base/BaseTypes.h>
#include <FileSystem/File.h>
#include <FileSystem/FilePath.h>
#include <Logger/Logger.h>

#include <Math/TransformUtils.h>
#include <Scene3D/Components/ComponentHelpers.h>
#include <Scene3D/Components/RenderComponent.h>
#include <Scene3D/Components/TransformComponent.h>
#include <Scene3D/Entity.h>

#include <REPlatform/Deprecated/EditorConfig.h>

#include <string>
#include <vector>

namespace
{

DAVA::Matrix4 GetEntityWorldTransform(DAVA::Entity* node)
{
    if (node == NULL)
    {
        return DAVA::Matrix4::IDENTITY;
    }

    const DAVA::TransformComponent* transformComponent =
    node->GetComponent<DAVA::TransformComponent>();
    if (transformComponent == NULL)
    {
        return DAVA::Matrix4::IDENTITY;
    }

    return DAVA::TransformUtils::ToMatrix(transformComponent->GetWorldTransform());
}

/**
 *	Material-kind heuristic. The original WG code classified each mesh from
 *	SpeedTree data; with a converted Dava entity only the material name is
 *	available. Foliage-looking material names collide as SPT_COLLMAT_LEAVES,
 *	everything else as SPT_COLLMAT_SOLID.
 */
bool LooksLikeFoliage(const DAVA::String& materialName)
{
    static const char* foliageMarkers[] = {
        "leaf", "leaves", "foliage", "billboard"
    };

    DAVA::String lower(materialName);
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (size_t i = 0; i < sizeof(foliageMarkers) / sizeof(foliageMarkers[0]); ++i)
    {
        if (lower.find(foliageMarkers[i]) != DAVA::String::npos)
        {
            return true;
        }
    }

    return false;
}

/**
 *	Collects world-space collision triangles from one entity's render object
 *	(only the highest LOD layer of each switch index, mirroring
 *	Model::FindMaxLodIndex) and recurses into children.
 */
void CollectEntityTriangles(DAVA::Entity* entity, RealWTriangleSet& out)
{
    if (entity == NULL)
    {
        return;
    }

    const DAVA::Matrix4 world = GetEntityWorldTransform(entity);

    DAVA::RenderObject* renderObject = DAVA::GetRenderObject(entity);
    // Tree sc2 entities may carry their geometry on a SpeedTreeObject
    // (TYPE_SPEED_TREE) rather than a Mesh; both expose standard render
    // batches, so collect from either.
    if (renderObject != NULL &&
        (renderObject->GetType() == DAVA::RenderObject::TYPE_MESH ||
         renderObject->GetType() == DAVA::RenderObject::TYPE_SPEED_TREE))
    {
        const DAVA::uint32 batchCount = renderObject->GetRenderBatchCount();

        // Highest LOD layer per switch index (so shared geometry is not
        // double-counted across LOD levels).
        std::vector<DAVA::int32> maxLodPerSwitch;

        for (DAVA::uint32 k = 0; k < batchCount; ++k)
        {
            DAVA::int32 lodIndex = -1;
            DAVA::int32 switchIndex = -1;
            renderObject->GetRenderBatch(k, lodIndex, switchIndex);

            // Standalone batches (switchIndex == -1, e.g. SpeedTreeObject's
            // own batches) have no LOD group to track - indexing
            // maxLodPerSwitch[-1] would read out of bounds. They are
            // collected directly in the pass below instead.
            if (switchIndex < 0)
            {
                continue;
            }
            if (switchIndex >= static_cast<DAVA::int32>(maxLodPerSwitch.size()))
            {
                maxLodPerSwitch.resize(switchIndex + 1, -1);
            }
            if (lodIndex > maxLodPerSwitch[switchIndex])
            {
                maxLodPerSwitch[switchIndex] = lodIndex;
            }
        }

        for (DAVA::uint32 k = 0; k < batchCount; ++k)
        {
            DAVA::int32 lodIndex = -1;
            DAVA::int32 switchIndex = -1;
            DAVA::RenderBatch* batch =
            renderObject->GetRenderBatch(k, lodIndex, switchIndex);

            // Batches registered without LOD structure (lodIndex/switchIndex
            // == -1, e.g. a SpeedTreeObject's own batches or a plain mesh
            // added via AddRenderBatch(batch)) are standalone geometry -
            // always collect them. Managed batches still dedupe to the
            // highest LOD layer per switch index.
            const bool standalone = (switchIndex < 0);
            if (!standalone &&
                (switchIndex >= static_cast<DAVA::int32>(maxLodPerSwitch.size()) ||
                 lodIndex != maxLodPerSwitch[switchIndex]))
            {
                continue; // lower-LOD duplicate of the same geometry
            }

            DAVA::PolygonGroup* polygonGroup = batch->GetPolygonGroup();
            if (polygonGroup == NULL || polygonGroup->GetPrimitiveCount() == 0)
            {
                continue;
            }

            // Collision flags follow the WG convention used by Model.cpp:
            // solid mesh: double-sided, transparent to projectiles;
            // foliage: additionally no tank collision.
            const bool foliage = LooksLikeFoliage(
            batch->GetMaterial() != NULL ?
            batch->GetMaterial()->GetMaterialName().c_str() :
            "");

            DAVA::int32 collisionFlags = DAVA::int32(TRIANGLE_DOUBLESIDED |
                                                     TRIANGLE_PROJECTILENOCOLLIDE);
            if (foliage)
            {
                collisionFlags |= DAVA::int32(TRIANGLE_NOCOLLIDE);
            }

            const DAVA::int32 materialKind =
            foliage ? SPT_COLLMAT_LEAVES : SPT_COLLMAT_SOLID;

            const WorldTriangle::Flags flags =
            WorldTriangle::packFlags(collisionFlags, materialKind);

            const DAVA::int32 vertexCount = polygonGroup->GetVertexCount();
            const DAVA::int32 indexCount = polygonGroup->GetPrimitiveCount() * 3;

            // Cache world-space vertices.
            std::vector<DAVA::Vector3> worldPositions(vertexCount);
            for (DAVA::int32 i = 0; i < vertexCount; ++i)
            {
                polygonGroup->GetCoord(i, worldPositions[i]);
                worldPositions[i] = worldPositions[i] * world;
            }

            for (DAVA::int32 i = 0; (i + 2) < indexCount; i += 3)
            {
                DAVA::int32 i0 = 0;
                DAVA::int32 i1 = 0;
                DAVA::int32 i2 = 0;
                polygonGroup->GetIndex(i, i0);
                polygonGroup->GetIndex(i + 1, i1);
                polygonGroup->GetIndex(i + 2, i2);

                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
                {
                    continue;
                }

                const WorldTriangle triangle(worldPositions[i0],
                                             worldPositions[i1],
                                             worldPositions[i2],
                                             flags);

                // Skip degenerates — they cannot be partitioned usefully.
                if (triangle.normal().SquareLength() < 0.000001f)
                {
                    continue;
                }

                out.push_back(triangle);
            }
        }
    }

    const DAVA::int32 childCount = entity->GetChildrenCount();
    for (DAVA::int32 child = 0; child < childCount; ++child)
    {
        CollectEntityTriangles(entity->GetChild(child), out);
    }
}

} // anonymous namespace

/**
 *	Builds a server-side collision BSP from the (already world-transformed)
 *	entity tree of a converted SpeedTree model and saves the standalone
 *	cache file "<filename>.bsp2" next to it, following the BigWorld
 *	speedtree_collision.cpp convention (getBSPFileName).
 */
BSPTree* createBSPTree(
DAVA::Entity* entity,
const DAVA::String& filename)
{
    RealWTriangleSet triangles;
    CollectEntityTriangles(entity, triangles);

    if (triangles.empty())
    {
        // An empty tree partitions nothing: its bounding box stays
        // inside-out and BSPTree::save would assert in BoundingBox.
        // Skip the .bsp2 cache entirely (the server treats a missing
        // cache as "no collision geometry" for the tree).
        DAVA::Logger::Info(
        "createBSPTree: no collision triangles in %s - skipping .bsp2", filename.c_str());
        return NULL;
    }

    BSPTree* pTree = new BSPTree(triangles);

    // Save the cached .bsp2 alongside the model.
    DataSection bspSection("bsp2");
    if (pTree->save(&bspSection))
    {
        BinaryPtr block = bspSection.asBinary();
        if (block != NULL && !block->empty())
        {
            const DAVA::FilePath bspPath(filename + ".bsp2");

            DAVA::File* file = DAVA::File::Create(
            bspPath, DAVA::File::CREATE | DAVA::File::WRITE);
            if (file != NULL)
            {
                file->Write(&(*block)[0],
                            static_cast<DAVA::uint32>(block->size()));
                file->Release();
            }
            else
            {
                DAVA::Logger::Warning(
                "createBSPTree: could not write %s", bspPath.GetAbsolutePathname().c_str());
            }
        }
    }

    return pTree;
}

// speedtree_collision.cpp
