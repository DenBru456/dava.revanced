#include "DataOutputPath.h"

#include "FileSystem/FilePath.h"

// DataOutputPath.cpp - reconstructed file (the original was never published
// with the TerrainConverter plugin). Mirrors the ModelTemplatePath.cpp style.
// The output location for the generated arena definition file.

const DAVA::FilePath& DataOutputPath::GetArenaDef()
{
    static DAVA::FilePath arenaDef("~res:/Data/arena_def.xml");
    return arenaDef;
}
