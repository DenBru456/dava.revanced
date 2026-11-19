#ifndef __TERRAIN_CONVERTER_STRATEGY_POINT_DATA_H__
#define __TERRAIN_CONVERTER_STRATEGY_POINT_DATA_H__

// StrategyPointData.h - reconstructed file (the original was never
// published in the public dava.engine repository; only StrategyPointData.cpp
// was). Field set, defaults and the method contract are derived from
// StrategyPointData.cpp (SStrategyPointData::Create / FillPattern) and from
// ArenaDefCompilator.h which stores these objects in a Vector.

#include <DAVAEngine.h>

class StrategyPointData
{
public:
    static const DAVA::int32 INVALID_BASE_ID = -1;

    StrategyPointData() = default;

    static StrategyPointData Create(DAVA::KeyedArchive* customProperties);

    void FillPattern(DAVA::String& pattern);

    DAVA::int32 baseID = INVALID_BASE_ID;
    DAVA::int32 bonusCapture = 0;
    DAVA::int32 penaltyLoss = 0;
    DAVA::int32 incomeVictoryPoints = 0;
    DAVA::float32 spawnVictoryPointsTime = 0.f;
    DAVA::int32 capturePoints = 0;
    DAVA::float32 pointsPerSecond = 0.f;
    DAVA::float32 maxPointsPerSecond = 0.f;
    DAVA::float32 radius = 0.f;
};

#endif // __TERRAIN_CONVERTER_STRATEGY_POINT_DATA_H__
