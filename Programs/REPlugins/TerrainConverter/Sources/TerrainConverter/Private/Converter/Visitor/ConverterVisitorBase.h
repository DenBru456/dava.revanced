#ifndef __TERRAIN_CONVERTER_CONVERTER_VISITOR_BASE_H__
#define __TERRAIN_CONVERTER_CONVERTER_VISITOR_BASE_H__

// ConverterVisitorBase.h - reconstructed file (the original was never
// published in the public dava.engine repository; only the .cpp stub and
// the derived classes were). Interface derived from ArenaDefCompilator.h,
// ConverterVisitorElement.h, BoundingBoxData.h and Converter.cpp.

#include <Base/BaseTypes.h>

class StrategyPointData;
class BoundingBoxData;

class ConverterVisitorBase
{
public:
    virtual ~ConverterVisitorBase() = default;

    virtual void Start(const DAVA::String& mapName) = 0;
    virtual void End() = 0;

    virtual void Visit(StrategyPointData& data) = 0;
    virtual void Visit(BoundingBoxData& data) = 0;
};

#endif // __TERRAIN_CONVERTER_CONVERTER_VISITOR_BASE_H__
