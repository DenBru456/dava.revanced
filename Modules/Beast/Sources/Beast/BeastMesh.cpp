#include "Beast/BeastMesh.h"
#include "Beast/BeastDebug.h"
#include "Beast/BeastManager.h"
#include "Beast/BeastMaterial.h"
#include "Beast/LandscapeGeometry.h"

#include <Logger/Logger.h>
#include <Render/Material/NMaterial.h>

#include <cstring>

namespace Beast
{
namespace
{
const DAVA::uint64 HASH_SEED = 1469598103934665603ull; // FNV-1a 64

void HashBytes(DAVA::uint64& hash, const void* data, size_t size)
{
    const DAVA::uint8* bytes = static_cast<const DAVA::uint8*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= static_cast<DAVA::uint64>(bytes[i]);
        hash *= 1099511628211ull;
    }
}

void HashInt(DAVA::uint64& hash, DAVA::int32 value)
{
    HashBytes(hash, &value, sizeof(value));
}

void HashFloat(DAVA::uint64& hash, float value)
{
    DAVA::uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    HashBytes(hash, &bits, sizeof(bits));
}

void HashString(DAVA::uint64& hash, const DAVA::String& value)
{
    HashInt(hash, static_cast<DAVA::int32>(value.size()));
    HashBytes(hash, value.data(), value.size());
}

DAVA::String ToHex16(DAVA::uint64 value)
{
    static const char* digits = "0123456789abcdef";
    char text[17] = {};
    for (DAVA::int32 i = 15; i >= 0; --i)
    {
        text[i] = digits[value & 0xf];
        value >>= 4;
    }
    return DAVA::String(text);
}
}

BeastMesh::BeastMesh(const DAVA::String& name, BeastManager* manager)
    : BeastResource(name, manager)
{
}

void BeastMesh::InitWithSpeedTreeLeaf(DAVA::RenderBatch* batch, DAVA::int32 partIndex, const DAVA::Matrix4& leafTransform)
{
    if (mesh != nullptr)
    {
        return; // this pass already has the finished geometry
    }

    DAVA::PolygonGroup* polygonGroup = batch->GetPolygonGroup();
    if (CheckVertexFormat(polygonGroup))
    {
        // the leaf transform is baked into the vertices, and the mesh name
        // carries the render batch pointer, so the hash covers the result
        if (BeginBeastMesh(ComputeContentHash(polygonGroup, batch->GetMaterial())) != eBeginResult::Created)
        {
            return;
        }
        AddTranfromedVertices(polygonGroup, leafTransform);
        AddIndices(polygonGroup, batch->GetMaterial());
        textureCoordCount = polygonGroup->textureCoordCount;
        for (int32 i = 0; i < textureCoordCount; ++i)
        {
            BEAST_VERIFY(ILBBeginUVLayer(mesh, STRING_TO_BEAST_STRING(DAVA::Format("%d", i))));
            AddUV(polygonGroup, i);
            BEAST_VERIFY(ILBEndUVLayer(mesh));
        }
        BEAST_VERIFY(ILBEndMesh(mesh));
    }
}

void BeastMesh::InitWithMeshInstancePart(DAVA::RenderBatch* batch, DAVA::int32 partIndex)
{
    if (mesh != nullptr)
    {
        return; // this pass already has the finished geometry
    }

    DAVA::PolygonGroup* polygonGroup = batch->GetPolygonGroup();
    if (CheckVertexFormat(polygonGroup))
    {
        if (BeginBeastMesh(ComputeContentHash(polygonGroup, batch->GetMaterial())) != eBeginResult::Created)
        {
            return;
        }
        AddVertices(polygonGroup);
        AddIndices(polygonGroup, batch->GetMaterial());
        textureCoordCount = polygonGroup->textureCoordCount;
        for (int32 i = 0; i < textureCoordCount; ++i)
        {
            BEAST_VERIFY(ILBBeginUVLayer(mesh, STRING_TO_BEAST_STRING(DAVA::Format("%d", i))));
            AddUV(polygonGroup, i);
            BEAST_VERIFY(ILBEndUVLayer(mesh));
        }
        BEAST_VERIFY(ILBEndMesh(mesh));
    }
}

bool BeastMesh::CheckVertexFormat(DAVA::PolygonGroup* polygonGroup)
{
    DAVA::int32 vertexFormat = polygonGroup->GetFormat();
    if ((vertexFormat & DAVA::EVF_VERTEX) == 0)
    {
        DAVA::Logger::Error("BeastMesh::CheckVertexFormat wrong vertex format");
        return false;
    }

    return true;
}

void BeastMesh::AddTranfromedVertices(DAVA::PolygonGroup* polygonGroup, const DAVA::Matrix4& transform)
{
    DAVA::int32 vertexCount = polygonGroup->GetVertexCount();
    DAVA::Vector<ILBVec3> coordData(vertexCount);
    DAVA::Vector<ILBVec3> normalData(vertexCount);

    DAVA::int32 vertexFormat = polygonGroup->GetFormat();
    DAVA::Vector4 vertexPivot;
    DAVA::Vector3 vertexDataCoord;
    DAVA::Vector3 vertexDataNormal;

    for (DAVA::int32 vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
    {
        polygonGroup->GetCoord(vertexIndex, vertexDataCoord);

        if (vertexFormat & DAVA::EVF_PIVOT4)
        {
            polygonGroup->GetPivot(vertexIndex, vertexPivot);
        }

        vertexDataCoord = DAVA::Vector3(vertexPivot) + DAVA::MultiplyVectorMat3x3(vertexDataCoord - DAVA::Vector3(vertexPivot), transform);

        if (vertexFormat & DAVA::EVF_NORMAL)
        {
            polygonGroup->GetNormal(vertexIndex, vertexDataNormal);
            vertexDataNormal = DAVA::MultiplyVectorMat3x3(vertexDataNormal, transform);
            vertexDataNormal.Normalize();
        }

        coordData[vertexIndex] = ILBVec3(vertexDataCoord.x, vertexDataCoord.y, vertexDataCoord.z);
        normalData[vertexIndex] = ILBVec3(vertexDataNormal.x, vertexDataNormal.y, vertexDataNormal.z);
    }

    BEAST_VERIFY(ILBAddVertexData(mesh, coordData.data(), normalData.data(), vertexCount));
}

void BeastMesh::AddVertices(DAVA::PolygonGroup* polygonGroup)
{
    DAVA::int32 vertexCount = polygonGroup->GetVertexCount();
    DAVA::Vector<ILBVec3> coordData(vertexCount);
    DAVA::Vector<ILBVec3> normalData(vertexCount);

    DAVA::int32 vertexFormat = polygonGroup->GetFormat();
    DAVA::Vector3 vertexDataCoord;
    DAVA::Vector3 vertexDataNormal;

    for (DAVA::int32 vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
    {
        polygonGroup->GetCoord(vertexIndex, vertexDataCoord);
        coordData[vertexIndex] = ILBVec3(vertexDataCoord.x, vertexDataCoord.y, vertexDataCoord.z);

        if (vertexFormat & DAVA::EVF_NORMAL)
        {
            polygonGroup->GetNormal(vertexIndex, vertexDataNormal);
            vertexDataNormal.Normalize();
        }

        normalData[vertexIndex] = ILBVec3(vertexDataNormal.x, vertexDataNormal.y, vertexDataNormal.z);
    }

    BEAST_VERIFY(ILBAddVertexData(mesh, coordData.data(), normalData.data(), vertexCount));
}

void BeastMesh::AddTangents(DAVA::PolygonGroup* polygonGroup)
{
    DAVA::int32 vertexCount = polygonGroup->GetVertexCount();
    DAVA::Vector<ILBVec3> tangentData(vertexCount);
    DAVA::Vector<ILBVec3> biTangentData(vertexCount);

    DAVA::Vector3 vertexDataNormal;
    DAVA::Vector3 vertexDataTangent;
    for (DAVA::int32 vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
    {
        polygonGroup->GetTangent(vertexIndex, vertexDataTangent);
        vertexDataTangent.Normalize();
        DAVA::Vector3 biTangent = vertexDataNormal.CrossProduct(vertexDataTangent);

        tangentData[vertexIndex] = ILBVec3(vertexDataTangent.x, vertexDataTangent.y, vertexDataTangent.z);
        biTangentData[vertexIndex] = ILBVec3(biTangent.x, biTangent.y, biTangent.z);
    }

    BEAST_VERIFY(ILBAddTangentData(mesh, tangentData.data(), biTangentData.data(), vertexCount));
}

void BeastMesh::AddIndices(DAVA::PolygonGroup* polygonGroup, DAVA::NMaterial* material)
{
    BEAST_VERIFY(ILBBeginMaterialGroup(mesh, STRING_TO_BEAST_STRING(PointerToString(material))));
    DAVA::int32 vertexCount = polygonGroup->GetVertexCount();
    indecesCount = polygonGroup->GetPrimitiveCount() * 3;
    DAVA::Vector<DAVA::int32> indeces(indecesCount);
    DAVA::int32 sourceIndex;
    for (DAVA::int32 indexOfIndex = 0; indexOfIndex < indecesCount; indexOfIndex++)
    {
        polygonGroup->GetIndex(indexOfIndex, sourceIndex);
        indeces[indexOfIndex] = sourceIndex;
    }
    BEAST_VERIFY(ILBAddTriangleData(mesh, indeces.data(), indecesCount));
    BEAST_VERIFY(ILBEndMaterialGroup(mesh));
}

void BeastMesh::AddUV(DAVA::PolygonGroup* polygonGroup, DAVA::int32 textureCoordIndex)
{
    DAVA::Vector2 texCoord;
    DAVA::int32 vertexCount = polygonGroup->GetVertexCount();
    DAVA::Vector<ILBVec2> uvData(vertexCount);
    for (DAVA::int32 i = 0; i < vertexCount; ++i)
    {
        polygonGroup->GetTexcoord(textureCoordIndex, i, texCoord);
        uvData[i] = ILBVec2(texCoord.x, 1.f - texCoord.y);
    }
    BEAST_VERIFY(ILBAddUVData(mesh, uvData.data(), vertexCount));
}

void BeastMesh::InitWithLandscape(LandscapeGeometry* geometry, BeastMaterial* material)
{
    if (nullptr == mesh)
    {
        if (BeginBeastMesh(ComputeContentHash(geometry, material)) != eBeginResult::Created)
        {
            // Reused: the landscape is a single 263k-vertex mesh and the next LOD
            // pass must not push it into Beast again.
            return;
        }
        // One line per step: a crash dump with a null dereference deep in the CRT says nothing
        // about which call handed over the bad pointer, but the last line here does.
        DAVA::Logger::Info("[BeastMesh] terrain: mesh begun, filling %d vertices / %d indices (normals %d)",
                           static_cast<int32>(geometry->vertices.size()), static_cast<int32>(geometry->indices.size()),
                           static_cast<int32>(geometry->normals.size()));
        AddVertices(geometry);
        DAVA::Logger::Info("[BeastMesh] terrain: vertex data accepted");
        AddIndices(geometry, material);
        DAVA::Logger::Info("[BeastMesh] terrain: triangles accepted, material group '%s'",
                           (material != nullptr) ? material->GetName().c_str() : "none");

        BEAST_VERIFY(ILBBeginUVLayer(mesh, CONST_STRING_TO_BEAST_STRING("0")));
        AddUV(geometry);
        BEAST_VERIFY(ILBEndUVLayer(mesh));
        DAVA::Logger::Info("[BeastMesh] terrain: uv layer accepted");

        BEAST_VERIFY(ILBEndMesh(mesh));
        DAVA::Logger::Info("[BeastMesh] terrain: mesh finished");
    }
}

void BeastMesh::AddVertices(LandscapeGeometry* geometry)
{
    DAVA::size_type vertexCount = geometry->vertices.size();
    DAVA::Vector<ILBVec3> coordData(vertexCount);
    DAVA::Vector<ILBVec3> normalData(vertexCount);

    // A normals vector shorter than the vertex buffer is an out of range read on every vertex and
    // the one place in this path where a bad pointer can be handed straight to the CRT.
    if (geometry->normals.size() != vertexCount)
    {
        DAVA::Logger::Warning("[BeastMesh] terrain: %d normals for %d vertices, recomputing",
                              static_cast<int32>(geometry->normals.size()), static_cast<int32>(vertexCount));
        geometry->ComputeNormals();
    }

    for (DAVA::size_type i = 0; i < vertexCount; ++i)
    {
        const DAVA::Vector3& position = geometry->vertices[i].position;
        const DAVA::Vector3& normal = (i < geometry->normals.size()) ? geometry->normals[i] : DAVA::Vector3(0.0f, 0.0f, 1.0f);

        coordData[i] = ILBVec3(position.x, position.y, position.z);
        normalData[i] = ILBVec3(normal.x, normal.y, normal.z);
    }

    BEAST_VERIFY(ILBAddVertexData(mesh, coordData.data(), normalData.data(), static_cast<int32>(vertexCount)));
}

void BeastMesh::AddIndices(LandscapeGeometry* geometry, BeastMaterial* material)
{
    BEAST_VERIFY(ILBBeginMaterialGroup(mesh, STRING_TO_BEAST_STRING(material->GetName())));
    BEAST_VERIFY(ILBAddTriangleData(mesh, geometry->indices.data(), static_cast<int32>(geometry->indices.size())));
    BEAST_VERIFY(ILBEndMaterialGroup(mesh));
}

void BeastMesh::AddUV(LandscapeGeometry* geometry)
{
    DAVA::size_type vertexCount = geometry->vertices.size();
    DAVA::Vector<ILBVec2> uvData(vertexCount);
    for (DAVA::size_type i = 0; i < vertexCount; ++i)
    {
        const DAVA::Vector2& texCoord = geometry->vertices[i].texCoord;
        uvData[i] = ILBVec2(texCoord.x, texCoord.y);
    }
    BEAST_VERIFY(ILBAddUVData(mesh, uvData.data(), static_cast<DAVA::int32>(vertexCount)));
}

BeastMesh::eBeginResult BeastMesh::BeginBeastMesh(DAVA::uint64 contentHash)
{
    // The name is the identity Beast caches on, so it has to describe the data.
    const DAVA::String beastName = DAVA::Format("%s_%s", resourceName.c_str(), ToHex16(contentHash).c_str());

    ILBMeshHandle cached = nullptr;
    if (ILBFindMesh(manager->GetILBManager(), STRING_TO_BEAST_STRING(beastName), &cached) == ILB_ST_SUCCESS && cached != nullptr)
    {
        mesh = cached;
        // Finalised in an earlier pass: keep the handle, add nothing.
        return eBeginResult::Reused;
    }

    const ILBStatus status = ILBBeginMesh(manager->GetILBManager(), STRING_TO_BEAST_STRING(beastName), &mesh);
    if (status != ILB_ST_SUCCESS || mesh == nullptr)
    {
        // Stop here instead of letting AddVertexData / BeginMaterialGroup /
        // AddTriangleData each fail on a null handle: that turned one bad mesh
        // into about ten error lines.
        ILBStringHandle extendedError = nullptr;
        ILBGetExtendErrorInformation(&extendedError);
        DAVA::Logger::Error("[BeastMesh] ILBBeginMesh failed for '%s' (status %d): %s",
                            beastName.c_str(), static_cast<DAVA::int32>(status), ConvertBeastString(extendedError).c_str());
        mesh = nullptr;
        return eBeginResult::Failed;
    }

    return eBeginResult::Created;
}

DAVA::uint64 BeastMesh::ComputeContentHash(DAVA::PolygonGroup* polygonGroup, DAVA::NMaterial* material) const
{
    DAVA::uint64 hash = HASH_SEED;
    if (polygonGroup == nullptr)
    {
        return hash;
    }

    const DAVA::int32 vertexCount = polygonGroup->GetVertexCount();
    const DAVA::int32 primitiveCount = polygonGroup->GetPrimitiveCount();
    const DAVA::int32 format = polygonGroup->GetFormat();
    HashInt(hash, vertexCount);
    HashInt(hash, primitiveCount);
    HashInt(hash, format);
    HashInt(hash, polygonGroup->textureCoordCount);

    if (material != nullptr)
    {
        // GetMaterialName() is a FastName, and a FastName without a name is a null pointer, not
        // an empty string: DAVA::String(nullptr) is a strlen(NULL) crash.
        const DAVA::FastName& materialName = material->GetMaterialName();
        HashString(hash, materialName.IsValid() ? DAVA::String(materialName.c_str()) : DAVA::String());
    }

    const bool hasNormal = (format & DAVA::EVF_NORMAL) != 0;
    const bool hasPivot = (format & DAVA::EVF_PIVOT4) != 0;

    DAVA::Vector3 coord;
    DAVA::Vector3 normal;
    DAVA::Vector4 pivot;
    for (DAVA::int32 i = 0; i < vertexCount; ++i)
    {
        polygonGroup->GetCoord(i, coord);
        HashFloat(hash, coord.x);
        HashFloat(hash, coord.y);
        HashFloat(hash, coord.z);

        if (hasNormal)
        {
            polygonGroup->GetNormal(i, normal);
            HashFloat(hash, normal.x);
            HashFloat(hash, normal.y);
            HashFloat(hash, normal.z);
        }

        if (hasPivot)
        {
            polygonGroup->GetPivot(i, pivot);
            HashFloat(hash, pivot.x);
            HashFloat(hash, pivot.y);
            HashFloat(hash, pivot.z);
            HashFloat(hash, pivot.w);
        }
    }

    DAVA::Vector2 texCoord;
    for (DAVA::int32 layer = 0; layer < polygonGroup->textureCoordCount; ++layer)
    {
        for (DAVA::int32 i = 0; i < vertexCount; ++i)
        {
            polygonGroup->GetTexcoord(layer, i, texCoord);
            HashFloat(hash, texCoord.x);
            HashFloat(hash, texCoord.y);
        }
    }

    DAVA::int32 sourceIndex = 0;
    const DAVA::int32 indexCount = primitiveCount * 3;
    for (DAVA::int32 i = 0; i < indexCount; ++i)
    {
        polygonGroup->GetIndex(i, sourceIndex);
        HashInt(hash, sourceIndex);
    }

    return hash;
}

DAVA::uint64 BeastMesh::ComputeContentHash(LandscapeGeometry* geometry, BeastMaterial* material) const
{
    DAVA::uint64 hash = HASH_SEED;
    if (geometry == nullptr)
    {
        return hash;
    }

    if (material != nullptr)
    {
        HashString(hash, material->GetName());
    }

    const DAVA::int32 vertexCount = static_cast<DAVA::int32>(geometry->vertices.size());
    HashInt(hash, vertexCount);
    HashInt(hash, static_cast<DAVA::int32>(geometry->indices.size()));

    for (const auto& vertex : geometry->vertices)
    {
        HashFloat(hash, vertex.position.x);
        HashFloat(hash, vertex.position.y);
        HashFloat(hash, vertex.position.z);
        HashFloat(hash, vertex.texCoord.x);
        HashFloat(hash, vertex.texCoord.y);
    }

    for (const DAVA::Vector3& normal : geometry->normals)
    {
        HashFloat(hash, normal.x);
        HashFloat(hash, normal.y);
        HashFloat(hash, normal.z);
    }

    for (DAVA::int32 index : geometry->indices)
    {
        HashInt(hash, index);
    }

    return hash;
}

ILBMeshHandle BeastMesh::GetILBMesh() const
{
    return mesh;
}

DAVA::int32 BeastMesh::GetTextureCoordCount() const
{
    return textureCoordCount;
}

bool BeastMesh::HasTangents(DAVA::PolygonGroup* polygonGroup)
{
    return (polygonGroup->GetFormat() & DAVA::EVF_TANGENT ? true : false);
}
}
