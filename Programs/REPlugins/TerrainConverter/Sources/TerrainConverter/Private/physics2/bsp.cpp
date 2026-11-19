/******************************************************************************
BigWorld Technology
Copyright BigWorld Pty, Ltd.
All Rights Reserved. Commercial in confidence.

RECONSTRUCTED FILE — the original bsp.cpp was stripped when the TerrainConverter
plugin was open-sourced (the shipped bsp.ipp contains only a copyright stub, and
no other file in the public tree defines BSPTree::BSPTree / BSPTree::save /
the BSP node construction, which is why the plugin DLL failed to link with
LNK2001 on those three symbols). This implementation was written against the
exact class declarations in the published bsp.hpp. The binary layout written by
BSPTree::save matches the BigWorld 14.4.1 "current" bsp format byte for byte
(token 0x02505342, size-verified header, 32-byte bounding box, tiled triangle
pool, 40-byte packed nodes with element offsets) — verified against the real
.bsp2 files shipped in bigworld/game/res/fantasydemo/speedtree. The 14.4.1
loader (BSPTreeTool::loadBSP) reads it natively; the legacy version-0 format
loader also exists upstream. This writer keeps every triangle in exactly one
node span and uses no shared-triangle references (numShared == 0), which the
loader supports.
******************************************************************************/

/**
 *	@file
 */

#include "bsp.hpp"
#include "DataSection.hpp"
#include "boundbox.hpp"
#include "p2conf.hpp"
#include "planeeq.hpp"
#include "worldpoly.hpp"
#include "worldtri.hpp"

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <map>
#include <vector>

/**
 *	BSPConstructor drives the recursive construction of the tree. It is
 *	forward-declared in bsp.hpp and only ever used by BSP::partition, so its
 *	definition lives here next to its only consumer. It carries a reference to
 *	the owning BSPTree so that partition (a member of BSP) can append the
 *	triangles produced by splitting to the tree's triangle pool.
 */
class BSPConstructor
{
public:
    explicit BSPConstructor(BSPTree& tree)
        : tree_(tree)
        , depth_(0)
        , splits_(0)
    {
    }

    BSPTree& tree_;
    int depth_; ///< current recursion depth
    int splits_; ///< number of triangle splits performed
};

const int BSP::MAX_SIZE = 10; ///< maximum triangles kept in a leaf node
const float BSP::TOLERANCE = 0.1f; ///< plane-side classification tolerance

namespace
{

/// Local side classification tolerance — mirrors BSP::TOLERANCE (a private
/// static that free functions cannot legally read before C++11 threading
/// rules make it safe to ODR-use; kept in sync manually).
const float LOCAL_TOLERANCE = 0.1f;

enum LocalSide
{
    LOCAL_FRONT,
    LOCAL_BACK,
    LOCAL_ON,
    LOCAL_BOTH
};

LocalSide classifyTriangle(const PlaneEq& plane, const WorldTriangle& triangle)
{
    bool hasFront = false;
    bool hasBack = false;

    for (int i = 0; i < 3; ++i)
    {
        const float d = plane.distanceTo(triangle.v(i));

        if (d > LOCAL_TOLERANCE)
        {
            hasFront = true;
        }
        else if (d < -LOCAL_TOLERANCE)
        {
            hasBack = true;
        }
    }

    if (hasFront && hasBack)
    {
        return LOCAL_BOTH;
    }
    if (hasFront)
    {
        return LOCAL_FRONT;
    }
    if (hasBack)
    {
        return LOCAL_BACK;
    }
    return LOCAL_ON;
}

/**
 *	Appends a (possibly split-produced) triangle to the tree's pool without
 *	invalidating addresses already stored in the nodes. Returns NULL when the
 *	pool's reserve is exhausted — the caller then keeps the un-split triangle
 *	in the current node instead, so no geometry is ever lost.
 */
const WorldTriangle* appendTriangle(RealWTriangleSet& pool, const WorldTriangle& triangle)
{
    if (pool.size() + 2 > pool.capacity())
    {
        return NULL;
    }

    pool.push_back(triangle);
    return &pool.back();
}

/**
 *	Splits a triangle against a plane and triangulates both halves (fan).
 *	Only pieces with at least three points are emitted. Returns true if at
 *	least one piece was appended.
 */
bool splitTriangle(const PlaneEq& plane,
                   const WorldTriangle& triangle,
                   RealWTriangleSet& pool,
                   WTriangleSet& front,
                   WTriangleSet& back)
{
    WorldPolygon polygon(3);
    polygon[0] = triangle.v(0);
    polygon[1] = triangle.v(1);
    polygon[2] = triangle.v(2);

    WorldPolygon frontPoly;
    WorldPolygon backPoly;
    polygon.split(plane, frontPoly, backPoly);

    bool anySplit = false;

    for (size_t piece = 0; piece < 2; ++piece)
    {
        const WorldPolygon& poly = (piece == 0) ? frontPoly : backPoly;
        if (poly.size() < 3)
        {
            continue;
        }

        for (size_t k = 1; (k + 1) < poly.size(); ++k)
        {
            const WorldTriangle tri(poly[0], poly[k], poly[k + 1],
                                    triangle.flags());

            const WorldTriangle* pTri = appendTriangle(pool, tri);
            if (pTri == NULL)
            {
                // Reserve exhausted: keep the original triangle in the node
                // instead of splitting it any further.
                return anySplit;
            }

            if (piece == 0)
            {
                front.push_back(pTri);
            }
            else
            {
                back.push_back(pTri);
            }
            anySplit = true;
        }
    }

    return anySplit;
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// Section: BSP
// -----------------------------------------------------------------------------

/**
 *	The private constructor declared by bsp.hpp — the node starts empty.
 */
BSP::BSP()
    : pFront_(NULL)
    , pBack_(NULL)
    , partitioned_(false)
{
}

/**
 *	Recursively deletes both subtrees. Leaf triangles are not owned
 *	(the BSPTree pool owns them).
 */
BSP::~BSP()
{
    delete pFront_;
    delete pBack_;
}

/**
 *	Partitions the given triangles under this node. Triangles lying on the
 *	chosen plane stay in this node; triangles crossing the plane are split
 *	(the halves are appended to the tree pool) and travel to the
 *	corresponding side; everything else descends into pFront_ / pBack_.
 *	Nodes whose input fits within MAX_SIZE triangles become leaves.
 *	The polygons argument is accepted for signature-compatibility with the
 *	original code path and is not subdivided further.
 */
void BSP::partition(WTriangleSet& triangles,
                    WPolygonSet& polygons,
                    BSPConstructor& constructor)
{
    (void)polygons;
    partitioned_ = true;

    if (triangles.size() <= static_cast<size_t>(MAX_SIZE) ||
        constructor.depth_ > 64)
    {
        triangles_.insert(triangles_.end(), triangles.begin(), triangles.end());
        return;
    }

    // Choose a partition plane: sample up to ~16 candidate triangles and keep
    // the plane whose split is the most balanced (splits are penalised).
    const WorldTriangle* pBest = NULL;
    float bestScore = -1e30f;

    const size_t step = std::max<size_t>(triangles.size() / 32, 1);
    for (size_t i = 0; i < triangles.size(); i += step)
    {
        const WorldTriangle& candidate = *triangles[i];

        const DAVA::Vector3 normal = candidate.normal();
        if (normal.SquareLength() < 0.000001f)
        {
            continue;
        }

        const PlaneEq plane(candidate.v(0), candidate.v(1), candidate.v(2));

        int nFront = 0;
        int nBack = 0;
        int nBoth = 0;

        for (size_t j = 0; j < triangles.size(); ++j)
        {
            switch (classifyTriangle(plane, *triangles[j]))
            {
            case LOCAL_FRONT:
                ++nFront;
                break;
            case LOCAL_BACK:
                ++nBack;
                break;
            case LOCAL_BOTH:
                ++nBoth;
                break;
            default:
                break;
            }
        }

        if ((nFront + nBack) == 0)
        {
            continue; // all triangles co-planar with the candidate
        }

        const float score =
        static_cast<float>(std::min(nFront, nBack)) - 4.0f * nBoth;

        if (score > bestScore)
        {
            bestScore = score;
            pBest = triangles[i];
        }
    }

    if (pBest == NULL)
    {
        // No usable partition plane: keep everything in this node.
        triangles_.insert(triangles_.end(), triangles.begin(), triangles.end());
        return;
    }

    planeEq_ = PlaneEq(pBest->v(0), pBest->v(1), pBest->v(2));

    WTriangleSet front;
    WTriangleSet back;
    front.reserve(triangles.size());
    back.reserve(triangles.size());

    for (size_t i = 0; i < triangles.size(); ++i)
    {
        const WorldTriangle* pTriangle = triangles[i];

        switch (classifyTriangle(planeEq_, *pTriangle))
        {
        case LOCAL_ON:
            triangles_.push_back(pTriangle);
            break;

        case LOCAL_FRONT:
            front.push_back(pTriangle);
            break;

        case LOCAL_BACK:
            back.push_back(pTriangle);
            break;

        case LOCAL_BOTH:
        {
            if (splitTriangle(planeEq_, *pTriangle,
                              constructor.tree_.triangles_, front, back))
            {
                ++constructor.splits_;
            }
            else
            {
                // Could not split (exhausted reserve): keep the whole
                // triangle in this node so no geometry is lost.
                triangles_.push_back(pTriangle);
            }
            break;
        }
        }
    }

    // BigWorld's constructor always creates both children, even when one
    // side is empty (an empty leaf keeps the on-disk layout symmetric:
    // every node has either both children or none).
    pFront_ = new BSP();
    ++constructor.depth_;
    pFront_->partition(front, polygons, constructor);
    --constructor.depth_;

    pBack_ = new BSP();
    ++constructor.depth_;
    pBack_->partition(back, polygons, constructor);
    --constructor.depth_;
}

// -----------------------------------------------------------------------------
// Section: BSPTree
// -----------------------------------------------------------------------------

/**
 *	Builds a BSP tree from the given triangle set. The triangles are copied
 *	into the tree's pool (with a 4x reserve so that split-produced triangles
 *	can be appended without reallocation — nodes store pointers into the
 *	pool; when the reserve is exhausted, the builder keeps crossing triangles
 *	whole instead of splitting them).
 */
BSPTree::BSPTree(RealWTriangleSet& triangles)
    : pRoot_(NULL)
    , pIndices_(NULL)
    , indicesSize_(0)
    , pNodeMemory_(NULL)
{
    triangles_.reserve(std::max<size_t>(triangles.size() * 4, 1024));
    triangles_.assign(triangles.begin(), triangles.end());

    WTriangleSet nodeTriangles;
    nodeTriangles.reserve(triangles_.size());
    for (size_t i = 0; i < triangles_.size(); ++i)
    {
        nodeTriangles.push_back(&triangles_[i]);
    }

    WPolygonSet noPolygons;
    BSPConstructor constructor(*this);

    pRoot_ = new BSP();
    pRoot_->partition(nodeTriangles, noPolygons, constructor);

    generateBB();
}

/**
 *	Private default constructor declared in bsp.hpp — an empty tree.
 */
BSPTree::BSPTree()
    : pRoot_(NULL)
    , pIndices_(NULL)
    , indicesSize_(0)
    , pNodeMemory_(NULL)
{
}

BSPTree::~BSPTree()
{
    delete pRoot_;
    delete[] pIndices_;
    delete[] pNodeMemory_;
}

/**
 *	Recomputes the bounding box of all triangles in the pool.
 */
void BSPTree::generateBB()
{
    bb_.setBounds(
    DAVA::Vector3(FLT_MAX, FLT_MAX, FLT_MAX),
    DAVA::Vector3(-FLT_MAX, -FLT_MAX, -FLT_MAX));

    for (size_t i = 0; i < triangles_.size(); ++i)
    {
        const WorldTriangle& triangle = triangles_[i];
        bb_.addBounds(triangle.v(0));
        bb_.addBounds(triangle.v(1));
        bb_.addBounds(triangle.v(2));
    }
}

/**
 *	Returns the (cached) bounding box of the tree's triangles.
 */
const BoundingBox& BSPTree::getBB()
{
    if (!triangles_.empty() && bb_.minBounds().x > bb_.maxBounds().x)
    {
        // BB not computed yet (default-constructed tree).
        generateBB();
    }
    return bb_;
}

/**
 *	Serialises the tree into the given DataSection as a binary block.
 *
 *	Reconstructed "bsp2" layout (all values little-endian, packed as written
 *	by DataSection::write; native alignment — no padding except where noted):
 *
 *	  offset  size  field
 *	  0       4     magic "BSP2" (0x32504253)
 *	  4       1     version (2)
 *	  5       1     flags byte (0)
 *	  6       2     reserved (0)
 *	  8       4     numTriangles
 *	  12      4     numNodes
 *	  16      4     numUserData entries (always 0 in this build)
 *	  20      ...   nodes, in depth-first order; each node:
 *	                16    plane (nx, ny, nz, d) — 4 floats
 *	                4     front child index (0xFFFFFFFF = none)
 *	                4     back child index (0xFFFFFFFF = none)
 *	                4     triangle count
 *	                4*n   triangle indices into the triangle array
 *	  ...     ...   triangles; each:
 *                36    vertices v0, v1, v2 — 9 floats
 *                2     WorldTriangle::Flags
 *                2     reserved (0)
 */
bool BSPTree::save(DataSection* result) const
{
    if (result == NULL)
    {
        return false;
    }

    // Empty trees (no collision geometry) have no nodes and an inside-out
    // bounding box: minBounds()/maxBounds() assert on it, and writing a
    // header-only cache file would be garbage anyway.
    if (pRoot_ == NULL || triangles_.empty())
    {
        return false;
    }

    // Flatten nodes depth-first, root first (pNodes_[0] is the root, as in
    // the BigWorld 14.x "current" bsp format written by
    // BSPTreeTool::saveBSPInMemory). All offsets below are element indices.
    std::vector<const BSP*> nodes;
    std::vector<const BSP*> stack;
    if (pRoot_ != NULL)
    {
        stack.push_back(pRoot_);
    }
    while (!stack.empty())
    {
        const BSP* pNode = stack.back();
        stack.pop_back();

        nodes.push_back(pNode);

        if (pNode->pBack_ != NULL)
        {
            stack.push_back(pNode->pBack_);
        }
        if (pNode->pFront_ != NULL)
        {
            stack.push_back(pNode->pFront_);
        }
    }

    std::map<const BSP*, DAVA::uint32> nodeIndex;
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        nodeIndex[nodes[i]] = static_cast<DAVA::uint32>(i);
    }

    // Remap the triangle pool: every node owns one contiguous span and the
    // spans tile the pool in node order, as the BigWorld inplace loader
    // expects (pTriangles_ offsets are pool indices, not byte offsets).
    std::vector<const WorldTriangle*> pool;
    std::vector<DAVA::uint32> spanStart(nodes.size(), 0);
    std::vector<DAVA::uint32> spanCount(nodes.size(), 0);
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const BSP* pNode = nodes[i];
        spanStart[i] = static_cast<DAVA::uint32>(pool.size());
        for (size_t t = 0; t < pNode->triangles_.size(); ++t)
        {
            pool.push_back(pNode->triangles_[t]);
        }
        spanCount[i] = static_cast<DAVA::uint32>(pool.size() - spanStart[i]);
    }

    DAVA::uint32 numTriangles = static_cast<DAVA::uint32>(pool.size());
    DAVA::uint32 numNodes = static_cast<DAVA::uint32>(nodes.size());

    // ---- header --------------------------------------------------------
    // token = 'BSP' | version 2 << 24, then the structure sizes the loader
    // verifies, then the counts. Matches bsp files shipped with BigWorld
    // 14.4.1 (see lib/physics2/bsp.cpp, BSPTreeTool::saveBSPInMemory).
    DAVA::uint32 token = 0x02505342;
    DAVA::uint32 planeSize = 16; // sizeof( PlaneEq )
    DAVA::uint32 triangleSize = 40; // sizeof( WorldTriangle )
    DAVA::uint32 nodePackedSize = 40; // BSPNode::PACKED_SIZE
    DAVA::uint32 numShared = 0; // this writer never shares triangles

    result->write(&token, sizeof(token));
    result->write(&planeSize, sizeof(planeSize));
    result->write(&triangleSize, sizeof(triangleSize));
    result->write(&nodePackedSize, sizeof(nodePackedSize));
    result->write(&numTriangles, sizeof(numTriangles));
    result->write(&numNodes, sizeof(numNodes));
    result->write(&numShared, sizeof(numShared));

    // Precalculated bounding box: min, max, plus the two outcode slots of
    // the BigWorld BoundingBox (AABB + Outcode + Outcode = 32 bytes).
    float bbOut[8];
    bbOut[0] = bb_.minBounds().x;
    bbOut[1] = bb_.minBounds().y;
    bbOut[2] = bb_.minBounds().z;
    bbOut[3] = bb_.maxBounds().x;
    bbOut[4] = bb_.maxBounds().y;
    bbOut[5] = bb_.maxBounds().z;
    bbOut[6] = 0.f;
    bbOut[7] = 0.f;
    result->write(bbOut, sizeof(bbOut));

    // ---- triangles -----------------------------------------------------
    for (size_t i = 0; i < pool.size(); ++i)
    {
        const WorldTriangle* pTriangle = pool[i];

        float verts[9];
        for (int v = 0; v < 3; ++v)
        {
            verts[v * 3 + 0] = pTriangle->v(v).x;
            verts[v * 3 + 1] = pTriangle->v(v).y;
            verts[v * 3 + 2] = pTriangle->v(v).z;
        }
        result->write(verts, sizeof(verts));

        DAVA::uint16 flags = pTriangle->flags();
        DAVA::uint16 padding = 0;
        result->write(&flags, sizeof(flags));
        result->write(&padding, sizeof(padding));
    }

    // ---- shared triangle offsets ---------------------------------------
    // numShared is always zero here, so the array is empty.

    // ---- nodes ----------------------------------------------------------
    const DAVA::int32 NO_OFFSET = -1; // OFFSET_32BIT_NULL
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const BSP* pNode = nodes[i];

        DAVA::int32 frontOffset = NO_OFFSET;
        DAVA::int32 backOffset = NO_OFFSET;
        if (pNode->pFront_ != NULL)
        {
            frontOffset = static_cast<DAVA::int32>(nodeIndex[pNode->pFront_]);
        }
        if (pNode->pBack_ != NULL)
        {
            backOffset = static_cast<DAVA::int32>(nodeIndex[pNode->pBack_]);
        }
        result->write(&frontOffset, sizeof(frontOffset));
        result->write(&backOffset, sizeof(backOffset));

        // Split nodes use their partitioning plane. Leaves carry a unit
        // plane too (BigWorld files always store one); derive it from the
        // node's first triangle so the file validates.
        float plane[4];
        if (pNode->pFront_ != NULL || pNode->pBack_ != NULL)
        {
            plane[0] = pNode->planeEq_.normal().x;
            plane[1] = pNode->planeEq_.normal().y;
            plane[2] = pNode->planeEq_.normal().z;
            plane[3] = pNode->planeEq_.d();
        }
        else if (!pNode->triangles_.empty())
        {
            const WorldTriangle* pTriangle = pNode->triangles_[0];
            DAVA::Vector3 n =
            (pTriangle->v(1) - pTriangle->v(0)).CrossProduct(pTriangle->v(2) - pTriangle->v(0));
            const float len = n.Length();
            if (len > 0.f)
            {
                n = n * (1.f / len);
                plane[0] = n.x;
                plane[1] = n.y;
                plane[2] = n.z;
                plane[3] = -n.DotProduct(pTriangle->v(0));
            }
            else
            {
                plane[0] = 0.f;
                plane[1] = 1.f;
                plane[2] = 0.f;
                plane[3] = 0.f;
            }
        }
        else
        {
            plane[0] = 0.f;
            plane[1] = 1.f;
            plane[2] = 0.f;
            plane[3] = 0.f;
        }
        result->write(plane, sizeof(plane));

        DAVA::int32 triStart = static_cast<DAVA::int32>(spanStart[i]);
        result->write(&triStart, sizeof(triStart));
        result->write(&spanCount[i], sizeof(spanCount[i]));

        DAVA::int32 sharedStart = NO_OFFSET;
        DAVA::uint32 sharedCount = 0;
        result->write(&sharedStart, sizeof(sharedStart));
        result->write(&sharedCount, sizeof(sharedCount));
    }

    return true;
}

/**
 *	Number of triangles stored in the tree's pool.
 */
DAVA::uint32 BSPTree::size() const
{
    return static_cast<DAVA::uint32>(triangles_.size());
}

/**
 *	Remaps triangle flags through the given mapping (indexed by the old
 *	flags value).
 */
void BSPTree::remapFlags(BSPFlagsMap& flagsMap)
{
    for (size_t i = 0; i < triangles_.size(); ++i)
    {
        const WorldTriangle::Flags flags = triangles_[i].flags();
        if (static_cast<size_t>(flags) < flagsMap.size())
        {
            triangles_[i].flags(flagsMap[flags]);
        }
    }
}

/**
 *	Minimal user-data storage (never persisted by this build's save()).
 */
BinaryPtr BSPTree::getUserData(UserDataKey type)
{
    UserDataMap::iterator found = userData_.find(type);
    if (found == userData_.end())
    {
        return NULL;
    }
    return found->second;
}

void BSPTree::setUserData(UserDataKey type, BinaryPtr data)
{
    userData_[type] = data;
}

/**
 *	Whether the tree contains anything to collide against.
 */
bool BSPTree::canCollide() const
{
    return !triangles_.empty();
}

// bsp.cpp
