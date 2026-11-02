#pragma once

#include "Beast/BeastTypes.h"

#include <Render/Highlevel/Landscape.h>
#include <Math/Vector.h>
#include <Base/BaseTypes.h>

#include <limits>

namespace Beast
{
// Fills geometry from the landscape level 0 data. Reconstructed from the
// published LandscapeGeometry.cpp (the original header was never published),
// using the exact parameter types of DAVA::Landscape::GetLevel0Geometry as
// the ground truth for the member containers.
class LandscapeGeometry
{
public:
    // Accumulates face normals into `normals` and normalizes them.
    // Expects vertices/indices to be filled (GetLevel0Geometry) first.
    void ComputeNormals();

    DAVA::Vector<DAVA::Landscape::LandscapeVertex> vertices;
    DAVA::Vector<DAVA::int32> indices;
    DAVA::Vector<DAVA::Vector3> normals;
};
} // namespace Beast
