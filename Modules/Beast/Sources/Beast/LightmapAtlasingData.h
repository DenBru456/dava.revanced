#pragma once

#include <Base/BaseTypes.h>
#include <FileSystem/FilePath.h>
#include <Math/Vector.h>

namespace Beast
{
// One lightmapped mesh instance placement inside the packed lightmap atlas.
// Recovered from the historical dava.engine sources (the file was dropped
// from the repository before the public export); moved into the Beast
// namespace to match the `Beast::LightmapAtlasingData` references used by
// BeastManager.h and LightmapsPacker.h in the same revision.
struct LightmapAtlasingData
{
    DAVA::String meshInstanceName;
    DAVA::FilePath textureName;
    DAVA::Vector2 uvOffset;
    DAVA::Vector2 uvScale;
};
} // namespace Beast
