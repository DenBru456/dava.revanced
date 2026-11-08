#pragma once

#include <Base/BaseTypes.h>

namespace Beast
{
enum eBeastMode : DAVA::uint32
{
    MODE_LIGHTMAPS = 0,
    MODE_SPHERICAL_HARMONICS,
    MODE_PREVIEW,
    // eRnsT: a live session that lights the scene continuously and never writes
    // files. No render passes, no output, runs until it is stopped.
    MODE_LIVE
};
}