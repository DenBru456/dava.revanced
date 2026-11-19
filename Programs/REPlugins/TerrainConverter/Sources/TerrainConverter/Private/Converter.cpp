// BW texture-name fields are fixed-size, deliberately non-terminated
// strncpy copies - keep the semantics, silence the CRT deprecation.
#define _CRT_SECURE_NO_WARNINGS
#include "Private/Converter.h"
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "Converter/ArenaDefCompilator.h"
#include "DataOutputPath.h"
#include "FilePathHelper.h"
#include "ModelTemplatePath.h"
#include "Utils.h"
#include "terrain.h"

#include <REPlatform/Deprecated/EditorConfig.h>
#include <REPlatform/Scene/Components/CollisionTypeComponent.h>

#include <Base/BaseTypes.h>
#include <Render/Image/ImageConvert.h>
#include <Render/PixelFormatDescriptor.h>

#include <Render/Highlevel/RenderObject.h>
#include <Scene3D/Components/RenderComponent.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <libpng/png.h>
#include <libpng/zconf.h>
#include <string.h>

// dava.revanced compatibility: entity transforms live on TransformComponent
// (the plugin was written against dava.engine master's Entity::GetWorldTransform)
#include <Math/TransformUtils.h>
#include <Scene3D/Components/TransformComponent.h>

// BigWorld chunk light emission needs the Dava light component
#include <Scene3D/Components/LightComponent.h>

// DAVA's Snprintf maps to raw _snprintf on Windows, which does NOT
// zero-terminate when the formatted string exactly fills the buffer
// (C6053); the plugin strlen()s these buffers right after. Route every
// call through the C99-conformant vsnprintf instead, which always
// terminates, plus a belt-and-braces terminator.
#include <cstdarg>
static void SnprintfSafe(char* buffer, size_t size, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, size, format, args);
    va_end(args);
    if (size > 0)
    {
        buffer[size - 1] = 0;
    }
}
#undef Snprintf
#define Snprintf SnprintfSafe

// BigWorld light defaults.
// Dava lights have no radius / cone angle, so these are used unless the artist
// overrides them with custom properties (bw.outerRadius, bw.innerRadius,
// bw.cosConeAngle, bw.priority, bw.dynamic) on the light entity.
static const DAVA::float32 DEFAULT_LIGHT_OUTER_RADIUS = 25.0f;
static const DAVA::float32 DEFAULT_LIGHT_INNER_RADIUS = 1.0f;
static const DAVA::float32 DEFAULT_LIGHT_CONE_COS = 0.766f; // ~40 degrees half-angle

static DAVA::Matrix4 GetEntityWorldTransform(DAVA::Entity* node)
{
    if (node == nullptr)
        return DAVA::Matrix4::IDENTITY;
    const DAVA::TransformComponent* transformComponent = node->GetComponent<DAVA::TransformComponent>();
    if (transformComponent == nullptr)
        return DAVA::Matrix4::IDENTITY;
    return DAVA::TransformUtils::ToMatrix(transformComponent->GetWorldTransform());
}

// ============================================================================
// BW-compliance patch (v3):
//  - emit into out\spaces\<spaceName>\ instead of a flat out\spaces\
//  - chunk grid follows the real space bounds (was hardcoded -6..5)
//  - space.settings uses the real bounds (was hardcoded -6,5,-6,5)
//  - terrain chunk XML pretty-printed like shipped BW chunks
//  - untagged mesh entities get a derived editor.referenceToOwner fallback
// ============================================================================
static DAVA::String g_spacesPath = "out\\spaces\\";
static DAVA::int32 g_minChunk = -6;
static DAVA::int32 g_maxChunk = 5;

static bool HasMeshRenderObject(DAVA::Entity* entity)
{
    if (entity == nullptr)
    {
        return false;
    }
    DAVA::RenderComponent* renderComponent = entity->GetComponent<DAVA::RenderComponent>();
    if (renderComponent == nullptr)
    {
        return false;
    }
    DAVA::RenderObject* renderObject = renderComponent->GetRenderObject();
    if (renderObject == nullptr)
    {
        return false;
    }
    const DAVA::RenderObject::eType type = renderObject->GetType();
    // statics are TYPE_MESH; trees/bushes render through SpeedTreeObject
    // (TYPE_SPEED_TREE) - both are exportable geometry.
    return type == DAVA::RenderObject::TYPE_MESH || type == DAVA::RenderObject::TYPE_SPEED_TREE;
}

static DAVA::String SanitizeModelName(const DAVA::FastName& name)
{
    DAVA::String result = name.c_str();
    static const char* badChars = "\\/:*?\"<>| ";
    for (size_t i = 0; i < result.length(); ++i)
    {
        if (strchr(badChars, result[i]) != nullptr)
        {
            result[i] = '_';
        }
    }
    if (result.empty())
    {
        result = "object";
    }
    return result;
}

// Any render batch whose material (or template parent chain) has "water" in
// its name - Blitz water is manual plane meshes carrying the Water*
// materials (WaterPerPixel*/WaterPerVertex*, shader
// ~res:/Materials/Shaders/Default/water). Names ("surf", "waves") are just
// names - water decorations are alphablend planes with non-water materials,
// so the material chain is the only trustworthy signal.
static bool HasWaterMaterial(DAVA::Entity* entity)
{
    DAVA::RenderObject* renderObject = DAVA::GetRenderObject(entity);
    if (renderObject == nullptr)
    {
        return false;
    }
    const DAVA::uint32 batchCount = renderObject->GetRenderBatchCount();
    for (DAVA::uint32 k = 0; k < batchCount; ++k)
    {
        DAVA::RenderBatch* batch = renderObject->GetRenderBatch(k);
        DAVA::NMaterial* material = (batch != nullptr) ? batch->GetMaterial() : nullptr;
        for (int depth = 0; material != nullptr && depth < 8; material = material->GetParent(), ++depth)
        {
            DAVA::String lower = material->GetMaterialName().c_str();
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.find("water") != DAVA::String::npos)
            {
                return true;
            }
        }
    }
    return false;
}

// Publish-stripped Blitz scenes carry none of WG's editor tags, and entity
// names carry no water meaning. Detect water purely by content: any render
// batch using a Water* material. Only entities WITHOUT an explicit
// CollisionType tag are auto-detected - an explicit tag always wins.
// Flat FX planes that ride on top of the real water (surf/wave/foam/... in the
// entity or material name). They are water DECORATIONS, not water: Blitz's real
// water is the plane that owns the water CollisionType. Exporting a decor plane
// as water puts a second surface into the chunk.
static bool HasWaterDecorMarker(const DAVA::String& raw)
{
    DAVA::String lower = raw.c_str();
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    // Compare whole words, not substrings. "seaSurface.sc2" is the map's actual
    // water body, but it contains "surf", so the old substring test classified it
    // as an FX decoration and the converter skipped the only water in rudniki.
    static const char* markers[] = { "surf", "wave", "foam", "ripple", "splash", "spray", "decor", "waterfx", "wake" };

    size_t i = 0;
    while (i < lower.size())
    {
        if (lower[i] < 'a' || lower[i] > 'z')
        {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < lower.size() && lower[i] >= 'a' && lower[i] <= 'z')
        {
            ++i;
        }
        const DAVA::String token = lower.substr(start, i - start);
        for (size_t m = 0; m < sizeof(markers) / sizeof(markers[0]); ++m)
        {
            if (token == markers[m])
            {
                return true;
            }
        }
    }
    return false;
}

static bool IsWaterDecoration(DAVA::Entity* entity)
{
    if (entity == nullptr)
    {
        return false;
    }
    if (HasWaterDecorMarker(DAVA::String(entity->GetName().c_str())))
    {
        return true;
    }
    DAVA::RenderObject* renderObject = DAVA::GetRenderObject(entity);
    if (renderObject == nullptr)
    {
        return false;
    }
    for (DAVA::uint32 k = 0; k < renderObject->GetRenderBatchCount(); ++k)
    {
        DAVA::RenderBatch* batch = renderObject->GetRenderBatch(k);
        DAVA::NMaterial* material = (batch != nullptr) ? batch->GetMaterial() : nullptr;
        for (int depth = 0; material != nullptr && depth < 8; material = material->GetParent(), ++depth)
        {
            if (HasWaterDecorMarker(DAVA::String(material->GetMaterialName().c_str())))
            {
                return true;
            }
        }
    }
    return false;
}

// "guid" properties are not guaranteed to be filename-safe; a slash or colon in
// one makes File::Create fail and used to leave a null File* to be written to
static DAVA::String MakeFilenameToken(const DAVA::String& raw)
{
    DAVA::String token = raw.c_str();
    for (size_t i = 0; i < token.size(); ++i)
    {
        const char c = token[i];
        const bool safe = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
        if (!safe)
        {
            token[i] = '_';
        }
    }
    if (token.empty())
    {
        token = "noid";
    }
    return token;
}

static bool DetectWaterPlane(DAVA::Entity* entity)
{
    if (entity == nullptr || !HasWaterMaterial(entity))
    {
        return false;
    }
    if (IsWaterDecoration(entity))
    {
        DAVA::Logger::Info("[TerrainConverter] '%s' carries a water material but reads as a water decoration "
                           "(surf/wave/foam/fx in the name or material chain) - not treated as water",
                           entity->GetName().c_str());
        return false;
    }
    return true;
}

// ============================================================================
// OSE bake pack (v4): terrain2 sections the BW 14.4 runtime client requires.
// Formats verified against BigWorld-Open-Source-Edition-2014:
//   terrain2/heights1..4  - standalone qpng height blocks at halved resolution
//   terrain2/normals     - "nrm\0" header + 16bpp grey PNG, pixel = (nx<<8)|nz,
//                          byte = (component + 1) * 255.9 / 2 (A8L8 encoding)
//   terrain2/lodNormals  - same dimensions as normals (low-detail copy)
//   terrain2/horizonShadows - "shd\0" header + zlib payload; pixel = uint16 east
//                          + uint16 west, all zero = no terrain shadows
//   terrain2/ambientOcclusion - "aom\0" header + 8bpp PNG, all 255 = no AO
//   terrain2/lodTexture.dds   - uncompressed RGBA DDS from customColorTexture,
//                          alpha = flat specular
//   terrain2/layer 4     - 4th blend channel (alpha of the tilemask)
// zlib wrapper mirrors BW BinaryBlock::compress (magic1/magic2/size/stream)
// ============================================================================
static std::vector<uint8_t> g_heightMips[5]; // mips 1..5, (heightMapSize >> lod) + 1 each
static std::vector<uint8_t> g_normalsBlock;
static std::vector<uint8_t> g_shadowBlock;
static std::vector<uint8_t> g_aoBlock;
static std::vector<uint8_t> g_lodTextureBlock;
// BW blend-layer u/vProjection components (uProjection.x / vProjection.z),
// derived from the landscape material tiling; the defaults give BW's own
// 10 m tile pitch (environment/terrainTextureSpacing). Plain floats: no DAVA
// or BW types are in scope this high in the TU (see the constexpr int32 lesson).
static float g_blendUProjX[4] = { 0.1f, 0.1f, 0.1f, 0.1f };
static float g_blendVProjZ[4] = { 0.1f, 0.1f, 0.1f, 0.1f };
// True when layer 0 carries the map-wide colorTexture instead of a tiled detail
// pattern: its projection then has to span the whole landscape once, not tile.
static bool g_layer0SpansMap = false;
// The colour map is its own layer (index 4), not a replacement for tile 0: all four
// Blitz tiles keep their own mask and their own tile texture. Because the four masks
// normalise to a sum of 255, the base would get nothing, so the tile masks are scaled
// by this gain and the colour map takes the remainder. 0.80 keeps the painted ground
// clearly visible under the detail.
// Share of every pixel the four detail tiles take; the map-wide base (fullTiledTexture,
// else colorTexture) keeps the rest. Measured against Dava's finished ground on rudniki:
// base 20% / tiles 80% leaves the blue channel at 2.3x the target (error 0.128), while a
// tile share of 0.20-0.25 stays within 0.03 of it and still shows the tile detail.
static float g_tileMaskGain = 0.25f;
static DAVA::String g_colormapLayerRef; // res-ref of the exported colorTexture
static std::vector<DAVA::uint8> g_baseMask; // colour-map coverage per texel
static std::vector<DAVA::uint8> g_baseLayerBlock;
// world extent of the landscape in metres, set in Do(); bake uses it to make a
// single ground texture span the whole map instead of repeating every few metres
static float g_mapWorldSize = 0.0f;
// BW's terrain heightmap PNG stores ABSOLUTE MILLIMETRES, so 65535 is a hard
// 65.535 m ceiling and there is no scale field to raise it. A Dava landscape does not
// start at 0: on rudniki the engine space begins at ~14.8 m, so the peaks reached 78 m
// and everything above 65.5 m was flattened - whole hilltops vanished. The shape was
// never wrong, only the base level (proved against a BW 16-bit heightmap export of the
// same map: identical relief, slope 1.03, offset -15.2 m). So shift the world down to
// its own minimum instead of rescaling it, and only squeeze the relief in the rare case
// that the span itself exceeds the ceiling.
static float g_heightOffsetM = 0.0f;
// chunk extents actually occupied by items (not just terrain), see AddEntityToChunk
// proxy VLOs would need a companion .odata file, which is not written yet
extern DAVA::String g_bwTextureSearchRoot; // defined in Model.cpp

// Published space extent. The terrain grid (g_minChunk..g_maxChunk) only covers the
// landscape, while distant scenery such as the mountains sits outside it; space.settings
// was widened to cover those items, so every writer that scatters activation items into
// chunks has to walk the same extent or the item ends up referenced by nothing.
static DAVA::int32 g_spaceMinX = 1;
static DAVA::int32 g_spaceMaxX = 0; // min > max means "not computed yet"
static DAVA::int32 g_spaceMinZ = 1;
static DAVA::int32 g_spaceMaxZ = 0;

// Proxy VLOs were switched off while they looked invisible; the cause was the published
// space bounds, not the proxy format - a reference space in b8d80f600_watertest.zip has a
// proxy VLO with NO .odata beside it, so proxies do not need the odata companion the way
// water does. They are the correct representation for a giant model: the activation item
// is written into every chunk the bounding box overlaps, so it never depends on one
// chunk staying resident.
static bool g_useProxyVlo = true;

// Horizon shadow maps are the one terrain block whose on-disk wrapper we have not
// pinned down for this build. Both shapes tried so far (32-byte "shd" header and the
// bare "piz" compressed blob that BW 2.0.1 ships) trip
//   ASSERTION FAILED: pHeader->magic_ == ShadowHeader::MAGIC
// inside HorizonShadowMap2::load, and a wrong block kills the whole chunk. Omitting the
// section is legal - spaces authored by the Blitz toolchain carry no shadow block at all
// and load fine - so it stays off until the real header is captured from a space this
// editor saved.
static bool g_writeHorizonShadows = true;

// Sizes of the per-chunk terrain maps. BW reads each block's own header, but the space
// settings also declare them and a block whose resolution disagrees with the settings is
// sampled as if it had the declared size - a 16 texel AO map addressed as 32 puts every
// second texel in the wrong place, which shows up as a soft dark patch that does not
// follow the geometry. They are read from the same template that writes space.settings so
// the two can never drift apart.
// Plain int on purpose: at this point of the file neither the DAVA typedefs nor the
// "using namespace DAVA" are in scope, so int32 is either unknown or ambiguous (C2872),
// and the whole translation unit cascades from there.
static int g_normalMapSize = 32;
static int g_aoMapSize = 32;
static int g_shadowMapSize = 32;
static int g_blendMapSize = 128;

static int ReadTemplateMapSize(const char* key, int fallback)
{
    DAVA::File* tf = DAVA::File::Create(DAVA::FilePath("~res:/modelTemplate\\space.settings"), DAVA::File::OPEN | DAVA::File::READ);
    if (tf == nullptr)
    {
        return fallback;
    }
    static char buf[8192];
    DAVA::uint32 got = tf->Read(buf, sizeof(buf) - 1);
    DAVA::SafeRelease(tf);
    buf[got] = 0;

    char token[64];
    SnprintfSafe(token, sizeof(token), "<%s>", key);
    const char* at = strstr(buf, token);
    if (at == nullptr)
    {
        return fallback;
    }
    at += strlen(token);
    while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n')
    {
        ++at;
    }
    const int value = atoi(at);
    return value > 0 ? value : fallback;
}

static DAVA::int32 g_itemMinX = 0x7fffffff;
static DAVA::int32 g_itemMaxX = -0x7fffffff;
static DAVA::int32 g_itemMinZ = 0x7fffffff;
static DAVA::int32 g_itemMaxZ = -0x7fffffff; // added to every height and object Y
static float g_heightReliefScale = 1.0f; // 1.0 unless the span cannot fit 65.535 m
// the map-wide ground colour (material property colorTexture, landscape/colormap.dds).
// BW has no macro channel for the tiled layers, so this is what terrain2/lodTexture
// must carry instead of the passability palette colours.
static DAVA::Image* g_colormapImage = nullptr;
static DAVA::int32 g_colormapWidth = 0;
static DAVA::int32 g_colormapHeight = 0;
// which world->landscape convention produced relief; -1 = not probed yet
static DAVA::int32 g_heightMapMode = -1;
// BW repeats a layer texture every textureTiling metres and has no second, map-wide
// colour channel, so the baked colour only survives if one layer spans the whole map.
// BW 2.0.1 defines none of WG's user data object types (SpawnPoint, BotSpawn, Patrol,
// ControlPoint, StrategyPoint), and an undefined type fails the ENTIRE chunk load
// ("No definition for user data object type 'SpawnPoint'"), taking its terrain and props
// with it. Off by default: keep the maps loadable.
static const bool g_exportGameplayPoints = false;
static const bool g_groundSpansMap = false; // v44: layers tile per 10 m like shipped Blitz cdata
// false = layers are only tile pattern * tileColor (the map's own tints are 0.95..1.0 and
// the tile texture is RGBA4444, so that result is grey; the colour lives in colormap_*.dds)
// tiles = tile texture channel * tileColor exactly as authored: no map-wide colour map,
// no gain, no white wash. The tints are the colours the Blitz material actually declares.
static const bool g_multiplyColormapIntoTiles = false;
static const bool g_tilesWhite = false;
static std::vector<uint8_t> g_colorImage; // copy of customColorTexture RGBA
static int32_t g_colorImageWidth = 0;
static int32_t g_colorImageHeight = 0;
// true only once the terrain block really produced its output (heightmap, tilemask,
// chunk files). Without it every model/water/proxy write lands in the bare
// out\spaces\ default folder, which does not exist, and fails one by one.
static bool g_terrainReady = false;

static const uint32_t BW_COMPRESSED_MAGIC1 = 0x7a697000; // "zip\0"
static const uint32_t BW_COMPRESSED_MAGIC2 = 0x42af9021;
static const uint32_t OSE_SHADOW_MAGIC = 0x00646873; // "shd\0"
static const uint32_t OSE_NORMAL_MAGIC = 0x006d726e; // "nrm\0"
static const uint32_t OSE_AO_MAGIC = 0x006f616d; // "aom\0"
static const uint32_t DDS_MAGIC = 0x20534444; // "DDS "

static std::vector<uint8_t> BWZlibCompress(const uint8_t* data, uint32_t len)
{
    std::vector<uint8_t> out;
    if (data == nullptr || len == 0)
    {
        return out;
    }
    uLongf compressedSz = compressBound(len);
    std::vector<uint8_t> large(compressedSz);
    if (compress2(&large[0], &compressedSz, data, len, 6) != Z_OK)
    {
        return out;
    }
    out.resize(static_cast<uint32_t>(compressedSz) + 3 * sizeof(uint32_t));
    uint32_t* p = reinterpret_cast<uint32_t*>(&out[0]);
    *p++ = BW_COMPRESSED_MAGIC1;
    *p++ = BW_COMPRESSED_MAGIC2;
    *p++ = len;
    memcpy(p, &large[0], compressedSz);
    return out;
}

using namespace DAVA;

// ---------------------------------------------------------------------------
// Blitz landscape .heightmap
//   u32 sizeX, u32 bitsPerSample (16), then sizeX*sizeX little endian uint16
//   stored in 16x16 TILES, tiles row major, 16 rows of 16 samples per tile.
//   sample value = height in millimetres.
// The engine's own heightmap round-trip is not trusted here: PlaceLandscapePoint
// returned a usable height only for the first sampled row of every chunk, which
// wrote terrain blocks with 68 empty rows. The file is read directly instead.
// ---------------------------------------------------------------------------
struct BlitzHeightFile
{
    DAVA::int32 size = 0;
    std::vector<DAVA::float32> metres;

    bool valid() const
    {
        return size > 1 && metres.size() == static_cast<size_t>(size) * static_cast<size_t>(size);
    }
};

static BlitzHeightFile g_heightFile;
static bool g_heightFileFlipV = false;

static bool LoadBlitzHeightmapFile(const DAVA::FilePath& path, BlitzHeightFile& out)
{
    DAVA::File* f = DAVA::File::Create(path, DAVA::File::OPEN | DAVA::File::READ);
    if (f == nullptr)
    {
        return false;
    }

    DAVA::uint32 head[2] = { 0, 0 };
    bool ok = (f->Read(head, (DAVA::uint32)sizeof(head)) == (DAVA::uint32)sizeof(head));
    if (!ok)
    {
        DAVA::Logger::Warning("[TerrainConverter] heightmap '%s': header unreadable",
                              path.GetAbsolutePathname().c_str());
        SafeRelease(f);
        return false;
    }

    // Two header shapes exist: (sizeX, bitsPerSample = 16) with 16x16 tiled
    // storage, and (sizeX, sizeY) with plain row major storage. Anything else is
    // refused with the numbers in the log so the file can be identified.
    const bool tiled = (head[1] == 16 && head[0] > 1 && head[0] <= 8192);
    const bool square = (!tiled && head[1] == head[0] && head[0] > 1 && head[0] <= 8192);
    if (!tiled && !square)
    {
        DAVA::Logger::Warning("[TerrainConverter] heightmap '%s': header %u/%u is neither "
                              "(size, 16 bits) nor (sizeX, sizeY) - rejected",
                              path.GetAbsolutePathname().c_str(), head[0], head[1]);
        SafeRelease(f);
        return false;
    }
    DAVA::Logger::Info("[TerrainConverter] heightmap '%s': %u x %u, %s",
                       path.GetAbsolutePathname().c_str(), head[0], head[1],
                       tiled ? "16x16 tiled 16 bit" : "row major 16 bit");

    ok = true;
    {
        const DAVA::int32 n = (DAVA::int32)head[0];
        const DAVA::uint32 want = (DAVA::uint32)(sizeof(DAVA::uint16) * (size_t)n * (size_t)n);
        std::vector<DAVA::uint16> raw((size_t)n * (size_t)n, 0);
        ok = (f->Read(&raw[0], want) == want);
        if (ok)
        {
            out.size = n;
            out.metres.assign((size_t)n * (size_t)n, 0.0f);
            if (tiled)
            {
                const DAVA::int32 tiles = n / 16;
                size_t k = 0;
                for (DAVA::int32 ty = 0; ty < tiles; ++ty)
                {
                    for (DAVA::int32 tx = 0; tx < tiles; ++tx)
                    {
                        for (DAVA::int32 y = 0; y < 16; ++y)
                        {
                            const DAVA::int32 row = ty * 16 + y;
                            for (DAVA::int32 x = 0; x < 16; ++x)
                            {
                                out.metres[(size_t)row * (size_t)n + (size_t)(tx * 16 + x)] = raw[k + (size_t)x] * 0.001f;
                            }
                            k += 16;
                        }
                    }
                }
            }
            else
            {
                for (size_t i = 0; i < raw.size(); ++i)
                {
                    out.metres[i] = raw[i] * 0.001f;
                }
            }
        }
    }

    SafeRelease(f);
    return ok;
}

static bool FindLandscapeHeightmap(const DAVA::FilePath& dataSource, const DAVA::String& mapName, DAVA::FilePath& outPath)
{
    DAVA::FileSystem* fs = DAVA::FileSystem::Instance();
    if (fs == nullptr)
    {
        return false;
    }

    DAVA::String roots[2];
    roots[0] = dataSource.GetAbsolutePathname() + "/3d/Maps/" + mapName + "/landscape/";
    roots[1] = dataSource.GetAbsolutePathname() + "/3d/Maps/" + mapName + "/";

    for (int32 r = 0; r < 2; ++r)
    {
        DAVA::FilePath dir(roots[r]);
        if (!dir.IsDirectoryPathname())
        {
            dir = DAVA::FilePath(dir.GetAbsolutePathname() + "/");
        }
        if (!fs->IsDirectory(dir))
        {
            DAVA::Logger::Info("[TerrainConverter] heightmap probe: '%s' is not a directory", roots[r].c_str());
            continue;
        }

        const DAVA::String names[3] = { mapName + ".heightmap", "heightmap.heightmap", "landscape.heightmap" };
        for (int32 k = 0; k < 3; ++k)
        {
            DAVA::FilePath candidate(roots[r] + names[k]);
            if (fs->IsFile(candidate))
            {
                outPath = candidate;
                return true;
            }
        }

        const DAVA::Vector<DAVA::FilePath> files = fs->EnumerateFilesInDirectory(dir, false);
        for (size_t i = 0; i < files.size(); ++i)
        {
            DAVA::String lowered = files[i].GetFilename();
            for (size_t c = 0; c < lowered.size(); ++c)
            {
                lowered[c] = static_cast<char>(tolower(static_cast<unsigned char>(lowered[c])));
            }
            if (lowered.size() > 10 && lowered.compare(lowered.size() - 10, 10, ".heightmap") == 0)
            {
                outPath = files[i];
                return true;
            }
        }
    }
    return false;
}

// bilinear sample of the loaded file, u/v in 0..1 over the landscape box
static DAVA::float32 SampleHeightFileOriented(DAVA::float32 u, DAVA::float32 v, bool flipV)
{
    if (!g_heightFile.valid())
    {
        return 0.0f;
    }
    const DAVA::int32 n = g_heightFile.size;
    if (u < 0.0f)
        u = 0.0f;
    if (u > 1.0f)
        u = 1.0f;
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    if (flipV)
    {
        v = 1.0f - v;
    }

    const DAVA::float32 fx = u * (DAVA::float32)(n - 1);
    const DAVA::float32 fy = v * (DAVA::float32)(n - 1);
    DAVA::int32 x0 = (DAVA::int32)fx;
    DAVA::int32 y0 = (DAVA::int32)fy;
    if (x0 > n - 2)
        x0 = n - 2;
    if (y0 > n - 2)
        y0 = n - 2;
    const DAVA::float32 tx = fx - (DAVA::float32)x0;
    const DAVA::float32 ty = fy - (DAVA::float32)y0;

    const DAVA::float32* g = &g_heightFile.metres[0];
    const DAVA::float32 h00 = g[(size_t)y0 * n + x0];
    const DAVA::float32 h10 = g[(size_t)y0 * n + x0 + 1];
    const DAVA::float32 h01 = g[(size_t)(y0 + 1) * n + x0];
    const DAVA::float32 h11 = g[(size_t)(y0 + 1) * n + x0 + 1];
    const DAVA::float32 a = h00 + (h10 - h00) * tx;
    const DAVA::float32 b = h01 + (h11 - h01) * tx;
    return a + (b - a) * ty;
}

static DAVA::float32 SampleHeightFile(DAVA::float32 u, DAVA::float32 v)
{
    return SampleHeightFileOriented(u, v, g_heightFileFlipV);
}

// ---------------------------------------------------------------------------
// The landscape's own heightmap is the authority, exactly like the editor sees
// it: Heightmap::GetPoint() maps a grid cell back to world space, so sampling it
// needs no assumption about file layout, resolution, row order or units. This is
// the same mapping Terrain::PlaceLandscapePoint() uses, minus its early return
// for points outside the bounding box (which is what left 68 of 69 rows at zero).
// ---------------------------------------------------------------------------
static DAVA::Landscape* g_heightLandscape = nullptr;

static bool LandscapeHeightAvailable()
{
    if (g_heightLandscape == nullptr)
    {
        return false;
    }
    DAVA::Heightmap* hm = g_heightLandscape->GetHeightmap();
    return (hm != nullptr && hm->Data() != nullptr && hm->Size() > 1);
}

static DAVA::float32 SampleLandscapeHeightWorld(DAVA::float32 wx, DAVA::float32 wy)
{
    DAVA::Heightmap* hm = g_heightLandscape->GetHeightmap();
    const AABBox3& bb = g_heightLandscape->GetBoundingBox();
    const DAVA::float32 w = bb.max.x > bb.min.x ? (bb.max.x - bb.min.x) : (bb.min.x - bb.max.x);
    const DAVA::float32 h = bb.max.y > bb.min.y ? (bb.max.y - bb.min.y) : (bb.min.y - bb.max.y);
    const DAVA::int32 n = (DAVA::int32)hm->Size();
    if (w < 1.0f || h < 1.0f || n < 2)
    {
        return 0.0f;
    }

    DAVA::float32 fx = (DAVA::float32)n * (wx - bb.min.x) / w;
    DAVA::float32 fy = (DAVA::float32)n * (wy - bb.min.y) / h;
    if (fx < 0.0f)
        fx = 0.0f;
    if (fy < 0.0f)
        fy = 0.0f;
    if (fx > (DAVA::float32)(n - 1))
        fx = (DAVA::float32)(n - 1);
    if (fy > (DAVA::float32)(n - 1))
        fy = (DAVA::float32)(n - 1);

    DAVA::int32 x0 = (DAVA::int32)fx;
    DAVA::int32 y0 = (DAVA::int32)fy;
    if (x0 > n - 2)
        x0 = n - 2;
    if (y0 > n - 2)
        y0 = n - 2;
    const DAVA::float32 tx = fx - (DAVA::float32)x0;
    const DAVA::float32 ty = fy - (DAVA::float32)y0;

    const DAVA::float32 h00 = hm->GetPoint(x0, y0, bb).z;
    const DAVA::float32 h10 = hm->GetPoint(x0 + 1, y0, bb).z;
    const DAVA::float32 h01 = hm->GetPoint(x0, y0 + 1, bb).z;
    const DAVA::float32 h11 = hm->GetPoint(x0 + 1, y0 + 1, bb).z;
    const DAVA::float32 a = h00 + (h10 - h00) * tx;
    const DAVA::float32 b = h01 + (h11 - h01) * tx;
    // GetPoint() is in world metres but the landscape does not start at 0. BW stores
    // absolute millimetres and clips above 65.535 m, so drop to the map's own base
    // level here (see g_heightOffsetM).
    return ((a + (b - a) * ty) + g_heightOffsetM) * g_heightReliefScale;
}

// world metres -> height. Engine heightmap first, .heightmap file as fallback.
static DAVA::float32 HeightAtWorld(DAVA::float32 wx, DAVA::float32 wy, const AABBox3& box)
{
    if (LandscapeHeightAvailable())
    {
        return SampleLandscapeHeightWorld(wx, wy);
    }
    if (!g_heightFile.valid())
    {
        return 0.0f;
    }
    const DAVA::float32 spanX = (box.max.x - box.min.x) > 1.0f ? (box.max.x - box.min.x) : 1.0f;
    const DAVA::float32 spanY = (box.max.y - box.min.y) > 1.0f ? (box.max.y - box.min.y) : 1.0f;
    return SampleHeightFile((wx - box.min.x) / spanX, (wy - box.min.y) / spanY);
}

Converter::Converter(const DAVA::FilePath& templatesPath_, const DAVA::FilePath& clientDataSource_)
    : templatesPath(templatesPath_)
    , clientDataSource(clientDataSource_)
{
}

void Converter::Do(const DAVA::FilePath& filePath, const DAVA::Vector<DAVA::String>& tags)
{
    FilePath::AddResourcesFolder(templatesPath);
    FilePath::AddResourcesFolder(clientDataSource);

    g_normalMapSize = ReadTemplateMapSize("normalMapSize", 32);
    g_aoMapSize = ReadTemplateMapSize("aoMapSize", 32);
    g_shadowMapSize = ReadTemplateMapSize("shadowMapSize", 32);
    g_blendMapSize = ReadTemplateMapSize("blendMapSize", 128);
    DAVA::Logger::Info("[TerrainConverter] space maps: normals %d, ao %d, shadows %d, blend %d",
                       g_normalMapSize, g_aoMapSize, g_shadowMapSize, g_blendMapSize);

    // Model.cpp searches the DataSource by texture name when a material's descriptor
    // cannot resolve one (Blitz keeps the same image under several folders).
    g_bwTextureSearchRoot = clientDataSource.GetAbsolutePathname();

    sceneNodesLinks = new KeyedArchive();
    arenaDefCompilator = ArenaDefCompliator::Create(ModelTemplate::GetArenaDef(), DataOutputPath::GetArenaDef());

    nextid = 0;
    DAVA::Logger::Info("Terrain Converter started: %s", filePath.GetAbsolutePathname().c_str());

    inputFile = filePath;
    String fullPath = inputFile.GetAbsolutePathname();
    size_t noff = fullPath.rfind("Data");
    if (noff < fullPath.size())
    {
        projectPath = fullPath.substr(0, noff);
    }

    FilePath lkaPath = FilePathHelper::GetLKAPath(inputFile, tags);
    DAVA::FileSystem* fs = GetEngineContext()->fileSystem;
    fs->CreateDirectory(lkaPath.GetDirectory());

    // No trailing separator: every consumer adds the one it needs, which is what
    // used to produce "content/rudniki//WoodenStack01_SN.model" and
    // "out\content\rudniki\/images\".
    mapName = inputFile.GetBasename();
    while (!mapName.empty() && (mapName[mapName.length() - 1] == '/' || mapName[mapName.length() - 1] == '\\'))
    {
        mapName.erase(mapName.length() - 1);
    }
    // Belt and braces: every resource name below is built from this string, so a
    // separator that crept into the middle of it would be repeated in all of them.
    {
        DAVA::String collapsed;
        collapsed.reserve(mapName.size());
        for (size_t i = 0; i < mapName.size(); ++i)
        {
            const char c = mapName[i];
            if (c == '/' || c == '\\')
            {
                if (collapsed.empty() || collapsed[collapsed.size() - 1] == '/')
                {
                    continue;
                }
                collapsed.push_back('/');
            }
            else
            {
                collapsed.push_back(c);
            }
        }
        mapName = collapsed;
    }

    arenaDefCompilator->Start(inputFile.GetBasename());

    config = new DAVA::EditorConfig();
    config->ParseConfig(projectPath.GetAbsolutePathname() + "EditorConfig.yaml");

    scene = new Scene();
    SceneFileV2::eError mapLoadError = scene->LoadScene(inputFile);
    if (mapLoadError == SceneFileV2::ERROR_NO_ERROR)
    {
        // update for transform matrix calculation
        scene->transformSystem->Process(0.1f);
        DoConvertHM();
        if (g_terrainReady)
        {
            GenerateMaterialKindMap();
            CreateBorderFile();
            DoConvertModels(tags);
            SetupVersionLKA();

            File* f = File::Create(lkaPath, File::CREATE | File::WRITE);
            if (f != nullptr)
            {
                sceneNodesLinks->Save(f);
                SafeRelease(f);
            }
            else
            {
                DAVA::Logger::Error("[TerrainConverter] can not write %s", lkaPath.GetAbsolutePathname().c_str());
            }

            DAVA::Logger::Info("Terrain Converter finished converting models from %s", inputFile.GetAbsolutePathname().c_str());
        }
        else
        {
            DAVA::Logger::Error("[TerrainConverter] model pass skipped: the terrain did not convert, so "
                                "out\\spaces\\<map>\\ was never created and every model, water and proxy "
                                "write into it would fail. Nothing was written - fix the heightmap/tilemask "
                                "problem reported above first.");
        }
    }
    else
    {
        DAVA::Logger::Error("Failed to load input file %s", inputFile.GetAbsolutePathname().c_str());
    }
    DAVA::Logger::Info("Terrain Converter finished converting %s", (mapName + "\\").c_str());

    arenaDefCompilator->End();

    SafeDelete(config);
    SafeRelease(sceneNodesLinks);
    arenaDefCompilator.reset();
    FilePath::RemoveResourcesFolder(clientDataSource);
    FilePath::RemoveResourcesFolder(templatesPath);
}

// template loads share one shape: File::Create + Read + terminate + release. File::Create returns
// nullptr when the file is not there, and the templates folder is a user setting, so every one of
// them has to be reported instead of dereferenced (a null File* took the editor down)
static bool LoadTemplateFile(const DAVA::FilePath& path, char* buffer, DAVA::uint32 bufferSize)
{
    buffer[0] = 0;

    DAVA::File* f = DAVA::File::Create(path, DAVA::File::OPEN | DAVA::File::READ);
    if (f == nullptr)
    {
        DAVA::Logger::Error("[TerrainConverter] template '%s' not found - check the Templates Path setting",
                            path.GetAbsolutePathname().c_str());
        return false;
    }

    DAVA::uint32 read = f->Read(buffer, bufferSize - 1);
    if (read >= bufferSize)
    {
        read = bufferSize - 1;
    }
    buffer[read] = 0;
    DAVA::SafeRelease(f);
    return true;
}

constexpr int CHUNKBUFFER_SIZE = 1048576; // VCR101: was a macro
void Converter::DoConvertModels(const DAVA::Vector<DAVA::String>& tags)
{
    // prepare buffers
    using namespace DAVA;

    if (LoadTemplateFile(FilePath("~res:/modelTemplate\\model.xml"), modelContentFormatPattern, static_cast<DAVA::uint32>(sizeof(modelContentFormatPattern))) == false)
    {
        DAVA::Logger::Error("[TerrainConverter] %s not converted: modelTemplate\\model.xml is required");
        return;
    }

    LoadTemplateFile(FilePath("~res:/modelTemplate\\speedTree.xml"), speedTreeFormatPattern, static_cast<DAVA::uint32>(sizeof(speedTreeFormatPattern)));

    LoadTemplateFile(FilePath("~res:/modelTemplate\\spawnpoint.xml"), spawnPointFormatPattern, static_cast<DAVA::uint32>(sizeof(spawnPointFormatPattern)));

    LoadTemplateFile(FilePath("~res:/modelTemplate\\botSpawn.xml"), botSpawnFormatPattern, static_cast<DAVA::uint32>(sizeof(botSpawnFormatPattern)));

    LoadTemplateFile(FilePath("~res:/modelTemplate\\patrolNode.xml"), patrolFormatPattern, static_cast<DAVA::uint32>(sizeof(patrolFormatPattern)));

    LoadTemplateFile(FilePath("~res:/modelTemplate\\controlpoint.xml"), controlPointFormatPattern, static_cast<DAVA::uint32>(sizeof(controlPointFormatPattern)));

    LoadTemplateFile(FilePath("~res:/modelTemplate\\water.xml"), waterFormatPattern, static_cast<DAVA::uint32>(sizeof(waterFormatPattern)));

    LoadTemplateFile(FilePath("~res:/modelTemplate\\water.vlo"), waterVloFormatPattern, static_cast<DAVA::uint32>(sizeof(waterVloFormatPattern)));

    File* f = File::Create(FilePath("~res:/modelTemplate\\proxy.xml"), File::OPEN | File::READ);
    if (f)
    {
        uint32 res = f->Read(proxyFormatPattern, sizeof(proxyFormatPattern));
        proxyFormatPattern[res] = 0;
        SafeRelease(f);
    }
    else
    {
        proxyFormatPattern[0] = 0;
        DAVA::Logger::Error("[TerrainConverter] missing template ~res:/modelTemplate/proxy.xml");
    }

    f = File::Create(FilePath("~res:/modelTemplate\\proxy.vlo"), File::OPEN | File::READ);
    if (f)
    {
        uint32 res = f->Read(proxyVloFormatPattern, sizeof(proxyVloFormatPattern));
        proxyVloFormatPattern[res] = 0;
        SafeRelease(f);
    }
    else
    {
        proxyVloFormatPattern[0] = 0;
        DAVA::Logger::Error("[TerrainConverter] missing template ~res:/modelTemplate/proxy.vlo");
    }

    LoadTemplateFile(FilePath("~res:/modelTemplate\\lightOmni.xml"), lightOmniFormatPattern, static_cast<DAVA::uint32>(sizeof(lightOmniFormatPattern)));

    LoadTemplateFile(FilePath("~res:/modelTemplate\\lightSpot.xml"), lightSpotFormatPattern, static_cast<DAVA::uint32>(sizeof(lightSpotFormatPattern)));

    LoadTemplateFile(FilePath("~res:/modelTemplate\\lightDirectional.xml"), lightDirectionalFormatPattern, static_cast<DAVA::uint32>(sizeof(lightDirectionalFormatPattern)));

    LoadTemplateFile(FilePath("~res:/modelTemplate\\lightAmbient.xml"), lightAmbientFormatPattern, static_cast<DAVA::uint32>(sizeof(lightAmbientFormatPattern)));

    strategyPointPattern = Utils::GetContentOfFile(FilePath("~res:/modelTemplate\\chunk\\StrategicPoint.xml"));

    chunkBuffer = new char[CHUNKBUFFER_SIZE];
    CheckNodes(scene, tags);
    CheckWater(scene, tags);
    CheckUserNodes(scene, tags);
    CheckLights(scene, tags);
    CheckPath(scene, tags);

    // ---- publish the real space bounds -------------------------------------
    // space.settings used to be written while the terrain pass was starting, when no
    // item had been placed yet, so <bounds> was the landscape grid alone (-3..2).
    // BW clamps the camera to that box and only streams chunks inside it, which is
    // exactly what was seen: the camera stops at the edge of the real terrain, the
    // water sheet (12 x 20 chunks around the landscape) is never loaded, and the
    // mountains vanish once you get near them because their chunks are out of range.
    // Now the bounds are the union of the landscape grid and everything that was
    // actually written, and every cell in them exists as a chunk.
    {
        DAVA::int32 sx0 = g_minChunk;
        DAVA::int32 sz0 = g_minChunk;
        DAVA::int32 sx1 = g_maxChunk;
        DAVA::int32 sz1 = g_maxChunk;
        if (g_itemMinX <= g_itemMaxX)
        {
            if (g_itemMinX < sx0)
                sx0 = g_itemMinX;
            if (g_itemMaxX > sx1)
                sx1 = g_itemMaxX;
            if (g_itemMinZ < sz0)
                sz0 = g_itemMinZ;
            if (g_itemMaxZ > sz1)
                sz1 = g_itemMaxZ;
        }

        DAVA::int32 created = 0;
        for (DAVA::int32 fz = sz0; fz <= sz1; ++fz)
        {
            for (DAVA::int32 fx = sx0; fx <= sx1; ++fx)
            {
                const String id = outsideChunkIdentifier(fx, fz);
                const FilePath chunkPath(g_spacesPath + id + ".chunk");
                if (DAVA::FileSystem::Instance()->IsFile(chunkPath))
                {
                    continue;
                }
                File* f = File::Create(chunkPath, File::CREATE | File::WRITE);
                if (f != nullptr)
                {
                    const char* emptyChunk = "<root>\r\n</root>\r\n";
                    f->Write(emptyChunk, (uint32)strlen(emptyChunk));
                    SafeRelease(f);
                    ++created;
                }
            }
        }

        g_spaceMinX = sx0;
        g_spaceMaxX = sx1;
        g_spaceMinZ = sz0;
        g_spaceMaxZ = sz1;
        CreateSettingsFile(sx0, sz0, sx1, sz1);

        DAVA::Logger::Info("[TerrainConverter] space bounds published: chunks %d..%d x %d..%d "
                           "(terrain %d..%d, items/water %d..%d x %d..%d, %d empty chunk(s) filled)",
                           sx0, sx1, sz0, sz1, g_minChunk, g_maxChunk,
                           g_itemMinX, g_itemMaxX, g_itemMinZ, g_itemMaxZ, created);
    }

    delete chunkBuffer;
}

bool Converter::GetCollisionType(DAVA::Entity* curr, eModelPreset& result)
{
    KeyedArchive* customProperties = GetCustomPropertiesArchieve(curr);
    if (customProperties && customProperties->IsKeyExists("CollisionType"))
    {
        result = (eModelPreset)customProperties->GetInt32("CollisionType", eMP_NO_COLLISION);
        return true;
    }

    // BW-compliance patch: WG-era assets store the collision type in the
    // CollisionTypeComponent (runtime-only in revanced, absent from the
    // KeyedArchive). Read it as a fallback so extracted Blitz assets export
    // with their original collision semantics.
    DAVA::CollisionTypeComponent* collisionComponent = curr->GetComponent<DAVA::CollisionTypeComponent>();
    if (collisionComponent != nullptr)
    {
        DAVA::int32 componentType = collisionComponent->GetCollisionType();
        if (componentType >= 0)
        {
            result = (eModelPreset)componentType;
            return true;
        }
    }
    {
        int size = curr->GetChildrenCount();
        for (int i = 0; i < size; i++)
        {
            if (GetCollisionType(curr->GetChild(i), result))
            {
                return true;
            }
        }
        return false;
    }
}

void Converter::CheckPath(Entity* curr, const Vector<String>& tags)
{
    if (!CheckTags(curr, tags))
    {
        return;
    }
    bool processed = false;
    PathComponent* path = GetPathComponent(curr);
    if (path)
    {
        if (path->GetPoints().size() > 1)
        {
            KeyedArchive* customProperties = GetCustomPropertiesArchieve(curr);
            int32 team = customProperties->GetInt32("team", 0);

            const String& botname = customProperties->GetString("botname", "");

            const String& vehicle = customProperties->GetString("vehicle", "none");
            const String& script = customProperties->GetString("script", "");
            int32 group = customProperties->GetInt32("group", 0);
            bool isPerformanceTestBot = customProperties->GetBool("performanceTestBot");
            UniqueID guid = GetEntityID(curr);
            Snprintf(resultBuff, sizeof(resultBuff), botSpawnFormatPattern,
                     guid.toString().c_str(),
                     team,
                     vehicle.c_str(),
                     (botname.empty() ? botname.c_str() : Format("\n\t\t\t<botName>\t%s\t</botName>", botname.c_str()).c_str()),
                     nextid,
                     (isPerformanceTestBot ? "1" : "0"),
                     script.c_str(),
                     group);

            PathComponent::Waypoint* startWP = path->GetPoints()[0];
            PathComponent::Waypoint* nextWP = NULL;
            Matrix4 pos;
            Vector3 nextPos;
            if (startWP->edges.size() > 0)
            {
                nextWP = startWP->edges[0]->destination;
                Quaternion q = Quaternion::MakeRotation(Vector3(0.0f, 1.0f,
                                                                0.0f),
                                                        (nextWP->position - startWP->position));
                pos = q.GetMatrix();
            }
            pos.SetTranslationVector(startWP->position);
            AddEntityToChunk(resultBuff, pos, 0);
            if (nextWP)
            {
                AddWayPointToChunk(nextWP, 0, curr);
                nextid++;
            }
        }
        processed = true;
    }
    if (!processed)
    {
        int size = curr->GetChildrenCount();
        for (int i = 0; i < size; i++)
        {
            CheckPath(curr->GetChild(i), tags);
        }
    }
} // Converter::CheckPath

void Converter::AddWayPointToChunk(PathComponent::Waypoint* wp, uint32 pointNumber, DAVA::Entity* parentEntity)
{
    UniqueID guid = GetEntityID(parentEntity, pointNumber);
    String directive = wp->GetProperties()->GetString("directive", "");
    bool needNewPath = false;
    String::size_type switchPos = directive.find("switch(");
    if (switchPos != std::string::npos)
    {
        String s = Format(", %d", nextid + 1);
        directive = directive.insert(directive.find(")", switchPos), s);
        needNewPath = true;
    }
    Snprintf(resultBuff, sizeof(resultBuff), patrolFormatPattern,
             guid.toString().c_str(),
             nextid,
             pointNumber,
             directive.c_str());
    Matrix4 wt;
    wt.BuildTranslation(wp->position);
    AddEntityToChunk(resultBuff, wt, 0);

    if (wp->edges.size() == 1)
    {
        if (needNewPath)
        {
            nextid++;
            AddWayPointToChunk(wp->edges[0]->destination, 0, parentEntity);
        }
        else
        {
            AddWayPointToChunk(wp->edges[0]->destination, pointNumber + 1, parentEntity);
        }
    }
    else if (wp->edges.size() > 1)
    {
        for (size_t i = 0; i < wp->edges.size(); i++)
        {
            // todo here we need create correct branching
            nextid++;
            AddWayPointToChunk(wp->edges[i]->destination, 0, parentEntity);
        }
    }
}

void Converter::AddEntityToChunk(char* textBuf, const Matrix4& wt, const uint32 nodeId)
{
    Matrix4 t = Model::globalTransformInverse * wt * Model::globalTransform;

    Vector3 offset = Vector3(0, 0, 0) * t;
    int32 chunkX = (int32)floor(offset.x / 100.0f);
    int32 chunkZ = (int32)floor(offset.z / 100.0f);
    // Remember where items actually land: mountains and other distant props sit far
    // outside the landscape's own chunk grid, and space.settings has to cover them or
    // the client never loads their chunks (that is why the mountains were missing).
    if (chunkX < g_itemMinX)
        g_itemMinX = chunkX;
    if (chunkX > g_itemMaxX)
        g_itemMaxX = chunkX;
    if (chunkZ < g_itemMinZ)
        g_itemMinZ = chunkZ;
    if (chunkZ > g_itemMaxZ)
        g_itemMaxZ = chunkZ;
    String identifier = outsideChunkIdentifier(chunkX, chunkZ);

    t = t * Matrix4::MakeTranslation(Vector3(-chunkX * 100.0f, 0, -chunkZ * 100.0f));

    char* transformStart = strstr(textBuf, "<transform>");
    if (transformStart == nullptr)
    {
        DAVA::Logger::Error("Converter::AddEntityToChunk - template has no <transform> block - skipped");
        return;
    }

    // Replace the template's whole <transform>...</transform> body. The old code
    // resumed 12 characters after "<transform>", which lands inside the tag when
    // the template is written without a newline and left the old rows in place -
    // BigWorld then reads a nameless start tag and reports
    // XMLSection::createFromStream: Tags do not match Start tag = "" End tag = "row0".
    char* transformEnd = strstr(transformStart, "</transform>");
    if (transformEnd == nullptr)
    {
        DAVA::Logger::Error("Converter::AddEntityToChunk - template has no </transform> - skipped");
        return;
    }

    Snprintf(transformBuf, sizeof(transformBuf),
             "\r\n\t\t\t<row0> %f %f %f </row0>\r\n\t\t\t<row1> %f %f %f </row1>\r\n\t\t\t<row2> %f %f %f </row2>\r\n\t\t\t<row3> %f %f %f </row3>\r\n\t\t",
             t._00, t._01, t._02,
             t._10, t._11, t._12,
             t._20, t._21, t._22,
             // BW up is row3.y: move only the translation, never the basis rows, or every
             // prop would also be squashed. Same offset and scale as the terrain, so
             // objects keep standing on the ground.
             t._30, (t._31 + g_heightOffsetM) * g_heightReliefScale, t._32);

    File* f = File::Create(g_spacesPath + identifier + ".chunk", File::OPEN | File::READ);
    if (f == nullptr)
    {
        // Item in a cell the terrain pass never wrote (mountains and other distant
        // props sit far outside the landscape grid). Create the chunk, but with an
        // EMPTY root: a chunk that carries a <terrain> item makes BW look for
        // <id>.cdata/terrain2 and fail with "DataSection ... is NULL (FNF)".
        f = File::Create(g_spacesPath + identifier + ".chunk", File::CREATE | File::WRITE);
        if (f != nullptr)
        {
            const char* emptyChunk = "<root>\r\n</root>\r\n";
            f->Write(emptyChunk, (uint32)strlen(emptyChunk));
            SafeRelease(f);
        }
        f = File::Create(g_spacesPath + identifier + ".chunk", File::OPEN | File::READ);
    }
    if (f)
    {
        uint32 res = f->Read(chunkBuffer, CHUNKBUFFER_SIZE - 1); // keep room for the terminator
        chunkBuffer[res] = 0;
        SafeRelease(f);

        char* str = strstr(chunkBuffer, "</root>");
        if (str != NULL)
        {
            // str += 6;
            const char* afterOpen = transformStart + strlen("<transform>");
            f = File::Create(g_spacesPath + identifier + ".chunk", File::CREATE | File::WRITE);
            f->Write(chunkBuffer, (uint32)(str - chunkBuffer));
            f->Write(textBuf, (uint32)(afterOpen - textBuf)); // up to and including <transform>
            f->Write(transformBuf, (uint32)strlen(transformBuf)); // fresh rows only
            f->Write(transformEnd, (uint32)strlen(transformEnd)); // </transform> and the rest
            f->Write(str, (uint32)strlen(str));
            SafeRelease(f);
        }
    }
    resultBuff[0] = 0;
    transformBuf[0] = 0;

    if (nodeId != 0)
    {
        // store and increment counter for models in chunk
        // bigger 16bit is chunkID
        uint32 value = (chunkIDFromChunkIndexes(chunkX, chunkZ) & 0xFFFF); // << 16;
        value = value << 16;
        // lower 16biit is model index
        value += chunkIndices[identifier];
        // store value
        sceneNodesLinks->SetUInt32(Format("%u", nodeId), value);

        // here is increment
        chunkIndices[identifier] = chunkIndices[identifier] + 1;
        if (chunkIndices[identifier] > 255)
        {
            DAVA::Logger::Error("Error, objects counter on chunk %s is %d (bigger than 255!)",
                                identifier.c_str(), chunkIndices[identifier]);
        }
    }
} // Converter::AddEntityToChunk

// Water is a BW water body: _<uid>.vlo (WaterState) plus the _<uid>.odata grid that
// BW always expects beside it. A water VLO with no .odata makes EditorChunkWater take
// the legacy oldCreate path and build the very large object on the loading thread:
//    ASSERTION FAILED: Moo::isRenderThread()
//    very_large_object_behaviour.cpp(51) <- EditorChunkWater::oldCreate <- Chunk::loadInclude
// so the two files are always written together. Flip g_exportWater off to fall back to
// the old static-plane export instead.
static const bool g_exportWater = true;

// Fallback only, used when g_exportWater is off: draws the sheet as an ordinary static
// model (a proxy VLO when it is big) instead of a water body.
static const bool g_waterAsStaticModel = true;

// One line per entity, written next to the export. Guessing at which node is the water
// has cost several rounds; the report names every candidate with its world size, its
// collision tag and its material chain, so the answer comes from the scene itself.
static void ReportEntity(DAVA::Entity* entity, const DAVA::String& mapName, DAVA::int32 collision)
{
    if (entity == nullptr)
    {
        return;
    }
    DAVA::RenderObject* renderObject = DAVA::GetRenderObject(entity);
    if (renderObject == nullptr || renderObject->GetRenderBatchCount() == 0)
    {
        return;
    }
    // Opened and closed on every call: a File* kept in a static was never flushed
    // or released, so a run that ended without unwinding left a 0 KB file and
    // Windows kept the handle locked for as long as ResourceEditor was open.
    // The text is kept per map, so converting a second map in the same session
    // starts its own file instead of continuing the first one.
    static DAVA::String reportMap;
    static DAVA::String reportText;
    const DAVA::AABBox3 box = entity->GetWTMaximumBoundingBoxSlow();
    const DAVA::Vector3 size = box.GetSize();
    const DAVA::Vector3 center = box.GetCenter();
    DAVA::String materials;
    for (DAVA::uint32 k = 0; k < renderObject->GetRenderBatchCount() && k < 4; ++k)
    {
        DAVA::RenderBatch* batch = renderObject->GetRenderBatch(k);
        DAVA::NMaterial* material = (batch != nullptr) ? batch->GetMaterial() : nullptr;
        if (material == nullptr)
        {
            continue;
        }
        if (!materials.empty())
        {
            materials += '|';
        }
        materials += material->GetMaterialName().c_str();
    }
    char line[1024];
    Snprintf(line, sizeof(line), "%s\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%d\t%u\t%s\n",
             entity->GetName().c_str(), size.x, size.y, size.z,
             center.x, center.y, center.z, static_cast<int>(collision),
             static_cast<unsigned>(renderObject->GetRenderBatchCount()), materials.c_str());
    // Whole file is rewritten with the only flags this project uses elsewhere
    // (CREATE|WRITE), so there is no append mode to get wrong.
    if (reportMap != mapName)
    {
        reportMap = mapName;
        reportText = "name\tsize_x\tsize_y\tsize_z\tcenter_x\tcenter_y\tcenter_z\tcollision\tbatches\tmaterials\n";
    }
    reportText += line;
    DAVA::File* report = DAVA::File::Create(DAVA::FilePath("out\\entity_report_" + mapName + ".txt"),
                                            DAVA::File::CREATE | DAVA::File::WRITE);
    if (report == nullptr)
    {
        return;
    }
    report->Write(reportText.c_str(), static_cast<DAVA::uint32>(reportText.size()));
    DAVA::SafeRelease(report);
}

void Converter::CheckWater(Entity* curr, const Vector<String>& tags)
{
    if (!CheckTags(curr, tags))
    {
        return;
    }
    // Diagnostics: rudniki does contain water, but nothing reaches AddWater, so the
    // detection has to be visible before the export is switched on. Anything whose name
    // or material chain smells of water, and anything big and dead flat, is reported with
    // its world size, so the next convert names the real water node.
    {
        const String nodeName = curr->GetName().c_str();
        String lower = nodeName;
        for (size_t c = 0; c < lower.size(); ++c)
        {
            lower[c] = static_cast<char>(tolower(static_cast<unsigned char>(lower[c])));
        }
        const bool byName = lower.find("water") != String::npos || lower.find("river") != String::npos ||
        lower.find("lake") != String::npos || lower.find("pond") != String::npos ||
        lower.find("sea") != String::npos || lower.find("ocean") != String::npos ||
        lower.find("wave") != String::npos || lower.find("surf") != String::npos ||
        lower.find("liquid") != String::npos || lower.find("voda") != String::npos ||
        lower.find("\xd0\xb2\xd0\xbe\xd0\xb4") != String::npos; // "vod" in UTF-8
        const bool byMaterial = IsWaterDecoration(curr);
        const AABBox3 wt = curr->GetWTMaximumBoundingBoxSlow();
        const Vector3 sz = wt.GetSize();
        eModelPreset probePreset = eMP_NO_COLLISION;
        const bool hasPreset = GetCollisionType(curr, probePreset);
        if (byName || byMaterial)
        {
            DAVA::Logger::Warning("[TerrainConverter] water-like entity '%s' (name=%d material=%d collision=%d) "
                                  "size %.2f x %.2f x %.2f at (%.2f %.2f %.2f), children %d - not exported yet, "
                                  "BW water needs its .odata grid",
                                  nodeName.c_str(), byName ? 1 : 0, byMaterial ? 1 : 0, hasPreset ? (int32)probePreset : -1,
                                  sz.x, sz.y, sz.z, wt.min.x, wt.min.y, wt.min.z, curr->GetChildrenCount());
        }
        else if (sz.x > 25.0f && sz.y > 25.0f && sz.z < 2.0f)
        {
            DAVA::Logger::Info("[TerrainConverter] flat plane '%s' size %.1f x %.1f x %.2f at (%.1f %.1f %.1f), "
                               "collision=%d - possible water surface",
                               nodeName.c_str(), sz.x, sz.y, sz.z, wt.min.x, wt.min.y, wt.min.z,
                               hasPreset ? (int32)probePreset : -1);
        }
    }

    bool processed = false;
    KeyedArchive* customProperties = GetCustomPropertiesArchieve(curr);
    {
        eModelPreset probe = eMP_NO_COLLISION;
        if (GetCollisionType(curr, probe) && probe == eMP_WATER &&
            (customProperties == nullptr || !customProperties->IsKeyExists("editor.referenceToOwner")))
        {
            DAVA::Logger::Info("[TerrainConverter] water candidate '%s' has CollisionType water but no "
                               "editor.referenceToOwner - left to the material fallback in CheckNodes",
                               curr->GetName().c_str());
        }
    }
    if (customProperties)
    {
        if (customProperties->IsKeyExists("editor.referenceToOwner"))
        {
            eModelPreset collision = eMP_NO_COLLISION;
            GetCollisionType(curr, collision);
            if (collision == eMP_WATER && IsWaterDecoration(curr) == false && g_waterAsStaticModel == false)
            {
                UniqueID guid = GetEntityID(curr);
                String guidStr = MakeFilenameToken(guid.toString());
                guidStr = MakeFilenameToken(customProperties->GetString("guid", guidStr));
                AddWater(curr, guidStr);
            }
            else if (collision == eMP_WATER && IsWaterDecoration(curr))
            {
                DAVA::Logger::Info("[TerrainConverter] water decoration '%s' skipped - flat FX plane over the real water, not a water surface",
                                   curr->GetName().c_str());
            }
            else if (collision == eMP_WATER)
            {
                DAVA::Logger::Info("[TerrainConverter] water surface '%s' handled by the static-plane export in CheckNodes "
                                   "(BW water body needs the .odata grid)",
                                   curr->GetName().c_str());
            }
            processed = true;
        }
    }
    if (!processed)
    {
        int size = curr->GetChildrenCount();
        for (int i = 0; i < size; i++)
        {
            CheckWater(curr->GetChild(i), tags);
        }
    }
}

void Converter::CheckUserNodes(Entity* curr, const Vector<String>& tags)
{
    if (!CheckTags(curr, tags))
    {
        return;
    }
    bool processed = false;
    UserComponent* userComp = curr->GetComponent<DAVA::UserComponent>();
    if (userComp)
    {
        AddPointToChunk(curr);
        processed = true;
    }
    if (!processed)
    {
        int32 size = curr->GetChildrenCount();
        for (int32 i = 0; i < size; i++)
        {
            CheckUserNodes(curr->GetChild(i), tags);
        }
    }
}

void Converter::CheckLights(Entity* curr, const Vector<String>& tags)
{
    if (!CheckTags(curr, tags))
    {
        return;
    }
    bool processed = false;
    DAVA::LightComponent* lightComp = curr->GetComponent<DAVA::LightComponent>();
    if (lightComp)
    {
        AddLightToChunk(curr, lightComp);
        processed = true;
    }
    if (!processed)
    {
        int32 size = curr->GetChildrenCount();
        for (int32 i = 0; i < size; i++)
        {
            CheckLights(curr->GetChild(i), tags);
        }
    }
} // Converter::CheckLights

void Converter::AddLightToChunk(Entity* node, DAVA::LightComponent* lightComp)
{
    DAVA::Logger::Info("AddLightToChunk %s", node->GetName().c_str());
    DAVA::Light* light = lightComp->GetLightObject();
    if (light == nullptr)
    {
        DAVA::Logger::Error("Converter::AddLightToChunk light component without a light object on %s - skipped",
                            node->GetName().c_str());
        return;
    }

    const Matrix4 wt = GetEntityWorldTransform(node);
    // rebase into BigWorld space (the same sandwich AddEntityToChunk applies to the item transform)
    const Matrix4 t = Model::globalTransformInverse * wt * Model::globalTransform;

    // world-space position in BigWorld axes
    const Vector3 posBW = Vector3(0.0f, 0.0f, 0.0f) * t;
    // chunk items store chunk-local coordinates
    const int32 chunkX = (int32)floor(posBW.x / 100.0f);
    const int32 chunkZ = (int32)floor(posBW.z / 100.0f);
    Vector3 localPos = posBW - Vector3(chunkX * 100.0f, 0.0f, chunkZ * 100.0f);
    // same base-level shift the terrain and the item transforms get
    localPos.y = (localPos.y + g_heightOffsetM) * g_heightReliefScale;

    // Dava lights keep the direction on the light object (serialized with the scene);
    // fall back to the entity -Y axis, exactly like Light::SetPositionDirectionFromMatrix
    Vector3 dirDava = light->GetDirection();
    if (dirDava.SquareLength() < 0.0001f)
    {
        dirDava = MultiplyVectorMat3x3(Vector3(0.0f, -1.0f, 0.0f), wt);
    }
    // direction vectors rebase with the same row-vector convention as points
    Vector3 dirBW = MultiplyVectorMat3x3(dirDava, Model::globalTransform);
    dirBW.Normalize();

    const Color diffuse = light->GetDiffuseColor();
    const float32 multiplier = light->GetIntensity();

    // Dava lights carry no radius / cone, so these are default-until-overridden custom properties
    KeyedArchive* customProperties = GetCustomPropertiesArchieve(node);
    const int32 priority = customProperties ? customProperties->GetInt32("bw.priority", 0) : 0;
    const float32 innerRadius = customProperties ? customProperties->GetFloat("bw.innerRadius", DEFAULT_LIGHT_INNER_RADIUS) : DEFAULT_LIGHT_INNER_RADIUS;
    const float32 outerRadius = customProperties ? customProperties->GetFloat("bw.outerRadius", DEFAULT_LIGHT_OUTER_RADIUS) : DEFAULT_LIGHT_OUTER_RADIUS;
    const float32 cosConeAngle = customProperties ? customProperties->GetFloat("bw.cosConeAngle", DEFAULT_LIGHT_CONE_COS) : DEFAULT_LIGHT_CONE_COS;

    // BW 2.0.1 flags (WE's light mode switch); OSE 14 readers ignore them
    const bool isDynamic = customProperties ? customProperties->GetBool("bw.dynamic", light->IsDynamic()) : light->IsDynamic();
    const char* dynamicStr = isDynamic ? "true" : "false";
    const char* staticStr = isDynamic ? "false" : "true";
    const char* specularStr = "true";

    switch (light->GetType())
    {
    case DAVA::Light::TYPE_POINT:
        Snprintf(resultBuff, sizeof(resultBuff), lightOmniFormatPattern,
                 diffuse.r * 255.0f, diffuse.g * 255.0f, diffuse.b * 255.0f,
                 localPos.x, localPos.y, localPos.z,
                 innerRadius, outerRadius, priority, multiplier,
                 dynamicStr, staticStr, specularStr);
        break;
    case DAVA::Light::TYPE_SPOT:
        Snprintf(resultBuff, sizeof(resultBuff), lightSpotFormatPattern,
                 diffuse.r * 255.0f, diffuse.g * 255.0f, diffuse.b * 255.0f,
                 localPos.x, localPos.y, localPos.z,
                 dirBW.x, dirBW.y, dirBW.z,
                 innerRadius, outerRadius, cosConeAngle, priority, multiplier,
                 dynamicStr, staticStr, specularStr);
        break;
    case DAVA::Light::TYPE_DIRECTIONAL:
        Snprintf(resultBuff, sizeof(resultBuff), lightDirectionalFormatPattern,
                 diffuse.r * 255.0f, diffuse.g * 255.0f, diffuse.b * 255.0f,
                 dirBW.x, dirBW.y, dirBW.z,
                 multiplier);
        break;
    case DAVA::Light::TYPE_SKY: // Dava sky light has no BigWorld counterpart - emit as ambient
    case DAVA::Light::TYPE_AMBIENT:
        Snprintf(resultBuff, sizeof(resultBuff), lightAmbientFormatPattern,
                 diffuse.r * 255.0f, diffuse.g * 255.0f, diffuse.b * 255.0f,
                 multiplier);
        break;
    default:
        DAVA::Logger::Error("Converter::AddLightToChunk unsupported light type %d on %s - skipped",
                            (int)light->GetType(), node->GetName().c_str());
        resultBuff[0] = 0;
        return;
    }

    AddEntityToChunk(resultBuff, wt, 0);
    resultBuff[0] = 0;
} // Converter::AddLightToChunk

bool Converter::CheckTags(Entity* curr, const DAVA::Vector<DAVA::String>& tags)
{
    bool okTags = true; // all nodes should be converted by default
    if (curr && tags.size() > 0)
    {
        KeyedArchive* customProperties = GetCustomPropertiesArchieve(curr);
        if (customProperties)
        {
            String currTags = customProperties->GetString("tags");
            if (currTags.size() > 0)
            {
                // found non-empty tags in node (if we won't find any of tags - node must not be converted)
                okTags = false;
                for (const auto& tag : tags)
                {
                    if (currTags.find(tag.c_str()) != String::npos)
                    {
                        // found tag in node tags - node should be converted
                        DAVA::Logger::Info("requested tags found in %s (%s)", curr->GetName().c_str(), currTags.c_str());
                        okTags = true;
                        break;
                    }
                }
                if (!okTags)
                {
                    DAVA::Logger::Info("requested tags not found in %s (%s) - skipped", curr->GetName().c_str(), currTags.c_str());
                }
            }
        }
    }
    return okTags;
}

void Converter::CheckNodes(Entity* curr, const Vector<String>& tags)
{
    if (!CheckTags(curr, tags))
    {
        return;
    }
    {
        eModelPreset reportCollision = eMP_NO_COLLISION;
        GetCollisionType(curr, reportCollision);
        ReportEntity(curr, mapName, static_cast<DAVA::int32>(reportCollision));
    }

    bool processed = false;
    bool derivedOwner = false;
    String owner;

    KeyedArchive* customProperties = GetCustomPropertiesArchieve(curr);
    if (customProperties && customProperties->IsKeyExists("editor.referenceToOwner"))
    {
        owner = customProperties->GetString("editor.referenceToOwner", "");
        processed = true;
    }
    else if (HasMeshRenderObject(curr))
    {
        // BW-compliance patch: WG's original pipeline required every exportable
        // static to carry editor.referenceToOwner. Scenes rebuilt from extracted
        // assets don't have it, so derive a model name from the entity name and
        // keep any explicit CollisionType (default: eMP_BUILDING). Same-named
        // entities dedupe into one model, like WG's referenceToOwner did.
        eModelPreset taggedCollision = eMP_NO_COLLISION;
        const bool hasExplicitCollisionTag = GetCollisionType(curr, taggedCollision);
        if (!hasExplicitCollisionTag && g_waterAsStaticModel == false && DetectWaterPlane(curr))
        {
            // BW-compliance patch: publish-stripped scenes can't carry WG's
            // editor.referenceToOwner / CollisionType=Water tags, and Blitz
            // water is manual plane meshes with a Water* material rather
            // than a tagged water object. Route detected water planes
            // straight to the VLO writer (an explicit CollisionType tag
            // still overrides detection).
            DAVA::Logger::Info("[TerrainConverter] '%s' detected as a water plane - "
                               "exporting as .vlo",
                               curr->GetName().c_str());
            UniqueID guid = GetEntityID(curr);
            String guidStr = guid.toString();
            KeyedArchive* waterProps = GetCustomPropertiesArchieve(curr);
            if (waterProps != nullptr)
            {
                guidStr = waterProps->GetString("guid", guidStr);
            }
            AddWater(curr, guidStr);
            processed = true; // don't recurse into water planes either
        }
        else
        {
            owner = SanitizeModelName(curr->GetName());
            derivedOwner = true;
            DAVA::Logger::Warning("[TerrainConverter] entity '%s' has no editor.referenceToOwner - "
                                  "derived model name '%s' (default collision: eMP_BUILDING)",
                                  curr->GetName().c_str(), owner.c_str());
            processed = true;
        }
    }

    if (processed && !owner.empty())
    {
        eModelPreset collision = eMP_NO_COLLISION;
        GetCollisionType(curr, collision);
        if (collision == eMP_NO_COLLISION && derivedOwner)
        {
            collision = eMP_BUILDING; // fallback default only for derived models
        }

        const bool waterBody = (collision == eMP_WATER && IsWaterDecoration(curr) == false);
        bool waterSurface = false;
        if (waterBody)
        {
            if (g_exportWater)
            {
                DAVA::Logger::Warning("[TerrainConverter] water surface '%s' exported as a BW water VLO + .odata grid",
                                      curr->GetName().c_str());
            }
            else if (g_waterAsStaticModel)
            {
                waterSurface = true;
                DAVA::Logger::Warning("[TerrainConverter] water surface '%s' exported as a static plane - "
                                      "g_exportWater is off",
                                      curr->GetName().c_str());
            }
        }

        if (waterBody && g_exportWater)
        {
            // The VLO route: AddWater cuts the sheet into 90 m tiles, each with the
            // verified .odata grid, and activates them from their own chunks.
            UniqueID waterGuid = GetEntityID(curr);
            String waterGuidStr = MakeFilenameToken(waterGuid.toString());
            if (customProperties != nullptr)
            {
                waterGuidStr = MakeFilenameToken(customProperties->GetString("guid", waterGuidStr));
            }
            AddWater(curr, waterGuidStr);
        }
        else if (collision == eMP_NO_COLLISION && !derivedOwner)
        {
            // tagged as NO_COLLISION on purpose: skip, as WG's pipeline did
        }
        else if (collision == eMP_SPEED_TREE)
        {
            if (savedTrees.find(owner) == savedTrees.end())
            {
                // ServerSPT and its destructible XML were written against the old
                // "name/" form, so it keeps getting one.
                savedTrees[owner] = new ServerSPT(owner, mapName + "/", curr);
            }
            if (!savedTrees[owner]->IsWrong())
            {
                AddTreeToChunk(curr, savedTrees[owner]);
            }
        }
        else if (collision == eMP_WATER && IsWaterDecoration(curr))
        {
            DAVA::Logger::Info("[TerrainConverter] water decoration '%s' skipped - flat FX plane over the real water, not a water surface",
                               curr->GetName().c_str());
        }
        else if (collision == eMP_WATER && derivedOwner && g_waterAsStaticModel == false)
        {
            // BW-compliance patch: water surface tagged via CollisionType=Water
            // but without editor.referenceToOwner - export as .vlo like WG did,
            // instead of silently dropping it (CheckWater only handles the
            // fully-tagged referenceToOwner case).
            UniqueID guid = GetEntityID(curr);
            String guidStr = MakeFilenameToken(guid.toString());
            if (customProperties != nullptr)
            {
                guidStr = MakeFilenameToken(customProperties->GetString("guid", guidStr));
            }
            AddWater(curr, guidStr);
        }
        else if (collision != eMP_WATER || waterSurface)
        {
            if (savedModels.find(owner) == savedModels.end())
            {
                const AABBox3& wtBox = curr->GetWTMaximumBoundingBoxSlow();
                float32 dx = wtBox.GetSize().x;
                float32 dy = wtBox.GetSize().y;
                float32 dz = wtBox.GetSize().z;
                if (dx > 100.0f || dy > 100.0f || dz > 100.0f)
                {
                    if (dx > 200.0f || dy > 200.0f)
                    {
                        // really big objects export as proxy VLOs below
                        DAVA::Logger::Info("[TerrainConverter] '%s' spans more than 200m - "
                                           "exporting as proxy VLO",
                                           owner.c_str());
                    }
                    else
                    {
                        DAVA::Logger::Warning("bbox is too big %s %s min=(%f, %f, %f) max=(%f, %f, %f)",
                                              mapName.c_str(),
                                              owner.c_str(), wtBox.min.x, wtBox.min.y, wtBox.min.z, wtBox.max.x, wtBox.max.y,
                                              wtBox.max.z);
                    }
                }
                savedModels[owner] = new Model(owner, mapName, collision, curr, config);
            }
            if (!savedModels[owner]->IsWrong())
            {
                const DAVA::AABBox3& itemBox = curr->GetWTMaximumBoundingBoxSlow();
                if (itemBox.GetSize().x > 200.0f || itemBox.GetSize().y > 200.0f)
                {
                    // Cross-chunk giant (mountains, sidewalks, decorations). The proxy
                    // VLO route is dead weight in practice: BW only loads a .vlo that has
                    // its companion .odata, which this converter does not write, so every
                    // proxy came out invisible (that is why the mountains were missing).
                    // A plain chunk item is drawn wherever its chunk is, so keep the model
                    // and let the widened space bounds cover the chunk it lands in.
                    if (g_useProxyVlo && waterSurface == false)
                    {
                        AddProxyVLO(curr, savedModels[owner]);
                    }
                    else
                    {
                        DAVA::Logger::Info("[TerrainConverter] '%s' spans %.0f m - written as a chunk item "
                                           "(a proxy VLO stays invisible without its .odata)",
                                           owner.c_str(), itemBox.GetSize().x);
                        AddModelToChunk(curr, savedModels[owner]);
                    }
                }
                else
                {
                    AddModelToChunk(curr, savedModels[owner]);
                }
            }
        }
    }
    else if (!processed)
    {
        int size = curr->GetChildrenCount();
        for (int i = 0; i < size; i++)
        {
            CheckNodes(curr->GetChild(i), tags);
        }
    }
} // Converter::CheckNodes

// Every resource name handed to BigWorld goes through this. A duplicated
// separator ("content/rudniki//Stone_ss04.model") is a failed lookup on the
// engine side, and it only ever comes from two path fragments being joined with
// their own separators, so collapse runs of "/" and "\" into one "/".
static DAVA::String BwResPath(const DAVA::String& path)
{
    DAVA::String out;
    out.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i)
    {
        const char c = path[i];
        const bool isSeparator = (c == '/' || c == '\\');
        if (isSeparator)
        {
            if (out.empty() || out[out.size() - 1] == '/')
            {
                if (out.empty())
                {
                    out.push_back('/');
                }
                continue;
            }
            out.push_back('/');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

void Converter::AddModelToChunk(Entity* node, Model* model)
{
    {
        // GetName() is a FastName, it does not convert to String implicitly
        const String skyName(node->GetName().c_str());
        // ASCII lower-case match, no DAVA string helper involved
        bool isSkyObject = false;
        for (String::size_type i = 0; i + 2 < skyName.size(); ++i)
        {
            char chars[3] = { skyName[i], skyName[i + 1], skyName[i + 2] };
            for (int32 ci = 0; ci < 3; ++ci)
            {
                if (chars[ci] >= 'A' && chars[ci] <= 'Z')
                {
                    chars[ci] = (char)(chars[ci] - 'A' + 'a');
                }
            }
            if (chars[0] == 's' && chars[1] == 'k' && chars[2] == 'y')
            {
                isSkyObject = true;
                break;
            }
        }
        if (isSkyObject)
        {
            DAVA::Logger::Info("[TerrainConverter] sky object '%s' left out of the chunk list - BW draws the sky "
                               "from space.settings",
                               node->GetName().c_str());
            return;
        }
    }

    DAVA::Logger::Info("AddModelToChunk model %s", model->GetModelName().c_str());
    Snprintf(resultBuff, sizeof(resultBuff), modelContentFormatPattern, "",
             BwResPath("content/" + mapName + "/" + model->GetModelName()).c_str(),
             "<destructibleState> undamaged </destructibleState>");
    AddEntityToChunk(resultBuff, GetEntityWorldTransform(node), node->GetID());
    resultBuff[0] = 0;
}

const String SPAWN_PREFERRED_VEHICLE_TYPES[eSPVT_COUNT] = { "&lt;any&gt;", "lightTank", "mediumTank", "heavyTank", "AT-SPG" };

// ---------------------------------------------------------------------------
// SpeedTree 4.x .spt export, inline so the converter does not need a second
// translation unit (the plugin is built as a unity pack and a bare include of a
// sibling header is not resolvable from there).
//
// A .spt is a run of chunks, and its texture references are stored the way the
// SpeedTree modeler wrote them, e.g.
//   C:\__WORK__\SpeedTree\SPT Working Data\Coconut Palm\\CoconutPalm.dds
// which BigWorld can not resolve. Every length prefixed texture string is
// rewritten to <resPrefix>/<file> and the images are copied next to the tree.
// A string is only touched when the 32 bit length field in front of it matches
// its own length, so an unexpected layout leaves the copy byte identical.
// ---------------------------------------------------------------------------
// A shipped water .odata is the grid summary the world editor writes next to a
// water VLO: magic 0x42A14E65, a packed grid blob, then a name/value block
// {alpha <blob>, rigid 0, version 0x60}. These are the exact bytes of a water the
// BW world editor itself produced for this build (90 x 90 m, cellsize 100,
// tessellation 10), taken from a shipped space, so it is known-good data rather
// than a guess. Two editor-made waters of the same size carry different grids, so
// the blob is per cell content (heights, alpha) and not a record of the extents:
// this is the flat, featureless grid, which is all a single plane needs.
static const uint8 g_bwWaterOdata[128] = {
    0x65,
    0x4e,
    0xa1,
    0x42,
    0x35,
    0x00,
    0x00,
    0x00,
    0xff,
    0x63,
    0x00,
    0x00,
    0x00,
    0xff,
    0x00,
    0x00,
    0x35,
    0x01,
    0x00,
    0x35,
    0x00,
    0x63,
    0x00,
    0x00,
    0x02,
    0x00,
    0x00,
    0x00,
    0x0a,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x05,
    0x00,
    0x00,
    0x00,
    0x61,
    0x6c,
    0x70,
    0x68,
    0x61,
    0x00,
    0x00,
    0x00,
    0x07,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x05,
    0x00,
    0x00,
    0x00,
    0x72,
    0x69,
    0x67,
    0x69,
    0x64,
    0x00,
    0x00,
    0x00,
    0x04,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x07,
    0x00,
    0x00,
    0x00,
    0x76,
    0x65,
    0x72,
    0x73,
    0x69,
    0x6f,
    0x6e,
    0x00,
    0x60,
    0x00,
    0x00,
    0x00,
};

namespace
{
typedef std::vector<unsigned char> SptBytes;

bool SptReadAll(const std::string& path, SptBytes& out)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f.is_open())
    {
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0)
    {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(&out[0]), size);
    return f.good();
}

bool SptWriteAll(const std::string& path, const SptBytes& data)
{
    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!f.is_open())
    {
        return false;
    }
    if (!data.empty())
    {
        f.write(reinterpret_cast<const char*>(&data[0]), static_cast<std::streamsize>(data.size()));
    }
    f.close();
    return !f.fail();
}

std::string SptLowered(const std::string& s)
{
    std::string r(s);
    for (size_t i = 0; i < r.size(); ++i)
    {
        const char c = r[i];
        if (c >= 'A' && c <= 'Z')
        {
            r[i] = static_cast<char>(c - 'A' + 'a');
        }
    }
    return r;
}

bool SptIsTextureName(const std::string& s)
{
    const std::string lower = SptLowered(s);
    static const char* extensions[] = { ".dds", ".tga", ".png", ".bmp", ".tif", ".jpg" };
    for (size_t e = 0; e < sizeof(extensions) / sizeof(extensions[0]); ++e)
    {
        const std::string ext(extensions[e]);
        if (lower.size() > ext.size() && lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0)
        {
            return true;
        }
    }
    return false;
}

std::string SptBaseName(const std::string& path)
{
    const size_t cut = path.find_last_of("\\/");
    return (cut == std::string::npos) ? path : path.substr(cut + 1);
}

unsigned SptReadU32(const SptBytes& data, size_t offset)
{
    return static_cast<unsigned>(data[offset]) | (static_cast<unsigned>(data[offset + 1]) << 8) |
    (static_cast<unsigned>(data[offset + 2]) << 16) | (static_cast<unsigned>(data[offset + 3]) << 24);
}

void SptPushU32(SptBytes& out, unsigned value)
{
    out.push_back(static_cast<unsigned char>(value & 0xff));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    out.push_back(static_cast<unsigned char>((value >> 16) & 0xff));
    out.push_back(static_cast<unsigned char>((value >> 24) & 0xff));
}

bool SptCopyFileBytes(const std::string& from, const std::string& to)
{
    SptBytes data;
    if (!SptReadAll(from, data))
    {
        return false;
    }
    return SptWriteAll(to, data);
}

// Where the tree actually lives in a Blitz DataSource is not fixed: shipping
// builds keep it under <map>/speedTree, some keep it under content/<map>/speedTree
// or spaces/<map>/speedTree, and a few keep the .spt next to the map files. Ask
// every plausible place instead of betting on one, and log the list when none of
// them has it so the next run says exactly what it looked for.
bool FindSpeedTreeSource(const DAVA::String& dataSourceRoot, const DAVA::String& mapFolder,
                         const DAVA::String& modelName, std::string& outPath)
{
    static const char* const dirs[] = {
        "%s\\speedTree",
        "content\\%s\\speedTree",
        "spaces\\%s\\speedTree",
        "%s\\SpeedTree",
        "content\\%s\\SpeedTree",
        "%s\\tree",
        "content\\%s\\tree",
        "speedTree",
        "%s",
        "content\\%s",
        "spaces\\%s",
        // Blitz DataSource also keeps shared art outside the map folder
        "3d\\Objects\\%s",
        "3d\\Maps\\00_global_content\\%s",
        "3d\\Maps\\00_global_content\\speedTree\\%s",
    };

    std::string root = dataSourceRoot.c_str();
    while (!root.empty() && (root[root.size() - 1] == '/' || root[root.size() - 1] == '\\'))
    {
        root.erase(root.size() - 1);
    }

    std::vector<std::string> tried;
    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); ++d)
    {
        char dirBuf[512];
        if (strstr(dirs[d], "%s") != nullptr)
        {
            Snprintf(dirBuf, sizeof(dirBuf), dirs[d], mapFolder.c_str());
        }
        else
        {
            Snprintf(dirBuf, sizeof(dirBuf), "%s", dirs[d]);
        }

        const char* exts[2] = { ".spt", ".SPT" };
        for (int e = 0; e < 2; ++e)
        {
            std::string candidate = root + "\\" + dirBuf + "\\" + modelName.c_str() + exts[e];
            tried.push_back(candidate);
            FILE* probe = fopen(candidate.c_str(), "rb");
            if (probe != nullptr)
            {
                fclose(probe);
                outPath = candidate;
                DAVA::Logger::Info("[TerrainConverter] speedTree source '%s' -> '%s'",
                                   modelName.c_str(), candidate.c_str());
                return true;
            }
        }
    }

    // Last resort: walk the map's own folders and match the file name. Boundaries of
    // a DataSource layout differ between builds, and guessing the folder is how the
    // trees ended up exported with no .spt at all.
    DAVA::FileSystem* fs = DAVA::FileSystem::Instance();
    const char* const scanRoots[3] = { "", "content\\", "spaces\\" };
    DAVA::String wanted = modelName + ".spt";
    DAVA::String wantedLower = wanted;
    for (size_t c = 0; c < wantedLower.size(); ++c)
    {
        wantedLower[c] = static_cast<char>(tolower(static_cast<unsigned char>(wantedLower[c])));
    }

    for (int r = 0; r < 3; ++r)
    {
        DAVA::String scanBase = DAVA::String(root.c_str()) + "\\" + scanRoots[r] + mapFolder;
        if (scanBase.empty() || (scanBase[scanBase.size() - 1] != '\\' && scanBase[scanBase.size() - 1] != '/'))
        {
            scanBase += '\\';
        }

        // FileList asserts IsDirectoryPathname(), which only accepts a trailing '/'
        // (not '\\'), so normalise through FilePath instead of trusting the string.
        DAVA::FilePath scanDir(scanBase);
        if (!scanDir.IsDirectoryPathname())
        {
            scanDir = DAVA::FilePath(scanDir.GetAbsolutePathname() + "/");
        }
        if (!fs->IsDirectory(scanDir))
        {
            DAVA::Logger::Info("[TerrainConverter] speedTree scan: '%s' is not a directory",
                               scanBase.c_str());
            continue;
        }

        DAVA::Vector<DAVA::FilePath> files = fs->EnumerateFilesInDirectory(scanDir, true);
        for (size_t i = 0; i < files.size(); ++i)
        {
            DAVA::String name = files[i].GetFilename();
            DAVA::String lowered = name;
            for (size_t c = 0; c < lowered.size(); ++c)
            {
                lowered[c] = static_cast<char>(tolower(static_cast<unsigned char>(lowered[c])));
            }
            if (lowered == wantedLower)
            {
                outPath = files[i].GetAbsolutePathname().c_str();
                DAVA::Logger::Info("[TerrainConverter] speedTree source '%s' found by scan: '%s' (root '%s')",
                                   modelName.c_str(), outPath.c_str(), scanRoots[r]);
                return true;
            }
        }
    }

    DAVA::Logger::Warning("[TerrainConverter] speedTree source '%s' not found - chunk item stays dangling",
                          modelName.c_str());
    for (size_t t = 0; t < tried.size(); ++t)
    {
        DAVA::Logger::Warning("[TerrainConverter]   tried '%s'", tried[t].c_str());
    }
    DAVA::Logger::Warning("[TerrainConverter]   then scanned <DataSource>\\[content|spaces]\\%s recursively",
                          mapFolder.c_str());
    return false;
}

bool ExportSpeedTree(const DAVA::String& sourceDir, const DAVA::String& targetDir, const DAVA::String& resPrefix,
                     const DAVA::String& modelName)
{
    const std::string source = sourceDir.empty() ? std::string() : std::string(sourceDir.c_str()) + "\\" + modelName.c_str() + ".spt";
    const std::string target = std::string(targetDir.c_str()) + "\\" + modelName.c_str() + ".spt";

    SptBytes data;
    if (source.empty() || !SptReadAll(source, data))
    {
        return false; // FindSpeedTreeSource already logged every candidate it tried
    }

    struct SptReference
    {
        size_t offset;
        size_t length;
        std::string text;
    };

    std::vector<SptReference> references;
    size_t i = 0;
    while (i < data.size())
    {
        if (data[i] < 32 || data[i] > 126)
        {
            ++i;
            continue;
        }

        size_t end = i;
        while (end < data.size() && data[end] >= 32 && data[end] <= 126)
        {
            ++end;
        }

        const std::string run(reinterpret_cast<const char*>(&data[i]), end - i);
        if (run.size() >= 5 && SptIsTextureName(run) && i >= 4)
        {
            const unsigned declared = SptReadU32(data, i - 4);
            if (declared == run.size())
            {
                SptReference ref;
                ref.offset = i;
                ref.length = run.size();
                ref.text = run;
                references.push_back(ref);
            }
            else
            {
                DAVA::Logger::Info("[TerrainConverter] speedTree '%s': '%s' has no matching length (%u), left as is",
                                   modelName.c_str(), run.c_str(), declared);
            }
        }
        i = end;
    }

    SptBytes output;
    size_t cursor = 0;
    std::vector<std::string> textures;
    for (size_t r = 0; r < references.size(); ++r)
    {
        const std::string base = SptBaseName(references[r].text);
        if (base.empty())
        {
            continue;
        }

        if (references[r].offset < 4 || references[r].offset + references[r].length > data.size())
        {
            continue;
        }

        const std::string replacement = std::string(resPrefix.c_str()) + "/" + base;

        output.insert(output.end(), data.begin() + cursor, data.begin() + references[r].offset - 4);
        SptPushU32(output, static_cast<unsigned>(replacement.size()));
        output.insert(output.end(), replacement.begin(), replacement.end());
        cursor = references[r].offset + references[r].length;

        bool known = false;
        for (size_t t = 0; t < textures.size(); ++t)
        {
            if (textures[t] == base)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            textures.push_back(base);
        }
    }
    output.insert(output.end(), data.begin() + cursor, data.end());

    if (!SptWriteAll(target, output))
    {
        DAVA::Logger::Warning("[TerrainConverter] can not write '%s'", target.c_str());
        return false;
    }

    for (size_t t = 0; t < textures.size(); ++t)
    {
        const std::string from = std::string(sourceDir.c_str()) + "\\" + textures[t];
        const std::string to = std::string(targetDir.c_str()) + "\\" + textures[t];
        if (!SptCopyFileBytes(from, to))
        {
            DAVA::Logger::Warning("[TerrainConverter] tree texture '%s' not found next to the .spt", from.c_str());
        }
    }

    DAVA::Logger::Info("[TerrainConverter] speedTree '%s' -> '%s' (%d texture reference(s), %d image(s))",
                       modelName.c_str(), target.c_str(), static_cast<int>(references.size()),
                       static_cast<int>(textures.size()));
    return true;
}
} // namespace

void Converter::AddTreeToChunk(Entity* node, ServerSPT* tree)
{
    DAVA::Logger::Info("AddModelToChunk model %s", tree->GetModelName().c_str());

    // Nothing used to copy the tree itself, so every chunk item pointed at a file
    // that never existed. Export once per model: SpeedTree 4.x .spt with its texture
    // references rewritten to res relative content/<map>/speedTree/<file>.
    static std::set<DAVA::String> exportedTrees;
    if (exportedTrees.find(tree->GetModelName()) == exportedTrees.end())
    {
        exportedTrees.insert(tree->GetModelName());

        DAVA::String mapFolder = mapName;
        while (!mapFolder.empty() && mapFolder[mapFolder.size() - 1] == '/')
        {
            mapFolder.erase(mapFolder.size() - 1);
        }

        const DAVA::String targetDir = "out/content/" + mapFolder + "/speedTree";
        DAVA::FileSystem::Instance()->CreateDirectory(targetDir);

        std::string sourcePath;
        if (FindSpeedTreeSource(clientDataSource.GetAbsolutePathname(), mapFolder, tree->GetModelName(), sourcePath))
        {
            const DAVA::String sourceDir(sourcePath.substr(0, sourcePath.find_last_of("\\/")).c_str());
            // client copy: referenced by the chunk item as content/<map>/speedTree/<name>.spt
            ExportSpeedTree(sourceDir, targetDir, BwResPath("content/" + mapFolder + "/speedTree"),
                            tree->GetModelName());
        }
    }

    String fn = BwResPath("content/" + mapName + "/speedTree/" + tree->GetModelName());
    Snprintf(resultBuff, sizeof(resultBuff), speedTreeFormatPattern, fn.c_str());
    AddEntityToChunk(resultBuff, GetEntityWorldTransform(node), node->GetID());
    resultBuff[0] = 0;
}

void Converter::AddPointToChunk(Entity* node)
{
    DAVA::Logger::Info("AddPointToChunk %s", node->GetName().c_str());
    if (!g_exportGameplayPoints)
    {
        // every item this function writes is a WG user data object type that BW does not
        // define, and one undefined type makes BW drop the whole chunk
        return;
    }
    resultBuff[0] = 0;
    String val = "";
    int32 team = 0;
    int32 preferredVehicleType = eSPVT_ANY;
    float32 pointsPerSecond = 1.0f;
    float32 maxPointsPerSecond = 3.0f;
    float32 radius = 25.0f;
    int32 group = 0;

    String vehicle = "none";
    String botname = "";
    String script = "";
    bool isPerformanceTestBot = false;
    int32 pathIDbotspawn = 0;

    int32 pathIDpatrol = 1;
    int32 pointNumber = 1;
    String directive = "";

    KeyedArchive* customProperties = GetCustomPropertiesArchieve(node);
    if (customProperties)
    {
        val = customProperties->GetString("type", "");
        team = customProperties->GetInt32("team", 0);
        preferredVehicleType = customProperties->GetInt32("SpawnPreferredVehicleType", eSPVT_ANY);

        pointsPerSecond = customProperties->GetFloat("pointsPerSecond", 1.0f);
        maxPointsPerSecond = customProperties->GetFloat("maxPointsPerSecond", 3.0f);
        radius = customProperties->GetFloat("radius", 25.0f);

        botname = customProperties->GetString("botname", "");
        vehicle = customProperties->GetString("vehicle", "none");
        isPerformanceTestBot = customProperties->GetBool("performanceTestBot");
        pathIDbotspawn = customProperties->GetInt32("pathID", 0);
        script = customProperties->GetString("script", "");
        group = customProperties->GetInt32("group", 0);

        pathIDpatrol = customProperties->GetInt32("pathID", 1);
        pointNumber = customProperties->GetInt32("pointNumber", 1);

        directive = customProperties->GetString("directive", "");
    }

    UniqueID guid = GetEntityID(node);
    if (val.find("spawnpoint") != std::string::npos)
    {
        String preferredVehicleTypeStr;

        if (0 <= preferredVehicleType && preferredVehicleType < eSPVT_COUNT)
        {
            preferredVehicleTypeStr = SPAWN_PREFERRED_VEHICLE_TYPES[preferredVehicleType];
        }
        else
        {
            DAVA::Logger::Warning("Converter::AddPointToChunk unknown preferred vehicle type %d for spawnpoint",
                                  preferredVehicleType);
            preferredVehicleTypeStr = SPAWN_PREFERRED_VEHICLE_TYPES[eSPVT_ANY];
        }
        Snprintf(resultBuff, sizeof(resultBuff), spawnPointFormatPattern,
                 guid.toString().c_str(), team, preferredVehicleTypeStr.c_str(), group, pointNumber);
    }
    else if (val.find("controlpoint") != std::string::npos)
    {
        Snprintf(resultBuff, sizeof(resultBuff), controlPointFormatPattern,
                 radius,
                 pointsPerSecond,
                 maxPointsPerSecond);
    }
    else if (val.find("botspawn") != std::string::npos)
    {
        Snprintf(resultBuff, sizeof(resultBuff), botSpawnFormatPattern,
                 guid.toString().c_str(),
                 team,
                 vehicle.c_str(),
                 (botname.empty() ? botname.c_str() : Format("\n\t\t\t<botName>\t%s\t</botName>", botname.c_str()).c_str()),
                 pathIDbotspawn,
                 (isPerformanceTestBot ? "1" : "0"),
                 script.c_str(),
                 group);
    }
    else if (val.find("patrol") != std::string::npos)
    {
        Snprintf(resultBuff, sizeof(resultBuff), patrolFormatPattern,
                 guid.toString().c_str(),
                 pathIDpatrol,
                 pointNumber,
                 directive.c_str());
    }
    else if (val.find("strategicpoint") != std::string::npos)
    {
        StrategyPointData strategyPointData = StrategyPointData::Create(customProperties);

        String currentStrategyPoint = strategyPointPattern;
        strategyPointData.FillPattern(currentStrategyPoint);

        Snprintf(resultBuff, sizeof(resultBuff), "%s", currentStrategyPoint.c_str());

        arenaDefCompilator->Visit(strategyPointData);
    }

    if (resultBuff[0] != 0)
    {
        AddEntityToChunk(resultBuff, GetEntityWorldTransform(node), 0);
    }
    resultBuff[0] = 0;
} // Converter::AddPointToChunk

void Converter::CreateSettingsFile(int32 minx, int32 miny, int32 maxx, int32 maxy)
{
    char content[8192];
    char result[8192];

    File* f = File::Create(FilePath("~res:/modelTemplate\\space.settings"), File::OPEN | File::READ);
    if (f == nullptr)
    {
        DAVA::Logger::Error("space.settings template not found at ~res:/modelTemplate\\space.settings - copy it from the compliance package");
        return;
    }
    uint32 res = f->Read(content, sizeof(content));
    content[res] = 0;
    SafeRelease(f);

    // %MAPNAME% token support (used by the flora/globalMap entries of the template)
    String mapToken = inputFile.GetBasename();
    {
        char* pos = strstr(content, "%MAPNAME%");
        while (pos != nullptr)
        {
            if (strlen(pos + 9) + mapToken.length() + 1 > sizeof(content))
            {
                break;
            }
            char tail[4096];
            Snprintf(tail, sizeof(tail), "%s", pos + 9);
            Snprintf(pos, sizeof(content) - (pos - content), "%s%s", mapToken.c_str(), tail);
            pos = strstr(pos + mapToken.length(), "%MAPNAME%");
        }
    }

    Snprintf(result, sizeof(result), content, minx, maxx, miny, maxy);
    f = File::Create(g_spacesPath + "space.settings", File::CREATE | File::WRITE);
    if (f == nullptr)
    {
        DAVA::Logger::Error("Cannot create %s - check that the spaces folder exists", (g_spacesPath + "space.settings").c_str());
        return;
    }
    f->Write(result, (uint32)strlen(result));
    SafeRelease(f);
}

void Converter::CreateBorderFile(void)
{
    KeyedArchive* customProperties = GetCustomPropertiesArchieve(land);
    float32 topX(-250.0f), topY(-250.0f), botX(250.0f), botY(250.0f);

    if (customProperties)
    {
        topX = customProperties->GetFloat("borderTopX", topX);
        topY = customProperties->GetFloat("borderTopY", topY);
        botX = customProperties->GetFloat("borderBotX", botX);
        botY = customProperties->GetFloat("borderBotY", botY);
    }

    char content[8192];
    char result[8192];

    if (LoadTemplateFile(FilePath("~res:/modelTemplate\\boundingBox.xml"), content, static_cast<DAVA::uint32>(sizeof(content))) == false)
    {
        DAVA::Logger::Error("[TerrainConverter] boundingBox.xml skipped: template missing");
        return;
    }

    //	Snprintf( result, sizeof(result), content, minx, maxx, miny, maxy);
    Snprintf(result, sizeof(result), content, topX, topY, botX, botY);
    File* outFile = File::Create("out\\boundingBox.xml", File::CREATE | File::WRITE);
    if (outFile != nullptr)
    {
        outFile->Write(result, (uint32)strlen(result));
        SafeRelease(outFile);
    }
    else
    {
        DAVA::Logger::Error("[TerrainConverter] can not write out\\boundingBox.xml");
    }

    AABBox2 bbox(Vector2(topX, topY), Vector2(botX, botY));
    BoundingBoxData bboxData(bbox);
    arenaDefCompilator->Visit(bboxData);
}

// ---------------------------------------------------------------------------
// BW terrain tiles
//
// Dava packs all four landscape layers into ONE texture, tileTexture0, where
// each channel is a layer, and tints them with the material properties
// tileColor0..3. The blend weights live in the separate tileMask. BW instead
// needs one texture per layer, so the channels are baked out here (times their
// tint) as single mip uncompressed BGRA DDS, and textureNames[] is repointed at
// those files instead of the OSE atlas paths that Blitz does not ship.
// ---------------------------------------------------------------------------
static DAVA::uint8 ClampTileByte(DAVA::float32 value)
{
    DAVA::int32 result = static_cast<DAVA::int32>(value + 0.5f);
    if (result < 0)
        return 0;
    if (result > 255)
        return 255;
    return static_cast<DAVA::uint8>(result);
}

// ---------------------------------------------------------------------------
// PNG writer used for the BW height blocks.
//
// libpng is fine in itself, but every height PNG this converter produced so far
// decoded back with a single populated row, which is exactly what a flat world
// looks like. This writer takes an explicit row stride of width*4 and builds the
// zlib stream itself with stored (uncompressed) deflate blocks, so the only
// inputs are the grid and its dimensions. Verified against Pillow: a 69x69 grid
// round-trips with all rows populated.
// ---------------------------------------------------------------------------
static void BwPutBe32(std::vector<DAVA::uint8>& v, DAVA::uint32 x)
{
    v.push_back((DAVA::uint8)(x >> 24));
    v.push_back((DAVA::uint8)(x >> 16));
    v.push_back((DAVA::uint8)(x >> 8));
    v.push_back((DAVA::uint8)x);
}

static DAVA::uint32 BwCrc32(const DAVA::uint8* d, size_t n)
{
    static DAVA::uint32 table[256];
    static bool ready = false;
    if (!ready)
    {
        for (DAVA::uint32 i = 0; i < 256; ++i)
        {
            DAVA::uint32 c = i;
            for (DAVA::int32 k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    DAVA::uint32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
        c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void BwPngChunk(std::vector<DAVA::uint8>& out, const char* type, const std::vector<DAVA::uint8>& data)
{
    BwPutBe32(out, (DAVA::uint32)data.size());
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    std::vector<DAVA::uint8> crc(type, type + 4);
    crc.insert(crc.end(), data.begin(), data.end());
    BwPutBe32(out, BwCrc32(crc.data(), crc.size()));
}

static void WriteBwRgbaPng(DAVA::int32 w, DAVA::int32 h, const DAVA::uint8* rgba, std::vector<DAVA::uint8>& out)
{
    out.clear();
    if (rgba == nullptr || w <= 0 || h <= 0)
        return;

    const DAVA::uint8 sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    out.insert(out.end(), sig, sig + 8);

    std::vector<DAVA::uint8> ihdr;
    BwPutBe32(ihdr, (DAVA::uint32)w);
    BwPutBe32(ihdr, (DAVA::uint32)h);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // RGBA
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    BwPngChunk(out, "IHDR", ihdr);

    std::vector<DAVA::uint8> raw;
    raw.reserve((size_t)(w * 4 + 1) * (size_t)h);
    for (DAVA::int32 y = 0; y < h; ++y)
    {
        raw.push_back(0); // filter: none
        raw.insert(raw.end(), rgba + (size_t)y * w * 4, rgba + (size_t)(y + 1) * w * 4);
    }

    std::vector<DAVA::uint8> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t done = 0;
    const size_t rawSize = raw.size();
    while (done < rawSize)
    {
        size_t n = rawSize - done;
        if (n > 65535)
            n = 65535;
        const bool last = (done + n == rawSize);
        z.push_back((DAVA::uint8)(last ? 1 : 0));
        z.push_back((DAVA::uint8)(n & 0xFF));
        z.push_back((DAVA::uint8)(n >> 8));
        z.push_back((DAVA::uint8)(~n & 0xFF));
        z.push_back((DAVA::uint8)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + done, raw.begin() + done + n);
        done += n;
    }
    {
        DAVA::uint32 a = 1, b = 0;
        for (size_t i = 0; i < raw.size(); ++i)
        {
            a = (a + raw[i]) % 65521u;
            b = (b + a) % 65521u;
        }
        BwPutBe32(z, (b << 16) | a); // zlib trailer is Adler-32, not CRC
    }
    BwPngChunk(out, "IDAT", z);
    std::vector<DAVA::uint8> none;
    BwPngChunk(out, "IEND", none);
}

static bool WriteBwRgbaDds(const DAVA::FilePath& target, const DAVA::uint8* rgba, DAVA::uint32 width, DAVA::uint32 height)
{
    if (rgba == nullptr || width == 0 || height == 0)
        return false;

    // BW/Blitz terrain tiles are minified hard (a 4096 tile sheet seen at 200 m is a
    // handful of texels), so a single mip makes the ground crawl and sparkle. Build the
    // full box-filtered chain and declare it in the header.
    DAVA::uint32 mipCount = 1;
    {
        DAVA::uint32 w = width;
        DAVA::uint32 h = height;
        while (w > 1 || h > 1)
        {
            w = (w > 1) ? (w / 2) : 1;
            h = (h > 1) ? (h / 2) : 1;
            ++mipCount;
        }
    }

    std::vector<DAVA::uint8> levels;
    levels.resize((size_t)width * height * 4);
    for (DAVA::uint32 i = 0; i < width * height; ++i)
    {
        levels[i * 4 + 0] = rgba[i * 4 + 2]; // BGRA
        levels[i * 4 + 1] = rgba[i * 4 + 1];
        levels[i * 4 + 2] = rgba[i * 4 + 0];
        levels[i * 4 + 3] = rgba[i * 4 + 3];
    }

    std::vector<DAVA::uint32> levelOffset;
    levelOffset.push_back(0);
    {
        DAVA::uint32 w = width;
        DAVA::uint32 h = height;
        while (w > 1 || h > 1)
        {
            const DAVA::uint32 nw = (w > 1) ? (w / 2) : 1;
            const DAVA::uint32 nh = (h > 1) ? (h / 2) : 1;
            const size_t prev = levelOffset.back();
            const size_t prevBytes = (size_t)w * h * 4;
            const size_t offset = prev + prevBytes;
            levels.resize(offset + (size_t)nw * nh * 4, 0);
            for (DAVA::uint32 y = 0; y < nh; ++y)
            {
                for (DAVA::uint32 x = 0; x < nw; ++x)
                {
                    const DAVA::uint32 sx0 = (x * 2 < w - 1) ? (x * 2) : (w - 1);
                    const DAVA::uint32 sx1 = (x * 2 + 1 < w - 1) ? (x * 2 + 1) : (w - 1);
                    const DAVA::uint32 sy0 = (y * 2 < h - 1) ? (y * 2) : (h - 1);
                    const DAVA::uint32 sy1 = (y * 2 + 1 < h - 1) ? (y * 2 + 1) : (h - 1);
                    for (DAVA::uint32 c = 0; c < 4; ++c)
                    {
                        const DAVA::uint32 a = levels[offset - prevBytes + ((size_t)sy0 * w + sx0) * 4 + c];
                        const DAVA::uint32 b = levels[offset - prevBytes + ((size_t)sy0 * w + sx1) * 4 + c];
                        const DAVA::uint32 cc = levels[offset - prevBytes + ((size_t)sy1 * w + sx0) * 4 + c];
                        const DAVA::uint32 d = levels[offset - prevBytes + ((size_t)sy1 * w + sx1) * 4 + c];
                        levels[offset + ((size_t)y * nw + x) * 4 + c] = (DAVA::uint8)((a + b + cc + d + 2) / 4);
                    }
                }
            }
            w = nw;
            h = nh;
            levelOffset.push_back(offset);
        }
    }

    const DAVA::uint32 headerSize = 4 + 124;
    const DAVA::uint32 dataSize = headerSize + (DAVA::uint32)levels.size();
    DAVA::uint8* dds = new DAVA::uint8[dataSize];
    memset(dds, 0, dataSize);

    const DAVA::uint32 magic = 0x20534444; // "DDS "
    memcpy(dds, &magic, 4);

    DAVA::uint32* h = (DAVA::uint32*)(dds + 4);
    h[0] = 124; // dwSize
    h[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000; // caps|height|width|pitch|pixelformat
    h[1] |= 0x400000 | 0x8; // DDSCAPS_MIPMAP | DDSCAPS_COMPLEX
    h[2] = height;
    h[3] = width;
    h[4] = width * 4; // pitch
    h[6] = mipCount; // dwMipMapCount
    h[18] = 32; // pf size
    h[19] = 0x1 | 0x40; // pf flags: alphapixels | rgb
    h[20] = 0; // pf fourCC: 0, this is an RGB layout
    h[21] = 32; // bit count
    h[22] = 0x00ff0000; // R mask
    h[23] = 0x0000ff00; // G mask
    h[24] = 0x000000ff; // B mask
    h[25] = 0xff000000; // A mask
    h[26] = 0x1000; // caps: texture

    memcpy(dds + headerSize, &levels[0], levels.size());

    DAVA::File* f = DAVA::File::Create(target, DAVA::File::CREATE | DAVA::File::WRITE);
    if (f == nullptr)
    {
        DAVA::Logger::Warning("[TerrainConverter] can not write tile texture '%s'", target.GetAbsolutePathname().c_str());
        delete[] dds;
        return false;
    }

    f->Write(dds, dataSize);
    DAVA::SafeRelease(f);
    delete[] dds;

    DAVA::Logger::Info("[TerrainConverter] tile '%s': %ux%u, %u mips",
                       target.GetFilename().c_str(), width, height, mipCount);
    return true;
}

// Material properties live on the instance or on a template parent.
static bool SameNameIgnoringCase(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr)
    {
        return false;
    }
    while (*a != 0 && *b != 0)
    {
        char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a - 'A' + 'a') : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b - 'A' + 'a') : *b;
        if (ca != cb)
        {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

// Dava declares tileColor0..3 as [material][instance] float3 properties. They can be
// spelled differently by tools, and they can sit on any template parent, so match the
// name exactly first and case-insensitively second instead of silently falling back
// to white (which is what made the first bake colourless).
static DAVA::Vector4 ReadTileColor(DAVA::NMaterial* material, const DAVA::FastName& name, const char* nameLiteral)
{
    DAVA::NMaterial* current = material;
    for (DAVA::int32 depth = 0; current != nullptr && depth < 8; ++depth)
    {
        const DAVA::UnorderedMap<DAVA::FastName, DAVA::NMaterialProperty*>& properties = current->GetLocalProperties();
        DAVA::UnorderedMap<DAVA::FastName, DAVA::NMaterialProperty*>::const_iterator it = properties.find(name);
        if (it == properties.end() || it->second == nullptr || it->second->data == nullptr)
        {
            for (it = properties.begin(); it != properties.end(); ++it)
            {
                if (it->second != nullptr && it->second->data != nullptr &&
                    SameNameIgnoringCase(it->first.c_str(), nameLiteral))
                {
                    break;
                }
            }
        }

        if (it != properties.end() && it->second != nullptr && it->second->data != nullptr)
        {
            const DAVA::float32* value = it->second->data.get();
            DAVA::Logger::Info("[TerrainConverter] %s taken from material '%s' depth %d as '%s'",
                               nameLiteral, current->GetMaterialName().c_str(), depth, it->first.c_str());
            return DAVA::Vector4(value[0], value[1], value[2], value[3]);
        }
        current = current->GetParent();
    }

    DAVA::Logger::Warning("[TerrainConverter] %s not found on any material level - using white", nameLiteral);
    return DAVA::Vector4(1.0f, 1.0f, 1.0f, 1.0f);
}

static void BakeLandscapeTiles(DAVA::Entity* landscapeEntity, const DAVA::String& mapName, DAVA::Vector<DAVA::String>& textureNames, const DAVA::Vector<DAVA::Color>& palette, DAVA::Image* maskImage)
{
    if (landscapeEntity == nullptr)
        return;

    DAVA::RenderComponent* renderComponent = landscapeEntity->GetComponent<DAVA::RenderComponent>();
    if (renderComponent == nullptr)
        return;

    DAVA::Landscape* landscape = static_cast<DAVA::Landscape*>(renderComponent->GetRenderObject());
    if (landscape == nullptr)
        return;

    DAVA::NMaterial* material = landscape->GetMaterial();
    if (material == nullptr)
    {
        DAVA::Logger::Warning("[TerrainConverter] landscape has no material - tile textures not generated");
        return;
    }

    DAVA::Texture* tileTexture = material->GetEffectiveTexture(DAVA::Landscape::TEXTURE_TILE);
    if (tileTexture == nullptr || tileTexture->texDescriptor == nullptr)
    {
        DAVA::Logger::Warning("[TerrainConverter] landscape material has no %s - tile textures not generated",
                              DAVA::Landscape::TEXTURE_TILE.c_str());
        return;
    }

    DAVA::FileSystem* fs = DAVA::FileSystem::Instance();
    DAVA::FilePath source = tileTexture->texDescriptor->GetSourceTexturePathname();
    if (source.IsEmpty() || !fs->IsFile(source))
    {
        static const DAVA::eGPUFamily gpuOrder[] = {
            DAVA::GPU_ORIGIN, DAVA::GPU_DX11, DAVA::GPU_POWERVR_IOS, DAVA::GPU_POWERVR_ANDROID,
            DAVA::GPU_TEGRA, DAVA::GPU_MALI, DAVA::GPU_ADRENO
        };

        source = DAVA::FilePath();
        for (DAVA::uint32 i = 0; i < sizeof(gpuOrder) / sizeof(gpuOrder[0]); ++i)
        {
            DAVA::FilePath candidate = tileTexture->texDescriptor->CreateMultiMipPathnameForGPU(gpuOrder[i]);
            if (!candidate.IsEmpty() && fs->IsFile(candidate))
            {
                source = candidate;
                break;
            }
        }
    }

    if (source.IsEmpty())
    {
        DAVA::Logger::Warning("[TerrainConverter] no image for '%s' - tile textures not generated",
                              tileTexture->texDescriptor->pathname.GetAbsolutePathname().c_str());
        return;
    }

    DAVA::Vector<DAVA::Image*> images;
    DAVA::ImageSystem::Load(source, images);
    if (images.empty())
    {
        DAVA::Logger::Warning("[TerrainConverter] can not load tile image '%s'", source.GetAbsolutePathname().c_str());
        return;
    }

    DAVA::Image* image = images[0];
    DAVA::Image* converted = nullptr;
    if (image->GetPixelFormat() != DAVA::FORMAT_RGBA8888)
    {
        if (!DAVA::ImageConvert::CanConvertFromTo(image->GetPixelFormat(), DAVA::FORMAT_RGBA8888))
        {
            DAVA::Logger::Warning("[TerrainConverter] tile image format %d of '%s' can not be converted",
                                  static_cast<DAVA::int32>(image->GetPixelFormat()), source.GetAbsolutePathname().c_str());
            for (DAVA::uint32 i = 0; i < images.size(); ++i)
                DAVA::SafeRelease(images[i]);
            return;
        }

        converted = DAVA::Image::Create(image->GetWidth(), image->GetHeight(), DAVA::FORMAT_RGBA8888);
        DAVA::ImageConvert::ConvertImage(image, converted);
        image = converted;
    }

    image->FlipVertical(); // same orientation convention as the custom color texture

    // colorTexture is the hand-painted ground colour (landscape/colormap_*.dds) and is
    // the last factor of Dava's landscape shader:
    //   color = (sum tile_i * mask_i * tileColor_i) * colorTexture * 2
    // Blitz ships it for every map, so bake it in; without it the ground is grey.
    DAVA::Image* colorMap = nullptr;
    {
        // Ground colour source, in order: the baked full tiled texture when the
        // material carries one, else the hand-painted colorTexture.
        DAVA::Texture* colorTex = material->GetEffectiveTexture(DAVA::FastName("fullTiledTexture"));
        if (colorTex != nullptr)
        {
            DAVA::Logger::Info("[TerrainConverter] ground colour source: fullTiledTexture");
        }
        else
        {
            colorTex = material->GetEffectiveTexture(DAVA::FastName("colorTexture"));
            DAVA::Logger::Info("[TerrainConverter] ground colour source: %s",
                               colorTex != nullptr ? "colorTexture (no fullTiledTexture on the material)" : "none on the material, folder search");
        }
        if (colorTex != nullptr && colorTex->texDescriptor != nullptr)
        {
            DAVA::FilePath colorSource = colorTex->texDescriptor->GetSourceTexturePathname();
            // Blitz ships landscape.thumbnail.png (the fullTiledTexture source) stored
            // upside-down against its dx11 DDS (checked: the flipped PNG equals the DDS
            // at 0.996 correlation), and the FlipVertical() below assumes the DDS
            // orientation that colorTexture already loads in. Take the DDS first.
            const bool preferGpuFile = (material->GetEffectiveTexture(DAVA::FastName("fullTiledTexture")) == colorTex);
            if (colorSource.IsEmpty() || !fs->IsFile(colorSource) || preferGpuFile)
            {
                static const DAVA::eGPUFamily colorGpuOrder[] = {
                    DAVA::GPU_ORIGIN, DAVA::GPU_DX11, DAVA::GPU_POWERVR_IOS, DAVA::GPU_POWERVR_ANDROID,
                    DAVA::GPU_TEGRA, DAVA::GPU_MALI, DAVA::GPU_ADRENO
                };
                const DAVA::FilePath sourceFallback = colorSource;
                colorSource = DAVA::FilePath();
                for (DAVA::uint32 i = 0; i < sizeof(colorGpuOrder) / sizeof(colorGpuOrder[0]); ++i)
                {
                    DAVA::FilePath candidate = colorTex->texDescriptor->CreateMultiMipPathnameForGPU(colorGpuOrder[i]);
                    if (!candidate.IsEmpty() && fs->IsFile(candidate))
                    {
                        colorSource = candidate;
                        break;
                    }
                }
                if (colorSource.IsEmpty() && !sourceFallback.IsEmpty() && fs->IsFile(sourceFallback))
                {
                    colorSource = sourceFallback;
                }
            }

            if (!colorSource.IsEmpty())
            {
                DAVA::Logger::Info("[TerrainConverter] ground colour file: '%s'", colorSource.GetAbsolutePathname().c_str());
                DAVA::Vector<DAVA::Image*> colorImages;
                DAVA::ImageSystem::Load(colorSource, colorImages);
                if (!colorImages.empty())
                {
                    colorMap = colorImages[0];
                    if (colorMap->GetPixelFormat() != DAVA::FORMAT_RGBA8888)
                    {
                        if (DAVA::ImageConvert::CanConvertFromTo(colorMap->GetPixelFormat(), DAVA::FORMAT_RGBA8888))
                        {
                            DAVA::Image* convertedColor = DAVA::Image::Create(colorMap->GetWidth(), colorMap->GetHeight(), DAVA::FORMAT_RGBA8888);
                            DAVA::ImageConvert::ConvertImage(colorMap, convertedColor);
                            colorMap = convertedColor;
                        }
                        else
                        {
                            colorMap = nullptr;
                        }
                    }
                    if (colorMap != nullptr)
                    {
                        colorMap->FlipVertical();
                    }
                }
            }
        }
        if (colorMap == nullptr)
        {
            // The Blitz landscape material carries no colorTexture property, so the
            // hand-painted ground colour has to be found in the map folder instead of
            // being skipped - skipping is why the tiles came out colourless.
            DAVA::FilePath colorDir(mapName + "/landscape/");
            if (!colorDir.IsDirectoryPathname())
            {
                // FileList asserts IsDirectoryPathname(), which wants a trailing '/'
                colorDir = DAVA::FilePath(colorDir.GetAbsolutePathname() + "/");
            }
            if (fs->IsDirectory(colorDir))
            {
                const DAVA::Vector<DAVA::FilePath> entries = fs->EnumerateFilesInDirectory(colorDir, false);
                for (size_t i = 0; i < entries.size() && colorMap == nullptr; ++i)
                {
                    DAVA::String base = entries[i].GetBasename();
                    for (size_t c = 0; c < base.size(); ++c)
                    {
                        base[c] = static_cast<char>(::tolower(base[c]));
                    }
                    if (base.find("colormap") == DAVA::String::npos && base.find("fulltiled") == DAVA::String::npos)
                    {
                        continue;
                    }

                    DAVA::Vector<DAVA::Image*> found;
                    DAVA::ImageSystem::Load(entries[i], found);
                    if (found.empty())
                    {
                        continue;
                    }

                    DAVA::Image* colorImg = found[0];
                    if (colorImg->GetPixelFormat() != DAVA::FORMAT_RGBA8888)
                    {
                        if (!DAVA::ImageConvert::CanConvertFromTo(colorImg->GetPixelFormat(), DAVA::FORMAT_RGBA8888))
                        {
                            DAVA::Logger::Warning("[TerrainConverter] colour map '%s' is %d and cannot be converted to RGBA8888",
                                                  entries[i].GetAbsolutePathname().c_str(), (int)colorImg->GetPixelFormat());
                            continue;
                        }
                        DAVA::Image* convertedMap = DAVA::Image::Create(colorImg->GetWidth(), colorImg->GetHeight(), DAVA::FORMAT_RGBA8888);
                        DAVA::ImageConvert::ConvertImage(colorImg, convertedMap);
                        colorImg = convertedMap;
                    }
                    colorImg->FlipVertical();
                    colorMap = colorImg;
                    DAVA::Logger::Info("[TerrainConverter] ground colour from the map folder: '%s' (%ux%u)",
                                       entries[i].GetAbsolutePathname().c_str(),
                                       (unsigned)colorImg->GetWidth(), (unsigned)colorImg->GetHeight());
                }
            }
        }

        if (colorMap != nullptr)
        {
            g_colormapImage = colorMap;
            g_colormapWidth = (DAVA::int32)colorMap->GetWidth();
            g_colormapHeight = (DAVA::int32)colorMap->GetHeight();
        }
        DAVA::Logger::Info("[TerrainConverter] colorTexture (ground colour): %s",
                           colorMap != nullptr ? "loaded, tiles are multiplied by it" : "NOT available - tiles stay unshaded by the colour map");
    }

    const DAVA::uint32 width = image->GetWidth();
    const DAVA::uint32 height = image->GetHeight();
    const DAVA::uint8* data = image->GetData();
    const DAVA::uint32 pixelCount = width * height;

    // The tileMask is only needed for the passability map (the mkm) elsewhere in
    // this file. It is deliberately NOT used as a colour source here: its channels
    // are layer weights, and the EditorConfig palette its dominant values index is
    // the passability palette, not ground colour.
    const DAVA::uint8* maskData = nullptr;
    DAVA::uint32 maskWidth = 0;
    DAVA::uint32 maskHeight = 0;
    if (maskImage != nullptr && maskImage->GetPixelFormat() == DAVA::FORMAT_RGBA8888)
    {
        maskData = maskImage->GetData();
        maskWidth = maskImage->GetWidth();
        maskHeight = maskImage->GetHeight();
    }
    DAVA::Logger::Info("[TerrainConverter] tileMask for the tile bake: %s (%u x %u)",
                       maskData != nullptr ? "loaded" : "missing", maskWidth, maskHeight);

    {
        DAVA::Logger::Info("[TerrainConverter] landscape palette: %d colour(s)", static_cast<DAVA::int32>(palette.size()));
        for (DAVA::uint32 i = 0; i < palette.size() && i < 8; ++i)
        {
            DAVA::Logger::Info("[TerrainConverter]   palette[%u] = %.3f %.3f %.3f %.3f",
                               i, palette[i].r, palette[i].g, palette[i].b, palette[i].a);
        }

        DAVA::NMaterial* current = material;
        for (DAVA::int32 depth = 0; current != nullptr && depth < 3; ++depth)
        {
            const DAVA::UnorderedMap<DAVA::FastName, DAVA::NMaterialProperty*>& properties = current->GetLocalProperties();
            DAVA::Logger::Info("[TerrainConverter] material '%s' depth %d: %d propert(ies)",
                               current->GetMaterialName().c_str(), depth, static_cast<DAVA::int32>(properties.size()));
            DAVA::int32 printed = 0;
            for (DAVA::UnorderedMap<DAVA::FastName, DAVA::NMaterialProperty*>::const_iterator it = properties.begin();
                 it != properties.end() && printed < 40; ++it, ++printed)
            {
                DAVA::float32 first = 0.0f;
                if (it->second != nullptr && it->second->data != nullptr)
                {
                    first = it->second->data.get()[0];
                }
                DAVA::Logger::Info("[TerrainConverter]   '%s' = %.4f", it->first.c_str(), first);
            }
            current = current->GetParent();
        }
    }

    // Terrain layers are resolved from the res root like every other landscape
    // texture (maps/landscape/...), they do not live beside the map models.
    const DAVA::String outDirString = "out\\maps\\landscape\\" + mapName + "\\";
    fs->CreateDirectory(DAVA::FilePath(outDirString), true);

    static const DAVA::FastName tintNames[4] = {
        DAVA::FastName("tileColor0"), DAVA::FastName("tileColor1"),
        DAVA::FastName("tileColor2"), DAVA::FastName("tileColor3")
    };
    static const char* tintLiterals[4] = { "tileColor0", "tileColor1", "tileColor2", "tileColor3" };

    // The four baked layers are kept so a single map-wide ground texture can be
    // composed from them: BW repeats a layer every textureTiling metres, so a layer
    // texture that carries map-wide colour paints the same blotch everywhere.
    DAVA::Vector<std::vector<DAVA::uint8>> layerPixels(4);

    for (DAVA::int32 layer = 0; layer < 4; ++layer)
    {
        DAVA::Vector4 tint = ReadTileColor(material, tintNames[layer], tintLiterals[layer]);
        if (tint.x <= 0.0f || tint.y <= 0.0f || tint.z <= 0.0f)
        {
            // A zero component would black out the whole layer, and this value comes
            // from a raw float property, so treat it as "no tint".
            DAVA::Logger::Warning("[TerrainConverter] %s is zero (%.3f %.3f %.3f) - using white",
                                  tintLiterals[layer], tint.x, tint.y, tint.z);
            tint = DAVA::Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        }

        // Dava composes the ground like this (landscape-editor-fp.sl):
        //   color = (t.r * mask.r * tileColor0.rgb + t.g * mask.g * tileColor1.rgb
        //          + t.b * mask.b * tileColor2.rgb + t.a * mask.a * tileColor3.rgb)
        //          * colorTexture.rgb * 2.0
        // tileTexture0 holds greyscale tile detail, and the visible colour of a
        // layer is its tileColor. The EditorConfig landscape palette is NOT a
        // colour map: its entries are passability kinds (red impassable, green
        // passable, yellow slow), which is why multiplying by it painted the tiles
        // neon red/green/yellow. colorTexture, the hand-painted map that would be
        // the real colour source, is not shipped with Blitz maps and cannot be
        // rebuilt from passability colours, so a layer is baked as
        //   channel * 2 * tileColor
        // and nothing else. BW multiplies the four layers by their mask weights,
        // which reproduces the tinted ground Dava shows.
        DAVA::uint8* pixels = new DAVA::uint8[pixelCount * 4];
        for (DAVA::uint32 p = 0; p < pixelCount; ++p)
        {
            const DAVA::uint8 channel = data[p * 4 + layer];

            if (g_tilesWhite)
            {
                pixels[p * 4 + 0] = 255;
                pixels[p * 4 + 1] = 255;
                pixels[p * 4 + 2] = 255;
                pixels[p * 4 + 3] = 255;
                continue;
            }

            DAVA::float32 colorR = 1.0f, colorG = 1.0f, colorB = 1.0f;
            if (colorMap != nullptr)
            {
                const DAVA::uint32 colorW = colorMap->GetWidth();
                const DAVA::uint32 colorH = colorMap->GetHeight();
                const DAVA::uint8* colorData = colorMap->GetData();
                const DAVA::uint32 colorX = (p % width) * colorW / width;
                const DAVA::uint32 colorY = (p / width) * colorH / height;
                const DAVA::uint8* colorPixel = colorData + ((size_t)colorY * colorW + colorX) * 4;
                colorR = colorPixel[0] / 255.0f;
                colorG = colorPixel[1] / 255.0f;
                colorB = colorPixel[2] / 255.0f;
            }

            // Dava's shader is (sum tile_i * mask_i * tileColor_i) * colorTexture.
            // No x2: the gain clipped everything above 127 to white, which is what made
            // the ground one flat pattern. The colour map is applied here and the layer
            // pitch is the whole map (g_groundSpansMap), so it is not repeated.
            const DAVA::float32 colourR = g_multiplyColormapIntoTiles ? colorR : 1.0f;
            const DAVA::float32 colourG = g_multiplyColormapIntoTiles ? colorG : 1.0f;
            const DAVA::float32 colourB = g_multiplyColormapIntoTiles ? colorB : 1.0f;
            pixels[p * 4 + 0] = ClampTileByte(static_cast<DAVA::float32>(channel) * tint.x * colourR);
            pixels[p * 4 + 1] = ClampTileByte(static_cast<DAVA::float32>(channel) * tint.y * colourG);
            pixels[p * 4 + 2] = ClampTileByte(static_cast<DAVA::float32>(channel) * tint.z * colourB);
            pixels[p * 4 + 3] = 255;
        }

        {
            double sumR = 0.0, sumG = 0.0, sumB = 0.0;
            const DAVA::uint32 step = pixelCount > 4096 ? pixelCount / 4096 : 1;
            DAVA::uint32 sampled = 0;
            for (DAVA::uint32 p = 0; p < pixelCount; p += step)
            {
                sumR += pixels[p * 4 + 0];
                sumG += pixels[p * 4 + 1];
                sumB += pixels[p * 4 + 2];
                ++sampled;
            }
            DAVA::Logger::Info("[TerrainConverter] layer %d colour mean R %.0f G %.0f B %.0f (colour map %s)",
                               layer,
                               sampled ? sumR / sampled : 0.0,
                               sampled ? sumG / sampled : 0.0,
                               sampled ? sumB / sampled : 0.0,
                               colorMap != nullptr ? "applied" : "MISSING - ground stays grey");
        }

        layerPixels[layer].assign(pixels, pixels + pixelCount * 4);

        const DAVA::String fileName = DAVA::Format("tile%d.dds", layer);
        const bool written = WriteBwRgbaDds(DAVA::FilePath(outDirString + fileName), pixels, width, height);
        delete[] pixels;

        if (written)
        {
            if (textureNames.size() > static_cast<size_t>(layer))
                textureNames[layer] = "maps/landscape/" + mapName + "/" + fileName;
            else
                textureNames.push_back("maps/landscape/" + mapName + "/" + fileName);

            DAVA::Logger::Info("[TerrainConverter] landscape tile %d <- '%s' channel %d (W %u H %u) tileColor %.3f %.3f %.3f x2 -> %s",
                               layer, source.GetAbsolutePathname().c_str(), layer, width, height,
                               tint.x, tint.y, tint.z, textureNames[layer].c_str());
        }
    }

    // The map's hand-painted colorTexture is what gives Blitz ground its macro colour.
    // BW's blend is additive, so the cheapest faithful use of it is as the BASE layer:
    // layer 0's texture, projected once across the whole landscape. Detail layers keep
    // tiling on top of it wherever the tilemask puts weight.
    if (g_colormapImage != nullptr && g_colormapWidth > 2 && g_colormapHeight > 2)
    {
        const DAVA::String cmFile = "colormap.dds";
        if (WriteBwRgbaDds(DAVA::FilePath(outDirString + cmFile), g_colormapImage->GetData(),
                           static_cast<DAVA::uint32>(g_colormapWidth), static_cast<DAVA::uint32>(g_colormapHeight)))
        {
            g_colormapLayerRef = "maps/landscape/" + mapName + "/" + cmFile; // same folder the tiles are written to
            // The colour map is its own layer 4, which already spans the map once.
            // Stretching tile0 as well (the old "colormap is layer 0" design) turned the
            // first detail tile into one 600 m blur; Dava tiles all four 50x (12 m).
            DAVA::Logger::Info("[TerrainConverter] colour map '%s' (%d x %d) exported as its OWN layer 4 - "
                               "all four tiles keep their masks",
                               g_colormapLayerRef.c_str(), g_colormapWidth, g_colormapHeight);
        }
    }

    DAVA::SafeRelease(converted);
    for (DAVA::uint32 i = 0; i < images.size(); ++i)
    {
        DAVA::SafeRelease(images[i]);
    }
}

bool Converter::PrepareIndexTexture(void)
{
    Vector<Color> customColors = config->GetColorPropertyValues("LandscapeCustomColors");
    defaultColorIndex = 1;
    uint8* data = 0;
    texDataWidth = 10;
    texDataHeight = 10;

    KeyedArchive* customProperties = GetCustomPropertiesArchieve(land);
    if (customProperties && customProperties->IsKeyExists("customColorTexture"))
    {
        FilePath fullPath = projectPath + customProperties->GetString("customColorTexture", "");
        Vector<Image*> texs;
        ImageSystem::Load(fullPath, texs);
        if (texs.size() > 0)
        {
            Image* tex = texs[0];
            if (tex)
            {
                // need fix after framework patch
                tex->FlipVertical();

                data = tex->GetData();
                texDataWidth = tex->GetWidth();
                texDataHeight = tex->GetHeight();
                if (texDataWidth != TILEMASK_SZ)
                {
                    DAVA::Logger::Error("Custom texture size do not match %d (%d)", TILEMASK_SZ, texDataWidth);
                    return false;
                }
            }
        }
        else
        {
            DAVA::Logger::Error("Cant find custom texture (for level passability) %s",
                                fullPath.GetAbsolutePathname().c_str());
        }
    }

    if (data != nullptr)
    {
        // OSE bake pack: remember the raw custom color image for lodTexture.dds
        g_colorImage.assign(data, data + texDataWidth * texDataHeight * 4);
        g_colorImageWidth = texDataWidth;
        g_colorImageHeight = texDataHeight;
    }

    // Blitz ships no customColorTexture, and without it the 10x10 defaults above
    // survived: the blend sampling factor kk, the dominant/passability index and the
    // old lodTexture all read a 10x10 corner of the map. That single bug is why one
    // layer covered the entire terrain. Derive it from the tilemask instead.
    std::vector<uint8> fromMask;
    if (data == nullptr && tileMask != nullptr)
    {
        texDataWidth = (int32)tileMask->GetWidth();
        texDataHeight = (int32)tileMask->GetHeight();
        fromMask.resize((size_t)texDataWidth * (size_t)texDataHeight, (uint8)defaultColorIndex);
        const uint8* maskPixels = tileMask->GetData();
        for (int32 t = 0; t < texDataWidth * texDataHeight; ++t)
        {
            int32 best = 0;
            int32 bestValue = -1;
            for (int32 c = 0; c < 4; ++c)
            {
                if (maskPixels[t * 4 + c] > bestValue)
                {
                    bestValue = maskPixels[t * 4 + c];
                    best = c;
                }
            }
            fromMask[(size_t)t] = (uint8)(best % 4);
        }
        DAVA::Logger::Info("[TerrainConverter] no customColorTexture: passability and blend sampling derived from the "
                           "tilemask (%dx%d) instead of the 10x10 default",
                           texDataWidth, texDataHeight);
    }

    int32 size = texDataWidth * texDataHeight;
    texData = new uint8[size];
    memset(texData, defaultColorIndex, size);
    if (!fromMask.empty())
    {
        memcpy(texData, &fromMask[0], (size_t)size);
    }
    else if (data)
    {
        int32 offs = 0;
        for (int i = 0; i < size * 4; i += 4)
        {
            Color c((float32)data[i] / 255.f,
                    (float32)data[i + 1] / 255.f,
                    (float32)data[i + 2] / 255.f,
                    (float32)data[i + 3] / 255.f);

            for (uint8 j = 0; j < (uint8)customColors.size(); ++j)
            {
                if (c == customColors[j])
                {
                    texData[offs] = j;
                    break;
                }
            }
            offs++;
        }
    }

    static const String materialsMap[8] = {
        "snow", "maps/landscape/00_AllTiles/Snow",
        "ground", "maps/landscape/00_AllTiles/Ground_N_1",
        "sand", "maps/landscape/00_AllTiles/sand_grass",
        "stone", "maps/landscape/00_AllTiles/Road"
    };

    KeyedArchive* materialOptions = new KeyedArchive();
    if (materialOptions->LoadFromYamlFile(FilePathHelper::MapEffectsPath(inputFile)))
    {
        const KeyedArchive::UnderlyingMap mapData = materialOptions->GetArchieveData();
        for (KeyedArchive::UnderlyingMap::const_iterator it = mapData.begin(); it != mapData.end(); ++it)
        {
            const String& typeName = it->second->AsString();
            for (int i = 0; i < 4; i++)
            {
                if (materialsMap[i << 1] == typeName)
                {
                    textureNames.push_back(materialsMap[(i << 1) + 1]);
                    break;
                }
            }
        }
    }
    else
    {
        textureNames.push_back("maps/landscape/00_AllTiles/Ground_N_1");
        textureNames.push_back("maps/landscape/00_AllTiles/Ground_N_2");
        textureNames.push_back("maps/landscape/00_AllTiles/Ground_N_3");
        textureNames.push_back("maps/landscape/00_AllTiles/Ground_N_4");
    }
    SafeRelease(materialOptions);

    // OSE bake pack: the blend writers index textureNames[0..3] (layer 1..4);
    // a partial MapEffects config must never leave fewer than 4 entries.
    while (textureNames.size() < 4)
    {
        static const String fallbacks[4] = {
            "maps/landscape/00_AllTiles/Ground_N_1",
            "maps/landscape/00_AllTiles/Ground_N_2",
            "maps/landscape/00_AllTiles/Ground_N_3",
            "maps/landscape/00_AllTiles/Ground_N_4"
        };
        textureNames.push_back(fallbacks[textureNames.size() % 4]);
    }

    dominantSize = DOM_RES * DOM_RES;
    dominant = new uint8[dominantSize];

    blends[0] = new uint8[dominantSize];
    blends[1] = new uint8[dominantSize];
    blends[2] = new uint8[dominantSize];
    blends[3] = new uint8[dominantSize];

    // Real tile art instead of the OSE atlas names, when the landscape material
    // still carries tileTexture0 (Blitz maps do).
    BakeLandscapeTiles(land, mapName, textureNames, customColors, tileMask);

    return true;
} // Converter::PrepareIndexTexture

static uint8 GetDominantIndex(uint8* image, uint32 offset)
{
    uint8 maxVal = 0;
    int32 maxInd = 0;
    for (uint8 i = 0; i < BLENDS_SZ; i++)
    {
        uint8 v = image[offset + i];
        if (maxVal < v)
        {
            maxVal = v;
            maxInd = i;
        }
    }
    return maxInd;
}

uint8 downValues[4];

static uint8 Downscale4Pixels(uint8* image, uint32 strip, uint32 offset)
{
    memset(downValues, 0, 4);

    downValues[GetDominantIndex(image, offset)]++;
    downValues[GetDominantIndex(image, offset + 4)]++;
    downValues[GetDominantIndex(image, offset + strip)]++;
    downValues[GetDominantIndex(image, offset + strip + 4)]++;

    uint8 maxVal = 0;
    int32 maxInd = 0;
    for (uint8 i = 0; i < 4; i++)
    {
        if (maxVal < downValues[i])
        {
            maxVal = downValues[i];
            maxInd = i;
        }
    }
    return maxInd;
}

struct DominantBlitzHeader
{
    static const uint32 VERSION_ZIP = 1;
    uint32 magic_;
    uint32 version_;
    uint32 width_;
    uint32 dataSize_;
    uint32 pad_[2];
    static const uint32 MAGIC = 0x006d6b6b; //"mkm"

    DominantBlitzHeader()
    {
        magic_ = MAGIC;
        version_ = VERSION_ZIP;
        width_ = 0;
        dataSize_ = 0;
        pad_[0] = 0;
        pad_[1] = 0;
    }

    bool check() const
    {
        return (magic_ == MAGIC && version_ == VERSION_ZIP);
    }

    void Save(File* f)
    {
        f->Write(&magic_, 4);
        f->Write(&version_, 4);
        f->Write(&width_, 4);
        f->Write(&dataSize_, 4);
        f->Write(&pad_, 8);
    }

    void Load(File* f)
    {
        f->Read(&magic_, 4);
        f->Read(&version_, 4);
        f->Read(&width_, 4);
        f->Read(&dataSize_, 4);
        f->Read(&pad_, 8);
    }
};

void Converter::GenerateMaterialKindMap(void)
{
    if (tileMask)
    {
        uint8* tileMaskData = tileMask->GetData();
        uint32 width = tileMask->GetWidth();
        uint32 bufferSz = ((width >> 1) * (width >> 1)) >> 2;
        uint8* matKind = new uint8[bufferSz];
        memset(matKind, 0, bufferSz);
        uint8 val;
        uint32 offset = 0;
        uint32 matOff = 0;
        for (uint32 y = 0; y < width; y += 2)
        {
            for (uint32 x = 0; x < width; x += 8)
            {
                val = 0;
                for (int i = 0; i < 4; i++)
                {
                    val = val + ((Downscale4Pixels(tileMaskData, width * 4, offset + i * 8)) << (i * 2));
                }
                if (matOff < bufferSz)
                {
                    matKind[matOff] = val;
                }
                matOff++;
                offset += 32;
            }
            offset += width * 4;
        }

        File* f = File::Create(FilePathHelper::GetMKMPath(inputFile), File::CREATE | File::WRITE);
        if (f)
        {
            DominantBlitzHeader header;
            header.width_ = (width >> 1);
            header.dataSize_ = bufferSz;
            header.Save(f);
            f->Write(matKind, bufferSz);
            SafeRelease(f);
        }
    }
}

void Converter::DoConvertHM()
{
    g_terrainReady = false;
    g_heightLandscape = nullptr;
    heightMap = NULL;
    landscapeObject = NULL;
    tileMask = NULL;
    FileSystem* fs = GetEngineContext()->fileSystem;
    fs->DeleteDirectory("out\\");
    fs->CreateDirectory("out\\");
    // out is the res root. Only space data (chunks, cdata, vlo, settings) lives in
    // spaces/<map>; geometry, textures and trees are shared res resources, referenced
    // the way Blitz references its own (characters/..., sets/...).
    fs->CreateDirectory("out\\spaces\\");
    fs->CreateDirectory("out\\spaces\\" + mapName + "\\");
    fs->CreateDirectory("out\\content\\");
    fs->CreateDirectory("out\\content\\" + mapName + "\\");
    fs->CreateDirectory("out\\content\\" + mapName + "\\images\\");
    fs->CreateDirectory("out\\content\\" + mapName + "\\speedTree\\");
    fs->CreateDirectory("out\\destructible\\");
    fs->CreateDirectory("out\\tree\\");
    fs->CreateDirectory("out\\falling\\");

    Terrain* terrain = NULL;
    land = scene->FindByName("Landscape");
    if (land == nullptr)
    {
        DAVA::Logger::Error("[Landscape] no entity named 'Landscape' in this scene - terrain pass skipped");
    }
    else
    {
        DAVA::RenderComponent* landscapeRender = land->GetComponent<DAVA::RenderComponent>();
        if (landscapeRender == nullptr)
        {
            DAVA::Logger::Error("[Landscape] '%s' has no RenderComponent - terrain pass skipped",
                                land->GetName().c_str());
        }
        else
        {
            landscapeObject = (Landscape*)landscapeRender->GetRenderObject();
        }

        if (landscapeObject == nullptr)
        {
            DAVA::Logger::Error("[Landscape] '%s' does not render as a Landscape - terrain pass skipped",
                                land->GetName().c_str());
        }
        else
        {
            g_heightLandscape = landscapeObject;
            heightMap = landscapeObject->GetHeightmap();
            if (heightMap == nullptr)
            {
                DAVA::Logger::Error("[Landscape] '%s' has no heightmap - terrain pass skipped", land->GetName().c_str());
            }
            else
            {
                terrain = new Terrain(landscapeObject);

                // ---- landscape size / height check -------------------------------
                // The Properties panel shows these as Render Object fields: Size (the
                // world extent, 600 m on rudniki) and Height (the relief span, 222 m
                // there). Everything below depends on them, so verify them instead of
                // discovering a bad box three passes later: the extent decides the chunk
                // grid, and the heightmap grid decides how many source texels one BW
                // sample covers.
                const AABBox3& lb = landscapeObject->GetBoundingBox();
                const DAVA::float32 sizeX = lb.max.x - lb.min.x;
                const DAVA::float32 sizeY = lb.max.y - lb.min.y;
                const DAVA::int32 hmSize = (DAVA::int32)heightMap->Size();

                DAVA::float32 zlo = 1e30f;
                DAVA::float32 zhi = -1e30f;
                if (heightMap->Data() != nullptr && hmSize > 1)
                {
                    const DAVA::int32 steps = 15;
                    for (DAVA::int32 gy = 0; gy <= steps; ++gy)
                    {
                        for (DAVA::int32 gx = 0; gx <= steps; ++gx)
                        {
                            const DAVA::int32 px = gx * (hmSize - 1) / steps;
                            const DAVA::int32 py = gy * (hmSize - 1) / steps;
                            const DAVA::float32 z = heightMap->GetPoint(px, py, lb).z;
                            if (z < zlo)
                                zlo = z;
                            if (z > zhi)
                                zhi = z;
                        }
                    }
                }

                // Base level: BW terrain is absolute millimetres and clips at 65535
                // (65.535 m), while the landscape starts wherever the author put it
                // (14.8 m here). Shift the map so its lowest point is 0 and the whole
                // relief fits under the ceiling. Only if the span itself is taller than
                // 65.535 m do we squeeze it, and then the same factor goes on the props.
                g_heightOffsetM = 0.0f;
                g_heightReliefScale = 1.0f;
                if (zhi > zlo)
                {
                    const DAVA::float32 span = zhi - zlo;
                    g_heightOffsetM = -zlo;
                    if (span > 65.535f)
                    {
                        g_heightReliefScale = 65.535f / span;
                    }
                    DAVA::Logger::Info("[TerrainConverter] vertical: engine %.2f .. %.2f m -> %.2f .. %.2f m "
                                       "(offset %+.2f m, scale %.4f, BW ceiling 65.535 m)",
                                       zlo, zhi, 0.0f, span * g_heightReliefScale,
                                       g_heightOffsetM, g_heightReliefScale);
                    if (g_heightReliefScale < 1.0f)
                    {
                        DAVA::Logger::Warning("[TerrainConverter] relief %.2f m is taller than the 65.535 m BW "
                                              "heightmap ceiling - squeezed by %.4f",
                                              span, g_heightReliefScale);
                    }
                }

                const DAVA::float32 quad = (hmSize > 1 && sizeX > 0.5f) ? sizeX / (DAVA::float32)(hmSize - 1) : 0.0f;
                DAVA::Logger::Info("[TerrainConverter] landscape size %.2f x %.2f m, heightmap %d x %d "
                                   "(%.4f m per quad), relief %.2f .. %.2f m (span %.2f m)",
                                   sizeX, sizeY, hmSize, hmSize, quad, zlo, zhi, zhi - zlo);

                if (hmSize < 2 || heightMap->Data() == nullptr)
                {
                    DAVA::Logger::Error("[TerrainConverter] landscape heightmap has no data (%d samples) - "
                                        "heights would all come out at 0",
                                        hmSize);
                }
                if (sizeX > 0.5f && sizeY > 0.5f && fabsf(sizeX - sizeY) > 1.0f)
                {
                    DAVA::Logger::Warning("[TerrainConverter] landscape box is not square (%.2f x %.2f m): the chunk "
                                          "grid is derived from the X extent and the map will be cropped",
                                          sizeX, sizeY);
                }
                if (zhi > zlo && zhi - zlo < 0.01f)
                {
                    DAVA::Logger::Warning("[TerrainConverter] landscape relief is %.3f m - a flat map. If the scene "
                                          "shows hills, the heightmap scale (Render Object > Height) is not applied",
                                          zhi - zlo);
                }
                const DAVA::float32 chunksF = sizeX / 100.0f;
                if (chunksF > 0.5f && fabsf(chunksF - floorf(chunksF + 0.5f)) > 0.01f)
                {
                    DAVA::Logger::Warning("[TerrainConverter] landscape size %.2f m is not a whole number of 100 m "
                                          "chunks (%d will be written)",
                                          sizeX, (DAVA::int32)ceilf(sizeX * 0.5f / 100.0f) * 2);
                }
                // one BW sample spans 100/chunkSize metres of source texels
                if (quad > 0.0001f)
                {
                    DAVA::Logger::Info("[TerrainConverter] terrain source texels per BW sample: %.2f "
                                       "(BW spacing %.4f m / source quad %.4f m)",
                                       (100.0f / 64.0f) / quad, 100.0f / 64.0f, quad);
                }
            }

            NMaterial* landscapeMaterial = landscapeObject->GetMaterial();
            if (landscapeMaterial == nullptr)
            {
                DAVA::Logger::Error("[Landscape] no landscape material");
            }
            else
            {
                Texture* tx = landscapeMaterial->GetEffectiveTexture(FastName("tileMask"));
                if (tx == nullptr)
                {
                    DAVA::Logger::Error("[Landscape] material '%s' has no tileMask texture",
                                        landscapeMaterial->GetMaterialName().c_str());
                }
                else
                {
                    const FilePath txPath = tx->GetPathname();
                    DAVA::Logger::Info("[Landscape] tileMask '%s'", txPath.GetAbsolutePathname().c_str());

                    TextureDescriptor* desc = TextureDescriptor::CreateFromFile(txPath);
                    if (desc == nullptr)
                    {
                        DAVA::Logger::Error("[Landscape] no descriptor at '%s'", txPath.GetAbsolutePathname().c_str());
                    }
                    else
                    {
                        DAVA::FileSystem* vfs = GetEngineContext()->fileSystem;

                        // Ask the descriptor where the runtime container is instead of guessing names:
                        // shipped spaces glue the GPU name to the basename (tilemask_fdx11.pvr) and the
                        // descriptor already knows the GPU and container format it was exported for.
                        const eGPUFamily firstGpu = Texture::GetGPUForLoading(GPU_DX11, desc);
                        FilePath candidates[GPU_FAMILY_COUNT + 1];
                        int32 candidateCount = 0;
                        candidates[candidateCount++] = desc->CreateMultiMipPathnameForGPU(firstGpu);
                        for (int32 g = GPU_POWERVR_IOS; g < GPU_FAMILY_COUNT; ++g)
                        {
                            const eGPUFamily gpu = (eGPUFamily)g;
                            if (gpu != firstGpu)
                            {
                                candidates[candidateCount++] = desc->CreateMultiMipPathnameForGPU(gpu);
                            }
                        }
                        candidates[candidateCount++] = desc->GetSourceTexturePathname();

                        FilePath loadedPath;
                        for (int32 c = 0; c < candidateCount; ++c)
                        {
                            if (candidates[c].IsEmpty() == false && vfs->IsFile(candidates[c]) == true)
                            {
                                loadedPath = candidates[c];
                                break;
                            }
                        }

                        if (loadedPath.IsEmpty())
                        {
                            DAVA::Logger::Error("[Landscape] descriptor '%s' resolves to no file on disk - "
                                                "checked the source and every GPU container",
                                                txPath.GetAbsolutePathname().c_str());
                        }
                        else
                        {
                            DAVA::Logger::Info("[Landscape] loading tileMask '%s'", loadedPath.GetAbsolutePathname().c_str());

                            Image* loaded = ImageSystem::LoadSingleMip(loadedPath);
                            if (loaded == nullptr)
                            {
                                DAVA::Logger::Error("[Landscape] '%s' could not be decoded", loadedPath.GetAbsolutePathname().c_str());
                            }
                            else if (loaded->GetPixelFormat() == PixelFormat::FORMAT_RGBA8888)
                            {
                                tileMask = loaded;
                            }
                            else
                            {
                                DAVA::Logger::Info("[Landscape] '%s' decoded as pixel format %d, converting to RGBA8888",
                                                   loadedPath.GetAbsolutePathname().c_str(),
                                                   static_cast<int32>(loaded->GetPixelFormat()));

                                Image* converted = Image::Create(loaded->GetWidth(), loaded->GetHeight(),
                                                                 PixelFormat::FORMAT_RGBA8888);
                                if (converted != nullptr && ImageConvert::ConvertImage(loaded, converted))
                                {
                                    tileMask = converted;
                                }
                                else
                                {
                                    SafeRelease(converted);
                                    DAVA::Logger::Error("[Landscape] pixel format %d has no RGBA8888 conversion",
                                                        static_cast<int32>(loaded->GetPixelFormat()));
                                }
                                SafeRelease(loaded);
                            }
                        }
                        delete desc; // TextureDescriptor is a plain class, not RefCounted
                    }
                }
            }
        }
    }
    if (tileMask)
    {
        if (tileMask->GetPixelFormat() != PixelFormat::FORMAT_RGBA8888)
        {
            DAVA::Logger::Error("TileMask decoded to pixel format %d, RGBA8888 required",
                                static_cast<int>(tileMask->GetPixelFormat()));
            SafeRelease(tileMask);
        }
        else
        {
            // need fix after framework patch
            tileMask->FlipVertical();

            if (tileMask->GetWidth() != tileMask->GetHeight())
            {
                DAVA::Logger::Error("TileMask is not square (%d x %d)", tileMask->GetWidth(), tileMask->GetHeight());
                SafeRelease(tileMask);
            }
            else if (tileMask->GetWidth() != TILEMASK_SZ)
            {
                // shipped maps are not all 2048 - every consumer takes the size from the image,
                // so use it as shipped instead of rejecting the whole terrain
                DAVA::Logger::Info("[Landscape] tileMask is %d x %d (%d is only the reference size) - using it",
                                   tileMask->GetWidth(), tileMask->GetHeight(), TILEMASK_SZ);
            }
        }
    }
    if (heightMap && tileMask && terrain)
    {
        uint8* tileMaskData = tileMask->GetData();
        const int32 tmWidth = (int32)tileMask->GetWidth();
        const int32 tmHeight = (int32)tileMask->GetHeight();
        if (PrepareIndexTexture())
        {
            g_terrainReady = true;
            AABBox3 box = landscapeObject->GetBoundingBox();
            float realSize = (box.max.x - box.min.x) * 0.5f;

            int32 sizeInChunks = (int32)ceil(realSize * 0.01f);
            float32 newRealSize = sizeInChunks * 100.0f;

            DAVA::Logger::Info("[TerrainConverter] landscape box x %.2f..%.2f  y %.2f..%.2f  (%d chunks per axis, "
                               "space %.0f m)",
                               (float)box.min.x, (float)box.max.x, (float)box.min.y, (float)box.max.y,
                               sizeInChunks, (float)newRealSize);

            // Terrain heights come from the landscape .heightmap read directly: the
            // engine sampler only produced heights for the first row of every chunk.
            g_heightFile = BlitzHeightFile();
            {
                DAVA::FilePath hmPath;
                if (FindLandscapeHeightmap(clientDataSource, mapName, hmPath) &&
                    LoadBlitzHeightmapFile(hmPath, g_heightFile))
                {
                    DAVA::float32 lo = 1e9f;
                    DAVA::float32 hi = -1e9f;
                    for (size_t i = 0; i < g_heightFile.metres.size(); ++i)
                    {
                        if (g_heightFile.metres[i] < lo)
                            lo = g_heightFile.metres[i];
                        if (g_heightFile.metres[i] > hi)
                            hi = g_heightFile.metres[i];
                    }
                    DAVA::Logger::Info("[TerrainConverter] landscape heightmap '%s': %d x %d, %.3f .. %.3f m",
                                       hmPath.GetAbsolutePathname().c_str(), g_heightFile.size, g_heightFile.size, lo, hi);
                }
                else
                {
                    DAVA::Logger::Warning("[TerrainConverter] no usable .heightmap under the map folder - "
                                          "falling back to the engine landscape sampler");
                }
            }

            // BW layer u/vProjection from the landscape MATERIAL tiling:
            // textureTiling/tileScaleN are NMaterial instance properties
            // ([material][instance] in tilemask-vp/fp), NOT entity custom
            // properties. Dava: texCoordTiled = uv01 * textureTiling, then
            // texCoordTiledN = texCoordTiled * tileScaleN per layer - the
            // per-layer props only exist when the material carries the
            // LANDSCAPE_SCALED_TILES_NON_PBR flag (or is PBR); without them
            // every layer shares the plain tiling (scale = 1). One tile spans
            // mapSize / (tiling * scale) metres and BW wants the inverse.
            {
                DAVA::Vector2 tiling(50.0f, 50.0f); // shader defaults
                float32 scales[BLENDS_SZ] = { 1.0f, 1.0f, 1.0f, 1.0f };
                bool scaledTiles = false;
                NMaterial* landscapeMaterial = landscapeObject->GetMaterial();
                if (landscapeMaterial != NULL)
                {
                    if (landscapeMaterial->HasLocalProperty(NMaterialParamName::PARAM_LANDSCAPE_TEXTURE_TILING))
                    {
                        const float32* v = landscapeMaterial->GetLocalPropValue(NMaterialParamName::PARAM_LANDSCAPE_TEXTURE_TILING);
                        if (v != NULL)
                        {
                            tiling = DAVA::Vector2(v[0], v[1]);
                        }
                    }
                    // tileScaleN only exists with the scaled-tiles flag (material
                    // editor toggle) or on PBR landscape materials, and Dava only
                    // applies it when that flag is set - so only trust it then
                    scaledTiles = landscapeMaterial->HasLocalFlag(NMaterialFlagName::FLAG_LANDSCAPE_SCALED_TILES_NON_PBR);
                    for (int32 i = 0; i < BLENDS_SZ; ++i)
                    {
                        char scaleKey[32];
                        SnprintfSafe(scaleKey, sizeof(scaleKey), "tileScale%d", i);
                        if (landscapeMaterial->HasLocalProperty(FastName(scaleKey)))
                        {
                            const float32* v = landscapeMaterial->GetLocalPropValue(FastName(scaleKey));
                            if (v != NULL)
                            {
                                scales[i] = v[0];
                            }
                        }
                    }
                    // relaxmap mode replaces texCoordTiled = uv01 * textureTiling
                    // with a relaxmap-warped fetch (tilemask-vp LANDSCAPE_RELAXMAP
                    // branch) - the straight pitch below would not match it
                    if (landscapeMaterial->HasLocalFlag(NMaterialFlagName::FLAG_LANDSCAPE_USE_RELAXMAP))
                    {
                        DAVA::Logger::Error("[TerrainConverter] landscape material has LANDSCAPE_USE_RELAXMAP: the blend tile pitch is derived from textureTiling and will NOT match relaxmap-warped sampling");
                    }
                    DAVA::Logger::Info("[TerrainConverter] landscape material: textureTiling=%.1f, %.1f%s",
                                       tiling.x, tiling.y,
                                       scaledTiles ? " (scaled tiles enabled)" : "");
                }
                const float32 mapSize = realSize * 2.0f; // same world extent as the blend-mask mapping (kk)
                g_mapWorldSize = mapSize;
                // Layer projection, matched to the shipped Blitz cdata instead of to
                // Dava's material number. A shipped Blitz space stores exactly
                // uProjection.x = vProjection.z = 0.1 for every layer, i.e. one detail
                // tile per 10 m, and never stores textureTiling at all: the tiling and
                // the per-layer tileScale only shape the EDITOR's preview UVs
                // (uv01 * textureTiling, then * tileScaleN). What the client needs is the
                // final metres-per-tile, so: 10 m baseline, textureTiling folded in only
                // when the material really carries one, and the per-layer UV multiplier
                // applied when the scaled-tiles flag is on.
                for (int32 i = 0; i < BLENDS_SZ; ++i)
                {
                    float32 pitchMetres = 10.0f;
                    if (tiling.x > 1.0f)
                    {
                        pitchMetres = mapSize / tiling.x;
                    }
                    if (scaledTiles && scales[i] > 0.0f && scales[i] != 1.0f)
                    {
                        pitchMetres /= scales[i];
                    }
                    if (pitchMetres < 0.05f)
                    {
                        pitchMetres = 0.05f;
                    }
                    g_blendUProjX[i] = 1.0f / pitchMetres;
                    g_blendVProjZ[i] = 1.0f / pitchMetres;
                }
                if (g_layer0SpansMap && (box.max.x - box.min.x) > 0.5f && (box.max.y - box.min.y) > 0.5f)
                {
                    g_blendUProjX[0] = 1.0f / (box.max.x - box.min.x);
                    g_blendVProjZ[0] = 1.0f / (box.max.y - box.min.y);
                }
                if (g_groundSpansMap && mapSize > 0.5f)
                {
                    // one repeat across the map: the baked ground colour must not tile
                    for (int32 i = 0; i < BLENDS_SZ; ++i)
                    {
                        g_blendUProjX[i] = 1.0f / mapSize;
                        g_blendVProjZ[i] = 1.0f / mapSize;
                    }
                }
                for (int32 i = 0; i < BLENDS_SZ; ++i)
                {
                    DAVA::Logger::Info("[TerrainConverter] terrain layer %d: tileScale=%.3f (applied: %s), "
                                       "one tile per %.2f m, uProjection=%.6f",
                                       i, scales[i], scaledTiles ? "yes" : "no",
                                       1.0f / Max(0.0001f, g_blendUProjX[i]), g_blendUProjX[i]);
                }
            }

            g_minChunk = -sizeInChunks;
            g_maxChunk = sizeInChunks - 1;
            g_spacesPath = "out\\spaces\\" + mapName + "\\";
            if (!g_spacesPath.empty() && g_spacesPath[g_spacesPath.length() - 1] == '/')
            {
                g_spacesPath[g_spacesPath.length() - 1] = '\\';
            }
            fs->CreateDirectory("out\\spaces\\");
            fs->CreateDirectory(g_spacesPath);
            // out\ is CWD-relative by WG design - print where it actually landed
            DAVA::Logger::Info("[TerrainConverter] output root: %s", FilePath("out\\").GetAbsolutePathname().c_str());
            DAVA::Logger::Info("[TerrainConverter] spaces folder: %s", FilePath(g_spacesPath).GetAbsolutePathname().c_str());
            {
                DAVA::int32 sx0 = g_minChunk;
                DAVA::int32 sz0 = g_minChunk;
                DAVA::int32 sx1 = g_maxChunk;
                DAVA::int32 sz1 = g_maxChunk;
                if (g_itemMinX <= g_itemMaxX)
                {
                    if (g_itemMinX < sx0)
                        sx0 = g_itemMinX;
                    if (g_itemMaxX > sx1)
                        sx1 = g_itemMaxX;
                    if (g_itemMinZ < sz0)
                        sz0 = g_itemMinZ;
                    if (g_itemMaxZ > sz1)
                        sz1 = g_itemMaxZ;
                    if (sx0 != g_minChunk || sz0 != g_minChunk || sx1 != g_maxChunk || sz1 != g_maxChunk)
                    {
                        DAVA::Logger::Info("[TerrainConverter] space bounds widened to the item extents: "
                                           "%d..%d x %d..%d chunks",
                                           sx0, sx1, sz0, sz1);
                    }
                }
                // Every cell inside the published bounds has to exist, even if it holds
                // nothing: BW walks the bounds and reports "DataSection ... is NULL (FNF)"
                // for a cell whose chunk it can see but whose cdata it cannot open. Empty
                // chunks are legal, so fill the gaps with an empty root.
                int32 created = 0;
                for (DAVA::int32 fz = sz0; fz <= sz1; ++fz)
                {
                    for (DAVA::int32 fx = sx0; fx <= sx1; ++fx)
                    {
                        const String id = outsideChunkIdentifier(fx, fz);
                        const FilePath chunkPath(g_spacesPath + id + ".chunk");
                        if (DAVA::FileSystem::Instance()->IsFile(chunkPath))
                        {
                            continue;
                        }
                        File* f = File::Create(chunkPath, File::CREATE | File::WRITE);
                        if (f != nullptr)
                        {
                            const char* emptyChunk = "<root>\r\n</root>\r\n";
                            f->Write(emptyChunk, (uint32)strlen(emptyChunk));
                            SafeRelease(f);
                            ++created;
                        }
                    }
                }
                if (created > 0)
                {
                    DAVA::Logger::Info("[TerrainConverter] %d empty chunk(s) written to fill the space bounds", created);
                }
                g_spaceMinX = sx0;
                g_spaceMaxX = sx1;
                g_spaceMinZ = sz0;
                g_spaceMaxZ = sz1;
                CreateSettingsFile(sx0, sz0, sx1, sz1);
            }

            // ---- which world->landscape mapping actually yields relief ----------
            // Scenes differ in where they place the landscape, and the wrong convention
            // makes PlaceLandscapePoint return a constant - which is exactly what a flat
            // heightmap is. Probe three conventions on a coarse grid, keep the one with
            // real variance, and log all three so the choice is visible.
            if (g_heightMapMode < 0)
            {
                double variance[3] = { 0.0, 0.0, 0.0 };
                double coverage[3] = { 0.0, 0.0, 0.0 };
                const float32 boxW = box.max.x - box.min.x;
                const float32 boxH = box.max.y - box.min.y;
                for (int32 mode = 0; mode < 3; ++mode)
                {
                    double sum = 0.0;
                    double sumSq = 0.0;
                    int32 count = 0;
                    int32 nonZero = 0;
                    for (int32 j = 0; j < 25; ++j)
                    {
                        for (int32 i = 0; i < 25; ++i)
                        {
                            const float32 tx = ((float32)i / 24.0f) * 2.0f - 1.0f;
                            const float32 ty = ((float32)j / 24.0f) * 2.0f - 1.0f;
                            Vector3 probePoint(0.0f, 0.0f, 0.0f);
                            if (mode == 0)
                            {
                                probePoint.x = tx * newRealSize;
                                probePoint.y = ty * newRealSize;
                            }
                            else if (mode == 1)
                            {
                                probePoint.x = box.min.x + (tx * 0.5f + 0.5f) * boxW;
                                probePoint.y = box.min.y + (ty * 0.5f + 0.5f) * boxH;
                            }
                            else
                            {
                                probePoint.x = tx * newRealSize - box.min.x;
                                probePoint.y = ty * newRealSize - box.min.y;
                            }
                            Vector3 probeResult(0.0f, 0.0f, 0.0f);
                            terrain->PlaceLandscapePoint(probePoint, probeResult, NULL);
                            if (probeResult.z != 0.0f)
                            {
                                ++nonZero;
                            }
                            sum += probeResult.z;
                            sumSq += (double)probeResult.z * probeResult.z;
                            ++count;
                        }
                    }
                    const double mean = count > 0 ? sum / (double)count : 0.0;
                    variance[mode] = count > 0 ? (sumSq / (double)count) - mean * mean : 0.0;
                    coverage[mode] = count > 0 ? (double)nonZero / (double)count : 0.0;
                }

                // Coverage comes first: a convention that samples past the heightmap can
                // show a bigger variance while covering a quarter of the map, and that is
                // exactly what leaves chunks with no relief. Only then compare variance.
                int32 best = 0;
                for (int32 mode = 1; mode < 3; ++mode)
                {
                    if (coverage[mode] > coverage[best] + 0.02 ||
                        (coverage[mode] > coverage[best] - 0.02 && variance[mode] > variance[best]))
                    {
                        best = mode;
                    }
                }
                g_heightMapMode = best;
                DAVA::Logger::Info("[TerrainConverter] landscape box x %.1f..%.1f y %.1f..%.1f, chunk grid %d; "
                                   "variance world=%.3f/%.0f%% box=%.3f/%.0f%% shifted=%.3f/%.0f%% -> convention %d",
                                   box.min.x, box.max.x, box.min.y, box.max.y, sizeInChunks,
                                   variance[0], coverage[0] * 100.0,
                                   variance[1], coverage[1] * 100.0,
                                   variance[2], coverage[2] * 100.0,
                                   g_heightMapMode);
            }

            int16 chunkSize = 64;
            char content[1024];

            int16 nW = chunkSize + 5;
            int16 nH = chunkSize + 5;
            int16 offset;
            float32 min;
            float32 max;
            image32.resize(nW * nH);
            // Dedicated copy of the sampled grid. image32 is reused as scratch further
            // down, and anything that clears it used to leave the heights PNG with a
            // single populated row, which reads in game as perfectly flat terrain.
            std::vector<int32> sampledLevels(nW * nH, 0);

            Vector3 point;
            Vector3 resultV;
            float32 ww = (float32)(sizeInChunks * chunkSize);
            float kf = 100.0f / (float32)chunkSize;
            int32 level;
#ifdef __DAVAENGINE_DEBUG__
            const PixelFormatDescriptor& formatDescriptor = PixelFormatDescriptor::GetPixelFormatDescriptor(FORMAT_A16);
            int32 bytesInPixel = 2;
            Image* spaceImage = NULL;
            spaceImage = Image::Create(chunkSize * 12 + 5, chunkSize * 12 + 5, formatDescriptor.formatID);
            uint8* spaceImageData = spaceImage->GetData();
#endif

            // Map a landscape texel index to world space under one of the three conventions.
            // Shared by the per-chunk probe and the real sampling so they cannot disagree.
            auto landscapeWorldPoint = [&](int32 mode, int16 x, int16 y, Vector3& p)
            {
                if (mode == 1)
                {
                    const float32 ux = newRealSize > 0.0f ? (((float32)x * kf) / newRealSize) * 0.5f + 0.5f : 0.5f;
                    const float32 uy = newRealSize > 0.0f ? (((float32)y * kf) / newRealSize) * 0.5f + 0.5f : 0.5f;
                    p.x = box.min.x + ux * (box.max.x - box.min.x);
                    p.y = box.min.y + uy * (box.max.y - box.min.y);
                }
                else if (mode == 2)
                {
                    p.x = (float32)x * kf - box.min.x;
                    p.y = (float32)y * kf - box.min.y;
                }
                else
                {
                    p.x = (float32)x * kf;
                    p.y = (float32)y * kf;
                }
                p.z = 0.0f;
            };

            for (DAVA::int32 yc = g_minChunk; yc <= g_maxChunk; ++yc)
            {
                for (DAVA::int32 xc = g_minChunk; xc <= g_maxChunk; ++xc)
                {
                    min = (float32)INT_MAX;
                    max = (float32)-INT_MAX;
                    offset = 0;
                    // a chunk whose samples returned nothing is not terrain: BW accepts a
                    // chunk with no <terrain> at all, and a flat plate there would both
                    // draw a dead 100 m quad and hide the VLOs placed on it
                    bool chunkTerrainValid = true;

                    // Per-chunk convention. A single map-wide guess samples outside the
                    // heightmap for part of the map under any one of the three, which is
                    // exactly how some chunks came out flat next to neighbours with relief.
                    int32 chunkMode = g_heightMapMode;
                    {
                        int32 bestMode = g_heightMapMode;
                        int32 bestDistinct = -1;
                        for (int32 mode = 0; mode < 3 && bestDistinct < 8; ++mode)
                        {
                            float32 samples[32];
                            int32 sampleCount = 0;
                            int32 distinctCount = 0;
                            for (int16 sy = chunkSize * yc - 2; sy < chunkSize * (yc + 1) + 3; sy += 4)
                            {
                                for (int16 sx = chunkSize * xc - 2; sx < chunkSize * (xc + 1) + 3; sx += 4)
                                {
                                    Vector3 probePoint(0.0f, 0.0f, 0.0f);
                                    landscapeWorldPoint(mode, sx, sy, probePoint);
                                    Vector3 probeResult(0.0f, 0.0f, 0.0f);
                                    terrain->PlaceLandscapePoint(probePoint, probeResult, NULL);

                                    bool known = false;
                                    for (int32 si = 0; si < sampleCount; ++si)
                                    {
                                        if (fabsf(samples[si] - probeResult.z) < 0.05f)
                                        {
                                            known = true;
                                            break;
                                        }
                                    }
                                    if (!known)
                                    {
                                        if (sampleCount < 32)
                                        {
                                            samples[sampleCount++] = probeResult.z;
                                        }
                                        ++distinctCount;
                                    }
                                }
                            }
                            if (distinctCount > bestDistinct)
                            {
                                bestDistinct = distinctCount;
                                bestMode = mode;
                            }
                        }
                        chunkMode = bestMode;
                        if (bestMode != g_heightMapMode)
                        {
                            DAVA::Logger::Info("[TerrainConverter] chunk (%d,%d): height convention %d "
                                               "(%d distinct levels here)",
                                               xc, yc, bestMode, bestDistinct);
                        }
                    }

#ifdef __DAVAENGINE_DEBUG__
                    Image* chunkImage = NULL;
                    chunkImage = Image::Create(nW, nH, formatDescriptor.formatID);
                    uint8* chunkImageData = chunkImage->GetData();
#endif
                    // heights
                    int32 samplesWritten = 0;
                    for (int16 y = chunkSize * yc - 2; y < chunkSize * (yc + 1) + 3; ++y)
                    {
                        for (int16 x = chunkSize * xc - 2; x < chunkSize * (xc + 1) + 3; ++x)
                        {
                            landscapeWorldPoint(chunkMode, x, y, point);
                            resultV.z = 0.0f;

                            // BW sample index -> world metres: a chunk is 100 m and holds
                            // chunkSize samples, index 2 is the chunk's first sample (index
                            // 0 and 1 are the 2x2 border, 66 the seam).
                            {
                                const DAVA::float32 spacing = 100.0f / (DAVA::float32)chunkSize;
                                const DAVA::float32 wx =
                                (DAVA::float32)xc * 100.0f + ((DAVA::float32)x - (DAVA::float32)xc * chunkSize - 2.0f) * spacing;
                                const DAVA::float32 wy =
                                (DAVA::float32)yc * 100.0f + ((DAVA::float32)y - (DAVA::float32)yc * chunkSize - 2.0f) * spacing;
                                // z_bw = +y_dava (BigWorld is left-handed Y-up), so the row
                                // order follows the landscape's y directly - no mirror.
                                resultV.z = HeightAtWorld(wx, wy, box);
                            }

                            level = Quantise(resultV.z);

#ifdef __DAVAENGINE_DEBUG__
                            int32 pngIndex = offset * bytesInPixel;
                            int32 pngGlobalIndex = (y + 2 + chunkSize * 6) * (chunkSize * 12 + 5) * bytesInPixel + (x + 2 + chunkSize * 6) * bytesInPixel;

                            for (int32 c = 0; c < bytesInPixel; ++c)
                            {
                                int32 shift = 8 * (bytesInPixel - 1 - c);
                                chunkImageData[pngIndex + c] = (level >> shift) & 0xFF;
                                spaceImageData[pngGlobalIndex + c] = (level >> shift) & 0xFF;
                            }
#endif

                            image32[offset] = level;
                            sampledLevels[offset] = level;
                            ++offset;
                            ++samplesWritten;

                            if (resultV.z > max)
                            {
                                max = resultV.z;
                            }
                            if (resultV.z < min)
                            {
                                min = resultV.z;
                            }
                        }
                    }

                    // The grid is rebuilt from the landscape .heightmap whenever that file
                    // loaded. The engine sampler was only returning a usable height for the
                    // first row of every chunk (three exports in a row came out with one
                    // populated row and 68 empty ones), and heights, mips, normals, ambient
                    // occlusion and lodTexture all read this grid. Rebuilding here means the
                    // block cannot be flat while the file is present, and the engine path is
                    // only used when no file was found.
                    if (LandscapeHeightAvailable() || g_heightFile.valid())
                    {
                        const DAVA::float32 spacing = 100.0f / (DAVA::float32)chunkSize;
                        min = (float32)INT_MAX;
                        max = (float32)-INT_MAX;
                        for (int32 ry = 0; ry < nH; ++ry)
                        {
                            for (int32 rx = 0; rx < nW; ++rx)
                            {
                                // metres, not sample counts: a BW sample sits every
                                // 100 / chunkSize metres and index 2 is the chunk's origin
                                const DAVA::float32 wx =
                                (DAVA::float32)xc * 100.0f + ((DAVA::float32)rx - 2.0f) * spacing;
                                const DAVA::float32 wy =
                                (DAVA::float32)yc * 100.0f + ((DAVA::float32)ry - 2.0f) * spacing;
                                const DAVA::float32 h = HeightAtWorld(wx, wy, box);
                                const int32 lv = Quantise(h);
                                image32[ry * nW + rx] = lv;
                                sampledLevels[ry * nW + rx] = lv;
                                if (h > max)
                                {
                                    max = h;
                                }
                                if (h < min)
                                {
                                    min = h;
                                }
                            }
                        }
                        DAVA::Logger::Info("[TerrainConverter] chunk (%d,%d): heights %.2f .. %.2f m from the %s",
                                           xc, yc, (float)min, (float)max,
                                           LandscapeHeightAvailable() ? "scene heightmap" : "heightmap file");
                    }

                    // A heightmap where whole rows come back identical is not a terrain:
                    // it means PlaceLandscapePoint returned nothing for those samples and
                    // every section built from this grid (mips, normals, AO, lodTexture)
                    // comes out flat. Say so instead of writing a flat world.
                    {
                        int32 zeroRows = 0;
                        int32 distinct = 0;
                        int32 prevRowHash = -1;
                        for (int32 yy = 0; yy < nH; ++yy)
                        {
                            int32 rowMin = INT_MAX;
                            int32 rowMax = -INT_MAX;
                            int32 hash = 0;
                            for (int32 xx = 0; xx < nW; ++xx)
                            {
                                const int32 v = image32[yy * nW + xx];
                                if (v < rowMin)
                                    rowMin = v;
                                if (v > rowMax)
                                    rowMax = v;
                                hash = hash * 31 + v;
                            }
                            if (rowMin == rowMax)
                                ++zeroRows;
                            if (hash != prevRowHash)
                            {
                                ++distinct;
                                prevRowHash = hash;
                            }
                        }
                        DAVA::Logger::Info("[TerrainConverter] heights grid %dx%d: %d constant row(s), %d distinct row(s), "
                                           "height %.2f..%.2f m",
                                           nW, nH, zeroRows, distinct, min, max);
                        // Flat is not the same as missing: Blitz maps have genuinely flat
                        // ground and dropping those chunks punched holes in the whole map.
                        // Only a grid that sampled nothing at all (every height exactly
                        // zero) counts as absent.
                        if (min == 0.0f && max == 0.0f)
                        {
                            chunkTerrainValid = false;
                            DAVA::Logger::Warning("[TerrainConverter] heights grid for chunk (%d,%d) sampled nothing - "
                                                  "no terrain written for it",
                                                  xc, yc);
                        }
                    }

                    // Snapshot of the sampled height grid. image32 is scratch from here on,
                    // and the lod mips were being built from the zeroed buffer, which made
                    // every lod>0 block perfectly flat.
                    std::vector<int32> chunkLevels(nW * nH, 0);
                    for (int32 qi = 0; qi < nW * nH; ++qi)
                    {
                        chunkLevels[qi] = sampledLevels[qi];
                    }

                    // Height block layout, copied from the shipped Blitz spaces byte for
                    // byte: header = magic, sizeX, sizeY, compression, version (= the
                    // HeightMapHeader version the shipped files carry), minHeight,
                    // maxHeight, padding, then the qpng tag and a PNG whose pixel is
                    // the height in millimetres (red = low byte, green = high byte).
                    int32 hMin = INT_MAX;
                    int32 hMax = -INT_MAX;
                    for (int32 qi = 0; qi < nW * nH; ++qi)
                    {
                        if (chunkLevels[qi] < hMin)
                            hMin = chunkLevels[qi];
                        if (chunkLevels[qi] > hMax)
                            hMax = chunkLevels[qi];
                    }
                    if (hMax < hMin)
                    {
                        hMax = hMin;
                    }
                    std::vector<int32> qLevels(nW * nH, 0);
                    for (int32 qi = 0; qi < nW * nH; ++qi)
                    {
                        int32 v = chunkLevels[qi];
                        if (v < 0)
                            v = 0;
                        if (v > 65535)
                            v = 65535; // millimetres stop at 65.535 m in this encoding
                        qLevels[qi] = v;
                    }

                    {
                        int32 rowsWithData = 0;
                        for (int32 ry = 0; ry < nH; ++ry)
                        {
                            for (int32 rx = 0; rx < nW; ++rx)
                            {
                                if (chunkLevels[ry * nW + rx] != 0)
                                {
                                    ++rowsWithData;
                                    break;
                                }
                            }
                        }
                        DAVA::Logger::Info("[TerrainConverter] chunk (%d,%d): %d of %d samples written, "
                                           "%d/%d rows carry data (source: %s)",
                                           xc, yc, samplesWritten, nW * nH, rowsWithData, nH,
                                           g_heightFile.valid() ? "heightmap file" : "engine sampler");
                    }

                    std::vector<uint8> pngBytes;
                    WriteBwRgbaPng(nW, nH, reinterpret_cast<const uint8*>(&qLevels[0]), pngBytes);
                    if (pngBytes.empty())
                    {
                        DAVA::Logger::Error("[TerrainConverter] heights PNG for chunk (%d,%d) could not be built", xc, yc);
                    }

                    // Create header at front of buffer
                    HeightMapHeader hmh;
                    memset(&hmh, 0, sizeof(hmh));
                    hmh.magic_ = HeightMapHeader::MAGIC;
                    hmh.compression_ = 0;
                    hmh.width_ = nW;
                    hmh.height_ = nH;
                    hmh.minHeight_ = static_cast<float32>(hMin) * 0.001f;
                    hmh.maxHeight_ = static_cast<float32>(hMax) * 0.001f;
                    hmh.version_ = HeightMapHeader::VERSION_ABS_QFLOAT;

                    // Now add the header to the front:
                    std::vector<uint8> result(pngBytes.size() + sizeof(hmh) + sizeof(uint32));
                    ::memcpy(&result[0], (void*)&(hmh), sizeof(hmh));

                    *((uint32*)&result[sizeof(hmh)]) = QUANTISED_PNG_VERSION;
                    if (!pngBytes.empty())
                    {
                        ::memcpy(&result[sizeof(hmh) + sizeof(uint32)], &pngBytes[0], pngBytes.size());
                    }

#ifdef __DAVAENGINE_DEBUG__
                    String chunkId = outsideChunkIdentifier(xc, yc);
                    chunkImage->Save(g_spacesPath + chunkId + ".png");
                    SafeRelease(chunkImage);
#endif

                    // dominant texture
                    float32 xx, yy;
                    memset(dominant, defaultColorIndex, dominantSize);
                    offset = 0;

                    for (uint8 i = 0; i < BLENDS_SZ; i++)
                    {
                        memset(blends[i], (i == 0 ? 255 : 0), dominantSize);
                    }
                    if (!g_colormapLayerRef.empty())
                    {
                        g_baseMask.assign((size_t)dominantSize, 0);
                    }

                    // The landscape box is NOT symmetric about the origin and not even
                    // square: on rudniki the custom properties read borderTopX -260.74 /
                    // borderBotX +256.64 and borderTopY -259.57 / borderBotY +261.32, so its
                    // centre is (-2.05, +0.87) and the two extents differ. Mapping the masks
                    // with "-realSize .. +realSize" (and taking the half size from X only)
                    // slides them by that centre and scales Y by the wrong span, which is
                    // exactly the shift you see against the ground. Map through the box itself,
                    // the same way GetPoint does for the heights, and the two cannot disagree.
                    const float32 spanX = Max(1.0f, box.max.x - box.min.x);
                    const float32 spanY = Max(1.0f, box.max.y - box.min.y);
                    const float32 kkX = (float32)texDataWidth / spanX;
                    const float32 kkY = (float32)texDataHeight / spanY;
                    for (int32 y = 0; y < DOM_RES; ++y)
                    {
                        // Rows follow +y, exactly like the terrain samples: Dava's uv.y = 0 is
                        // the landscape minimum, so mask row 0 belongs at the chunk's low-y
                        // edge. Matching the .dds file's own bottom-up row order instead of the
                        // world is what mirrored the layers.
                        const float32 wz = yc * 100.0f + y * 100.0f / 128.0f;
                        yy = wz;
                        if (wz > box.min.y && wz < box.max.y)
                        {
                            int32 py = (int32)((wz - box.min.y) * kkY + 0.5f);
                            py = Min(texDataHeight - 1, py);
                            for (int32 x = 0; x < DOM_RES; ++x)
                            {
                                xx = xc * 100.0f + x * 100.0f / 128.0f;
                                if (xx > box.min.x && xx < box.max.x)
                                {
                                    int32 px = (int32)((xx - box.min.x) * kkX + 0.5f);
                                    px = Min(texDataWidth - 1, px);
                                    int32 off = py * texDataWidth + px;
                                    if (py == 0 && px == 0)
                                    {
                                        // Four greyscale layer patterns packed into one texture usually means
                                        // four vertical pages, not four channels of one page. Say which one
                                        // we are looking at, because slicing it wrong is what makes the
                                        // ground tiling look wrong in BW.
                                        DAVA::Logger::Info("[TerrainConverter] blend/passability sampling grid %dx%d",
                                                           texDataWidth, texDataHeight);
                                    }

                                    // texData is map-wide now, so index it with the same px/py the tilemask
                                    // uses - reading texData[off] took the top-left corner for every chunk
                                    int32 passIndex = py * texDataWidth + px;
                                    if (passIndex < 0 || passIndex >= texDataWidth * texDataHeight)
                                    {
                                        passIndex = 0;
                                    }
                                    uint8 val = (texData[passIndex] + 1) << 4;
                                    // the tilemask can ship at a different resolution than the colour
                                    // texture, so map the texel instead of reusing its offset
                                    // Bilinear, like the Dava shader samples it. Nearest-neighbour
                                    // at 128 texels per 100 m turned every soft painted transition
                                    // into a stair-step edge that BW then bled as a hard band.
                                    const float32 tmFx = Min((float32)(tmWidth - 1), (float32)px * (float32)tmWidth / (float32)texDataWidth);
                                    const float32 tmFy = Min((float32)(tmHeight - 1), (float32)py * (float32)tmHeight / (float32)texDataHeight);
                                    const int32 tmX0 = (int32)tmFx;
                                    const int32 tmY0 = (int32)tmFy;
                                    const int32 tmX1 = Min(tmWidth - 1, tmX0 + 1);
                                    const int32 tmY1 = Min(tmHeight - 1, tmY0 + 1);
                                    const float32 tmAx = tmFx - (float32)tmX0;
                                    const float32 tmAy = tmFy - (float32)tmY0;
                                    uint8 maxVal = 0;
                                    int32 maxInd = 0;
                                    int32 channelSum = 0;
                                    int32 samples[BLENDS_SZ] = { 0, 0, 0, 0 };
                                    for (uint8 i = 0; i < BLENDS_SZ; i++)
                                    {
                                        const float32 c00 = (float32)tileMaskData[((size_t)tmY0 * tmWidth + tmX0) * 4 + i];
                                        const float32 c10 = (float32)tileMaskData[((size_t)tmY0 * tmWidth + tmX1) * 4 + i];
                                        const float32 c01 = (float32)tileMaskData[((size_t)tmY1 * tmWidth + tmX0) * 4 + i];
                                        const float32 c11 = (float32)tileMaskData[((size_t)tmY1 * tmWidth + tmX1) * 4 + i];
                                        const float32 top = c00 + (c10 - c00) * tmAx;
                                        const float32 bot = c01 + (c11 - c01) * tmAx;
                                        const uint8 v = (uint8)(top + (bot - top) * tmAy + 0.5f);
                                        samples[i] = (int32)v;
                                        if (maxVal < v)
                                        {
                                            maxVal = v;
                                            maxInd = i;
                                        }
                                        channelSum += (int32)v;
                                    }
                                    // Dava blends the four tilemask channels as NORMALISED weights
                                    // (w_i = c_i / sum) - on rudniki the raw channels sum to as much
                                    // as 324. BW treats layers 1..3 as straight weights and gives the
                                    // base layer whatever is left, so unnormalised data makes the base
                                    // negative (>255) and the ground goes black and glossy in patches.
                                    int32 normalized[BLENDS_SZ] = { 0, 0, 0, 0 };
                                    for (uint8 i = 0; i < BLENDS_SZ; i++)
                                    {
                                        int32 w = 0;
                                        if (channelSum > 0)
                                        {
                                            w = (samples[i] * 255 + channelSum / 2) / channelSum;
                                        }
                                        if (w > 255)
                                        {
                                            w = 255;
                                        }
                                        normalized[i] = w;
                                    }
                                    // All four Blitz tiles keep their own normalised weight.
                                    // The gain leaves room for the colour-map layer underneath,
                                    // which otherwise would be starved: the four masks normalise
                                    // to a sum of 255, so leftover coverage for a fifth layer is 0.
                                    int32 usedByTiles = 0;
                                    for (uint8 i = 0; i < BLENDS_SZ; i++)
                                    {
                                        int32 w = normalized[i];
                                        if (!g_colormapLayerRef.empty())
                                        {
                                            w = (int32)(w * g_tileMaskGain + 0.5f);
                                            if (w > 255)
                                            {
                                                w = 255;
                                            }
                                        }
                                        blends[i][offset] = (uint8)w;
                                        usedByTiles += w;
                                    }
                                    if (!g_colormapLayerRef.empty())
                                    {
                                        if (usedByTiles > 255)
                                        {
                                            usedByTiles = 255;
                                        }
                                        if (g_baseMask.size() > (size_t)offset)
                                        {
                                            g_baseMask[offset] = (uint8)(255 - usedByTiles);
                                        }
                                    }
                                    dominant[offset] = (val + maxInd);
                                }
                                offset++;
                            }
                        }
                    }
                    SaveDominant();
                    for (int i = 0; i < BLENDS_SZ; i++)
                    {
                        SaveBlend(i);
                    }

                    // Layer 4: the map colorTexture, projected once over the landscape, with
                    // the coverage the four tiles did not take. The block is laid out by hand
                    // because BlendHeader is declared further down the file; layout is the same
                    // as SaveBlend writes:
                    //   [magic][w][h][bpp][uProj x4][vProj x4][version][pad x3][nameLen][name][png]
                    g_baseLayerBlock.clear();
                    if (!g_colormapLayerRef.empty() && g_baseMask.size() == (size_t)dominantSize)
                    {
                        PNGImageData basePng;
                        basePng.data_ = &g_baseMask[0];
                        basePng.width_ = DOM_RES;
                        basePng.height_ = DOM_RES;
                        basePng.bpp_ = 8;
                        basePng.stride_ = DOM_RES;
                        basePng.upsideDown_ = false;
                        BinaryPtr baseComp = compressPNG(basePng);
                        if (baseComp != NULL && baseComp->size() > 0)
                        {
                            // same rule as BwLayerTextureRef (declared lower in the file):
                            // the client resolves the container itself, so no extension
                            String baseRef = g_colormapLayerRef;
                            {
                                const String::size_type dot = baseRef.find_last_of('.');
                                const String::size_type slash = baseRef.find_last_of("/\\");
                                if (dot != String::npos && (slash == String::npos || dot > slash))
                                {
                                    baseRef = baseRef.substr(0, dot);
                                }
                            }
                            const size_t total = 68 + baseRef.length() + baseComp->size();
                            g_baseLayerBlock.assign(total, 0);
                            uint8* addr = &g_baseLayerBlock[0];
                            uint32* hdr = (uint32*)addr;
                            hdr[0] = 0x00646c62; // 'bld\0'
                            hdr[1] = (uint32)DOM_RES;
                            hdr[2] = (uint32)DOM_RES;
                            hdr[3] = 8;
                            float* uProj = (float*)(addr + 16);
                            uProj[0] = 1.0f / Max(1.0f, box.max.x - box.min.x);
                            float* vProj = (float*)(addr + 32);
                            vProj[2] = 1.0f / Max(1.0f, box.max.y - box.min.y);
                            hdr[12] = 2; // VERSION_PNG_BLENDS
                            uint8* nameAt = addr + 64;
                            uint32* lenAt = (uint32*)nameAt;
                            *lenAt = (uint32)baseRef.length();
                            ::memcpy(nameAt + 4, baseRef.c_str(), baseRef.length());
                            ::memcpy(nameAt + 4 + baseRef.length(), &(*baseComp)[0], baseComp->size());
                        }
                    }

                    // ---- OSE bake pack: build the sections the BW 14.4
                    // client requires before the block resource is packed ----
                    // heights mips 1..5: BW lod>0 blocks carry no border
                    // (visibleOffset == 0), so each block is
                    // (heightMapSize >> lod) + 1 samples wide - sampled from
                    // the lod0 grid at stride 2^lod; +2 skips the lod0 border.
                    {
                        for (int32 lod = 1; lod <= 5; ++lod)
                        {
                            const int32 nw = (chunkSize >> lod) + 1;
                            const int32 nh = nw;
                            const int32 stride = (1 << lod);
                            std::vector<int32> next(nw * nh, 0);
                            int32 mn = INT_MAX;
                            int32 mx = -INT_MAX;
                            for (int32 y = 0; y < nh; ++y)
                            {
                                for (int32 x = 0; x < nw; ++x)
                                {
                                    const int32 sx = x * stride + 2;
                                    const int32 sy = y * stride + 2;
                                    const int32 v = chunkLevels[sy * nW + sx];
                                    next[y * nw + x] = v;
                                    if (v < mn)
                                        mn = v;
                                    if (v > mx)
                                        mx = v;
                                }
                            }
                            std::vector<int32> mipLevels(nw * nh, 0);
                            for (int32 qi = 0; qi < nw * nh; ++qi)
                            {
                                int32 v = next[qi];
                                if (v < 0)
                                    v = 0;
                                if (v > 65535)
                                    v = 65535;
                                mipLevels[qi] = v;
                            }
                            std::vector<uint8> mipBytes;
                            WriteBwRgbaPng(nw, nh, reinterpret_cast<const uint8*>(&mipLevels[0]), mipBytes);
                            if (!mipBytes.empty())
                            {
                                std::vector<uint8>& block = g_heightMips[lod - 1];
                                block.clear();
                                block.resize(sizeof(HeightMapHeader) + sizeof(uint32) + mipBytes.size(), 0);
                                HeightMapHeader hmh;
                                memset(&hmh, 0, sizeof(hmh));
                                hmh.magic_ = HeightMapHeader::MAGIC;
                                hmh.compression_ = 0;
                                hmh.width_ = nw;
                                hmh.height_ = nh;
                                hmh.minHeight_ = static_cast<float32>(mn) * 0.001f;
                                hmh.maxHeight_ = static_cast<float32>(mx) * 0.001f;
                                hmh.version_ = HeightMapHeader::VERSION_ABS_QFLOAT;
                                memcpy(&block[0], &hmh, sizeof(hmh));
                                const uint32 qpngVersion = QUANTISED_PNG_VERSION;
                                memcpy(&block[sizeof(hmh)], &qpngVersion, sizeof(qpngVersion));
                                memcpy(&block[sizeof(hmh) + sizeof(qpngVersion)], &mipBytes[0], mipBytes.size());
                            }
                        }
                    }

                    // normals + lodNormals from the lod0 height grid
                    {
                        const int32 dim = g_normalMapSize; // must equal space.settings normalMapSize - BW samples by the declared size
                        const float32 gridStep = 100.0f / static_cast<float32>(chunkSize);
                        std::vector<uint16> normalData(dim * dim);
                        auto sampleHeight = [&](float32 lx, float32 lz) -> float32
                        {
                            // Sample the landscape itself, not just this chunk's grid. Clamping
                            // at the chunk border makes the edge normals one-sided while the
                            // neighbour computes the other side of the same slope, and BW draws
                            // that disagreement as a hard dark line down every chunk seam.
                            return HeightAtWorld(static_cast<float32>(xc) * 100.0f + lx,
                                                 static_cast<float32>(yc) * 100.0f + lz, box);
                        };
                        const float32 texel = 100.0f / static_cast<float32>(dim - 1);
                        for (int32 z = 0; z < dim; ++z)
                        {
                            for (int32 x = 0; x < dim; ++x)
                            {
                                const float32 wx = static_cast<float32>(x) * texel;
                                const float32 wz = static_cast<float32>(z) * texel;
                                const float32 hL = sampleHeight(wx - texel, wz);
                                const float32 hR = sampleHeight(wx + texel, wz);
                                const float32 hU = sampleHeight(wx, wz - texel);
                                const float32 hD = sampleHeight(wx, wz + texel);
                                float32 nx = -(hR - hL) / (2.0f * texel);
                                float32 nz = -(hD - hU) / (2.0f * texel);
                                const float32 invLen = 1.0f / sqrtf(nx * nx + nz * nz + 1.0f);
                                nx *= invLen;
                                nz *= invLen;
                                uint32 bx = static_cast<uint32>((nx + 1.0f) * 127.95f + 0.5f);
                                uint32 bz = static_cast<uint32>((nz + 1.0f) * 127.95f + 0.5f);
                                if (bx > 255)
                                    bx = 255;
                                if (bz > 255)
                                    bz = 255;
                                normalData[z * dim + x] = static_cast<uint16>((bx << 8) | bz);
                            }
                        }
                        PNGImageData normalPng;
                        normalPng.data_ = reinterpret_cast<uint8*>(&normalData[0]);
                        normalPng.width_ = dim;
                        normalPng.height_ = dim;
                        normalPng.bpp_ = 16;
                        normalPng.stride_ = dim * sizeof(uint16);
                        normalPng.upsideDown_ = false;
                        BinaryPtr normalPngData = compressPNG(normalPng);
                        if (normalPngData != NULL && normalPngData->size() > 0)
                        {
                            g_normalsBlock.clear();
                            g_normalsBlock.resize(16 + normalPngData->size(), 0);
                            uint32* hdr = reinterpret_cast<uint32*>(&g_normalsBlock[0]);
                            hdr[0] = OSE_NORMAL_MAGIC;
                            hdr[1] = 1; // VERSION_16_BIT_PNG
                            hdr[2] = 0;
                            hdr[3] = 0;
                            memcpy(&g_normalsBlock[16], &(*normalPngData)[0], normalPngData->size());
                        }
                    }

                    // horizonShadows. The block is REQUIRED - BW fails the whole terrain block
                    // with "No horizon shadows found in terrain chunk" when it is missing - and
                    // it is nested: a "piz" compression wrapper around the shadow payload.
                    // Decompressing a shipped block settles the layout (1056 bytes for 16x16):
                    //   [ 32 bytes: 'shd\0', width, height, bpp = 32, version = 1, pad[3] ]
                    //   [ width * height * 4 bytes of texels, 0xFF.. = open sky ]
                    // Both earlier attempts were wrong in a way the assert reports as a bad
                    // magic: one wrote the header uncompressed, one wrote the bare grid, and
                    // the client decompresses first and then checks pHeader->magic_.
                    if (g_writeHorizonShadows)
                    {
                        const int32 dim = g_shadowMapSize;
                        const int32 texelBytes = 4;
                        std::vector<uint8> payload(32 + (size_t)dim * dim * texelBytes, 0xFF);
                        uint32* hdr = reinterpret_cast<uint32*>(&payload[0]);
                        hdr[0] = OSE_SHADOW_MAGIC; // 'shd\0'
                        hdr[1] = (uint32)dim; // width
                        hdr[2] = (uint32)dim; // height
                        hdr[3] = (uint32)(texelBytes * 8); // bpp = 32
                        hdr[4] = 1; // version
                        hdr[5] = 0;
                        hdr[6] = 0;
                        hdr[7] = 0;
                        // header must stay intact, only the texel area is "open sky"
                        for (int32 i = 0; i < 32; ++i)
                        {
                            payload[i] = 0;
                        }
                        hdr[0] = OSE_SHADOW_MAGIC;
                        hdr[1] = (uint32)dim;
                        hdr[2] = (uint32)dim;
                        hdr[3] = (uint32)(texelBytes * 8);
                        hdr[4] = 1;

                        std::vector<uint8> compressed = BWZlibCompress(&payload[0], (uint32)payload.size());
                        if (!compressed.empty())
                        {
                            g_shadowBlock.clear();
                            g_shadowBlock.assign(compressed.begin(), compressed.end());
                            DAVA::Logger::Info("[TerrainConverter] horizonShadows: %d x %d, payload %u bytes, "
                                               "block %u bytes, head %02x %02x %02x %02x",
                                               dim, dim, (unsigned)payload.size(), (unsigned)compressed.size(),
                                               compressed[0], compressed[1], compressed[2], compressed[3]);
                        }
                        else
                        {
                            DAVA::Logger::Error("[TerrainConverter] horizonShadows could not be compressed");
                        }
                    }

                    // ambientOcclusion: all-255 8bpp PNG = no occlusion
                    {
                        const int32 dim = g_aoMapSize;
                        std::vector<uint8> aoPixels(dim * dim, 255);
                        PNGImageData aoPng;
                        aoPng.data_ = &aoPixels[0];
                        aoPng.width_ = dim;
                        aoPng.height_ = dim;
                        aoPng.bpp_ = 8;
                        aoPng.stride_ = dim;
                        aoPng.upsideDown_ = false;
                        BinaryPtr aoPngData = compressPNG(aoPng);
                        if (aoPngData != NULL && aoPngData->size() > 0)
                        {
                            g_aoBlock.clear();
                            g_aoBlock.resize(16 + aoPngData->size(), 0);
                            uint32* hdr = reinterpret_cast<uint32*>(&g_aoBlock[0]);
                            hdr[0] = OSE_AO_MAGIC;
                            hdr[1] = 1; // VERSION_8_BIT_PNG
                            hdr[2] = 0;
                            hdr[3] = 0;
                            memcpy(&g_aoBlock[16], &(*aoPngData)[0], aoPngData->size());
                        }
                    }

                    // lodTexture.dds: uncompressed RGBA DDS from the custom color
                    // texture (RGB) with flat specular in alpha
                    {
                        const int32 dim = 128; // lodMapSize
                        g_lodTextureBlock.clear();
                        g_lodTextureBlock.resize(4 + 124 + dim * dim * 4, 0);
                        uint8* dds = &g_lodTextureBlock[0];
                        memcpy(dds, &DDS_MAGIC, 4);
                        uint32* hdr = reinterpret_cast<uint32*>(dds + 4);
                        hdr[0] = 124; // dwSize
                        hdr[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000; // caps|height|width|pitch|pixelformat
                        hdr[2] = dim; // height
                        hdr[3] = dim; // width
                        hdr[4] = dim * 4; // pitch
                        hdr[18] = 32; // pf size
                        hdr[19] = 0x1 | 0x40; // pf flags: alphapixels | rgb
                        hdr[20] = 0; // pf fourCC: 0, this is an RGB layout
                        hdr[21] = 32; // bit count
                        hdr[22] = 0x00ff0000; // R mask
                        hdr[23] = 0x0000ff00; // G mask
                        hdr[24] = 0x000000ff; // B mask
                        hdr[25] = 0xff000000; // A mask
                        hdr[26] = 0x1000; // caps: texture
                        uint8* pixels = dds + 4 + 124;
                        // Same box-exact mapping as the blend masks above - the lod texture and
                        // the layers have to land on the same ground.
                        const float32 lodSpanX = Max(1.0f, box.max.x - box.min.x);
                        const float32 lodSpanY = Max(1.0f, box.max.y - box.min.y);
                        const float32 kk = (g_colorImageWidth > 0) ? (static_cast<float32>(texDataWidth) / lodSpanX) : 0.0f;
                        for (int32 y = 0; y < dim; ++y)
                        {
                            const float32 yy = yc * 100.0f + static_cast<float32>(y) * 100.0f / static_cast<float32>(dim);
                            for (int32 x = 0; x < dim; ++x)
                            {
                                const float32 xx = xc * 100.0f + static_cast<float32>(x) * 100.0f / static_cast<float32>(dim);
                                uint8* px = pixels + (y * dim + x) * 4;
                                px[0] = 128;
                                px[1] = 128;
                                px[2] = 128;
                                px[3] = 255;
                                if (g_colormapImage != nullptr && g_colormapWidth > 0 && xx > box.min.x && xx < box.max.x && yy > box.min.y && yy < box.max.y)
                                {
                                    // real macro ground colour: the map-wide colorTexture
                                    const float32 cmkX = static_cast<float32>(g_colormapWidth) / lodSpanX;
                                    const float32 cmkY = static_cast<float32>(g_colormapHeight) / lodSpanY;
                                    int32 cmx = static_cast<int32>((xx - box.min.x) * cmkX + 0.5f);
                                    int32 cmy = static_cast<int32>((yy - box.min.y) * cmkY + 0.5f);
                                    if (cmx > g_colormapWidth - 1)
                                        cmx = g_colormapWidth - 1;
                                    if (cmy > g_colormapHeight - 1)
                                        cmy = g_colormapHeight - 1;
                                    if (cmx < 0)
                                        cmx = 0;
                                    if (cmy < 0)
                                        cmy = 0;
                                    const uint8* cs = g_colormapImage->GetData() + ((size_t)cmy * g_colormapWidth + cmx) * 4;
                                    px[0] = cs[2]; // B8G8R8A8 buffer: DAVA RGBA -> B,G,R
                                    px[1] = cs[1];
                                    px[2] = cs[0];
                                    px[3] = 255;
                                }
                                else if (kk > 0.0f && xx > box.min.x && xx < box.max.x && yy > box.min.y && yy < box.max.y)
                                {
                                    int32 pxs = static_cast<int32>((xx - box.min.x) * kk + 0.5f);
                                    int32 pys = static_cast<int32>((yy - box.min.y) * (static_cast<float32>(texDataHeight) / lodSpanY) + 0.5f);
                                    if (pxs > texDataWidth - 1)
                                        pxs = texDataWidth - 1;
                                    if (pys > texDataHeight - 1)
                                        pys = texDataHeight - 1;
                                    if (pxs < 0)
                                        pxs = 0;
                                    if (pys < 0)
                                        pys = 0;
                                    const uint8* src = &g_colorImage[(pys * g_colorImageWidth + pxs) * 4];
                                    px[0] = src[2];
                                    px[1] = src[1];
                                    px[2] = src[0];
                                    px[3] = 255; // flat specular in alpha
                                }
                            }
                        }
                    }
                    // ---- end of OSE bake pack ----

                    String identifier = outsideChunkIdentifier(xc, yc);
                    if (chunkTerrainValid)
                    {
                        compressResult(g_spacesPath + identifier + ".cdata", (BinaryPtr)&result);
                    }
                    File* f = File::Create(g_spacesPath + identifier + ".chunk", File::CREATE | File::WRITE);
                    if (!chunkTerrainValid)
                    {
                        // BW loads a chunk with no <terrain>; a dead flat plate here would
                        // also hide the VLOs placed on this chunk
                        if (f)
                        {
                            const char* emptyChunk = "<root>\r\n</root>\r\n";
                            f->Write(emptyChunk, (uint32)strlen(emptyChunk));
                            SafeRelease(f);
                        }
                        continue;
                    }
                    if (f == nullptr)
                    {
                        continue;
                    }
                    Snprintf(content, sizeof(content),
                             "<root>\r\n\t<terrain>\r\n\t\t<editorOnly>\r\n\t\t\t<hidden>\tfalse\t</hidden>\r\n\t\t\t<frozen>\tfalse\t</frozen>\r\n\t\t</editorOnly>\r\n\t\t<visibilityMask>\t4294967295\t</visibilityMask>\r\n\t\t<metaData>\t</metaData>\r\n\t\t<resource>\t%s/terrain2\t</resource>\r\n\t</terrain>\r\n</root>\r\n",
                             (identifier + ".cdata").c_str());
                    int32 res = f->Write(content, (uint32)strlen(content));
                    SafeRelease(f);
                }
            }

#ifdef __DAVAENGINE_DEBUG__
            spaceImage->Save(g_spacesPath + "space.png");
            SafeRelease(spaceImage);
#endif

            SafeRelease(heightMap);
            SafeDelete(texData);
            SafeDelete(terrain);
        }
        else
        {
            DAVA::Logger::Error("Error at PrepareIndexTexture()");
        }
    }
    else
    {
        DAVA::Logger::Error("Can't load tilemask texture or heightmap texture");
    }
} // Converter::DoConvertHM

void Converter::archiveFile(String fileName, FilePath filePath, zipFile& zf)
{
    int32 bufferSize = 8192;
    char* buffer = new char[bufferSize];
    int32 readed = -1;
    File* fin = File::Create(filePath, File::OPEN | File::READ);

    zip_fileinfo zi = {};
    zi.dosDate = 0;
    zi.internal_fa = 0;
    zi.external_fa = 0;

    int32 err = zipOpenNewFileInZip(zf, fileName.c_str(), &zi, NULL, 0, NULL, 0, NULL, Z_DEFLATED, 6);
    if (err == ZIP_OK)
    {
        while (readed != 0)
        {
            readed = fin->Read(buffer, bufferSize);
            if (readed > 0)
            {
                err = zipWriteInFileInZip(zf, buffer, readed);
            }
        }
        err = zipCloseFileInZip(zf);
    }
    SafeRelease(fin);
    delete[] buffer;
}

void Converter::browseDir(String dirName, zipFile& archive)
{
    FileList* list = new FileList(dirName);
    int32 listSize = list->GetCount();
    for (int32 i = 0; i < listSize; ++i)
    {
        if (!list->IsNavigationDirectory(i) && !list->IsDirectory(i))
        {
            String path = dirName + list->GetFilename(i);
            path.erase(0, 9);
            archiveFile(path, list->GetPathname(i), archive);
        }
        if (!list->IsNavigationDirectory(i) && list->IsDirectory(i))
        {
            browseDir(dirName + list->GetFilename(i) + "\\", archive);
        }
    }

    list->Release();
}

void Converter::chunkIndexesFromChunkID(int chunkID, int& chunkX, int& chunkZ)
{
    chunkX = (chunkID >> 8) - 127;
    chunkZ = (chunkID & 0xFF) - 127;
}

uint32 Converter::chunkIDFromChunkIndexes(int chunkX, int chunkZ)
{
    return ((chunkX + 127) << 8) | (chunkZ + 127);
}

String Converter::outsideChunkIdentifier(int32 gridX, int32 gridZ)
{
    char chunkIdentifierCStr[32];
    String gridChunkIdentifier;

    uint16 gridxs = uint16(gridX), gridzs = uint16(gridZ);
    Snprintf(chunkIdentifierCStr, sizeof(chunkIdentifierCStr), "%04x%04xo", int(gridxs), int(gridzs));
    gridChunkIdentifier += chunkIdentifierCStr;
    return gridChunkIdentifier;
}

constexpr int WRITEBUFFERSIZE = 16384; // VCR101: was a macro
constexpr int MAXFILENAME = 256; // VCR101: was a macro

static void addFile2Zip(zipFile& zf, zip_fileinfo& zi, char* fileName, BinaryPtr ptr)
{
    int32 err = zipOpenNewFileInZip(zf, fileName, &zi, NULL, 0, NULL, 0, NULL, Z_DEFLATED, 6);
    if (err == ZIP_OK)
    {
        err = zipWriteInFileInZip(zf, &(ptr->front()), (uint32)ptr->size());
        if (err < 0)
        {
            printf("error in writing in the zipfile\n");
        }
    }
    err = zipCloseFileInZip(zf);
    if (err != ZIP_OK)
    {
        printf("error in closing in the zipfile\n");
    }
}

// BW 2.0.1 looks the terrain sections up with forward slashes, the Blitz builds ship
// them with backslashes. Writing both spellings costs a few KB per block and makes the
// same cdata readable by either engine instead of silently loading an empty flat block.
// Writing every section under two spellings doubles the entry count in any archive
// viewer and confused enough people that it is opt-in now.
static bool g_writeBothSectionNames = false;

static void addSection2Zip(zipFile& zf, zip_fileinfo& zi, const char* name, BinaryPtr ptr)
{
    char fwd[128];
    char bwd[128];
    ::snprintf(fwd, sizeof(fwd), "%s", name);
    ::snprintf(bwd, sizeof(bwd), "%s", name);
    for (char* c = bwd; *c != 0; ++c)
    {
        if (*c == '/')
        {
            *c = '\\';
        }
    }
    addFile2Zip(zf, zi, fwd, ptr);
    if (g_writeBothSectionNames)
    {
        addFile2Zip(zf, zi, bwd, ptr);
    }
}

void Converter::compressResult(String archiveName, BinaryPtr hMap)
{
    zipFile zf;
    int errclose;
    //	zf = zipOpen(archiveName.c_str(), APPEND_STATUS_ADDINZIP);
    zf = zipOpen(archiveName.c_str(), 0);
    zip_fileinfo zi = {};
    zi.dosDate = 0;
    zi.internal_fa = 0;
    zi.external_fa = 0;

    addSection2Zip(zf, zi, (char*)"terrain2/heights", hMap);
    addSection2Zip(zf, zi, (char*)"terrain2/dominantTextures", (BinaryPtr)&dominantBlock);
    // Shipped Blitz cdata carries "terrain2\layer 1" .. "layer 3" and no base
    // "terrain2\layer": channel 0 of the tilemask is the implicit base layer, so
    // writing it as layer 0 both invents a section the client does not expect and
    // shifts the meaning of the three real masks.
    if (!blendBlock[0].empty())
    {
        addSection2Zip(zf, zi, (char*)"terrain2/layer 0", (BinaryPtr)&blendBlock[0]);
    }
    if (!blendBlock[1].empty())
    {
        addSection2Zip(zf, zi, (char*)"terrain2/layer 1", (BinaryPtr)&blendBlock[1]);
    }
    if (!blendBlock[2].empty())
    {
        addSection2Zip(zf, zi, (char*)"terrain2/layer 2", (BinaryPtr)&blendBlock[2]);
    }
    if (!blendBlock[3].empty())
    {
        addSection2Zip(zf, zi, (char*)"terrain2/layer 3", (BinaryPtr)&blendBlock[3]); // alpha channel of the tilemask
    }
    if (!g_baseLayerBlock.empty())
    {
        // fifth layer: the map colorTexture, one repeat across the landscape
        addSection2Zip(zf, zi, (char*)"terrain2/layer 4", (BinaryPtr)&g_baseLayerBlock);
    }

    // OSE bake pack: sections the BW 14.4 runtime client requires
    {
        // BW writes this next to every block; ForcedLod -1 = automatic. BinaryBlock
        // here only has a default constructor, so fill it the way the png write
        // callbacks in this file do.
        static const char* terrainMeta = "<TerrainBlockMetaData>\r\n\t<ForcedLod>\t-1\t</ForcedLod>\r\n</TerrainBlockMetaData>\r\n";
        const uint32 metaLen = (uint32)strlen(terrainMeta);
        BinaryPtr metaBlock = new BinaryBlock();
        if (metaLen > 0)
        {
            metaBlock->resize(metaLen);
            memcpy(&(metaBlock->operator[](0)), terrainMeta, metaLen);
        }
        addSection2Zip(zf, zi, (char*)"terrain2/TerrainBlockMetaData", metaBlock);
    }
    addSection2Zip(zf, zi, (char*)"terrain2/heights1", (BinaryPtr)&g_heightMips[0]);
    addSection2Zip(zf, zi, (char*)"terrain2/heights2", (BinaryPtr)&g_heightMips[1]);
    addSection2Zip(zf, zi, (char*)"terrain2/heights3", (BinaryPtr)&g_heightMips[2]);
    addSection2Zip(zf, zi, (char*)"terrain2/heights4", (BinaryPtr)&g_heightMips[3]);
    addSection2Zip(zf, zi, (char*)"terrain2/heights5", (BinaryPtr)&g_heightMips[4]);
    if (!g_normalsBlock.empty())
    {
        addSection2Zip(zf, zi, (char*)"terrain2/normals", (BinaryPtr)&g_normalsBlock);
        addSection2Zip(zf, zi, (char*)"terrain2/lodNormals", (BinaryPtr)&g_normalsBlock);
    }
    if (!g_shadowBlock.empty())
    {
        addSection2Zip(zf, zi, (char*)"terrain2/horizonShadows", (BinaryPtr)&g_shadowBlock);
    }
    if (!g_aoBlock.empty())
    {
        addSection2Zip(zf, zi, (char*)"terrain2/ambientOcclusion", (BinaryPtr)&g_aoBlock);
    }

    // BW 2.x chunk cdats carry these two, and its TerrainBlock2 asks for them: "dirtyFlags"
    // is 12 bytes (0, 1, 1) in a shipped block, "navmeshDirty" is a single byte. They are
    // cheap and their absence is another "block not found" path.
    {
        // BinaryBlock is a std::vector<uint8>: it has no (const void*, size) constructor,
        // so build it empty and fill it the same way the metadata block above does.
        static const uint32 dirtyFlags[3] = { 0, 1, 1 };
        BinaryPtr dirtyBlock = new BinaryBlock();
        dirtyBlock->resize((uint32)sizeof(dirtyFlags));
        memcpy(&(dirtyBlock->operator[](0)), dirtyFlags, sizeof(dirtyFlags));
        addSection2Zip(zf, zi, (char*)"terrain2/dirtyFlags", dirtyBlock);

        BinaryPtr navBlock = new BinaryBlock();
        navBlock->resize(1);
        (navBlock->operator[](0)) = 1;
        addSection2Zip(zf, zi, (char*)"terrain2/navmeshDirty", navBlock);
    }
    if (!g_lodTextureBlock.empty())
    {
        addSection2Zip(zf, zi, (char*)"terrain2/lodTexture.dds", (BinaryPtr)&g_lodTextureBlock);
    }

    browseDir("~res:/template\\", zf);

    errclose = zipClose(zf, NULL);
    if (errclose != ZIP_OK)
    {
        printf("error in closing\n");
    }
}

namespace ConverterDetail
{
/**
 *	This is used when reading from an in-memory PNG buffer to make sure that
 *	we don't overrun the end.
 */
struct PNGReadBuffer
{
    uint8* begin_;
    uint8* current_;
    uint8* end_;
};

/**
 *	This is a libpng callback for writing out some compressed data.
 *
 *  @param pngPtr		Information about the PNG being compressed.
 *  @param bytes		A pointer to some compressed data.
 *  @param size			The size of the compressed data.
 */
static void PNGAPI pngWriteBuffer(
png_structp pngPtr,
png_bytep bytes,
png_size_t size)
{
    //	std::vector<BinaryPtr> *buffers = (std::vector<BinaryPtr> *)pngPtr->io_ptr;
    std::vector<BinaryPtr>* buffers = (std::vector<BinaryPtr>*)png_get_io_ptr(pngPtr);
    BinaryPtr buffer = new BinaryBlock();
    buffer->resize(size);

    uint8* fr = &(buffer->operator[](0));
    ::memcpy((void*)fr, bytes, size);
    buffers->push_back(buffer);
}

/**
 *	This is a libpng callback to signify the end of writing compressed
 *  data.  We do nothing, but we still need this function to use the
 *	user call-backs in libpng.
 */
static void PNGAPI pngWriteFlush(png_structp /*pngPtr*/)
{
}

/**
 *	This concatinates a collection of BinaryBlocks into one big
 *	BinaryBlock.  We do this because when compressing to a PNG the
 *	call-backs give back multiple small buffers.
 *
 *  @param buffers		A list of buffers to concatinate.
 *	@param resCntStr	A string for resource tracking.
 *  @returns			The concatinated buffers.
 */
static BinaryPtr concatenate(
std::vector<BinaryPtr> const& buffers)
{
    uint32 totalSize = 0;
    for (size_t i = 0; i < buffers.size(); ++i)
    {
        totalSize += (uint32)buffers[i]->size();
    }

    if (totalSize == 0)
    {
        return NULL;
    }

    BinaryPtr result = new BinaryBlock();
    result->resize(totalSize);
    uint8* data = (uint8*)&(result->operator[](0));
    uint8* p = data;
    for (size_t i = 0; i < buffers.size(); ++i)
    {
        ::memcpy(p, (void*)&(buffers[i]->operator[](0)), buffers[i]->size());
        p += buffers[i]->size();
    }
    return result;
}

/**
 *	This returns true if x is a power of two.
 *
 *  @param x			The number to test.
 *  @returns			True if the number is a power of two, false
 *						otherwise.  Zero is not a power of two.
 */
static bool isPower2(uint32 x)
{
    while (x != 0)
    {
        if (((x & 1) != 0) && ((x & ~(uint32)1) == 0))
        {
            return true;
        }
        else if ((x & 1) != 0)
        {
            return false;
        }
        x = x >> 1;
    }
    return false;
}

/**
 *  Error callback for png_error().  We have to throw an exception here because
 *  you aren't allowed to return from a libpng error function (i.e. expect a
 *  segfault very shortly if you attempt to continue execution after this).
 */
static void onPNGError(png_structp png_ptr, png_const_charp err_msg)
{
    //	DAVA::Logger::Error( "Got libpng error: %s\n", err_msg );
    throw int(0);
}

/**
 *  Warning callback for png_warning().
 */
static void onPNGWarning(png_structp png_ptr, png_const_charp err_msg)
{
    //	WARNING_MSG( "Got libpng warning: %s\n", err_msg );
    throw int(1);
}
} // namespace ConverterDetail

PNGImageData::PNGImageData()
    : data_(NULL)
    , width_(0)
    , height_(0)
    , bpp_(0)
    , stride_(0)
    , upsideDown_(false)
{
}

BinaryPtr Converter::compressImage(uint8* image)
{
    PNGImageData pngData;
    pngData.data_ = image;
    pngData.width_ = DOM_RES;
    pngData.height_ = DOM_RES;
    pngData.bpp_ = 8;
    pngData.stride_ = DOM_RES;
    pngData.upsideDown_ = false;
    return compressPNG(pngData);
}

BinaryPtr Converter::compressPNG(PNGImageData const& data)
{
    if (data.data_ == NULL || data.width_ == 0 || data.height_ == 0)
    {
        return NULL;
    }

    DVASSERT(ConverterDetail::isPower2(data.bpp_) && data.bpp_ <= 32);

    // Create the write and info structs:
    png_structp pngPtr = png_create_write_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
    if (pngPtr == NULL)
    {
        return NULL;
    }
    png_infop pngInfo = png_create_info_struct(pngPtr);
    if (pngInfo == NULL)
    {
        png_destroy_write_struct(&pngPtr, NULL);
        return NULL;
    }

    // Setup writing callbacks:
    std::vector<BinaryPtr> outputBuffers;
    png_set_write_fn(pngPtr, &outputBuffers, ConverterDetail::pngWriteBuffer, ConverterDetail::pngWriteFlush);

    // Set error and warning callbacks
    png_voidp errorPtr = png_get_error_ptr(pngPtr);
    png_set_error_fn(pngPtr, errorPtr, ConverterDetail::onPNGError, ConverterDetail::onPNGWarning);

    // Let libpng know about the image format:
    int colourType = PNG_COLOR_TYPE_GRAY;
    int bpp = data.bpp_;
    if (bpp == 32)
    {
        colourType = PNG_COLOR_TYPE_RGBA;
        bpp = 8; // per channel
    }
    png_set_IHDR(
    pngPtr,
    pngInfo,
    data.width_, data.height_, bpp,
    colourType,
    PNG_INTERLACE_NONE,
    PNG_COMPRESSION_TYPE_DEFAULT,
    PNG_FILTER_TYPE_DEFAULT);
    png_set_pHYs(
    pngPtr,
    pngInfo,
    data.width_,
    data.height_,
    PNG_RESOLUTION_UNKNOWN);

    // Set a compression level of three.  This gives reasonable compression
    // levels with good compression and decompression times.
    png_set_compression_level(pngPtr, 3);

    // Create the row pointers:
    png_byte** rowPointers = new png_byte*[data.height_];
    for (uint32 y = 0; y < data.height_; ++y)
    {
        if (data.upsideDown_)
        {
            rowPointers[y] = data.data_ + data.stride_ * (data.height_ - y - 1);
        }
        else
        {
            rowPointers[y] = data.data_ + data.stride_ * y;
        }
    }

    // Write the image:
    png_write_info(pngPtr, pngInfo);
    png_write_image(pngPtr, rowPointers);
    png_write_end(pngPtr, pngInfo);

    // Generate the result:
    BinaryPtr result = ConverterDetail::concatenate(outputBuffers);

    // Cleanup:
    png_destroy_write_struct(&pngPtr, &pngInfo);
    delete[] rowPointers;
    rowPointers = NULL;

    return result;
}

struct DominantTextureMapHeader
{
    static const uint32 VERSION_ZIP = 1;
    DAVA::uint32 magic_;
    DAVA::uint32 version_;
    DAVA::uint32 numTextures_;
    DAVA::uint32 textureNameSize_;
    DAVA::uint32 width_;
    DAVA::uint32 height_;
    DAVA::uint32 pad_[2];
    static const uint32 MAGIC = 0x0074616d; //"mat"
};

struct BlendHeader
{
    enum BHMVersions
    {
        VERSION_RAW_BLENDS = 1,
        VERSION_PNG_BLENDS = 2
    };

    DAVA::uint32 magic_; // Should be "bld\0"
    DAVA::uint32 width_;
    DAVA::uint32 height_;
    DAVA::uint32 bpp_; // Reserved for future use.
    DAVA::Vector4 uProjection_;
    DAVA::Vector4 vProjection_;
    DAVA::uint32 version_; // format version
    DAVA::uint32 pad_[3];
    // static const uint32 MAGIC = '\0dlb';
    static const uint32 MAGIC = 0x00646c62;
};

// Shipped BW/Blitz cdata stores layer texture paths WITHOUT the container
// extension ("maps/landscape/00_AllTiles/Road", "helpers/maps/aid_builder.tga"
// being the one TGA exception); the client resolves the .dds/.tga itself.
static DAVA::String BwLayerTextureRef(const DAVA::String& in)
{
    const size_t dot = in.find_last_of('.');
    const size_t slash = in.find_last_of("/\\");
    if (dot != DAVA::String::npos && (slash == DAVA::String::npos || dot > slash))
    {
        return in.substr(0, dot);
    }
    return in;
}

bool Converter::SaveDominant(void)
{
    // calculate data size from the num of textures and image size.
    uint32 numTextures = (uint32)textureNames.size();
    std::vector<String> textureRefs;
    textureRefs.reserve(textureNames.size());
    uint32 textureNameSize = 0;
    for (uint32 i = 0; i < numTextures; ++i)
    {
        textureRefs.push_back(BwLayerTextureRef(textureNames[i]));
        uint32 nameLength = (uint32)textureRefs.back().length();
        if (nameLength > textureNameSize)
        {
            textureNameSize = nameLength;
        }
    }
    uint32 textureDataSize = textureNameSize * numTextures;

    // create header
    std::vector<uint8> hdrData(sizeof(DominantTextureMapHeader) + textureDataSize, 0);
    DominantTextureMapHeader* pHeader = reinterpret_cast<DominantTextureMapHeader*>(&hdrData.front());

    pHeader->magic_ = DominantTextureMapHeader::MAGIC;
    pHeader->version_ = DominantTextureMapHeader::VERSION_ZIP;

    // save texture names
    pHeader->numTextures_ = numTextures;
    pHeader->textureNameSize_ = textureNameSize;
    char* p = (char*)(pHeader + 1);
    for (uint32 i = 0; i < numTextures; ++i)
    {
        strncpy(p, textureRefs[i].c_str(), textureNameSize);
        p += textureNameSize;
    }

    // save image and compress it
    pHeader->width_ = DOM_RES;
    pHeader->height_ = DOM_RES;

    //  std::vector<uint8> imgData( DOM_RES * DOM_RES );
    //  memcpy(&imgData.front() dominant
    //  image().copyTo(  );

    CompressDominant();

    // Write the header and image data out to the data section
    dominantBlock.resize(hdrData.size() + imageBlock.size());
    memcpy(&dominantBlock.front(), &hdrData.front(), hdrData.size());
    memcpy(&dominantBlock.front() + hdrData.size(), &imageBlock.front(), imageBlock.size());
    return true;
}

void Converter::CompressDominant(int level)
{
    // Sanity check on the compression level:
    if (level < RAW_COMPRESSION)
    {
        level = RAW_COMPRESSION;
    }
    if (level > BEST_COMPRESSION)
    {
        level = BEST_COMPRESSION;
    }

    // We don't know how well the data compresses.  ZLib can give us an upper
    // bound though.  We compress into a buffer of this size and then copy to
    // a smaller buffer below:
    uLongf compressedSz = compressBound((uint32)dominantSize);
    uint8* largeBuffer = new uint8[compressedSz];
    int result =
    compress2(
    largeBuffer,
    &compressedSz,
    (const Byte*)dominant,
    (uint32)dominantSize,
    level);
    //	MF_ASSERT(result == Z_OK);
    // compressedSz has been set to the size actually needed.

    // Now copy to a smaller buffer.  The format of this buffer is:
    //	COMPRESSED_MAGIC1		(uint32)
    //	COMPRESSED_MAGIC2		(uint32)
    //	decompressed size		(uint32)
    //	zipped data
    uint32 realBufSz = compressedSz + COMPRESSED_HEADER_SZ * sizeof(uint32);
    imageBlock.resize(realBufSz);

    uint32* magicAndSz = (uint32*)&imageBlock[0];
    *magicAndSz++ = COMPRESSED_MAGIC1;
    *magicAndSz++ = COMPRESSED_MAGIC2;
    *magicAndSz++ = (uint32)dominantSize;
    uint8* compData = (uint8*)magicAndSz;
    ::memcpy(compData, largeBuffer, compressedSz);
    delete[] largeBuffer;
}

bool Converter::SaveBlend(int32 index)
{
    // Compress the blends to PNG format:
    BinaryPtr compData = compressImage(blends[index]);

    const String textureRef = BwLayerTextureRef(textureNames[index]);
    size_t dataSz = sizeof(BlendHeader) + compData->size() + sizeof(uint32) + sizeof(char) * (textureRef.length() + 1);

    blendBlock[index].resize(dataSz, 0);

    uint8* addr = &(blendBlock[index].front());

    BlendHeader* bh = (BlendHeader*)addr;
    bh->magic_ = BlendHeader::MAGIC;
    bh->width_ = DOM_RES;
    bh->height_ = DOM_RES;
    bh->bpp_ = 8;
    // per-layer tile pitch from textureTiling * tileScaleN (landscape material)
    bh->uProjection_ = Vector4(g_blendUProjX[index], 0.f, 0.f, 0.f);
    bh->vProjection_ = Vector4(0.f, 0.f, g_blendVProjZ[index], 0.f);
    bh->version_ = BlendHeader::VERSION_PNG_BLENDS;
    addr += sizeof(BlendHeader);

    uint32* texLen = (uint32*)addr;
    *texLen = (uint32)textureRef.length();
    addr += sizeof(uint32);

    char* tex = (char*)addr;
    ::strncpy(tex, textureRef.c_str(), *texLen);
    addr += (uint32)textureRef.length();

    uint8* pHeight = (uint8*)(addr);
    ::memcpy(pHeight, &compData->front(), compData->size());

    delete compData;
    return true;
}

// A water body needs a UniqueID BW can parse: four dotted 32 bit hex groups. The
// guid tokens here are 8-8-8-8 hex, so keep that shape and step the last group per
// tile so every tile of one sheet gets its own id.
static DAVA::String BwDottedUid(const DAVA::String& token, DAVA::uint32 salt)
{
    DAVA::String hex;
    for (DAVA::String::size_type i = 0; i < token.size() && hex.size() < 32; ++i)
    {
        const char c = token[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
        {
            hex.push_back(c);
        }
    }
    while (hex.size() < 32)
    {
        hex.push_back('0');
    }
    if (hex.size() > 32)
    {
        hex.resize(32);
    }

    for (DAVA::String::size_type i = 0; i < hex.size(); ++i)
    {
        hex[i] = (char)tolower((int)hex[i]);
    }

    char tail[16] = {};
    Snprintf(tail, sizeof(tail), "%08x", salt);
    hex.replace(24, 8, tail);

    DAVA::String uid;
    uid.reserve(35);
    for (DAVA::int32 group = 0; group < 4; ++group)
    {
        if (group > 0)
        {
            uid.push_back('.');
        }
        uid.append(hex, (size_t)(group * 8), 8);
    }
    return uid;
}

static bool BwWriteTextFile(const DAVA::String& path, const char* text)
{
    DAVA::File* f = DAVA::File::Create(path, DAVA::File::CREATE | DAVA::File::WRITE);
    if (f == nullptr)
    {
        DAVA::Logger::Error("[TerrainConverter] can not write '%s'", DAVA::FilePath(path).GetAbsolutePathname().c_str());
        return false;
    }
    f->Write(text, (DAVA::uint32)strlen(text));
    DAVA::SafeRelease(f);
    return true;
}

static bool BwWriteBlobFile(const DAVA::String& path, const DAVA::uint8* data, DAVA::uint32 size)
{
    DAVA::File* f = DAVA::File::Create(path, DAVA::File::CREATE | DAVA::File::WRITE);
    if (f == nullptr)
    {
        DAVA::Logger::Error("[TerrainConverter] can not write '%s'", DAVA::FilePath(path).GetAbsolutePathname().c_str());
        return false;
    }
    f->Write(data, size);
    DAVA::SafeRelease(f);
    return true;
}

// Water is a BW water body, not a mesh: _<uid>.vlo carries the WaterState
// (position, size, wave and reflection settings) and _<uid>.odata the grid beside
// it, which BW always expects. A chunk activates the body with
// <vlo><uid/><type>water</type></vlo> - a <water> item would take
// EditorChunkWater::oldCreate and assert Moo::isRenderThread() instead.
//
// One body for one sheet, like the world editor writes it. The .odata grid is per
// cell content, not a geometry summary: two editor-made waters of identical size,
// cellsize and tessellation carry different grids, so the packed blob does not
// encode the extents. The bytes below are the flat, featureless grid of such a
// water, which is exactly what a single plane needs.
void Converter::AddWater(Entity* node, const String& guidStr)
{
    DAVA::Logger::Info("AddWater %s", node->GetName().c_str());

    if (!g_exportWater)
    {
        DAVA::Logger::Info("[TerrainConverter] water '%s' skipped - g_exportWater is off", node->GetName().c_str());
        return;
    }

    if (waterVloFormatPattern[0] == 0 || waterFormatPattern[0] == 0)
    {
        DAVA::Logger::Error("[TerrainConverter] AddWater '%s' - water templates not loaded, skipped",
                            node->GetName().c_str());
        return;
    }

    // DAVA world -> BW world. A Dava world point goes through the entity transform
    // and then globalTransform; the inverse must not be applied here, that sandwich
    // (globalTransformInverse * wt * globalTransform) only maps the entity origin,
    // which is why the sheet came out 1117 x 0 m and was skipped.
    const Matrix4 toBw = GetEntityWorldTransform(node) * Model::globalTransform;

    const AABBox3 wt = node->GetWTMaximumBoundingBoxSlow();
    Vector3 bwMin(1.0e30f, 1.0e30f, 1.0e30f);
    Vector3 bwMax(-1.0e30f, -1.0e30f, -1.0e30f);
    for (DAVA::int32 corner = 0; corner < 8; ++corner)
    {
        const Vector3 p((corner & 1) != 0 ? wt.max.x : wt.min.x,
                        (corner & 2) != 0 ? wt.max.y : wt.min.y,
                        (corner & 4) != 0 ? wt.max.z : wt.min.z);
        const Vector3 q = p * toBw;
        if (q.x < bwMin.x)
            bwMin.x = q.x;
        if (q.y < bwMin.y)
            bwMin.y = q.y;
        if (q.z < bwMin.z)
            bwMin.z = q.z;
        if (q.x > bwMax.x)
            bwMax.x = q.x;
        if (q.y > bwMax.y)
            bwMax.y = q.y;
        if (q.z > bwMax.z)
            bwMax.z = q.z;
    }

    const float32 planeY = (bwMax.y + g_heightOffsetM) * g_heightReliefScale;
    const float32 cx = (bwMin.x + bwMax.x) * 0.5f;
    const float32 cz = (bwMin.z + bwMax.z) * 0.5f;
    const float32 sizeX = bwMax.x - bwMin.x;
    const float32 sizeZ = bwMax.z - bwMin.z;

    if (sizeX <= 0.0f || sizeZ <= 0.0f)
    {
        DAVA::Logger::Warning("[TerrainConverter] water '%s' has no extent (%.2f x %.2f m) - skipped",
                              node->GetName().c_str(), sizeX, sizeZ);
        return;
    }

    const String uid = BwDottedUid(guidStr, 0);

    // BW builds the water grid from size / cellsize, and the .odata we hold is the
    // flat 2x2 grid of ONE cell (that is what the 90 m/100 water carries). With the
    // template's 100 m cell this 1117 x 1862 m sheet would ask for 12 x 19 cells and
    // the packed grid would not match, so the cell is stretched to keep it at 1.
    const float32 waterCellSize = ((sizeX > sizeZ) ? sizeX : sizeZ) + 1.0f;

    char vlo[8192] = {};
    Snprintf(vlo, sizeof(vlo), waterVloFormatPattern,
             uid.c_str(), cx, planeY, cz, sizeX, 0.0f, sizeZ, waterCellSize, uid.c_str());

    // Shipped spaces use the underscore spelling and BW's include probe tries
    // "_<uid>.vlo" before "<uid>.vlo"; the root tag inside has to match the file
    // name, so this is the one form written.
    const String waterVloPath = g_spacesPath + "_" + uid + ".vlo";
    const String waterOdaPath = g_spacesPath + "_" + uid + ".odata";

    const bool vloOk = BwWriteTextFile(waterVloPath, vlo);
    const bool odaOk = BwWriteBlobFile(waterOdaPath, g_bwWaterOdata, (DAVA::uint32)sizeof(g_bwWaterOdata));

    char item[2048] = {};
    Snprintf(item, sizeof(item), waterFormatPattern, uid.c_str());

    // Active in every chunk the sheet covers, so it is loaded wherever the player is.
    const DAVA::int32 chunkX0 = (DAVA::int32)floor(bwMin.x / 100.0f);
    const DAVA::int32 chunkX1 = (DAVA::int32)floor(bwMax.x / 100.0f);
    const DAVA::int32 chunkZ0 = (DAVA::int32)floor(bwMin.z / 100.0f);
    const DAVA::int32 chunkZ1 = (DAVA::int32)floor(bwMax.z / 100.0f);

    // The sheet is usually far wider than the landscape, so it has to widen the
    // published space bounds too - a chunk outside them is never loaded by the client.
    if (chunkX0 < g_itemMinX)
        g_itemMinX = chunkX0;
    if (chunkX1 > g_itemMaxX)
        g_itemMaxX = chunkX1;
    if (chunkZ0 < g_itemMinZ)
        g_itemMinZ = chunkZ0;
    if (chunkZ1 > g_itemMaxZ)
        g_itemMaxZ = chunkZ1;

    DAVA::int32 chunkCount = 0;
    for (DAVA::int32 cz2 = chunkZ0; cz2 <= chunkZ1; ++cz2)
    {
        for (DAVA::int32 cx2 = chunkX0; cx2 <= chunkX1; ++cx2)
        {
            const String chunkPath = g_spacesPath + outsideChunkIdentifier(cx2, cz2) + ".chunk";

            File* cf = File::Create(chunkPath, File::OPEN | File::READ);
            if (cf == nullptr)
            {
                // The sheet reaches cells the terrain pass never wrote; create an
                // empty chunk so the water is still activated there.
                cf = File::Create(chunkPath, File::CREATE | File::WRITE);
                if (cf != nullptr)
                {
                    const char* emptyChunk = "<root>\r\n</root>\r\n";
                    cf->Write(emptyChunk, (uint32)strlen(emptyChunk));
                    SafeRelease(cf);
                }
                cf = File::Create(chunkPath, File::OPEN | File::READ);
            }

            if (cf == nullptr)
            {
                continue;
            }

            const uint32 res = cf->Read(chunkBuffer, CHUNKBUFFER_SIZE - 1);
            chunkBuffer[res] = 0;
            SafeRelease(cf);

            char* rootEnd = strstr(chunkBuffer, "</root>");
            if (rootEnd == nullptr)
            {
                continue;
            }

            File* out = File::Create(chunkPath, File::CREATE | File::WRITE);
            if (out != nullptr)
            {
                out->Write(chunkBuffer, (uint32)(rootEnd - chunkBuffer));
                out->Write(item, (uint32)strlen(item));
                out->Write(rootEnd, (uint32)strlen(rootEnd));
                SafeRelease(out);
                ++chunkCount;
            }
        }
    }

    DAVA::Logger::Info("[TerrainConverter] water '%s': %.0f x %.0f m at (%.1f %.2f %.1f), cellsize %.0f, uid %s, "
                       "vlo %s, odata %s, active in %d chunk(s)",
                       node->GetName().c_str(), sizeX, sizeZ, cx, planeY, cz, waterCellSize, uid.c_str(),
                       vloOk ? "ok" : "FAILED", odaOk ? "ok" : "FAILED", chunkCount);
} // Converter::AddWater

void Converter::AddProxyVLO(Entity* node, Model* model)
{
    if (proxyFormatPattern[0] == 0 || proxyVloFormatPattern[0] == 0)
    {
        DAVA::Logger::Error("[TerrainConverter] AddProxyVLO '%s' - proxy templates not loaded, skipped",
                            model->GetModelName().c_str());
        return;
    }

    DAVA::Logger::Info("AddProxyVLO model %s", model->GetModelName().c_str());

    UniqueID guid = GetEntityID(node);
    String guidStr = MakeFilenameToken(guid.toString());
    KeyedArchive* customProperties = GetCustomPropertiesArchieve(node);
    if (customProperties != nullptr)
    {
        String rawGuid = customProperties->GetString("guid", guidStr);
        guidStr = MakeFilenameToken(rawGuid);
        if (guidStr != rawGuid)
        {
            DAVA::Logger::Info("[TerrainConverter] proxy guid '%s' is not filename-safe, using '%s'",
                               rawGuid.c_str(), guidStr.c_str());
        }
    }

    // BW parses a uid as four dotted hex groups, and the <uid> in the chunk item, the
    // file name and the root tag inside the file all have to be the same text.
    const String uid = BwDottedUid(guidStr, 0);

    // BW-space world transform of this instance (same rebase AddEntityToChunk uses).
    const Matrix4 wt = GetEntityWorldTransform(node);
    const Matrix4 t = Model::globalTransformInverse * wt * Model::globalTransform;

    // Shipped proxy VLOs (e.g. _5ab97d85.44d1c5c1.1cf84582.586630a2.vlo) carry the
    // absolute placement in <worldTransform> and worldTransform - chunk*100 in
    // <transform>. Writing the world position in both makes BW draw the model one
    // chunk offset away from where it really is.
    const Vector3 posBW = Vector3(0.0f, 0.0f, 0.0f) * t;
    const DAVA::int32 ownerX = (DAVA::int32)floor(posBW.x / 100.0f);
    const DAVA::int32 ownerZ = (DAVA::int32)floor(posBW.z / 100.0f);
    const float32 worldY = (posBW.y + g_heightOffsetM) * g_heightReliefScale;
    const float32 localX = posBW.x - ownerX * 100.0f;
    const float32 localZ = posBW.z - ownerZ * 100.0f;

    Snprintf(resultBuff, sizeof(resultBuff), proxyVloFormatPattern,
             uid.c_str(),
             BwResPath("content/" + mapName + "/" + model->GetModelName() + ".model").c_str(),
             t._00, t._01, t._02, t._10, t._11, t._12,
             t._20, t._21, t._22, localX, worldY, localZ,
             t._00, t._01, t._02, t._10, t._11, t._12,
             t._20, t._21, t._22, posBW.x, worldY, posBW.z,
             uid.c_str());

    const String proxyVloPath = g_spacesPath + "_" + uid + ".vlo";
    File* f = File::Create(proxyVloPath, File::CREATE | File::WRITE);
    if (f == nullptr)
    {
        DAVA::Logger::Error("[TerrainConverter] can not write '%s' - proxy VLO skipped",
                            FilePath(proxyVloPath).GetAbsolutePathname().c_str());
        resultBuff[0] = 0;
        return;
    }
    f->Write(resultBuff, (uint32)strlen(resultBuff));
    SafeRelease(f);

    // Activation item. BW's chunk loader (chunk_vlo.cpp) looks for <proxyvlo> with a
    // <type> child; a bare <proxy> item is ignored, which is why the mountains were
    // exported but never loaded.
    Snprintf(resultBuff, sizeof(resultBuff), proxyFormatPattern, uid.c_str());

    // Chunk range has to come from the BW-space footprint: Dava is Z up, so its y is
    // BW's -z, and testing the Dava box against BW chunk boxes put the item in the
    // wrong chunks.
    const AABBox3 wtBox = node->GetWTMaximumBoundingBoxSlow();
    Vector3 bwMin(1.0e30f, 1.0e30f, 1.0e30f);
    Vector3 bwMax(-1.0e30f, -1.0e30f, -1.0e30f);
    for (DAVA::int32 corner = 0; corner < 8; ++corner)
    {
        const Vector3 p((corner & 1) != 0 ? wtBox.max.x : wtBox.min.x,
                        (corner & 2) != 0 ? wtBox.max.y : wtBox.min.y,
                        (corner & 4) != 0 ? wtBox.max.z : wtBox.min.z);
        const Vector3 q = p * wt * Model::globalTransform;
        if (q.x < bwMin.x)
            bwMin.x = q.x;
        if (q.y < bwMin.y)
            bwMin.y = q.y;
        if (q.z < bwMin.z)
            bwMin.z = q.z;
        if (q.x > bwMax.x)
            bwMax.x = q.x;
        if (q.y > bwMax.y)
            bwMax.y = q.y;
        if (q.z > bwMax.z)
            bwMax.z = q.z;
    }

    const DAVA::int32 loX = (DAVA::int32)floor(bwMin.x / 100.0f);
    const DAVA::int32 hiX = (DAVA::int32)floor(bwMax.x / 100.0f);
    const DAVA::int32 loZ = (DAVA::int32)floor(bwMin.z / 100.0f);
    const DAVA::int32 hiZ = (DAVA::int32)floor(bwMax.z / 100.0f);

    DAVA::int32 chunkCount = 0;
    for (DAVA::int32 zc = loZ; zc <= hiZ; ++zc)
    {
        for (DAVA::int32 xc = loX; xc <= hiX; ++xc)
        {
            if (xc < g_itemMinX)
                g_itemMinX = xc;
            if (xc > g_itemMaxX)
                g_itemMaxX = xc;
            if (zc < g_itemMinZ)
                g_itemMinZ = zc;
            if (zc > g_itemMaxZ)
                g_itemMaxZ = zc;

            const String chunkPath = g_spacesPath + outsideChunkIdentifier(xc, zc) + ".chunk";

            File* chunkFile = File::Create(chunkPath, File::OPEN | File::READ);
            if (chunkFile == nullptr)
            {
                // Far-off chunks (the mountains sit outside the landscape grid) do
                // not exist yet; a chunk with no terrain is legal and is what makes
                // the client load the proxy where the model actually is.
                chunkFile = File::Create(chunkPath, File::CREATE | File::WRITE);
                if (chunkFile != nullptr)
                {
                    const char* emptyChunk = "<root>\r\n</root>\r\n";
                    chunkFile->Write(emptyChunk, (uint32)strlen(emptyChunk));
                    SafeRelease(chunkFile);
                }
                chunkFile = File::Create(chunkPath, File::OPEN | File::READ);
            }

            if (chunkFile == nullptr)
            {
                continue;
            }

            const uint32 res = chunkFile->Read(chunkBuffer, CHUNKBUFFER_SIZE - 1);
            chunkBuffer[res] = 0;
            SafeRelease(chunkFile);

            char* rootEnd = strstr(chunkBuffer, "</root>");
            if (rootEnd == nullptr)
            {
                continue;
            }

            chunkFile = File::Create(chunkPath, File::CREATE | File::WRITE);
            if (chunkFile == nullptr)
            {
                DAVA::Logger::Error("[TerrainConverter] can not write chunk '%s' for proxy VLO '%s'",
                                    chunkPath.c_str(), uid.c_str());
                continue;
            }
            chunkFile->Write(chunkBuffer, (uint32)(rootEnd - chunkBuffer));
            chunkFile->Write(resultBuff, (uint32)strlen(resultBuff));
            chunkFile->Write(rootEnd, (uint32)strlen(rootEnd));
            SafeRelease(chunkFile);
            ++chunkCount;
        }
    }

    DAVA::Logger::Info("[TerrainConverter] proxy VLO %s for '%s': world (%.1f %.1f %.1f), active in %d chunk(s)",
                       uid.c_str(), model->GetModelName().c_str(), posBW.x, worldY, posBW.z, chunkCount);
    resultBuff[0] = 0;
} // Converter::AddProxyVLO

void Converter::SetupVersionLKA()
{
    sceneNodesLinks->SetInt32("version", 1);
}

UniqueID Converter::GetEntityID(Entity* node)
{
    std::hash<std::string> hash_str;

    uint32 a = 0, b = 0, c = 0, d = 0;
    a = (uint32)hash_str(scene->GetName().c_str());
    b = node->GetID();

    return UniqueID(a, b, c, d);
}

UniqueID Converter::GetEntityID(Entity* node, uint32 index)
{
    std::hash<std::string> hash_fn;

    uint32 a = 0, b = 0, c = 0, d = 0;
    a = (uint32)hash_fn(scene->GetName().c_str());
    b = node->GetID();
    c = index;

    return UniqueID(a, b, c, d);
}