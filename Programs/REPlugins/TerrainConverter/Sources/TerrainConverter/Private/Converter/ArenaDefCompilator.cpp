// ArenaDefCompilator.cpp - reconstructed implementation (the original was
// never published with the TerrainConverter plugin; only ArenaDefCompilator.h
// exists in the public repository). The class contract comes from the header,
// Converter.cpp and StrategyPointData.cpp.
//
// Template semantics (best-effort reconstruction of the internal format):
// the arena_def.xml pattern contains "%(token)" placeholders. Global tokens
// are %(mapName) and the bounding box %(minX) %(minY) %(maxX) %(maxY).
// If the pattern contains "%(pointsBegin)" / "%(pointsEnd)" markers, the
// section between them is repeated once per StrategyPointData (all point
// tokens from StrategyPointData::FillPattern are substituted in each copy).
// If the pattern has point tokens but no markers, the first point is
// substituted and a warning is logged.

#include "Converter/ArenaDefCompilator.h"

#include <fstream>
#include <set>
#include <sstream>

namespace
{
const char* POINTS_BEGIN_TOKEN = "%(pointsBegin)";
const char* POINTS_END_TOKEN = "%(pointsEnd)";
}

namespace
{
DAVA::String ReadAllText(const DAVA::FilePath& path)
{
    std::ifstream in(path.GetAbsolutePathname().c_str());
    if (!in.is_open())
    {
        DAVA::Logger::Warning("[ArenaDefCompilator] can not open arena def pattern: %s",
                              path.GetAbsolutePathname().c_str());
        return DAVA::String();
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

DAVA::FilePath ResolveOutputFile(DAVA::FilePath path, const DAVA::String& mapName)
{
    if (path.IsEmpty())
        return DAVA::FilePath();

    if (!path.IsDirectoryPathname() && !path.GetExtension().empty())
        return path; // treated as a full file path

    return DAVA::FilePath(path.GetAbsolutePathname() + "/" + mapName + ".xml");
}
}

std::unique_ptr<ArenaDefCompliator> ArenaDefCompliator::Create(const DAVA::FilePath& arenaDefPatternPath,
                                                              const DAVA::FilePath& saveTo)
{
    return std::unique_ptr<ArenaDefCompliator>(
    new ArenaDefCompliator(ReadAllText(arenaDefPatternPath), saveTo));
}

ArenaDefCompliator::ArenaDefCompliator(const DAVA::String& arenaPattern, const DAVA::FilePath& saveTo)
    : arenaOutputFilepath(saveTo)
    , arenaPattern(arenaPattern)
{
}

void ArenaDefCompliator::Start(const DAVA::String& mapName_)
{
    mapName = mapName_;
    Reset();
}

void ArenaDefCompliator::Visit(StrategyPointData& data)
{
    points.push_back(data);
}

void ArenaDefCompliator::Visit(BoundingBoxData& data)
{
    boundingBox = data;
}

void ArenaDefCompliator::End()
{
    Verify();
    Dump();
}

void ArenaDefCompliator::Reset()
{
    points.clear();
    boundingBox = BoundingBoxData();
}

void ArenaDefCompliator::Verify() const
{
    if (arenaPattern.empty())
    {
        DAVA::Logger::Warning("[ArenaDefCompilator] arena def pattern is empty, output is skipped");
        return;
    }

    if (mapName.empty())
    {
        DAVA::Logger::Warning("[ArenaDefCompilator] map name is empty");
    }

    std::set<DAVA::int32> seenBaseIDs;
    for (const StrategyPointData& point : points)
    {
        if (point.baseID == StrategyPointData::INVALID_BASE_ID)
        {
            DAVA::Logger::Warning("[ArenaDefCompilator] strategy point without baseID found");
        }
        else if (seenBaseIDs.count(point.baseID) != 0)
        {
            DAVA::Logger::Warning("[ArenaDefCompilator] duplicate baseID %d", point.baseID);
        }
        else
        {
            seenBaseIDs.insert(point.baseID);
        }
    }
}

void ArenaDefCompliator::Dump()
{
    using namespace DAVA;

    if (arenaPattern.empty() || arenaOutputFilepath.IsEmpty())
        return;

    String content = arenaPattern;
    StringReplace(content, "%(mapName)", mapName);

    const AABBox2& bbox = boundingBox.bbox;
    StringReplace(content, "%(minX)", Format("%f", bbox.min.x));
    StringReplace(content, "%(minY)", Format("%f", bbox.min.y));
    StringReplace(content, "%(maxX)", Format("%f", bbox.max.x));
    StringReplace(content, "%(maxY)", Format("%f", bbox.max.y));

    const size_t begin = content.find(POINTS_BEGIN_TOKEN);
    if (begin != String::npos)
    {
        const size_t end = content.find(POINTS_END_TOKEN, begin);
        if (end == String::npos)
        {
            Logger::Warning("[ArenaDefCompilator] found %s without matching %s",
                            POINTS_BEGIN_TOKEN, POINTS_END_TOKEN);
        }
        else
        {
            const size_t blockBegin = begin + String(POINTS_BEGIN_TOKEN).length();
            const String blockTemplate = content.substr(blockBegin, end - blockBegin);
            String rendered;
            for (StrategyPointData& point : points)
            {
                String entry = blockTemplate;
                point.FillPattern(entry);
                rendered += entry;
            }
            content = content.substr(0, begin) + rendered + content.substr(end + String(POINTS_END_TOKEN).length());
        }
    }
    else if (!points.empty() && content.find("%(baseID)") != String::npos)
    {
        Logger::Warning("[ArenaDefCompilator] arena def pattern has point tokens but no %s/%s markers - only the first point is used",
                        POINTS_BEGIN_TOKEN, POINTS_END_TOKEN);
        StrategyPointData firstPoint = points.front();
        firstPoint.FillPattern(content);
    }

    const FilePath outputFile = ResolveOutputFile(arenaOutputFilepath, mapName);
    std::ofstream out(outputFile.GetAbsolutePathname().c_str());
    if (!out.is_open())
    {
        Logger::Error("[ArenaDefCompilator] can not write arena def output: %s",
                      outputFile.GetAbsolutePathname().c_str());
        return;
    }

    out << content;
}
