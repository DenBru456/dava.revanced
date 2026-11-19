/*==================================================================================
    Copyright (c) 2012, DAVA Consulting, LLC
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
 * Neither the name of the DAVA Consulting, LLC nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE DAVA CONSULTING, LLC AND CONTRIBUTORS "AS IS" AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL DAVA CONSULTING, LLC BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

    Revision History:
 * Created by Yury Danilov
   =====================================================================================*/
#include "Model.h"

// mapName arrives without a trailing separator. Anything that used to rely on
// one ("rudniki/" + "images/...") goes through this instead, so no resource name
// can come out with a double slash. Everything under content/<map>/ (models,
// visuals, textures, speedtrees) is referenced from the res root, the same way
// the chunk refers to content/<map>/<name>.model.
static DAVA::String MapDir(const DAVA::String& mapName)
{
    if (mapName.empty())
    {
        return DAVA::String();
    }
    return mapName + "/";
}

// Resource names bound for BigWorld: one separator, always '/'. mapName is
// stripped by the constructor, but these strings end up inside chunk items,
// destructible XML and .visual files, where a doubled separator is a failed
// lookup long before anything logs an error.
static DAVA::String CleanResPath(const DAVA::String& path)
{
    DAVA::String out;
    out.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i)
    {
        const char c = path[i];
        if (c == '/' || c == '\\')
        {
            if (out.empty() || out[out.size() - 1] == '/')
            {
                continue;
            }
            out.push_back('/');
        }
        else
        {
            out.push_back(c);
        }
    }
    return out;
}

// ---------------------------------------------------------------- DDS fixups
// BW 2.0.1 rejects textures it can not build a handle for, and the streaming
// manager then asserts on that invalid handle the next time it evicts
// (moo/texture_streaming_manager.cpp: handle.isValid()). Two things made that
// happen: a .visual naming an image the converter never wrote, and DX10 header
// DDS files (what current tools emit for BC1/BC3/BC5), which the legacy BW
// reader does not understand. Both are handled here.

static std::vector<DAVA::uint8> ReadWholeFileForDds(const DAVA::FilePath& path)
{
    std::vector<DAVA::uint8> data;
    DAVA::File* f = DAVA::File::Create(path, DAVA::File::OPEN | DAVA::File::READ);
    if (f == nullptr)
    {
        return data;
    }
    DAVA::uint8 chunk[64 * 1024];
    DAVA::uint32 got = 0;
    while ((got = f->Read(chunk, (DAVA::uint32)sizeof(chunk))) > 0)
    {
        data.insert(data.end(), chunk, chunk + got);
    }
    DAVA::SafeRelease(f);
    return data;
}

// one shared flat normal map per export run
static bool g_flatNormalWritten = false;

// Set by the converter: the Blitz DataSource root, so a texture the scene's
// descriptor cannot resolve can still be found by name. Sharing it as a plain
// global keeps Model.h untouched.
DAVA::String g_bwTextureSearchRoot;
static DAVA::Map<DAVA::String, DAVA::String> g_textureSearchCache;

// A flat 4x4 texture. Every material has to name a file BW can load: an image
// that is missing or unreadable leaves the material with an invalid texture
// handle, which is exactly what the worldeditor asserts on.
static bool WriteBwPlaceholderDds(const DAVA::FilePath& path, DAVA::String& error)
{
    const DAVA::uint32 side = 4;
    DAVA::uint32 h[31];
    memset(h, 0, sizeof(h));
    h[0] = 124; // dwSize
    h[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000; // caps | height | width | pitch | pixelformat
    h[2] = side; // height
    h[3] = side; // width
    h[4] = side * 4; // pitch
    h[18] = 32; // pf size
    h[19] = 0x1 | 0x40; // alphapixels | rgb
    h[21] = 32; // bit count
    h[22] = 0x00ff0000; // R
    h[23] = 0x0000ff00; // G
    h[24] = 0x000000ff; // B
    h[25] = 0xff000000; // A
    h[26] = 0x1000; // caps: texture

    std::vector<DAVA::uint8> dds(4 + 124 + side * side * 4, 0);
    const DAVA::uint32 magic = 0x20534444; // "DDS "
    memcpy(&dds[0], &magic, 4);
    memcpy(&dds[4], h, 124);
    for (DAVA::uint32 p = 0; p < side * side; ++p)
    {
        DAVA::uint8* px = &dds[4 + 124 + p * 4];
        px[0] = 0x80; // B
        px[1] = 0x80; // G
        px[2] = 0x80; // R
        px[3] = 0xff; // A
    }

    DAVA::File* out = DAVA::File::Create(path, DAVA::File::CREATE | DAVA::File::WRITE);
    if (out == nullptr)
    {
        error = "can not create the file";
        return false;
    }
    out->Write(&dds[0], (DAVA::uint32)dds.size());
    DAVA::SafeRelease(out);
    return true;
}

// Flat normal map (128,128,255). Every material is switched to BW's normal mapping
// effect, and that effect needs a normalMap that resolves, or the material ends up with
// an unbound handle.
static bool WriteFlatNormalDds(const DAVA::FilePath& path)
{
    const DAVA::uint32 side = 4;
    DAVA::uint32 h[31];
    memset(h, 0, sizeof(h));
    h[0] = 124;
    h[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000;
    h[2] = side;
    h[3] = side;
    h[4] = side * 4;
    h[18] = 32;
    h[19] = 0x1 | 0x40;
    h[21] = 32;
    h[22] = 0x00ff0000;
    h[23] = 0x0000ff00;
    h[24] = 0x000000ff;
    h[25] = 0xff000000;
    h[26] = 0x1000;

    std::vector<DAVA::uint8> dds(4 + 124 + side * side * 4, 0);
    const DAVA::uint32 magic = 0x20534444;
    memcpy(&dds[0], &magic, 4);
    memcpy(&dds[4], h, 124);
    for (DAVA::uint32 p = 0; p < side * side; ++p)
    {
        DAVA::uint8* px = &dds[4 + 124 + p * 4];
        px[0] = 0xff; // B = z
        px[1] = 0x80; // G = y
        px[2] = 0x80; // R = x
        // BW normal maps carry the specular/gloss term in alpha (normalmap.fx reads it),
        // so a flat normal with alpha = 1 makes every untextured Blitz prop look like
        // wet plastic. Blitz art has no specular map at all, so matte it is.
        px[3] = 0x00;
    }

    DAVA::File* out = DAVA::File::Create(path, DAVA::File::CREATE | DAVA::File::WRITE);
    if (out == nullptr)
    {
        return false;
    }
    out->Write(&dds[0], (DAVA::uint32)dds.size());
    DAVA::SafeRelease(out);
    return true;
}

// Rewrites a DX10 header into the legacy fourcc that matches its DXGI format and
// drops the 20 byte extension. The compressed payload is unchanged, so this is a
// header edit, not a re-encode. Returns false with a reason for containers BW
// can not read at all.
static bool RewriteDx10DdsForBw(const DAVA::FilePath& path, const DAVA::String& bwName, DAVA::String& reason)
{
    std::vector<DAVA::uint8> data = ReadWholeFileForDds(path);
    if (data.size() < 128 || memcmp(&data[0], "DDS ", 4) != 0)
    {
        reason = "not a DDS file";
        return false;
    }

    DAVA::uint32 h[31];
    memcpy(h, &data[4], 124);
    if (h[18] != 32)
    {
        reason = "pixel format block is not 32 bytes";
        return false;
    }
    if (h[20] != 0x30315844 /* "DX10" */)
    {
        return true; // legacy header, BW reads it as is
    }
    if (data.size() < 148)
    {
        reason = "DX10 header is truncated";
        return false;
    }

    DAVA::uint32 dx10[5] = { 0, 0, 0, 0, 0 };
    memcpy(dx10, &data[128], sizeof(dx10));
    const DAVA::uint32 dxgi = dx10[0];
    const DAVA::uint32 arraySize = dx10[3];
    if (arraySize != 1)
    {
        reason = DAVA::Format("texture array of %u elements", arraySize);
        return false;
    }

    DAVA::uint32 legacy = 0;
    switch (dxgi)
    {
    case 70: // BC1_TYPELESS
    case 71: // BC1_UNORM
    case 72: // BC1_UNORM_SRGB
        legacy = 0x31545844; // "DXT1"
        break;
    case 73: // BC2_TYPELESS
    case 74: // BC2_UNORM
    case 75: // BC2_UNORM_SRGB
        legacy = 0x33545844; // "DXT3"
        break;
    case 76: // BC3_TYPELESS
    case 77: // BC3_UNORM
    case 78: // BC3_UNORM_SRGB
        legacy = 0x35545844; // "DXT5"
        break;
    case 80: // BC4_UNORM
        legacy = 0x31495441; // "ATI1"
        break;
    case 83: // BC5_UNORM
    case 84: // BC5_SNORM
        legacy = 0x32495441; // "ATI2"
        break;
    default:
        reason = DAVA::Format("DXGI format %u (BC6H/BC7 and newer)", dxgi);
        return false;
    }

    // Legacy headers carry the top mip size in dwPitchOrLinearSize; some readers
    // (BW's included) use it to sanity check the payload before allocating.
    const DAVA::uint32 blockBytes = (legacy == 0x31545844 || legacy == 0x31495441) ? 8u : 16u;
    const DAVA::uint32 blocksWide = (h[3] + 3) / 4;
    const DAVA::uint32 blocksHigh = (h[2] + 3) / 4;
    h[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000; // caps | height | width | pixelformat | linearsize
    h[4] = blocksWide * blocksHigh * blockBytes;
    if (h[6] > 1)
    {
        h[26] |= 0x400008; // DDSCAPS_COMPLEX | DDSCAPS_MIPMAP
    }
    else
    {
        h[6] = 0;
    }

    h[19] = 0x4; // DDPF_FOURCC
    h[20] = legacy;
    h[21] = 0; // no rgb bit count
    h[22] = 0;
    h[23] = 0;
    h[24] = 0;
    h[25] = 0;
    h[27] = 0; // caps2: drop cubemap / volume flags
    h[28] = 0;
    h[29] = 0;
    h[30] = 0;

    std::vector<DAVA::uint8> out;
    out.reserve(data.size() - 20);
    out.insert(out.end(), data.begin(), data.begin() + 4);
    const DAVA::uint8* hb = (const DAVA::uint8*)h;
    out.insert(out.end(), hb, hb + 124);
    out.insert(out.end(), data.begin() + 148, data.end());

    DAVA::File* f = DAVA::File::Create(path, DAVA::File::CREATE | DAVA::File::WRITE);
    if (f == nullptr)
    {
        reason = "can not rewrite the file";
        return false;
    }
    f->Write(&out[0], (DAVA::uint32)out.size());
    DAVA::SafeRelease(f);

    DAVA::Logger::Info("[TerrainConverter] texture '%s': DX10 header (dxgi %u) rewritten to legacy fourcc",
                       bwName.c_str(), dxgi);
    return true;
}
#include "physics2/bsp.hpp"

#include <REPlatform/Deprecated/EditorConfig.h>

#include <Scene3D/Components/SwitchComponent.h>

// BW texture export needs the CPU image pipeline (not pulled in by DAVAEngine.h)
#include <Render/Image/ImageConvert.h>
#include <string.h>

// DAVA's Snprintf maps to raw _snprintf on Windows, which does NOT
// zero-terminate on exact-fit truncation (C6053); the plugin strlen()s
// these buffers right after. Route every call through C99-conformant
// vsnprintf instead, which always terminates.
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <vector>
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
// dava.revanced compatibility: entity transforms live on TransformComponent
// (the plugin was written against dava.engine master's Entity::GetWorldTransform)
#include <Math/TransformUtils.h>
#include <Scene3D/Components/TransformComponent.h>

static DAVA::Matrix4 GetEntityWorldTransform(DAVA::Entity* node)
{
    if (node == nullptr)
        return DAVA::Matrix4::IDENTITY;
    const DAVA::TransformComponent* transformComponent = node->GetComponent<DAVA::TransformComponent>();
    if (transformComponent == nullptr)
        return DAVA::Matrix4::IDENTITY;
    return DAVA::TransformUtils::ToMatrix(transformComponent->GetWorldTransform());
}

static DAVA::Map<DAVA::String, DAVA::int32> InitMaterialKindMap()
{
    using namespace DAVA;

    // from material_kinds.xml:
    //  <id> 112</id><effect_material>metal</effect_material>
    //  <id> 111</id><effect_material> stone </effect_material>
    //  <id> 110</id><effect_material>snow</effect_material>
    //  <id> 108 </id><effect_material> wood </effect_material>
    //  <id> 107 </id><effect_material> ground </effect_material>
    //  <id> 103 </id><effect_material> sand </effect_material>
    Map<String, int32> map;
    map["metal"] = 112;
    map["stone"] = 111;
    map["snow"] = 110;
    map["wood"] = 108;
    map["ground"] = 107;
    map["sand"] = 103;
    return map;
}

// enum
// {
//  // Nothing will ever collide with this triangle
//  TRIANGLE_NOT_IN_BSP			= 0xff,
//
//  TRIANGLE_CAMERANOCOLLIDE	= 1 << 0,
//  TRIANGLE_TRANSPARENT		= 1 << 1,
//  TRIANGLE_BLENDED			= 1 << 2,
//  TRIANGLE_TERRAIN			= 1 << 3,
//
//  // Player does not collide. Camera still does
//  TRIANGLE_NOCOLLIDE			= 1 << 4,
//
//  TRIANGLE_DOUBLESIDED		= 1 << 5,
//  TRIANGLE_DOOR				= 1 << 6,
//  TRIANGLE_PROJECTILENOCOLLIDE = 1 << 7,
// };

static DAVA::Map<DAVA::String, DAVA::int32> materialKindMap = InitMaterialKindMap();

static bool IsFragile(eModelPreset value)
{
    return (value == eMP_FRAGILE_PR || value == eMP_FRAGILE_NPR);
}

static bool IsFallingAtom(eModelPreset value)
{
    return (value == eMP_FALLING_ATOM);
}

static bool IsTree(eModelPreset value)
{
    return (value == eMP_TREE);
}

DAVA::int32 Model::GetBWCollision(DAVA::int32 primitiveGroup)
{
    DAVA::int32 result = 0;

    if (collision == eMP_NO_COLLISION)
    {
        result = TRIANGLE_NOT_IN_BSP;
    }
    else if (collision == eMP_TREE)
    {
        if (groups[primitiveGroup].identifier.find("trunk") != std::string::npos)
        {
            result = TRIANGLE_PROJECTILENOCOLLIDE;
        }
        else
        {
            result = TRIANGLE_NOCOLLIDE | TRIANGLE_PROJECTILENOCOLLIDE;
        }
    }
    else if (collision == eMP_BUSH)
    {
        result = TRIANGLE_PROJECTILENOCOLLIDE | TRIANGLE_NOCOLLIDE;
    }
    else if (collision == eMP_FRAGILE_PR)
    {
        if (primitiveGroup == 0)
        {
            result = 0;
        }
        else if (primitiveGroup == 1)
        {
            result = (groups[1].matCollision == eMP_BUILDING) ? 0 : TRIANGLE_NOT_IN_BSP;
        }
    }
    else if (collision == eMP_FRAGILE_NPR)
    {
        if (primitiveGroup == 0)
        {
            result = TRIANGLE_PROJECTILENOCOLLIDE;
        }
        else if (primitiveGroup == 1)
        {
            result = (groups[1].matCollision == eMP_BUILDING) ? TRIANGLE_PROJECTILENOCOLLIDE : TRIANGLE_NOT_IN_BSP;
        }
    }
    else if (collision == eMP_FALLING_ATOM)
    {
        result = TRIANGLE_PROJECTILENOCOLLIDE | TRIANGLE_CAMERANOCOLLIDE | TRIANGLE_TRANSPARENT;
    }
    else if (collision == eMP_BUILDING)
    {
        result = 0;
    }
    else if (collision == eMP_INVISIBLE_WALL)
    {
        result = TRIANGLE_CAMERANOCOLLIDE | TRIANGLE_TRANSPARENT | TRIANGLE_PROJECTILENOCOLLIDE;
        return result;
    }

    //	if (groups[primitiveGroup].twoSided)
    result = result | TRIANGLE_DOUBLESIDED;

    return result;
} // Model::GetBWCollision

const DAVA::int32 MATKIND_SPT_SOLID = 71;
const DAVA::int32 MATKIND_SPT_LEAVES = 72;

const DAVA::int32 MATKIND_UNDAMAGED = 78;
const DAVA::int32 MATKIND_UNDAMAGED1 = 73;
const DAVA::int32 MATKIND_DESTROYED = 87;

const DAVA::int32 MATKIND_STONE = 111;
const DAVA::int32 MATKIND_WOOD = 108;

DAVA::int32 Model::GetBWMaterialKind(DAVA::int32 primitiveGroup)
{
    if (collision == eMP_NO_COLLISION)
    {
        return MATKIND_UNDAMAGED;
    }
    else if (collision == eMP_BUSH)
    {
        return MATKIND_SPT_LEAVES;
    }
    else if (collision == eMP_TREE)
    {
        if (groups[primitiveGroup].identifier.find("trunk") != std::string::npos)
        {
            return MATKIND_SPT_SOLID;
        }
        else
        {
            return MATKIND_SPT_LEAVES;
        }
    }
    else if (collision == eMP_FRAGILE_PR || collision == eMP_FRAGILE_NPR)
    {
        if (primitiveGroup == 0)
        {
            return MATKIND_UNDAMAGED1;
        }
        else if (primitiveGroup == 1)
        {
            return (groups[1].matCollision == eMP_BUILDING) ? 111 : MATKIND_DESTROYED;
        }
    }
    else if (collision == eMP_FALLING_ATOM)
    {
        return MATKIND_UNDAMAGED1;
    }
    else if (collision == eMP_BUILDING)
    {
        DAVA::int32 res = materialKindMap[materialKind];
        if (res > 20)
        {
            res = 1; // BW material kinds are a small enum; Dava's 73..111 are not
        }
        if (res < 100)
        {
            return MATKIND_WOOD;
        }
        return res;
    }
    else if (collision == eMP_INVISIBLE_WALL)
    {
        return MATKIND_UNDAMAGED;
    }
    return MATKIND_UNDAMAGED;
}

static DAVA::Matrix4 InverseLambda(const DAVA::Matrix4& p)
{
    DAVA::Matrix4 tmp;
    p.GetInverse(tmp);
    return tmp;
};

// Dava space -> BigWorld space, written out explicitly.
//
//   x_bw =  x_dava      east  -> east
//   y_bw =  z_dava      up    -> up
//   z_bw =  y_dava      north -> north
//
// The two engines use opposite handedness: Dava is right-handed Z-up (OpenGL),
// BigWorld is left-handed Y-up (DirectX). Carrying the same "north is up, east is
// right" layout across a handedness change REQUIRES one sign flip, so this matrix
// has determinant -1 by construction - that is the conversion, not a bug. I had
// previously "fixed" it into a clean rotation (z_bw = -y_dava) and that is exactly
// what made the exported map a mirror image of the editor: terrain and objects were
// consistent with each other but both flipped against the source scene.
// Spelling the four rows out removes any dependence on how BuildRotation treats
// the sign of its angle. If meshes come out inside-out, flip the winding order -
// a det -1 basis change inverts triangle orientation.
const DAVA::Matrix4 Model::globalTransform(
1.0f, 0.0f, 0.0f, 0.0f, // row0: x  ->  x
0.0f, 0.0f, 1.0f, 0.0f, // row1: y  ->  z   (up)
0.0f, 1.0f, 0.0f, 0.0f, // row2: z  ->  y   (north)
0.0f, 0.0f, 0.0f, 1.0f);
const DAVA::Matrix4 Model::globalTransformInverse = InverseLambda(globalTransform);

Model::Model(const DAVA::String& _fileName, const DAVA::String& _mapName, eModelPreset _collision, DAVA::Entity* model, DAVA::EditorConfig* config)
    : modelName(_fileName)
    , mapName(_mapName)
    , collision(_collision)
    , materialKind("wood")
{
    using namespace DAVA;

    while (!mapName.empty() && (mapName[mapName.length() - 1] == '/' || mapName[mapName.length() - 1] == '\\'))
    {
        mapName.erase(mapName.length() - 1);
    }

    DAVA::Logger::Info("model %s %d", _fileName.c_str(), collision);

    FilePath fileName(_fileName);
    modelName = fileName.GetBasename();
    //  FilePath::Instance()->SplitPath(fileName, folder, modelName);
    //  modelName = FileSystem::Instance()->ReplaceExtension(modelName, "");

    srand((unsigned int)13);
    Scene* scene = new Scene();
    //	Entity * model = scene->GetRootNode(fileName);
    if (model != 0)
    {
        Entity* clone = model->Clone();
        clone->GetOrCreateComponent<DAVA::TransformComponent>()->SetLocalTransform(DAVA::Transform(DAVA::Matrix4::IDENTITY));
        scene->AddNode(clone);
        scene->Update(0.1f);

        scene->transformSystem->Process(0.1f);
        scene->lodSystem->Process(0.1f);

        ReadCustomProperties(clone, config);
        CreatePolygonGroup();
        Batch(scene);
        createTris();

        // BW-compliance patch: all render batches were empty/unusable -
        // do not export a degenerate model
        bool hasAnyGeometry = false;
        for (size_t gi = 0; gi < groups.size(); ++gi)
        {
            if (groups[gi].group.nVertices_ > 0)
            {
                hasAnyGeometry = true;
                break;
            }
        }
        if (!hasAnyGeometry)
        {
            isWrongModel = true;
            DAVA::Logger::Warning("[TerrainConverter] model '%s' has no usable geometry anywhere - not exported",
                                  modelName.c_str());
        }

        if (!isWrongModel)
        {
            Save();
        }
    }
    else
    {
        isWrongModel = true;
        DAVA::Logger::Warning("Failed to load model file %s", _fileName.c_str());
    }
    SafeRelease(scene);
}

void Model::ReadCustomProperties(DAVA::Entity* model, DAVA::EditorConfig* config)
{
    using namespace DAVA;

    KeyedArchive* p = GetCustomPropertiesArchieve(model);
    if (p)
    {
        health = p->GetInt32("Health", 5);
        fallType = (eFallType)p->GetInt32("FallType", 0);
        density = p->GetFloat("Density", DEFAULT_DENSITY);
        if (p->IsKeyExists("MaterialKind"))
        {
            int32 kind = p->GetInt32("MaterialKind");
            const Vector<String>& names = config->GetComboPropertyValues("MaterialKind");
            if (kind >= 0 && kind < names.size())
            {
                materialKind = names[kind];
            }
        }
    }
}

static void ReadBuffer(char* buffer, int bufferSize, DAVA::String fileName)
{
    using namespace DAVA;

    File* f = File::Create(FilePath(fileName), File::OPEN | File::READ);
    if (f == nullptr)
    {
        DAVA::Logger::Error("ReadBuffer - cannot open template %s", fileName.c_str());
        buffer[0] = 0;
        return;
    }
    uint32 res = f->Read(buffer, bufferSize);
    buffer[res] = 0;
    SafeRelease(f);
}

//  eMP_NO_COLLISION = 0,
//  eMP_TREE,
//  eMP_BUSH,
//  eMP_FRAGILE_PR,
//  eMP_FRAGILE_NPR,
//  eMP_FALLING_ATOM,
//  eMP_BUILDING,

void Model::Save()
{
    using namespace DAVA;

    if (IsFragile(collision))
    {
        groups[0].identifier = "n_wood_0";
        groups[1].identifier = "d_wood_0";
    }

    if (IsFallingAtom(collision))
    {
        groups[0].identifier = "partN_wood_01";
    }

    if (collision == eMP_INVISIBLE_WALL)
    {
        groups[0].identifier = "empty";
    }

    if (collision == eMP_WATER)
    {
        // A water sheet is a single planar layer: Blitz stores it one-sided with the
        // winding flipped to match the vertex normals, so a camera above it sees the
        // back faces and the sheet vanishes. Draw water from both sides.
        for (size_t i = 0; i < groups.size(); ++i)
        {
            groups[i].twoSided = true;
        }
    }

    Build();
    BinaryPtr binary = Recollect();

    File* f = File::Create("out\\content\\" + mapName + "\\" + modelName + ".primitives", File::CREATE | File::WRITE);

    f->Write(&(binary->operator[](0)), binary->size());
    SafeRelease(f);
    delete binary;

    char content[8192] = {};
    char result[8192] = {};
    char primitiveGroup[8192] = {};
    char nodes[8192];

    ReadBuffer(content, sizeof(content), "~res:/modelTemplate\\template.model");

    DVASSERT(!bbox.insideOut(),
             Format("object %s on map %s is probably without geometry", modelName.c_str(), mapName.c_str()).c_str());

    const Vector3& min = bbox.minBounds();
    const Vector3& max = bbox.maxBounds();

    // A water sheet has exactly zero thickness, and a zero-height box is what BW's
    // chunk culler drops. Give the XML boxes half a metre of slack so the sheet is
    // never treated as degenerate. The mesh, the BSP and the transform are untouched.
    Vector3 boxMin = min;
    Vector3 boxMax = max;
    if (collision == eMP_WATER && (max.y - min.y) < 0.5f)
    {
        boxMin.y -= 0.5f;
        boxMax.y += 0.5f;
        DAVA::Logger::Info("[TerrainConverter] '%s' is a flat water sheet - visibility box grown by 0.5 m",
                           modelName.c_str());
    }

    String nodeType;

    if (IsFragile(collision))
    {
        nodeType = "nodefullVisual";
    }
    else
    {
        nodeType = "nodelessVisual";
    }

    Snprintf(result, sizeof(result), content, modelName.c_str(), nodeType.c_str(),
             (CleanResPath("content/" + mapName + "/" + modelName)).c_str(), nodeType.c_str(),
             boxMin.x, boxMin.y, boxMin.z, boxMax.x, boxMax.y, boxMax.z,
             modelName.c_str());
    f = File::Create("out\\content\\" + mapName + "\\" + modelName + ".model", File::CREATE | File::WRITE);

    f->Write(result, (uint32)strlen(result));
    SafeRelease(f);

    String buf;

    if (collision == eMP_INVISIBLE_WALL)
    {
        ReadBuffer(primitiveGroup, sizeof(primitiveGroup), "~res:/modelTemplate\\invisible.primitiveGroup");

        Snprintf(result,
                 sizeof(result),
                 primitiveGroup,
                 groups[0].identifier.c_str(),
                 GetBWCollision(0),
                 GetBWMaterialKind(0));

        buf = buf + result;
    }
    else
    {
        ReadBuffer(primitiveGroup, sizeof(primitiveGroup), "~res:/modelTemplate\\template.primitiveGroup");

        for (uint32 i = 0; i < groups.size(); i++)
        {
            Snprintf(result,
                     sizeof(result),
                     primitiveGroup,
                     i,
                     groups[i].identifier.c_str(),
                     GetBWCollision(i),
                     GetBWMaterialKind(i),
                     (groups[i].hasAlpha ? "true" : "false"),
                     (groups[i].twoSided ? "true" : "false"),
                     // Res rooted: ExportBwTexture writes content/<map>/images/<name>, so the
                     // diffuseMap must name that same file or BW loads nothing.
                     CleanResPath("content/" + MapDir(mapName) + "images/" + groups[i].material).c_str());

            // The template only carries the unlit effect.  A batch with a normal
            // map is switched to BW's normal mapping effect and gets the extra
            // normalMap property, everything else keeps the template as it is.
            // lightonly.fx draws baked lighting only and nothing else, so every material
            // that uses it renders as a black silhouette. Everything with a diffuse map
            // goes through the normal mapping effect; without a real normal map it gets a
            // shared flat one so the effect still has a bound texture.
            const String flatNormal = CleanResPath("content/" + MapDir(mapName) + "images/flat_n.dds");
            if (!g_flatNormalWritten)
            {
                // The images folder may not exist yet on the first model, and a failed write
                // used to set the guard anyway - the whole export then referenced a flat_n.dds
                // that was never on disk, so BW loaded none of those materials and every prop
                // batch showed its missing-texture marker.
                //
                // NOTE the two different paths: the .visual wants the res-rooted name, but the
                // file itself has to be created under "out\\content\\<map>\\images\\".
                // Handing the res path to File::Create is why flat_n.dds kept coming out absent.
                const String flatNormalDisk = "out\\content\\" + MapDir(mapName) + "images\\flat_n.dds";
                FileSystem::Instance()->CreateDirectory(FilePath("out\\content\\" + MapDir(mapName) + "images\\"), true);
                if (WriteFlatNormalDds(FilePath(flatNormalDisk)))
                {
                    g_flatNormalWritten = true;
                }
                else
                {
                    DAVA::Logger::Warning("[TerrainConverter] could not write '%s' - materials that need it will show BW's missing texture",
                                          flatNormal.c_str());
                }
            }

            String groupXml = result;
            {
                String::size_type fxPos = groupXml.find("lightonly.fx");
                while (fxPos != String::npos)
                {
                    groupXml.replace(fxPos, 11, "normalmap.fx");
                    fxPos = groupXml.find("normalmap.fx", fxPos + 11);
                }

                const String normalProperty = String("\t\t\t\t\t<property>\tnormalMap\r\n"
                                                     "\t\t\t\t\t\t<Texture>\t") +
                // group->normal is already the full res path returned by
                // ExportBwTexture ("content/<map>/images/<name>_n.dds"): prefixing
                // it again produced rudniki/images/content/rudniki/images/...
                (groups[i].normal.empty() ? flatNormal : CleanResPath(groups[i].normal)) + "\t</Texture>\r\n"
                                                                                           "\t\t\t\t\t</property>\r\n"
                                                                                           "\t\t\t\t";
                const String::size_type closePos = groupXml.rfind("</material>");
                if (closePos != String::npos)
                {
                    groupXml.insert(closePos, normalProperty);
                }
                else
                {
                    DAVA::Logger::Warning("[TerrainConverter] template has no </material> anchor, normal map of '%s' skipped",
                                          groups[i].identifier.c_str());
                }
            }

            buf = buf + groupXml;
        }
    }

    ReadBuffer(content, sizeof(content), "~res:/modelTemplate\\template.visual");

    if (IsFragile(collision)) // destructible object!
    {
        ReadBuffer(nodes, sizeof(nodes), "~res:/modelTemplate\\nodes.xml");
    }
    else
    {
        nodes[0] = 0;
    }

    Snprintf(result, sizeof(result), content, modelName.c_str(), nodes,
             buf.c_str(), boxMin.x, boxMin.y, boxMin.z, boxMax.x, boxMax.y, boxMax.z, modelName.c_str());

    f = File::Create("out\\content\\" + mapName + "\\" + modelName + ".visual", File::CREATE | File::WRITE);

    f->Write(result, (uint32)strlen(result));
    SafeRelease(f);

    PrepareDestructible();
} // Model::Save

DAVA::String Model::GetPhysicsParams(DAVA::int32 fallType_)
{
    static char* fallParams[4] =
    //  {"800 10 0.26 20000 4000 20 0.15",
    //  "%.1f %.1f 0.03 300000 10000 0 0.1",
    //  "%.1f %.1f 0.03 300000 10000 0 0.1",
    //  "800 10 0.26 20000 4000 20 0.15"};
    {
        (char*)"800 10 0.26 20000 4000 20 0.15",
        (char*)"400 10 0.03 300000 10000 0 0.1",
        (char*)"400 2 0.03 300000 10000 0 0.02",
        (char*)"800 10 0.26 20000 4000 20 0.15"
    };
    // �����, �����, ���� �������, ���������, ������������� �������, ������������� �������, ���������� � �����

    //  if (fallType == eFT_STOLB || fallType == eFT_PALMA)
    //  {
    //      float32 h = bbox.height();
    //
    //      float32 mass1 = 400.0f; //kg
    //      float32 h1 = 10.0f; //meters
    //
    //      // mass = h ^ 3 * mass1 / h1^3
    //
    //      float k = h / h1;
    //      float32 mass = k * k * k * mass1;
    //
    //      char result[128];
    //      Snprintf( result, sizeof(result), fallParams[fallType], mass, h);
    //      return String(result);
    //  }
    //  else
    return DAVA::String(fallParams[fallType_]);
}

void Model::PrepareDestructible()
{
    using namespace DAVA;

    if (collision == eMP_NO_COLLISION || collision == eMP_BUILDING)
    {
        return;
    }

    char content[8192] = {};
    char result[8192] = {};

    String param = GetPhysicsParams(fallType);
    String path = "out\\";
    if (collision == eMP_TREE || collision == eMP_BUSH)
    {
        path = "out\\tree\\";
        ReadBuffer(content, sizeof(content), "~res:/modelTemplate\\destructibleTree.xml");
        Snprintf(result, sizeof(result), content,
                 (CleanResPath("content/" + mapName + "/" + modelName + ".model")).c_str(), health, density, param.c_str());
    }
    else if (collision == eMP_FALLING_ATOM)
    {
        path = "out\\falling\\";
        ReadBuffer(content, sizeof(content), "~res:/modelTemplate\\destructibleFalling.xml");
        Snprintf(result, sizeof(result), content, (CleanResPath("content/" + mapName + "/" + modelName + ".model")).c_str(), health, param.c_str());
    }
    else if (collision == eMP_FRAGILE_PR || collision == eMP_FRAGILE_NPR)
    {
        path = "out\\destructible\\";
        ReadBuffer(content, sizeof(content), "~res:/modelTemplate\\destructible.xml");
        Snprintf(result, sizeof(result), content, (CleanResPath("content/" + mapName + "/" + modelName + ".model")).c_str(), health,
                 materialKind.c_str());
    }

    File* f = File::Create(path + modelName + ".xml", File::CREATE | File::WRITE);
    f->Write(result, (uint32)strlen(result));
    SafeRelease(f);
}

void Model::Build(void)
{
    using namespace DAVA;

    // vertices
    DataSection* vert = new DataSection("vertices");
    int32 dataSize = sizeof(VertexXYZNUV) * vertices.size();
    vert->SetBinarySize(sizeof(VertexHeader) + dataSize);
    void* buff = vert->GetBuffer();

    VertexHeader* vh = (VertexHeader*)buff;
    vh->nVertices_ = vertices.size();
    Snprintf(vh->vertexFormat_, sizeof(vh->vertexFormat_), "xyznuv");
    ::memcpy((void*)(vh + 1), &vertices[0], dataSize);
    children_.push_back(vert);

    // indices
    DataSection* prim = new DataSection("indices");
    dataSize = sizeof(IndexValue) * primitive.size();
    prim->SetBinarySize(sizeof(IndexHeader) + dataSize + sizeof(PrimitiveGroup) * groups.size());
    buff = prim->GetBuffer();

    IndexHeader* ih = (IndexHeader*)buff;
    ih->nIndices_ = primitive.size();
    Snprintf(ih->indexFormat_, sizeof(ih->indexFormat_), "list");
    ih->nTriangleGroups_ = groups.size();
    ::memcpy((void*)(ih + 1), &primitive[0], dataSize);

    PrimitiveGroup* group = (PrimitiveGroup*)((uint8*)buff + sizeof(IndexHeader) + dataSize);
    for (uint32 i = 0; i < groups.size(); i++)
    {
        *group = groups[i].group;
        group++;
    }
    children_.push_back(prim);

    // bsp2
    if (pBSP == nullptr || tris.empty())
    {
        DAVA::Logger::Warning("[TerrainConverter] model '%s': no BSP triangles, bsp2 section omitted - the object loads "
                              "without collision instead of tripping BW's bsp node check",
                              modelName.c_str());
    }
    else
    {
        DataSection* bsp2 = new DataSection("bsp2");
        const bool saved = pBSP->save(bsp2);
        if (saved == false)
        {
            DAVA::Logger::Warning("[TerrainConverter] model '%s': BSPTree::save failed, bsp2 section omitted",
                                  modelName.c_str());
            delete bsp2;
        }
        else
        {
            // BW 2.0.1's loader (physics2/bsp.cpp loadBSPCurrent) reads
            //   7 * int32 header | BoundingBox (32 bytes) | triangles 40 each |
            //   shared offsets 4 each | nodes 40 each | user data
            // and then calls BSPTree::validate(), which asserts per node that
            // pTriangles_ is NULL exactly when numTriangles_ is 0. Dava's save
            // already produces the right layout - its BoundingBox is the same
            // struct BW uses (AABB min_/max_ plus two mutable Outcode uint32),
            // so it is also 32 bytes - but it leaves nodes that own no
            // triangles with a triangle offset instead of -1, which is what
            // trips the assert. Those are normalised here, in place, so the
            // section keeps its exact size.
            BinaryPtr raw = bsp2->asBinary();
            const char* src = (const char*)raw->data();
            const int32 srcLen = (int32)raw->size();

            int32 numTris = 0;
            int32 numNodes = 0;
            int32 numShared = 0;
            int32 nodeStart = 0;
            bool usable = false;
            if (src != nullptr && srcLen >= 28 + 32)
            {
                int32 header[7] = { 0, 0, 0, 0, 0, 0, 0 };
                memcpy(header, src, sizeof(header));
                numTris = header[4];
                numNodes = header[5];
                numShared = header[6];
                const int32 boundsSize = srcLen - 28 - (40 * numTris + 4 * numShared + 40 * numNodes);
                nodeStart = 28 + boundsSize + 40 * numTris + 4 * numShared;
                usable = numTris >= 0 && numNodes > 0 && numShared >= 0 && boundsSize >= 32 && nodeStart >= 0 &&
                nodeStart + 40 * numNodes <= srcLen;
            }

            if (usable == false)
            {
                DAVA::Logger::Warning("[TerrainConverter] model '%s': bsp2 section is not a usable BW bsp, omitted",
                                      modelName.c_str());
                delete bsp2;
            }
            else
            {
                std::vector<char> fixed((size_t)srcLen, 0);
                memcpy(fixed.data(), src, (size_t)srcLen);

                int32 repaired = 0;
                for (int32 i = 0; i < numNodes; ++i)
                {
                    char* n = &fixed[(size_t)(nodeStart + 40 * i)];

                    int32 front = 0;
                    int32 back = 0;
                    memcpy(&front, n, 4);
                    memcpy(&back, n + 4, 4);
                    if (front < -1 || front >= numNodes)
                    {
                        front = -1;
                        memcpy(n, &front, 4);
                    }
                    if (back < -1 || back >= numNodes)
                    {
                        back = -1;
                        memcpy(n + 4, &back, 4);
                    }

                    int32 triOffset = 0;
                    int32 triCount = 0;
                    memcpy(&triOffset, n + 24, 4);
                    memcpy(&triCount, n + 28, 4);
                    if (triCount <= 0 || triOffset < 0 || triOffset + triCount > numTris)
                    {
                        if (triCount > 0)
                        {
                            DAVA::Logger::Warning("[TerrainConverter] model '%s': bsp node %d triangle range %d+%d is "
                                                  "outside the %d stored triangles, cleared",
                                                  modelName.c_str(), (int32)i, (int32)triOffset, (int32)triCount,
                                                  (int32)numTris);
                        }
                        if (triCount != 0 || triOffset != -1)
                        {
                            ++repaired;
                        }
                        triOffset = -1;
                        triCount = 0;
                        memcpy(n + 24, &triOffset, 4);
                        memcpy(n + 28, &triCount, 4);
                    }

                    int32 sharedOffset = 0;
                    int32 sharedCount = 0;
                    memcpy(&sharedOffset, n + 32, 4);
                    memcpy(&sharedCount, n + 36, 4);
                    if (sharedCount <= 0 || sharedOffset < 0 || sharedOffset + sharedCount > numShared)
                    {
                        if (sharedCount > 0)
                        {
                            DAVA::Logger::Warning("[TerrainConverter] model '%s': bsp node %d shared range %d+%d is "
                                                  "outside the %d stored pointers, cleared",
                                                  modelName.c_str(), (int32)i, (int32)sharedOffset, (int32)sharedCount,
                                                  (int32)numShared);
                        }
                        if (sharedCount != 0 || sharedOffset != -1)
                        {
                            ++repaired;
                        }
                        sharedOffset = -1;
                        sharedCount = 0;
                        memcpy(n + 32, &sharedOffset, 4);
                        memcpy(n + 36, &sharedCount, 4);
                    }
                }

                if (repaired > 0)
                {
                    DAVA::Logger::Info("[TerrainConverter] model '%s': bsp2 %d triangles in %d nodes, %d node(s) "
                                       "normalised for BW",
                                       modelName.c_str(), (int32)numTris, (int32)numNodes, (int32)repaired);
                }

                memcpy(bsp2->GetBuffer(), fixed.data(), fixed.size());
                children_.push_back(bsp2);
            }
        }
    }

    // bsp2_materials
    DataSection* bspMat = new DataSection("bsp2_materials");
    String val = "<temp_bsp_materials.xml>";
    for (uint32 i = 0; i < groups.size(); i++)
    {
        val += "<id>" + groups[i].identifier + "</id>";
    }
    val += "</temp_bsp_materials.xml>";
    bspMat->SetBinarySize(val.size() + 1);
    buff = bspMat->GetBuffer();
    Snprintf((char*)buff, val.size() + 1, "%s", val.c_str());
    children_.push_back(bspMat);
} // Model::Build

void Model::CreatePolygonGroup()
{
    MaterialPrimitiveGroup group;

    group.twoSided = true;
    group.hasAlpha = true;
    group.matCollision = eMP_NO_COLLISION;

    group.group.startIndex_ = primitive.size();
    group.group.nPrimitives_ = 0;
    group.group.startVertex_ = vertices.size();
    group.group.nVertices_ = 0;
    groups.push_back(group);
}

void Model::AddGeometry(DAVA::Entity* entity, DAVA::RenderObject* renderObject, DAVA::int32 requestedLodIndex, DAVA::int32 requestedSwitchIndex)
{
    using namespace DAVA;

    // Exports one texture of the current model as a BW loadable DDS and returns
    // the resource name the .visual has to reference ("" when nothing written).
    // Shipped Blitz art has no source images, only .tex descriptors plus per GPU
    // containers, and BW needs a file it can load, so non DDS is re-encoded.
    auto ExportBwTexture = [](const FilePath& descriptorPath, const String& bwName, const String& targetMap) -> String
    {
        if (descriptorPath.IsEmpty())
            return String();

        const String outDirString = "out\\content\\" + targetMap + "\\images\\";
        FileSystem::Instance()->CreateDirectory(FilePath(outDirString), true);
        const FilePath target(outDirString + bwName);

        // A material has to name a file BW can actually load. Anything that can not
        // be exported becomes a placeholder instead of a dangling reference:
        // MaterialTextureProxy::set() would otherwise hand the texture manager an
        // image that failed to load, and evicting that invalid handle asserts.
        auto ensureLoadable = [&](const char* why) -> String
        {
            String err;
            if (WriteBwPlaceholderDds(target, err))
            {
                DAVA::Logger::Warning("[TerrainConverter] texture '%s' written as a placeholder (%s)",
                                      bwName.c_str(), why);
                return CleanResPath("content/" + targetMap + "/images/" + bwName);
            }
            DAVA::Logger::Warning("[TerrainConverter] texture '%s' has no usable image (%s) and the placeholder failed "
                                  "(%s) - the .visual will reference a missing file",
                                  bwName.c_str(), why, err.c_str());
            return String();
        };

        FilePath source;

        // A material can point at a descriptor the editor cannot even open (~res: not
        // mounted, stub missing), and then the old code went straight to the grey
        // placeholder. Both fallbacks below are therefore independent of the descriptor.
        auto findSibling = [](const FilePath& descPath) -> FilePath
        {
            // Blitz names its containers with a dot (bld703_vhouse.dx11.dds) while the
            // engine builds the name without one, so probe the descriptor's own folder.
            const FilePath dir = descPath.GetDirectory();
            const String stem = descPath.GetBasename();
            if (stem.empty() || !FileSystem::Instance()->IsDirectory(dir))
            {
                return FilePath();
            }
            String stemLower = stem;
            for (size_t c = 0; c < stemLower.size(); ++c)
            {
                stemLower[c] = static_cast<char>(tolower(static_cast<unsigned char>(stemLower[c])));
            }
            Vector<FilePath> siblings = FileSystem::Instance()->EnumerateFilesInDirectory(dir, false);
            FilePath best;
            int32 bestRank = 1000;
            for (uint32 i = 0; i < siblings.size(); ++i)
            {
                const String name = siblings[i].GetFilename();
                if (name.size() <= stemLower.size() || name[stemLower.size()] != '.')
                {
                    continue;
                }
                String head = name.substr(0, stemLower.size());
                for (size_t c = 0; c < head.size(); ++c)
                {
                    head[c] = static_cast<char>(tolower(static_cast<unsigned char>(head[c])));
                }
                if (head != stemLower)
                {
                    continue;
                }
                const String ext = siblings[i].GetExtension();
                int32 rank = 9;
                if (ext == ".dds")
                {
                    rank = 0;
                }
                else if (ext == ".tga")
                {
                    rank = 1;
                }
                else if (ext == ".png")
                {
                    rank = 2;
                }
                else if (ext == ".pvr" || ext == ".webp")
                {
                    rank = 3;
                }
                else
                {
                    continue;
                }
                if (rank < bestRank)
                {
                    bestRank = rank;
                    best = siblings[i];
                }
            }
            return best;
        };

        auto findInDataSource = [](const String& stemName) -> FilePath
        {
            if (stemName.empty() || g_bwTextureSearchRoot.empty())
            {
                return FilePath();
            }
            DAVA::Map<DAVA::String, DAVA::String>::iterator cached = g_textureSearchCache.find(stemName);
            if (cached != g_textureSearchCache.end())
            {
                return cached->second.empty() ? FilePath() : FilePath(cached->second);
            }

            String stem = stemName;
            const String::size_type dot = stem.find_last_of('.');
            if (dot != String::npos)
            {
                stem = stem.substr(0, dot);
            }
            String stemLower = stem;
            for (size_t c = 0; c < stemLower.size(); ++c)
            {
                stemLower[c] = static_cast<char>(tolower(static_cast<unsigned char>(stemLower[c])));
            }

            // The DataSource is NOT laid out as 3d/Maps/<map> for everything: on the
            // Blitz dump the same texture lives in "buildings/images" or "objects/images"
            // directly under the root. So search the root itself, recursively.
            FilePath best;
            const char* roots[3] = { "", "3d\\", "3d\\Maps\\" };
            for (int32 r = 0; r < 3 && best.IsEmpty(); ++r)
            {
                FilePath rootDir(g_bwTextureSearchRoot + "\\" + roots[r]);
                if (!FileSystem::Instance()->IsDirectory(rootDir))
                {
                    continue;
                }
                Vector<FilePath> found = FileSystem::Instance()->EnumerateFilesInDirectory(rootDir, true);
                int32 bestRank = 1000;
                for (uint32 f = 0; f < found.size(); ++f)
                {
                    const String name = found[f].GetFilename();
                    if (name.size() <= stemLower.size() || name[stemLower.size()] != '.')
                    {
                        continue;
                    }
                    String head = name.substr(0, stemLower.size());
                    for (size_t c = 0; c < head.size(); ++c)
                    {
                        head[c] = static_cast<char>(tolower(static_cast<unsigned char>(head[c])));
                    }
                    if (head != stemLower)
                    {
                        continue;
                    }
                    const String ext = found[f].GetExtension();
                    int32 rank = 9;
                    if (ext == ".dds")
                    {
                        rank = 0;
                    }
                    else if (ext == ".tga")
                    {
                        rank = 1;
                    }
                    else if (ext == ".png")
                    {
                        rank = 2;
                    }
                    else if (ext == ".pvr" || ext == ".webp")
                    {
                        rank = 3;
                    }
                    else
                    {
                        continue;
                    }
                    if (rank < bestRank)
                    {
                        bestRank = rank;
                        best = found[f];
                    }
                }
            }
            g_textureSearchCache[stemName] = best.IsEmpty() ? String() : best.GetAbsolutePathname();
            return best;
        };

        TextureDescriptor* descriptor = TextureDescriptor::CreateFromFile(descriptorPath);
        if (descriptor != nullptr)
        {
            source = descriptor->GetSourceTexturePathname();
            if (source.IsEmpty() || !FileSystem::Instance()->IsFile(source))
            {
                const eGPUFamily gpuOrder[] = {
                    GPU_ORIGIN, GPU_DX11, GPU_POWERVR_IOS, GPU_POWERVR_ANDROID, GPU_TEGRA, GPU_MALI, GPU_ADRENO
                };
                source = FilePath();
                for (int32 g = 0; g < (int32)(sizeof(gpuOrder) / sizeof(gpuOrder[0])); ++g)
                {
                    FilePath candidate = descriptor->CreateMultiMipPathnameForGPU(gpuOrder[g]);
                    if (!candidate.IsEmpty() && FileSystem::Instance()->IsFile(candidate))
                    {
                        source = candidate;
                        break;
                    }
                }
            }
            delete descriptor; // TextureDescriptor is a plain class, never SafeRelease it
        }
        else
        {
            DAVA::Logger::Warning("[TerrainConverter] can not read descriptor '%s' - falling back to a name search",
                                  descriptorPath.GetAbsolutePathname().c_str());
        }

        if (source.IsEmpty() || !FileSystem::Instance()->IsFile(source))
        {
            // Fastest and most reliable lookup: the Blitz dump keeps the runtime container
            // right next to its .tex stub (buildings/images/bld703_vhouse.tex +
            // buildings/images/bld703_vhouse.dx11.dds). A handful of IsFile probes beat
            // any directory walk and they work wherever the DataSource root points.
            static const char* const containerTails[] = {
                ".dx11.dds", ".dds", ".dx11.pvr", ".pvr", ".dx11.ktx", ".tga", ".png", ".webp"
            };
            const String stubStem = descriptorPath.GetBasename();

            // Two bases to try: the stub's own folder as the engine sees it, and the same
            // folder re-rooted on the DataSource setting. A "~res:/..." path only resolves
            // when that folder happens to be mounted, which is not guaranteed while models
            // are being exported.
            String bases[2];
            bases[0] = descriptorPath.GetDirectory().GetAbsolutePathname();
            bases[1] = bases[0];
            {
                String raw = descriptorPath.GetAbsolutePathname();
                String rel = raw;
                if (rel.size() > 6 && rel.compare(0, 6, "~res:/") == 0)
                {
                    rel = rel.substr(6);
                }
                if (!g_bwTextureSearchRoot.empty())
                {
                    bases[1] = g_bwTextureSearchRoot;
                    if (bases[1][bases[1].size() - 1] != '\\' && bases[1][bases[1].size() - 1] != '/')
                    {
                        bases[1] += "\\";
                    }
                    bases[1] += rel.substr(0, rel.size() - stubStem.size());
                }
            }

            DAVA::String tried;
            for (DAVA::uint32 b = 0; b < 2 && source.IsEmpty(); ++b)
            {
                if (bases[b].empty())
                {
                    continue;
                }
                for (DAVA::uint32 t = 0; t < sizeof(containerTails) / sizeof(containerTails[0]); ++t)
                {
                    const DAVA::String candidatePath = bases[b] + stubStem + containerTails[t];
                    tried += candidatePath + " ";
                    if (FileSystem::Instance()->IsFile(DAVA::FilePath(candidatePath)))
                    {
                        source = DAVA::FilePath(candidatePath);
                        DAVA::Logger::Info("[TerrainConverter] texture '%s': container next to the stub '%s'",
                                           bwName.c_str(), source.GetAbsolutePathname().c_str());
                        break;
                    }
                }
            }
            if (source.IsEmpty())
            {
                DAVA::Logger::Warning("[TerrainConverter] texture '%s': no container next to the stub, tried: %s",
                                      bwName.c_str(), tried.c_str());
            }
        }
        if (source.IsEmpty() || !FileSystem::Instance()->IsFile(source))
        {
            source = findSibling(descriptorPath);
            if (!source.IsEmpty())
            {
                DAVA::Logger::Info("[TerrainConverter] texture '%s': sibling container '%s'",
                                   bwName.c_str(), source.GetFilename().c_str());
            }
        }
        if (source.IsEmpty() || !FileSystem::Instance()->IsFile(source))
        {
            source = findInDataSource(descriptorPath.GetBasename());
            if (!source.IsEmpty())
            {
                DAVA::Logger::Info("[TerrainConverter] texture '%s': found in the DataSource '%s'",
                                   bwName.c_str(), source.GetAbsolutePathname().c_str());
            }
        }

        if (source.IsEmpty())
        {
            DAVA::Logger::Warning("[TerrainConverter] no image for '%s' (searched source path and every GPU "
                                  "container)",
                                  bwName.c_str());
            return ensureLoadable("no source image");
        }

        if (source.IsEqualToExtension(".dds"))
        {
            // Stream copy, not FileSystem::CopyFile: the resolved source is a "~res:/..."
            // path, and CopyFile goes to the OS with it, which fails for every model
            // texture (the log said "copy failed" 17 times and nowhere else). File::Create
            // resolves the resource path the same way IsFile did.
            bool copied = false;
            {
                File* in = File::Create(source, File::OPEN | File::READ);
                if (in != nullptr)
                {
                    File* out = File::Create(target, File::CREATE | File::WRITE);
                    if (out != nullptr)
                    {
                        static uint8 copyBuffer[65536];
                        uint32 got = 0;
                        while ((got = in->Read(copyBuffer, (uint32)sizeof(copyBuffer))) > 0)
                        {
                            out->Write(copyBuffer, got);
                        }
                        SafeRelease(out);
                        copied = true;
                    }
                    SafeRelease(in);
                }
            }
            if (!copied)
            {
                return ensureLoadable("copy failed");
            }

            // Already a container: BW reads DXT natively, but only with a legacy
            // header. DX10 headers (BC1/BC3/BC5 as modern tools write them) are
            // rewritten in place; containers BW can not read at all are replaced.
            String reason;
            if (RewriteDx10DdsForBw(target, bwName, reason) == false)
            {
                // Do not give up on a container we can decode: re-encode it as plain BGRA.
                // Falling straight through to the placeholder is what left 16 props grey
                // even though their source DDS was a perfectly ordinary DXT1.
                DAVA::Logger::Warning("[TerrainConverter] '%s' not usable as-is (%s) - re-encoding",
                                      bwName.c_str(), reason.c_str());
                Vector<Image*> loaded;
                ImageSystem::Load(source, loaded);
                if (!loaded.empty())
                {
                    Image* src = loaded[0];
                    Image* dst = nullptr;
                    if (src->GetPixelFormat() != FORMAT_RGBA8888 && ImageConvert::CanConvertFromTo(src->GetPixelFormat(), FORMAT_RGBA8888))
                    {
                        dst = Image::Create(src->GetWidth(), src->GetHeight(), FORMAT_RGBA8888);
                        ImageConvert::ConvertImage(src, dst);
                        src = dst;
                    }
                    bool ok = false;
                    if (src != nullptr && src->GetPixelFormat() == FORMAT_RGBA8888)
                    {
                        const uint32 w = src->GetWidth();
                        const uint32 h = src->GetHeight();
                        const uint32 bytes = 4 + 124 + w * h * 4;
                        uint8* ddsOut = new uint8[bytes];
                        memset(ddsOut, 0, bytes);
                        const uint32 magic = 0x20534444;
                        memcpy(ddsOut, &magic, 4);
                        uint32* hh = (uint32*)(ddsOut + 4);
                        hh[0] = 124;
                        hh[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000;
                        hh[2] = h;
                        hh[3] = w;
                        hh[4] = w * 4;
                        hh[18] = 32;
                        hh[19] = 0x1 | 0x40;
                        hh[21] = 32;
                        hh[22] = 0x00ff0000;
                        hh[23] = 0x0000ff00;
                        hh[24] = 0x000000ff;
                        hh[25] = 0xff000000;
                        hh[26] = 0x1000;
                        const uint8* pixelsIn = src->GetData();
                        uint8* outPixels = ddsOut + 4 + 124;
                        for (uint32 p = 0; p < w * h; ++p)
                        {
                            outPixels[p * 4 + 0] = pixelsIn[p * 4 + 2];
                            outPixels[p * 4 + 1] = pixelsIn[p * 4 + 1];
                            outPixels[p * 4 + 2] = pixelsIn[p * 4 + 0];
                            outPixels[p * 4 + 3] = pixelsIn[p * 4 + 3];
                        }
                        File* out = File::Create(target, File::CREATE | File::WRITE);
                        if (out != nullptr)
                        {
                            out->Write(ddsOut, bytes);
                            SafeRelease(out);
                            ok = true;
                        }
                        delete[] ddsOut;
                    }
                    SafeRelease(dst);
                    for (uint32 k = 0; k < loaded.size(); ++k)
                    {
                        SafeRelease(loaded[k]);
                    }
                    if (ok)
                    {
                        DAVA::Logger::Info("[TerrainConverter] texture '%s' re-encoded from '%s'",
                                           bwName.c_str(), source.GetAbsolutePathname().c_str());
                        return CleanResPath("content/" + targetMap + "/images/" + bwName);
                    }
                }
                return ensureLoadable(reason.c_str());
            }
            return CleanResPath("content/" + targetMap + "/images/" + bwName);
        }

        Vector<Image*> images;
        ImageSystem::Load(source, images);
        if (images.empty())
        {
            DAVA::Logger::Warning("[TerrainConverter] can not load image '%s'", source.GetAbsolutePathname().c_str());
            return ensureLoadable("image unreadable");
        }

        Image* image = images[0]; // one level is enough, BW builds its own mip chain
        Image* converted = nullptr;
        if (image->GetPixelFormat() != FORMAT_RGBA8888)
        {
            if (ImageConvert::CanConvertFromTo(image->GetPixelFormat(), FORMAT_RGBA8888))
            {
                converted = Image::Create(image->GetWidth(), image->GetHeight(), FORMAT_RGBA8888);
                ImageConvert::ConvertImage(image, converted);
                image = converted;
            }
            else
            {
                DAVA::Logger::Warning("[TerrainConverter] image format %d of '%s' can not be converted to RGBA8888",
                                      (int32)images[0]->GetPixelFormat(), source.GetAbsolutePathname().c_str());
                image = nullptr;
            }
        }

        bool written = false;
        if (image != nullptr)
        {
            const uint32 width = image->GetWidth();
            const uint32 height = image->GetHeight();
            const uint32 pixelCount = width * height;
            const uint32 dataSize = 4 + 124 + pixelCount * 4;

            uint8* dds = new uint8[dataSize];
            memset(dds, 0, dataSize);

            const uint32 magic = 0x20534444; // "DDS "
            memcpy(dds, &magic, 4);

            uint32* h = (uint32*)(dds + 4);
            h[0] = 124; // dwSize
            h[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000; // caps | height | width | pitch | pixelformat
            h[2] = height;
            h[3] = width;
            h[4] = width * 4; // pitch
            h[18] = 32; // pf size
            h[19] = 0x1 | 0x40; // pf flags: alphapixels | rgb
            h[20] = 0; // pf fourCC: 0, this is an RGB layout
            h[21] = 32; // bit count
            h[22] = 0x00ff0000; // R mask
            h[23] = 0x0000ff00; // G mask
            h[24] = 0x000000ff; // B mask
            h[25] = 0xff000000; // A mask
            h[26] = 0x1000; // caps: texture

            const uint8* src = image->GetData();
            uint8* pixels = dds + 4 + 124;
            for (uint32 p = 0; p < pixelCount; ++p)
            {
                pixels[p * 4 + 0] = src[p * 4 + 2];
                pixels[p * 4 + 1] = src[p * 4 + 1];
                pixels[p * 4 + 2] = src[p * 4 + 0];
                pixels[p * 4 + 3] = src[p * 4 + 3];
            }

            File* out = File::Create(target, File::CREATE | File::WRITE);
            if (out != nullptr)
            {
                out->Write(dds, dataSize);
                SafeRelease(out);
                written = true;
                DAVA::Logger::Info("[TerrainConverter] texture '%s' -> '%s'", source.GetAbsolutePathname().c_str(),
                                   target.GetAbsolutePathname().c_str());
            }
            else
            {
                DAVA::Logger::Warning("[TerrainConverter] can not write texture '%s'", target.GetAbsolutePathname().c_str());
            }

            delete[] dds;
        }

        SafeRelease(converted);
        for (uint32 k = 0; k < images.size(); ++k)
        {
            SafeRelease(images[k]);
        }

        if (written == false)
        {
            return ensureLoadable("image could not be re-encoded");
        }
        return CleanResPath("content/" + targetMap + "/images/" + bwName);
    };

    Matrix4 createdWith = GetEntityWorldTransform(entity) * globalTransform;
    Matrix4 uniformMatrixNormal;
    createdWith.GetInverse(uniformMatrixNormal);
    uniformMatrixNormal.Transpose();
    Matrix3 matrixNormal = uniformMatrixNormal;

    VertexXYZNUV v;
    int32 index;

    MaterialPrimitiveGroup* group = &groups[groups.size() - 1];

    uint32 usedBatchIndex = 0;
    uint32 meshesSize = renderObject->GetRenderBatchCount();
    for (uint32 k = 0; k < meshesSize; ++k)
    {
        int32 lodIndex = -1, switchIndex = -1;
        RenderBatch* batch = renderObject->GetRenderBatch(k, lodIndex, switchIndex);

        bool isProperLodLayer = ((requestedLodIndex == lodIndex) || (requestedLodIndex == -1)); // -1 means that we
        // don't interested in
        // lod layer
        bool isProperSwitchIndex = ((switchIndex == requestedSwitchIndex) || (requestedSwitchIndex == -1)); // -1 means
        // that we
        // don't
        // interested
        // in switch
        // index
        if (!isProperLodLayer || !isProperSwitchIndex)
        {
            continue; // select only batches for proper lod and switch indexes
        }

        PolygonGroup* pg = batch->GetPolygonGroup();
        NMaterial* mat = batch->GetMaterial();

        // BW-compliance patch: extracted/rebuilt scenes contain batches with
        // empty or unloaded polygon groups (placeholder meshes). Feeding those
        // to PolygonGroup::GetCoord asserts (base == nullptr / baseOffset < 0).
        // Skip any batch that has no usable geometry instead of aborting.
        if (pg == nullptr || mat == nullptr ||
            pg->GetVertexCount() <= 0 || pg->GetPrimitiveCount() <= 0)
        {
            DAVA::Logger::Warning("[TerrainConverter] render batch %u of '%s' has no usable geometry"
                                  " (pg=0x%p mat=0x%p vertices=%d prims=%d) - skipped",
                                  k, entity->GetName().c_str(),
                                  (void*)pg, (void*)mat,
                                  (pg != nullptr) ? pg->GetVertexCount() : 0,
                                  (pg != nullptr) ? pg->GetPrimitiveCount() : 0);
            continue;
        }

        if (usedBatchIndex > 0 && collision == eMP_TREE)
        {
            CreatePolygonGroup();
            group = &groups[groups.size() - 1];
        }
        ++usedBatchIndex;

        group->twoSided = false;
        group->hasAlpha = false;

        // ---- BW material and texture export --------------------------------
        FilePath albedoPath;
        Texture* effectiveAlbedo = mat->GetEffectiveTexture(NMaterialTextureName::TEXTURE_ALBEDO);
        if (effectiveAlbedo != nullptr && effectiveAlbedo->texDescriptor != nullptr)
        {
            albedoPath = effectiveAlbedo->texDescriptor->pathname;
        }

        // The descriptor stem is stable (foo.tex -> foo); a per GPU container
        // name (foo.dx11.dds) is not, so it never becomes a BW resource name.
        const String materialStem = albedoPath.IsEmpty() ? String("default_texture") : albedoPath.GetBasename();
        group->identifier = materialStem + std::to_string((long long)groups.size());
        group->material = materialStem + ".dds";
        group->normal.clear();

        if (!albedoPath.IsEmpty())
        {
            // The reference stays even when the export fails: BW then shows its
            // own missing texture marker instead of an untextured batch.
            ExportBwTexture(albedoPath, group->material, mapName);

            FilePath normalPath;
            Texture* effectiveNormal = mat->GetEffectiveTexture(NMaterialTextureName::TEXTURE_NORMAL);
            if (effectiveNormal != nullptr && effectiveNormal->texDescriptor != nullptr)
            {
                normalPath = effectiveNormal->texDescriptor->pathname;
            }

            if (!normalPath.IsEmpty())
            {
                group->normal = ExportBwTexture(normalPath, materialStem + "_n.dds", mapName);
            }
        }
        else
        {
            DAVA::Logger::Warning("[TerrainConverter] batch '%s' of '%s' has no albedo texture", group->identifier.c_str(),
                                  entity->GetName().c_str());
        }
        int32 vertexCount = pg->GetVertexCount();
        int32 indexCount = pg->GetPrimitiveCount() * 3;

        for (int32 i = 0; i < vertexCount; i++)
        {
            pg->GetCoord(i, v.pos_);
            pg->GetTexcoord(0, i, v.uv_);

            // transform into world coordinates!
            v.pos_ = v.pos_ * createdWith;

            if (pg->normalArray)
            {
                pg->GetNormal(i, v.normal_);

                // normals we need rotate but not translate!
                v.normal_ = v.normal_ * matrixNormal;

                v.normal_.Normalize();
            }
            else
            {
                v.normal_ = Vector3(0, 0, 1);
            }

            vertices.push_back(v);
            bbox.addBounds(v.pos_);
        }

        for (int32 i = 0; i < indexCount; i++)
        {
            pg->GetIndex(i, index);
            primitive.push_back((IndexValue)(index + indexOffset));
        }

        group->group.nPrimitives_ += indexCount / 3;
        group->group.nVertices_ += vertexCount;

        indexOffset += vertexCount;
    }
} // Model::AddGeometry

void Model::RecursiveAddGeometry(DAVA::Entity* curr, DAVA::int32 switchIndex)
{
    using namespace DAVA;

    RenderObject* renderObject = GetRenderObject(curr);
    if (renderObject && renderObject->GetType() == RenderObject::TYPE_MESH)
    {
        int32 maxLodIndex = FindMaxLodIndex(renderObject, switchIndex);
        AddGeometry(curr, renderObject, maxLodIndex, switchIndex);
    }
    int size = curr->GetChildrenCount();
    for (int i = 0; i < size; i++)
    {
        RecursiveAddGeometry(curr->GetChild(i), switchIndex);
    }
}

void Model::Batch(DAVA::Entity* curr)
{
    using namespace DAVA;

    if (GetAnimationComponent(curr))
    {
        return;
    }
    RenderObject* renderObject = GetRenderObject(curr);
    SwitchComponent* sw = GetSwitchComponent(curr);
    if (sw)
    {
        KeyedArchive* customProperties = GetCustomPropertiesArchieve(curr);
        health = (customProperties) ? customProperties->GetInt32("Health", 5) : 5;

        RecursiveAddGeometry(curr, 0);

        CreatePolygonGroup();
        MaterialPrimitiveGroup* group = &groups[groups.size() - 1];
        group->matCollision = (customProperties) ? (eModelPreset)customProperties->GetInt32("CollisionTypeCrashed",
                                                                                            eMP_NO_COLLISION) :
                                                   eMP_NO_COLLISION;

        RecursiveAddGeometry(curr, 1);
    }
    else if (renderObject && renderObject->GetType() == RenderObject::TYPE_MESH)
    {
        LodComponent* lodComponent = GetLodComponent(curr);

        if (lodComponent)
        {
            int32 maxLodIndex = renderObject->GetMaxLodIndex();
            if (maxLodIndex >= 0)
            {
                AddGeometry(curr, renderObject, maxLodIndex); // switch not requested
            }
        }
        else
        {
            AddGeometry(curr, renderObject); // lod and switch not requested
        }
    }
    if (!sw)
    {
        int size = curr->GetChildrenCount();
        for (int i = 0; i < size; i++)
        {
            Batch(curr->GetChild(i));
        }
    }
}

void Model::createTris(void)
{
    using namespace DAVA;

    DAVA::Logger::Info("Creating Triangles for bsp...");

    // BigWorld culls back faces, so a mesh with the wrong winding vanishes exactly when
    // you look at its front. WG hard-coded the flip; with our own globalTransform the
    // handedness has to be measured, not assumed. Face normal vs the direction from the
    // mesh centre: positive means the faces point outward with the current winding.
    // BW renders through D3D, whose front faces are clockwise; Dava authors counter
    // clockwise, so every converted mesh is flipped once. BW convention, not a guess.
    {
        DAVA::Vector3 center(0.0f, 0.0f, 0.0f);
        for (size_t i = 0; i < vertices.size(); ++i)
        {
            center += vertices[i].pos_;
        }
        if (!vertices.empty())
        {
            center /= (DAVA::float32)vertices.size();
        }

        double outward = 0.0;
        double area = 0.0;
        for (uint32 i = 0; i + 2 < (uint32)primitive.size(); i += 3)
        {
            const DAVA::Vector3& a = vertices[primitive[i]].pos_;
            const DAVA::Vector3& b = vertices[primitive[i + 1]].pos_;
            const DAVA::Vector3& c = vertices[primitive[i + 2]].pos_;
            const DAVA::Vector3 faceNormal = DAVA::CrossProduct(b - a, c - a);
            const DAVA::Vector3 faceCenter = (a + b + c) / 3.0f - center;
            outward += (double)DAVA::DotProduct(faceNormal, faceCenter);
            area += (double)faceNormal.Length();
        }
        DAVA::Logger::Info("model %s: %.0f%% of face area points away from the mesh centre (informational)",
                           modelName.c_str(), area > 0.0 ? 100.0 * (outward / area) : 0.0);
        if (area <= 0.0)
        {
            DAVA::Logger::Warning("model %s: no usable face normals, winding unverified", modelName.c_str());
        }
    }

    // Orient every triangle against the normals the model already carries. BW renders in
    // D3D, where a front face is clockwise, so its right-hand geometric normal points INTO
    // the surface - the opposite of the vertex normal. Deciding this per triangle from the
    // data means culling and lighting can never disagree, and a mirrored transform or a
    // mixed-hander source cannot invert half a mesh.
    {
        // Measured in WorldEditor, not assumed: with the clockwise assumption every mesh
        // came out inside out, so BW's primitives read counter-clockwise as the front face.
        const bool bwClockwiseFront = false;
        uint32 flipped = 0;
        uint32 assessed = 0;
        for (uint32 i = 0; i + 2 < (uint32)primitive.size(); i += 3)
        {
            const VertexXYZNUV& v0 = vertices[primitive[i]];
            const VertexXYZNUV& v1 = vertices[primitive[i + 1]];
            const VertexXYZNUV& v2 = vertices[primitive[i + 2]];

            const DAVA::Vector3 faceNormal = DAVA::CrossProduct(v1.pos_ - v0.pos_, v2.pos_ - v0.pos_);
            const DAVA::Vector3 vertexNormal = v0.normal_ + v1.normal_ + v2.normal_;
            if (faceNormal.SquareLength() < 1.0e-12f || vertexNormal.SquareLength() < 1.0e-12f)
            {
                continue; // degenerate face or no normals - leave the authored order alone
            }

            ++assessed;
            const bool isClockwise = DAVA::DotProduct(faceNormal, vertexNormal) < 0.0f;
            if (isClockwise != bwClockwiseFront)
            {
                std::swap(primitive[i + 1], primitive[i + 2]);
                ++flipped;
            }
        }
        DAVA::Logger::Info("model %s: winding oriented to the vertex normals, %u of %u triangles flipped",
                           modelName.c_str(), flipped, assessed);
    }

    degenerateCount = 0;

    WorldTriangle::Flags flags;
    for (uint32 j = 0; j < groups.size(); j++)
    {
        MaterialPrimitiveGroup& group = groups[j];

        flags = WorldTriangle::packFlags(GetBWCollision(j), GetBWMaterialKind(j));
        //      WorldTriangle::Flags flags = WorldTriangle::packFlags(
        //          TRIANGLE_DOUBLESIDED | (solid ? TRIANGLE_PROJECTILENOCOLLIDE : TRIANGLE_NOCOLLIDE |
        // TRIANGLE_PROJECTILENOCOLLIDE),
        //          (solid ? MATKIND_SPT_SOLID : MATKIND_SPT_LEAVES ));

        for (int32 i = group.group.startIndex_; i < group.group.startIndex_ + group.group.nPrimitives_ * 3; i += 3)
        {
            VertexXYZNUV& v0 = vertices[primitive[i]];
            VertexXYZNUV& v1 = vertices[primitive[i + 1]];
            VertexXYZNUV& v2 = vertices[primitive[i + 2]];

            WorldTriangle triangle(v0.pos_, v1.pos_, v2.pos_, flags);

            if (triangle.normal().SquareLength() < 0.000001f)
            {
                degenerateCount++;
            }
            else
            {
                tris.push_back(triangle);
            }
        }
    }
    if (tris.empty() && !vertices.empty() && !groups.empty())
    {
        // BigWorld's loader asserts that a BSP node either has no triangle array
        // at all or a positive triangle count (physics2/bsp.cpp:1045,
        // pTriangles_ / numTriangles_). An empty triangle set serialises as
        // "array set, count 0", which trips it, so a degenerate mesh gets a box
        // around its vertices instead: the node stays valid and the object still
        // has collision.
        DAVA::Vector3 lo = vertices[0].pos_;
        DAVA::Vector3 hi = lo;
        for (size_t i = 1; i < vertices.size(); ++i)
        {
            const DAVA::Vector3& p = vertices[i].pos_;
            lo.x = std::min(lo.x, p.x);
            lo.y = std::min(lo.y, p.y);
            lo.z = std::min(lo.z, p.z);
            hi.x = std::max(hi.x, p.x);
            hi.y = std::max(hi.y, p.y);
            hi.z = std::max(hi.z, p.z);
        }

        DAVA::Vector3 c[8];
        for (int32 i = 0; i < 8; ++i)
        {
            c[i].x = (i & 1) ? hi.x : lo.x;
            c[i].y = (i & 2) ? hi.y : lo.y;
            c[i].z = (i & 4) ? hi.z : lo.z;
        }

        static const int32 faces[12][3] = {
            { 0, 1, 3 }, { 0, 3, 2 }, // -Z
            { 5, 4, 6 },
            { 5, 6, 7 }, // +Z
            { 4, 0, 2 },
            { 4, 2, 6 }, // -X
            { 1, 5, 7 },
            { 1, 7, 3 }, // +X
            { 4, 5, 1 },
            { 4, 1, 0 }, // -Y
            { 2, 3, 7 },
            { 2, 7, 6 } // +Y
        };

        const WorldTriangle::Flags flags = WorldTriangle::packFlags(GetBWCollision(0), GetBWMaterialKind(0));
        for (int32 f = 0; f < 12; ++f)
        {
            tris.push_back(WorldTriangle(c[faces[f][0]], c[faces[f][1]], c[faces[f][2]], flags));
        }

        DAVA::Logger::Info("[TerrainConverter] model '%s': %d degenerate and 0 usable triangles, BSP built from the "
                           "bounding box (%d triangles)",
                           modelName.c_str(), (int32)degenerateCount, (int32)tris.size());
    }

    pBSP = new BSPTree(tris);
}

BinaryPtr Model::Recollect(void)
{
    const DAVA::uint32 BIN_SECTION_MAGIC = 0x42a14e65;
    using namespace DAVA;

    uint32 bsz = 4; // for magic

    // collect our children
    std::vector<BinaryPtr> childBlocks;
    for (uint32 i = 0; i < children_.size(); i++)
    {
        childBlocks.push_back(children_[i]->asBinary());
        bsz += (childBlocks.back()->size() + 3) & (~3L);
    }

    // build the directory
    String stream;
    for (uint32 i = 0; i < children_.size(); i++)
    {
        const String& sectName = children_[i]->sectionName();
        int lens[6] = { (int)childBlocks[i]->size(), 0, 0, 0, 0, (int)sectName.length() };
        stream.append((char*)lens, sizeof(lens));
        stream.append(sectName.data(), lens[5]);
        stream.append((4 - lens[5]) & 3, '\0');
    }
    int streamLen = stream.length();
    stream.append((char*)&streamLen, sizeof(int));
    bsz += stream.length();

    // now make the mega-block and return
    BinaryPtr pBinary = new BinaryBlock();
    pBinary->resize(bsz);

    char* dataPtr = (char*)pBinary->data();
    *((uint32*&)dataPtr)++ = BIN_SECTION_MAGIC;
    for (uint32 i = 0; i < children_.size(); i++)
    {
        uint32 len = childBlocks[i]->size();
        memcpy(dataPtr, childBlocks[i]->data(), len);
        dataPtr += (len + 3) & (~3L);
    }
    memcpy(dataPtr, stream.data(), stream.length());

    // free memory
    for (uint32 i = 0; i < children_.size(); i++)
    {
        delete children_[i];
    }
    children_.clear();

    // and that's it
    return pBinary;
}

DAVA::int32 Model::FindMaxLodIndex(DAVA::RenderObject* object, DAVA::int32 switchIndex)
{
    using namespace DAVA;

    if (!object)
    {
        return LodComponent::INVALID_LOD_LAYER;
    }

    int32 maxLodIndex = LodComponent::INVALID_LOD_LAYER;

    uint32 batchCount = object->GetRenderBatchCount();
    for (uint32 i = 0; i < batchCount; ++i)
    {
        int32 lod = -1, sw = -1;
        object->GetRenderBatch(i, lod, sw);

        if ((sw == switchIndex) && (lod > maxLodIndex))
        {
            maxLodIndex = lod;
        }
    }

    return maxLodIndex;
}

DAVA::uint32 Model::GetSwitchCount(DAVA::RenderObject* object)
{
    using namespace DAVA;

    if (!object)
    {
        return 0;
    }

    int32 maxSwitchIndex = -1;

    uint32 batchCount = object->GetRenderBatchCount();
    for (uint32 i = 0; i < batchCount; ++i)
    {
        int32 lod = -1, sw = -1;
        object->GetRenderBatch(i, lod, sw);

        if (sw > maxSwitchIndex)
        {
            maxSwitchIndex = sw;
        }
    }

    return (maxSwitchIndex + 1);
}
