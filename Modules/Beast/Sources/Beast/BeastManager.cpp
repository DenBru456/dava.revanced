/*
    BeastManager.cpp - reconstruction for the dava.revanced Beast module restore.

    The original implementation of this file was never published in the public
    dava.engine repository (verified: the file is absent from every public
    commit and from the LFS store, while every other Modules/Beast file is
    present). This file is a faithful reconstruction built from:
      * the public BeastManager.h contract (the header IS published),
      * the ILB (Beast v5) SDK bundled in Modules/Beast/Thirdparty,
      * the call sites in the ResourceEditor (BeastRunner, BeastCommandLineTool)
        and in the module itself (SceneParser, BeastMeshInstance, TextureTarget,
        BeastPointCloud, BeastRunner's expected output layout).

    Bake flow (matches BeastRunner expectations):
      Run(scene)
        -> BeginScene (ILBBeginScene)
        -> SceneParser walks the scene (async: landscape color thumbnails)
        -> OnSceneParsingCompleted
             -> EndScene (ILBEndScene)
             -> GenerateLightmaps
                  ILBCreateJob with the mode xml (lightmaps_mode.xml /
                  sh_mode.xml / preview_mode.xml, deployed by the module CMake
                  MIX_APP_DATA into Data.beast/config)
                  - MODE_LIGHTMAPS / MODE_PREVIEW:
                      ILBCreateIlluminationPass  "sm" (ILB_IM_FULL)
                      ILBCreateAmbientOcclusionPass "ao"
                      one TextureTarget per lightmapped BeastMeshInstance,
                      ILBAddBakeInstance + both passes added to the target.
                  - MODE_SPHERICAL_HARMONICS:
                      ILBCreateIlluminationPassSH + ILBCreateVertexTarget,
                      point clouds attached through ILBAddBakePointCloud.
                  ILBStartJob (output path = lightmaps directory)
        -> Update() polls progress / completion (pumped by BeastRunner::Process
           every frame)
        -> OnJobCompleted
             -> SaveResults
                  - lightmaps: readback -> {lightmapsDirectory}/sm/*.png and
                    /ao/*.png (the exact layout LightmapsPacker consumes),
                    the landscape lightmap goes to the top level as
                    landscape.png (moved out by BeastRunner::PackLightmaps)
                  - SH: vertex buffer readback -> SpeedTreeObject::SetSphericalHarmonics
             -> LOD iteration: for lodLevel 1..maxLodLevel re-run the whole
                parse+job cycle, baking only the batches that were not baked
                yet (IsLightmapGenerated / SetLightmapGenerated bookkeeping).

    Deliberate simplifications (documented, none affect the bake itself):
      * BeastManager::UpdateAtlas is a stub. The atlasing data produced by
        LightmapsPacker is consumed on the editor side (texture descriptors /
        sprite frames); the published BeastManager.h has no member to retain
        it, so the original semantics cannot be recovered from public
        sources. It is kept as a logging no-op so the published header
        contract stays intact.
      * Ambient occlusion pass uses fixed defaults (max distance 100, cone
        angle 90 degrees). The original may have taken these from scene
        settings; the ILB defaults behave identically for typical scenes.
*/

#include "Beast/BeastManager.h"

#include "Beast/BeastDebug.h"
#include "Beast/BeastNames.h"
#include "Beast/BeastMaterial.h"
#include "Beast/BeastMesh.h"
#include "Beast/BeastLight.h"
#include "Beast/BeastMeshInstance.h"
#include "Beast/BeastPointCloud.h"
#include "Beast/BeastTexture.h"
#include "Beast/BeastResource.h"
#include <Render/Highlevel/RenderBatch.h>
#include <Render/Texture.h>
#include <Render/Material/NMaterialNames.h>
#include "Beast/LightmapAtlasingData.h"
#include "Beast/SceneParser.h"
#include "Beast/TextureTarget.h"

#include <Scene3D/Scene.h>
#include <Scene3D/Entity.h>
#include <Scene3D/Components/ComponentHelpers.h>
#include <Scene3D/Components/TransformComponent.h>
#include <Math/TransformUtils.h>
#include <beastapi/beastinstance.h>
#include <beastapi/beasttargetentity.h>
#include <beastapi/beastlightsource.h>
#include <Render/Highlevel/RenderObject.h>
#include <Render/Highlevel/Frustum.h>
#include <Render/Highlevel/Camera.h>
#include <Render/RHI/rhi_Public.h>
#include <Render/RHI/rhi_ShaderSource.h>
#include <Render/Material/NMaterial.h>
#include <Math/AABBox3.h>
#include <Render/Highlevel/SpeedTreeObject.h>
#include <Render/Image/Image.h>
#include <Render/Image/ImageSystem.h>
#include <Render/RenderBase.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <Base/BaseObject.h>
#include <cmath>
#include <cctype>
#include <cstring>

#include <FileSystem/File.h>
#include <FileSystem/FileSystem.h>
#include <FileSystem/FileList.h>
#include <Time/SystemTimer.h>
#include <Concurrency/Thread.h>
#include <Logger/Logger.h>
#include <Debug/DVAssert.h>
#include <Base/BaseTypes.h>

#include <cstring>
#include <cstdlib>

namespace
{
// How long ILBGetJobResult may keep failing after ILBIsJobCompleted flipped
// before the bake is declared failed. Poll ticks are ~15 ms apart.
const DAVA::int64 RESULT_QUERY_TIMEOUT_MS = 10000;

// How many ~15 ms ticks the job must stay not-running before its result is read.
// Beast drops the running flag between tool invocations, so a single false is
// not the end of the job.
const DAVA::int32 NOT_RUNNING_GRACE_TICKS = 120;

// Upper bound on job updates consumed per frame in a live session, so a fast
// renderer cannot starve the editor's UI thread.
const DAVA::int32 LIVE_UPDATES_PER_TICK = 96;
}

namespace Beast
{
namespace BeastManagerDetails
{
// Beast binaries / config are deployed by the module CMake MIX_APP_DATA into
// "Data.beast/bin" and "Data.beast/config" (see Modules/Beast/CMakeLists.txt).
// The editor can be started from different working directories, so try the
// usual candidates until one exists.
// Beast data is deployed by the module CMake MIX_APP_DATA to "Data.beast/",
// which is only reachable if the process working directory happens to be the
// deploy root. An editor started from Visual Studio, from a shortcut or from
// another folder would silently get an empty path, ILBSetBeastPath would fail
// and ILBBeginScene would then return null - the "scene != nullptr" assert.
// So look next to the executable first, then the working directory, then res.
DAVA::Vector<DAVA::String> GetBeastSearchRoots()
{
    DAVA::Vector<DAVA::String> roots;

    const char* envRoot = std::getenv("BEAST_ROOT");
    if (envRoot != nullptr && envRoot[0] != '\0')
    {
        DAVA::String root(envRoot);
        if (root.back() != '/')
        {
            root += '/';
        }
        roots.push_back(root);
    }

    DAVA::FilePath exeDir = DAVA::FileSystem::Instance()->GetCurrentExecutableDirectory();
    DAVA::String exePath = exeDir.GetAbsolutePathname();
    roots.push_back(exePath + "Data.beast/");
    roots.push_back(exePath + "Data/Data.beast/");
    roots.push_back(exePath + "../Data.beast/");
    roots.push_back(exePath + "../Data/Data.beast/");

    roots.push_back("Data.beast/");
    roots.push_back("Data/Data.beast/");
    roots.push_back("~res:/Data.beast/");
    roots.push_back("~res:/Data/Data.beast/");

    return roots;
}

// FileSystem::Exists() dispatches on the *path syntax*: a path without a
// trailing separator is treated as a file, so "Data.beast/bin" was tested with
// IsFile() and always failed even though the folder was there. Test both.
bool BeastResourceExists(const DAVA::FilePath& path)
{
    DAVA::FileSystem* fs = DAVA::FileSystem::Instance();
    return fs->IsFile(path) || fs->IsDirectory(path);
}

DAVA::FilePath FindBeastResource(const DAVA::String& relativePath)
{
    for (const DAVA::String& root : GetBeastSearchRoots())
    {
        DAVA::FilePath candidate(root + relativePath);
        if (BeastResourceExists(candidate))
        {
            // directories MUST come back with a trailing separator: callers
            // concatenate onto the result, and "Data.beast/bin" + "thebeast.exe"
            // produced the literal path "Data.beast/binthebeast.exe".
            if (DAVA::FileSystem::Instance()->IsDirectory(candidate))
            {
                return DAVA::FilePath(candidate.GetAbsolutePathname() + "/");
            }
            return candidate;
        }
    }

    DAVA::String searched;
    for (const DAVA::String& root : GetBeastSearchRoots())
    {
        searched += "\n    " + root + relativePath;
    }
    DAVA::Logger::Error("[BeastManager] '%s' not found, searched:%s", relativePath.c_str(), searched.c_str());

    return DAVA::FilePath();
}

// Fatal + advisory files. thebeast.exe and rendertool.xml are the two the
// manager cannot work without; the rest are reported so a partial deploy is
// visible in the log instead of turning into an assert three frames later.
void ReportBeastInstall(const DAVA::FilePath& binDir, const DAVA::FilePath& configDir)
{
    DAVA::Logger::Info("[BeastManager] executable dir: '%s'",
                       DAVA::FileSystem::Instance()->GetCurrentExecutableDirectory().GetAbsolutePathname().c_str());
    DAVA::Logger::Info("[BeastManager] working dir: '%s'",
                       DAVA::FileSystem::Instance()->GetCurrentWorkingDirectory().GetAbsolutePathname().c_str());

    if (binDir.IsEmpty())
    {
        DAVA::Logger::Error("[BeastManager] Beast data folder not found. Set BEAST_ROOT to the folder that holds bin/ and config/ (for example <repo>/_build/_ResourceEditor/Data.beast).");
        return;
    }

    DAVA::Logger::Info("[BeastManager] beast bin dir: '%s'", binDir.GetAbsolutePathname().c_str());
    DAVA::Logger::Info("[BeastManager] beast config dir: '%s'", configDir.GetAbsolutePathname().c_str());

    DAVA::FilePath beastShaders = BeastManagerDetails::FindBeastResource("shaders/");
    if (!beastShaders.IsEmpty())
    {
        DAVA::Logger::Info("[BeastManager] beast shaders dir: '%s'", beastShaders.GetAbsolutePathname().c_str());
        DAVA::FilePath legacyPhong = beastShaders + "beastlegacyphong.osl";
        if (!DAVA::FileSystem::Instance()->IsFile(legacyPhong))
        {
            DAVA::Logger::Error("[BeastManager] legacy shader is missing: '%s' (ILBBeginScene needs it; the repo copy is Modules/Beast/Thirdparty/examples-physical/data/phongish.osl, same shader name)",
                                legacyPhong.GetAbsolutePathname().c_str());
        }
    }

    static const char* fatalFiles[] = { "thebeast.exe", "rendertool.xml" };
    for (const char* file : fatalFiles)
    {
        DAVA::FilePath path = binDir + file;
        if (!DAVA::FileSystem::Instance()->IsFile(path))
        {
            DAVA::Logger::Error("[BeastManager] required Beast file is missing: '%s'", path.GetAbsolutePathname().c_str());
        }
    }

    static const char* advisoryFiles[] = {
        "rendertool-64.exe", "rendertool-32.exe", "rendertoolcontroller-64.exe",
        "ernst-64.exe", "ernsttool.xml", "ernsttool-32.exe",
        "filtertool-64.exe", "oslc-64.exe", "uvtool-64.exe"
    };
    for (const char* file : advisoryFiles)
    {
        DAVA::FilePath path = binDir + file;
        if (!DAVA::FileSystem::Instance()->IsFile(path))
        {
            DAVA::Logger::Warning("[BeastManager] Beast tool not deployed: '%s'", path.GetAbsolutePathname().c_str());
        }
    }

    static const char* configFiles[] = { "lightmaps_mode.xml", "preview_mode.xml", "sh_mode.xml" };
    for (const char* file : configFiles)
    {
        DAVA::FilePath path = configDir + file;
        if (!DAVA::FileSystem::Instance()->IsFile(path))
        {
            DAVA::Logger::Error("[BeastManager] Beast config is missing: '%s'", path.GetAbsolutePathname().c_str());
        }
    }
}

void LogILBFailure(const char* what, ILBStatus status)
{
    ILBStringHandle extendedError = nullptr;
    ILBGetExtendErrorInformation(&extendedError);
    DAVA::Logger::Error("[BeastManager] %s failed (ILBStatus %d): %s",
                        what,
                        static_cast<DAVA::int32>(status),
                        ConvertBeastString(extendedError).c_str());
}

// thebeast.exe reports why a render was refused (license, config, shaders) only in
// its own log, which the embedded job never sees. Echo its error lines into our log so
// a dead job explains itself, and collapse a license refusal into one actionable line
// instead of five dialog entries.
bool ContainsCaseInsensitive(const DAVA::String& haystack, const char* lowercaseNeedle)
{
    const size_t needleLength = strlen(lowercaseNeedle);
    if (needleLength == 0 || haystack.size() < needleLength)
    {
        return false;
    }

    for (size_t i = 0; i + needleLength <= haystack.size(); ++i)
    {
        size_t matched = 0;
        while (matched < needleLength
               && std::tolower(static_cast<unsigned char>(haystack[i + matched])) == lowercaseNeedle[matched])
        {
            ++matched;
        }
        if (matched == needleLength)
        {
            return true;
        }
    }
    return false;
}

void LogBeastRendererErrors(const DAVA::String& infoLogPath)
{
    if (infoLogPath.empty())
    {
        return;
    }

    DAVA::ScopedPtr<DAVA::File> file(DAVA::File::Create(DAVA::FilePath(infoLogPath), DAVA::File::OPEN | DAVA::File::READ));
    if (!file)
    {
        DAVA::Logger::Warning("[BeastManager] renderer log '%s' could not be read", infoLogPath.c_str());
        return;
    }

    DAVA::uint64 size = file->GetSize();
    if (size == 0)
    {
        return;
    }

    DAVA::String content;
    content.resize(static_cast<size_t>(size));
    file->Read(&content[0], static_cast<DAVA::uint32>(size));

    if (content.find("[ERROR]") == DAVA::String::npos)
    {
        DAVA::Logger::Error("[BeastManager] the renderer reported no error text; see '%s'", infoLogPath.c_str());
        return;
    }

    DAVA::Vector<DAVA::String> errorLines;
    bool licenseRefusal = false;

    size_t pos = content.find("[ERROR]");
    while (pos != DAVA::String::npos)
    {
        size_t end = content.find_first_of("\r\n", pos);
        if (end == DAVA::String::npos)
        {
            end = content.size();
        }

        DAVA::String line = content.substr(pos, end - pos);
        if (ContainsCaseInsensitive(line, "license"))
        {
            licenseRefusal = true;
        }
        errorLines.push_back(line);

        pos = content.find("[ERROR]", end);
    }

    // the raw lines are detail, not the headline
    for (const DAVA::String& line : errorLines)
    {
        DAVA::Logger::Warning("[BeastManager] renderer: %s", line.c_str());
    }

    if (licenseRefusal)
    {
        DAVA::Logger::Error("[BeastManager] the Beast renderer has no valid license key, so the bake cannot run. "
                            "Beast needs an Autodesk Gameware license token (a plain text string, not a product key): "
                            "set BEAST_LICENSE, or drop the token into Data.beast/beast_license.txt, then restart the editor.");
    }
}

DAVA::String ReadLicenseFile(const DAVA::FilePath& path)
{
    if (path.IsEmpty())
    {
        return DAVA::String();
    }

    DAVA::ScopedPtr<DAVA::File> file(DAVA::File::Create(path, DAVA::File::OPEN | DAVA::File::READ));
    if (!file || file->GetSize() == 0)
    {
        return DAVA::String();
    }

    DAVA::uint64 size = file->GetSize();
    DAVA::String token;
    token.resize(static_cast<size_t>(size));
    file->Read(&token[0], static_cast<DAVA::uint32>(size));

    // trim whitespace/newlines: a token with a trailing newline is invalid
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
    {
        token.pop_back();
    }
    size_t first = 0;
    while (first < token.size() && std::isspace(static_cast<unsigned char>(token[first])))
    {
        ++first;
    }

    return token.substr(first);
}

DAVA::String GetBeastLicenseKey()
{
    // Beast's key is an Autodesk Gameware token ("a simple text string token", per
    // the Beast documentation) handed to ILBCreateManager, which is what the
    // renderer expects. Look in the documented place first, then allow a plain
    // file next to the Beast data so a token can be dropped in without touching
    // environment variables.
    const char* envKey = std::getenv("BEAST_LICENSE");
    if (envKey != nullptr)
    {
        const DAVA::String key(envKey);
        if (!key.empty())
        {
            DAVA::Logger::Info("[BeastManager] license key: BEAST_LICENSE environment variable (%u characters)",
                               static_cast<DAVA::uint32>(key.size()));
            return key;
        }
    }

    static const char* licenseFiles[] = { "beast_license.txt", "license.txt" };
    for (const char* fileName : licenseFiles)
    {
        const DAVA::String token = ReadLicenseFile(FindBeastResource(fileName));
        if (!token.empty())
        {
            DAVA::Logger::Info("[BeastManager] license key: '%s' (%u characters)", fileName,
                               static_cast<DAVA::uint32>(token.size()));
            return token;
        }
    }

    DAVA::Logger::Info("[BeastManager] no license token found (BEAST_LICENSE unset, no Data.beast/beast_license.txt)");
    return DAVA::String();
}

// Saves one framebuffer as a plain 8bit RGBA png. LightmapsPacker expects
// png files (it loads them through ImageSystem), Beast exposes bake results
// as framebuffers, so readback + conversion happens here.
// The engine's lightmap shader samples one texture: the sm pass. Beast writes the ambient
// occlusion pass beside it, so a map bound from the sm atlas alone has no contact shadows at all,
// which is exactly what "the lit side has no shadows under the ledges" looks like. Multiplying the
// ao tile into the sm tile here keeps the shader untouched and requires no second slot.
// defined below the composite; declared here because the gutter pass needs the same rule
bool IsTexelPainted(const DAVA::uint8* texel);

bool MultiplyAOIntoLightmap(const DAVA::FilePath& smPath, const DAVA::FilePath& aoPath)
{
    if (!DAVA::FileSystem::Instance()->IsFile(smPath) || !DAVA::FileSystem::Instance()->IsFile(aoPath))
    {
        return false;
    }

    DAVA::ScopedPtr<DAVA::Image> sm(DAVA::ImageSystem::LoadSingleMip(smPath));
    DAVA::ScopedPtr<DAVA::Image> ao(DAVA::ImageSystem::LoadSingleMip(aoPath));
    if (!sm || !ao)
    {
        return false;
    }
    if ((sm->width != ao->width) || (sm->height != ao->height)
        || (sm->format != DAVA::PixelFormat::FORMAT_RGBA8888) || (ao->format != DAVA::PixelFormat::FORMAT_RGBA8888))
    {
        DAVA::Logger::Warning("[BeastManager] ao not composited into %s: %ux%u fmt %d vs %ux%u fmt %d",
                              smPath.GetFilename().c_str(), sm->width, sm->height, static_cast<int>(sm->format),
                              ao->width, ao->height, static_cast<int>(ao->format));
        return false;
    }

    // How hard the ao pass is allowed to bite. 1.0 is a straight multiply; the shipped map sits at
    // about 0.8. Measured on the rudniki tower against lightmaps2/sm of a v10.67 bake: in the texels
    // where this ao pass is below 128, the illumination before compositing is 115/103/74 while the
    // shipped page reads 35/39/37 there and our composited page reads 12/11/8. Solving
    // pre * (ao/255)^p = shipped gives p = 0.82 for an ao of 60, so the multiply is raised to 0.8
    // and the recesses (the lighthouse doorway) stop collapsing to near black.
    static const DAVA::float32 AO_STRENGTH_EXPONENT = 0.8f;
    DAVA::float32 aoCurve[256];
    for (DAVA::int32 i = 0; i < 256; ++i)
    {
        aoCurve[i] = std::pow(static_cast<DAVA::float32>(i) / 255.0f, AO_STRENGTH_EXPONENT);
    }

    const DAVA::uint32 texels = sm->width * sm->height;
    DAVA::uint8* s = sm->data;
    const DAVA::uint8* a = ao->data;

    // Which texels Beast actually painted, decided BEFORE the ao multiply and the floor clamp below,
    // because both of those turn an unpainted texel into a plausible looking one (the clamp writes
    // 28/33/33 into it).
    DAVA::Vector<DAVA::uint8> unpainted(texels, 0);
    for (DAVA::uint32 i = 0; i < texels; ++i)
    {
        unpainted[i] = IsTexelPainted(s + i * 4u) ? 0 : 1;
    }
    for (DAVA::uint32 i = 0; i < texels; ++i)
    {
        // The ao pass covers less area than the sm pass (measured: 0.61-0.91 of the tile against
        // 0.68-0.98), so a zero there usually means "no occlusion was traced at this texel", not
        // "fully shadowed". Multiplying those to zero painted black patches onto surfaces that had
        // been lit, so a zero now leaves the lightmap alone.
        for (DAVA::int32 ch = 0; ch < 3; ++ch)
        {
            const DAVA::uint32 idx = i * 4 + ch;
            if (a[idx] != 0)
            {
                static const DAVA::float32 AO_FACTOR_FLOOR = 0.35f;
                const DAVA::float32 factor = AO_FACTOR_FLOOR + (1.0f - AO_FACTOR_FLOOR) * aoCurve[a[idx]];
                s[idx] = static_cast<DAVA::uint8>(DAVA::Min(255.0f, s[idx] * factor + 0.5f));
            }

            // The ambient term an occluder cannot remove, as a limit on the FACTOR rather than on the
            // result. Clamping the composited value to a constant 28/33/33 lifted the recesses but
            // flattened them: every texel Beast had left below 31 became exactly 31, so 14.7% of the
            // tile turned into flat plateaus meeting lit charts at 105 across a single texel. That hard
            // edge is the thin dark line along wall edges. Flooring the factor instead keeps each
            // texel's own gradient (a dim texel stays dimmer than a bright one) and still refuses to
            // let occlusion consume all the light.

        }
    }

    // Gutters have to carry the chart's own colour. With the clamp confined to painted texels the
    // gutter stays at whatever Beast left (zero), and a zero against a lit chart is the same hard edge
    // in the other direction: the renderer filters across the chart border and picks up the black,
    // which is the thin tapering dark line along wall edges. So spread the painted values outward.
    DAVA::Vector<DAVA::float32> acc(static_cast<size_t>(texels) * 3u, 0.0f);
    DAVA::Vector<DAVA::uint8> done(unpainted);
    for (DAVA::uint32 i = 0; i < texels; ++i)
    {
        acc[i * 3 + 0] = static_cast<DAVA::float32>(s[i * 4 + 0]);
        acc[i * 3 + 1] = static_cast<DAVA::float32>(s[i * 4 + 1]);
        acc[i * 3 + 2] = static_cast<DAVA::float32>(s[i * 4 + 2]);
    }
    static const DAVA::int32 GUTTER_PASSES = 32;
    static const DAVA::int32 gutterOffsets[8][3] = { { -1, 0, 2 }, { 1, 0, 2 }, { 0, -1, 2 }, { 0, 1, 2 }, { -1, -1, 1 }, { 1, -1, 1 }, { -1, 1, 1 }, { 1, 1, 1 } };
    DAVA::int32 gutterLeft = 0;
    for (DAVA::int32 pass = 0; pass < GUTTER_PASSES; ++pass)
    {
        gutterLeft = 0;
        for (DAVA::uint32 y = 0; y < sm->height; ++y)
        {
            for (DAVA::uint32 x = 0; x < sm->width; ++x)
            {
                const DAVA::uint32 index = y * sm->width + x;
                if (done[index] == 0)
                {
                    continue;
                }
                DAVA::float32 r = 0.0f;
                DAVA::float32 g = 0.0f;
                DAVA::float32 b = 0.0f;
                DAVA::float32 total = 0.0f;
                for (const auto& offset : gutterOffsets)
                {
                    const DAVA::int32 nx = static_cast<DAVA::int32>(x) + offset[0];
                    const DAVA::int32 ny = static_cast<DAVA::int32>(y) + offset[1];
                    if ((nx < 0) || (ny < 0) || (nx >= static_cast<DAVA::int32>(sm->width)) || (ny >= static_cast<DAVA::int32>(sm->height)))
                    {
                        continue;
                    }
                    const DAVA::uint32 neighbour = static_cast<DAVA::uint32>(ny) * sm->width + static_cast<DAVA::uint32>(nx);
                    if (done[neighbour] != 0)
                    {
                        continue;
                    }
                    const DAVA::float32 weight = static_cast<DAVA::float32>(offset[2]);
                    r += acc[neighbour * 3 + 0] * weight;
                    g += acc[neighbour * 3 + 1] * weight;
                    b += acc[neighbour * 3 + 2] * weight;
                    total += weight;
                }
                if (total <= 0.0f)
                {
                    ++gutterLeft;
                    continue;
                }
                acc[index * 3 + 0] = r / total;
                acc[index * 3 + 1] = g / total;
                acc[index * 3 + 2] = b / total;
                done[index] = 0;
            }
        }
    }
    DAVA::int32 gutterFilled = 0;
    for (DAVA::uint32 i = 0; i < texels; ++i)
    {
        if (unpainted[i] != 0)
        {
            continue;
        }
        // only texels the pass actually reached; the rest stay untouched
        if ((acc[i * 3] <= 0.0f) && (acc[i * 3 + 1] <= 0.0f) && (acc[i * 3 + 2] <= 0.0f))
        {
            continue;
        }
        s[i * 4 + 0] = static_cast<DAVA::uint8>(DAVA::Min(255.0f, acc[i * 3 + 0] + 0.5f));
        s[i * 4 + 1] = static_cast<DAVA::uint8>(DAVA::Min(255.0f, acc[i * 3 + 1] + 0.5f));
        s[i * 4 + 2] = static_cast<DAVA::uint8>(DAVA::Min(255.0f, acc[i * 3 + 2] + 0.5f));
        ++gutterFilled;
    }
    if (gutterFilled > 0)
    {
        DAVA::Logger::Info("[BeastManager] gutters: %d texels dilated from the charts in %s (%d unreachable)", gutterFilled, smPath.GetFilename().c_str(), gutterLeft);
    }

    // Texel-scale speckle. The shipped page is smooth at texel level: its largest deviation from a
    // four-neighbour mean is 19 levels over the whole rudniki tile. Ours had 1021 texels over 30, max
    // 72 - isolated spikes drawn from sampling that never converged. A spike cannot be a real feature
    // at this scale, and on screen it reads as a thin tapering dark line. Replace a texel only when it
    // sits inside a painted 3x3 block (so chart borders are untouched) and disagrees with the block
    // median by more than 20 levels.
    DAVA::uint8* src = sm->data;
    DAVA::Vector<DAVA::uint8> copy(src, src + static_cast<size_t>(texels) * 4u);
    DAVA::int32 despiked = 0;
    for (DAVA::uint32 y = 1; y + 1 < sm->height; ++y)
    {
        for (DAVA::uint32 x = 1; x + 1 < sm->width; ++x)
        {
            const DAVA::uint32 centre = (y * sm->width + x) * 4u;
            DAVA::uint8 median[3] = { 0, 0, 0 };
            bool smoothBlock = true;
            for (DAVA::int32 ch = 0; ch < 3; ++ch)
            {
                DAVA::uint8 window[9];
                for (DAVA::int32 dy = -1; dy <= 1; ++dy)
                {
                    for (DAVA::int32 dx = -1; dx <= 1; ++dx)
                    {
                        const DAVA::uint32 idx = ((y + dy) * sm->width + (x + dx)) * 4u + static_cast<DAVA::uint32>(ch);
                        window[(dy + 1) * 3 + (dx + 1)] = copy[idx];
                        if ((static_cast<DAVA::int32>(copy[((y + dy) * sm->width + (x + dx)) * 4u]) + copy[((y + dy) * sm->width + (x + dx)) * 4u + 1] + copy[((y + dy) * sm->width + (x + dx)) * 4u + 2]) <= 6)
                        {
                            smoothBlock = false;
                        }
                    }
                }
                if (!smoothBlock)
                {
                    break;
                }
                for (DAVA::int32 i = 0; i < 9; ++i)
                {
                    for (DAVA::int32 j = i + 1; j < 9; ++j)
                    {
                        if (window[j] < window[i])
                        {
                            const DAVA::uint8 t = window[i];
                            window[i] = window[j];
                            window[j] = t;
                        }
                    }
                }
                median[ch] = window[4];
            }

            if (!smoothBlock)
            {
                continue;
            }

            DAVA::int32 worst = 0;
            for (DAVA::int32 ch = 0; ch < 3; ++ch)
            {
                worst = DAVA::Max(worst, DAVA::Abs(static_cast<DAVA::int32>(copy[centre + static_cast<DAVA::uint32>(ch)]) - static_cast<DAVA::int32>(median[ch])));
            }
            if (worst > 20)
            {
                for (DAVA::int32 ch = 0; ch < 3; ++ch)
                {
                    src[centre + static_cast<DAVA::uint32>(ch)] = median[ch];
                }
                ++despiked;
            }
        }
    }
    if (despiked > 0)
    {
        DAVA::Logger::Info("[BeastManager] despiked %d texels in %s", despiked, smPath.GetFilename().c_str());
    }

    return DAVA::ImageSystem::Save(smPath, sm.get(), DAVA::PixelFormat::FORMAT_RGBA8888) == DAVA::eErrorCode::SUCCESS;
}

bool IsTexelPainted(const DAVA::uint8* texel)
{
    // Beast leaves any texel outside a UV chart at zero. The shipped lightmaps have no such texel:
    // every pixel of their atlas is painted, which is what an edge filter needs to look right.
    return (static_cast<DAVA::int32>(texel[0]) + texel[1] + texel[2]) > 6;
}

void FillUnpaintedTexels(DAVA::uint8* pixels, DAVA::int32 width, DAVA::int32 height, DAVA::int32 relaxBudget = 200)
{
    // Beast leaves every texel outside a UV chart at zero, and the mesh samples those texels at the
    // chart border and wherever a part is buried inside another part. Two different jobs:
    //
    //   1. the 2-3 texel fringe a bilinear filter actually reaches: copy, because it must match the
    //      chart exactly or the edge shows;
    //   2. large holes (the interior rim of a part sunk into another part): these are tens of texels
    //      across. A pure copy fills them in flat bands - the rim value is cloned inward unchanged -
    //      and the boundary to the painted chart is then a step of 30+ levels, which in the render is
    //      a hard line exactly where the two parts interpenetrate. So the holes are relaxed instead:
    //      each free texel repeatedly takes the average of its neighbours while the painted chart
    //      stays fixed, which converges to a smooth gradient spanning the hole.
    const DAVA::int32 pixelCount = width * height;
    DAVA::Vector<DAVA::uint8> fixed(static_cast<size_t>(pixelCount), 0);
    DAVA::Vector<DAVA::uint8> settled(static_cast<size_t>(pixelCount), 0);
    DAVA::Vector<DAVA::float32> value(static_cast<size_t>(pixelCount) * 3u, 0.0f);
    DAVA::int32 unpainted = 0;
    for (DAVA::int32 i = 0; i < pixelCount; ++i)
    {
        const DAVA::uint8* texel = pixels + static_cast<size_t>(i) * 4;
        const bool isPainted = IsTexelPainted(texel);
        fixed[i] = isPainted ? 1 : 0;
        settled[i] = isPainted ? 1 : 0;
        value[i * 3 + 0] = static_cast<DAVA::float32>(texel[0]);
        value[i * 3 + 1] = static_cast<DAVA::float32>(texel[1]);
        value[i * 3 + 2] = static_cast<DAVA::float32>(texel[2]);
        unpainted += isPainted ? 0 : 1;
    }

    if (unpainted == 0)
    {
        return;
    }

    static const DAVA::int32 EDGE_PASSES = 8;      // bilinear footprint
    const DAVA::int32 RELAX_PASSES = relaxBudget; // hole gradients
    static const DAVA::uint8 fillFloor[3] = { 28, 33, 33 };
    static const DAVA::int32 offsets[8][3] =
    {
        { -1, 0, 2 }, { 1, 0, 2 }, { 0, -1, 2 }, { 0, 1, 2 },
        { -1, -1, 1 }, { 1, -1, 1 }, { -1, 1, 1 }, { 1, 1, 1 },
    };

    const DAVA::int32 totalUnpainted = unpainted;
    DAVA::int32 reachable = 0;

    for (DAVA::int32 pass = 0; (pass < EDGE_PASSES + RELAX_PASSES) && (unpainted > 0); ++pass)
    {
        DAVA::int32 filledThisPass = 0;
        for (DAVA::int32 y = 0; y < height; ++y)
        {
            for (DAVA::int32 x = 0; x < width; ++x)
            {
                const DAVA::int32 index = y * width + x;
                if (fixed[index] != 0)
                {
                    continue;
                }

                DAVA::float32 r = 0.0f;
                DAVA::float32 g = 0.0f;
                DAVA::float32 b = 0.0f;
                DAVA::float32 totalWeight = 0.0f;
                for (const auto& offset : offsets)
                {
                    const DAVA::int32 nx = x + offset[0];
                    const DAVA::int32 ny = y + offset[1];
                    if ((nx < 0) || (nx >= width) || (ny < 0) || (ny >= height))
                    {
                        continue;
                    }

                    const DAVA::int32 neighbour = ny * width + nx;
                    // during the fringe passes only painted texels may contribute, so the copy is
                    // exact; afterwards every settled texel joins in so the hole can relax
                    const bool usable = (pass < EDGE_PASSES) ? (fixed[neighbour] != 0)
                                                             : ((fixed[neighbour] != 0) || (value[neighbour * 3] > 0.0f) || (value[neighbour * 3 + 1] > 0.0f) || (value[neighbour * 3 + 2] > 0.0f));
                    if (!usable)
                    {
                        continue;
                    }

                    const DAVA::float32 weight = static_cast<DAVA::float32>(offset[2]);
                    r += value[neighbour * 3 + 0] * weight;
                    g += value[neighbour * 3 + 1] * weight;
                    b += value[neighbour * 3 + 2] * weight;
                    totalWeight += weight;
                }

                if (totalWeight <= 0.0f)
                {
                    continue;
                }

                value[index * 3 + 0] = r / totalWeight;
                value[index * 3 + 1] = g / totalWeight;
                value[index * 3 + 2] = b / totalWeight;
                settled[index] = 1;
                if (pass < EDGE_PASSES)
                {
                    fixed[index] = 2;   // fringe: frozen like painted
                    ++reachable;
                }
                ++filledThisPass;
            }
        }

        if (pass < EDGE_PASSES)
        {
            unpainted -= filledThisPass;
        }
        if (filledThisPass == 0 && pass >= EDGE_PASSES)
        {
            break;
        }
    }

    // write back; anything never reached at all (an isolated island with no chart around it) gets
    // the flat ambient floor instead of black
    for (DAVA::int32 i = 0; i < pixelCount; ++i)
    {
        DAVA::uint8* target = pixels + static_cast<size_t>(i) * 4;
        if (settled[i] != 0)
        {
            target[0] = static_cast<DAVA::uint8>(DAVA::Min(255.0f, value[i * 3 + 0] + 0.5f));
            target[1] = static_cast<DAVA::uint8>(DAVA::Min(255.0f, value[i * 3 + 1] + 0.5f));
            target[2] = static_cast<DAVA::uint8>(DAVA::Min(255.0f, value[i * 3 + 2] + 0.5f));
        }
        else
        {
            target[0] = fillFloor[0];
            target[1] = fillFloor[1];
            target[2] = fillFloor[2];
        }
    }

    DAVA::int32 settledCount = 0;
    for (DAVA::int32 i = 0; i < pixelCount; ++i)
    {
        settledCount += (settled[i] != 0) ? 1 : 0;
    }
    DAVA::Logger::Info("[BeastManager] lightmap coverage: %d of %d texels unpainted, %d settled (relaxed %d passes), %d left at the floor",
                       totalUnpainted, pixelCount, settledCount, RELAX_PASSES, pixelCount - settledCount);
}

bool SaveFramebufferAsPNG(ILBFramebufferHandle framebuffer, const DAVA::FilePath& outputPath)
{
    DAVA::int32 width = 0;
    DAVA::int32 height = 0;
    BEAST_VERIFY(ILBGetResolution(framebuffer, &width, &height));
    if (width <= 0 || height <= 0)
    {
        BEAST_VERIFY(ILBDestroyFramebuffer(framebuffer));
        return false;
    }

    const DAVA::int32 pixelCount = width * height;
    DAVA::Vector<DAVA::uint8> pixels(static_cast<size_t>(pixelCount * 4), 255);

    // gamma 1.0: lightmaps are stored raw and processed later
    BEAST_VERIFY(ILBReadRegionLDR(framebuffer, 0, 0, width, height, ILB_CS_RGBA, 1.0f, pixels.data()));

    // Count BEFORE the fill. Measuring after it always reports 100% because the fill paints the
    // whole image, which is how a blank second target kept slipping through as a flat tile.
    DAVA::int32 paintedRaw = 0;
    for (DAVA::int32 i = 0; i < width * height; ++i)
    {
        const DAVA::uint8* texel = pixels.data() + static_cast<size_t>(i) * 4;
        if ((static_cast<DAVA::int32>(texel[0]) + texel[1] + texel[2]) > 6)
        {
            ++paintedRaw;
        }
    }
    if (paintedRaw * 100 < width * height)
    {
        DAVA::Logger::Error("[BeastManager] framebuffer readback is blank (%d of %d texels painted) for %s - not writing it",
                            paintedRaw, width * height, outputPath.GetAbsolutePathname().c_str());
        return false;
    }
    DAVA::Logger::Info("[BeastManager] readback %dx%d: %d of %d texels painted (%.1f%%) -> %s",
                       width, height, paintedRaw, width * height, 100.0 * paintedRaw / (width * height), outputPath.GetFilename().c_str());

    FillUnpaintedTexels(pixels.data(), width, height);

    DAVA::Image* image = DAVA::Image::Create(static_cast<DAVA::uint32>(width), static_cast<DAVA::uint32>(height), DAVA::PixelFormat::FORMAT_RGBA8888);
    std::memcpy(image->data, pixels.data(), pixels.size());

    const bool saved = DAVA::ImageSystem::Save(outputPath, image, DAVA::PixelFormat::FORMAT_RGBA8888) == DAVA::eErrorCode::SUCCESS;
    if (!saved)
    {
        DAVA::Logger::Error("[BeastManager] Can't save lightmap: %s", outputPath.GetAbsolutePathname().c_str());
    }

    image->Release();
    BEAST_VERIFY(ILBDestroyFramebuffer(framebuffer));
    return saved;
}
} // namespace BeastManagerDetails

bool BeastManager::shouldInit = true;

BeastManager::BeastManager()
{
    if (shouldInit)
    {
        StaticInit();
    }

    // cache + temp folder live inside the parser's temporary folder
    DAVA::FilePath cacheDirectory = SceneParser::GetTemporaryFolder() + "cache/";
    DAVA::FileSystem::Instance()->CreateDirectory(SceneParser::GetTemporaryFolder(), true);
    DAVA::FileSystem::Instance()->CreateDirectory(cacheDirectory, true);

    DAVA::FilePath beastBinaries = BeastManagerDetails::FindBeastResource("bin");
    DAVA::FilePath beastConfig = BeastManagerDetails::FindBeastResource("config/");
    BeastManagerDetails::ReportBeastInstall(beastBinaries, beastConfig);

    if (beastBinaries.IsEmpty())
    {
        initializationError = "Beast binaries not found. Deployed data must sit in Data.beast/bin next to the editor or in the working directory (set BEAST_ROOT to override).";
        DAVA::Logger::Error("[BeastManager] %s", initializationError.c_str());
        return;
    }

    const DAVA::String licenseKey = BeastManagerDetails::GetBeastLicenseKey();
    ILBStatus status = ILB_ST_INVALID_LICENSE;
    if (licenseKey.empty())
    {
        // No token available. thebeast.exe prints the same "License key corrupted /
        // Invalid license key format" with an empty key and with none at all, so that
        // wording is its generic no-license message, not a complaint about our string.
        // Passing NULL keeps a license installed next to the renderer in charge.
        status = ILBCreateManager(STRING_TO_BEAST_STRING(cacheDirectory.GetAbsolutePathname()),
                                  ILB_CS_LOCAL,
                                  nullptr,
                                  &handle);
    }
    else
    {
        DAVA::Logger::Info("[BeastManager] passing the license token to Beast");
        status = ILBCreateManager(STRING_TO_BEAST_STRING(cacheDirectory.GetAbsolutePathname()),
                                  ILB_CS_LOCAL,
                                  STRING_TO_BEAST_STRING(licenseKey),
                                  &handle);
    }
    if (status != ILB_ST_SUCCESS || handle == nullptr)
    {
        BeastManagerDetails::LogILBFailure("ILBCreateManager", status);
        initializationError = DAVA::Format("ILBCreateManager failed with status %d (wrong beast64.dll, missing dependency or rejected license key)", static_cast<DAVA::int32>(status));
        return;
    }

    status = ILBSetBeastPath(handle, STRING_TO_BEAST_STRING(beastBinaries.GetAbsolutePathname()));
    if (status != ILB_ST_SUCCESS)
    {
        BeastManagerDetails::LogILBFailure("ILBSetBeastPath", status);
        initializationError = DAVA::Format("ILBSetBeastPath('%s') failed with status %d", beastBinaries.GetAbsolutePathname().c_str(), static_cast<DAVA::int32>(status));
        return;
    }

    // a cold cache is wanted, but a warm one only costs time, never correctness
    status = ILBClearCache(handle);
    if (status != ILB_ST_SUCCESS)
    {
        DAVA::Logger::Warning("[BeastManager] ILBClearCache failed with status %d, continuing with the existing cache", static_cast<DAVA::int32>(status));
    }

    ready = true;
}

BeastManager::~BeastManager()
{
    DAVA::SafeDelete(sceneParser);

    for (DAVA::NMaterial* instance : appliedMaterialInstances)
    {
        DAVA::SafeRelease(instance);
    }
    appliedMaterialInstances.clear();

    for (auto& page : atlasTextures)
    {
        DAVA::SafeRelease(page.second);
    }
    atlasTextures.clear();

    if (job != nullptr)
    {
        // The manager owns the job. Destroying the manager under a running job
        // kills the renderer while its tools are still writing into the scratch
        // directory, and the caller then deletes that directory. Cancel first and
        // give the job a bounded moment to leave the running state.
        if (state == STATE_GENERATING)
        {
            ILBCancelJob(job);
        }

        for (DAVA::int32 i = 0; i < 500; ++i)
        {
            ILBBool running = 0;
            if (ILBIsJobRunning(job, &running) != ILB_ST_SUCCESS || running == 0)
            {
                break;
            }
            DAVA::Thread::Sleep(10);
        }
        job = nullptr;
    }

    if (handle != nullptr)
    {
        BEAST_VERIFY(ILBDestroyManager(handle));
        handle = nullptr;
    }
}

void BeastManager::StaticInit()
{
    BEAST_VERIFY(ILBSetLogTarget(ILB_LT_ERROR, ILB_LS_STDERR, nullptr));
    BEAST_VERIFY(ILBSetLogTarget(ILB_LT_INFO, ILB_LS_STDOUT, nullptr));
    shouldInit = false;
}

ILBManagerHandle BeastManager::GetILBManager()
{
    return handle;
}

ILBSceneHandle BeastManager::GetILBScene()
{
    return scene;
}

bool BeastManager::IsReady() const
{
    return ready && (handle != nullptr);
}

const DAVA::String& BeastManager::GetInitializationError() const
{
    return initializationError;
}

void BeastManager::Run(DAVA::Scene* sceneForBake)
{
    davaScene = sceneForBake;
    generatedLightmaps.clear();
    bakedTargetsTotal = 0;
    lodLevel = 0;
    switchIndex = 0;
    curTaskName = "Parse scene";
    curTaskProgress = 0;
    lastLoggedProgress = -1;
    notRunningTicks = 0;
    completionLogged = false;

    if (!ready || handle == nullptr)
    {
        DAVA::Logger::Error("[BeastManager] bake aborted: %s",
                            initializationError.empty() ? "Beast manager is not initialized" : initializationError.c_str());
        state = STATE_CANCELED;
        return;
    }

    if (sceneForBake == nullptr)
    {
        DAVA::Logger::Error("[BeastManager] bake aborted: no scene to bake");
        state = STATE_CANCELED;
        return;
    }

    if (state == STATE_GENERATING)
    {
        DAVA::Logger::Warning("[BeastManager] Run() called while a bake is running, the previous job is dropped");
    }

    state = STATE_GENERATING;

    // Resuming is opt-in through a marker file in the lightmaps directory. It is read here
    // because this is the last moment before the parser decides which targets still need a
    // bake, and the file sits at the top level, outside the sm/ and ao/ trees that the
    // per-run clean removes.
    resumeExistingBakes = DAVA::FileSystem::Instance()->IsFile(DAVA::FilePath(lightmapsDirectory) + "resume.flag");
    resumeSkippedTargets = 0;
    if (resumeExistingBakes)
    {
        // Say what is actually on disk, because "it re-baked everything" looks identical to
        // "there was nothing usable to resume": pages written by a pre-v10.24 build are named
        // after batch pointers (uppercase hex) and can never match a stable key (lowercase).
        DAVA::int32 existingPages = 0;
        DAVA::FileList* pageList = new DAVA::FileList(DAVA::FilePath(lightmapsDirectory) + "sm/");
        const DAVA::int32 pageCount = static_cast<DAVA::int32>(pageList->GetCount());
        for (DAVA::int32 i = 0; i < pageCount; ++i)
        {
            if (!pageList->IsDirectory(i) && pageList->GetPathname(i).IsEqualToExtension(".png"))
            {
                ++existingPages;
            }
        }
        pageList->Release();

        DAVA::Logger::Info("[BeastManager] resume.flag found: %d existing sm/ pages kept, targets with a page are skipped, everything else is baked",
                           existingPages);
    }
    else
    {
        ClearLightmapsDirectory();
    }

    // Before the walk, so the instances the parser registers survive.
    ResetLiveInstanceRegistry();

    if (!BeginScene())
    {
        // ILB refused to create the scene: the reason is already in the log.
        // Cancelled (not Done) so BeastRunner skips packing an empty folder.
        state = STATE_CANCELED;
        return;
    }

    sceneParser = new SceneParser(this, DAVA::MakeFunction(this, &BeastManager::OnSceneParsingCompleted));
    sceneParser->SetDumpTextures(dumpTextures);
    sceneParser->SetTextureMaxSize(textureMaxSize);
    ParseScene(davaScene);
}

void BeastManager::SetLightmapsDirectory(const DAVA::String& path)
{
    lightmapsDirectory = path;
    if (!lightmapsDirectory.empty() && lightmapsDirectory.back() != '/')
    {
        lightmapsDirectory += '/';
    }
}

void BeastManager::SetMode(eBeastMode _mode)
{
    mode = _mode;
}

eBeastMode BeastManager::GetMode() const
{
    return mode;
}

void BeastManager::SetLodLevel(int32 _lodLevel)
{
    lodLevel = _lodLevel;
}

int32 BeastManager::GetLodLevel()
{
    return lodLevel;
}

void BeastManager::SetSwitchIndex(int32 _switchIndex)
{
    switchIndex = _switchIndex;
}

int32 BeastManager::GetSwitchIndex()
{
    return switchIndex;
}

void BeastManager::ParseScene(DAVA::Scene* sceneForBake)
{
    DVASSERT(sceneParser != nullptr);

    if ((mode == eBeastMode::MODE_LIVE) && (liveRadius > 0.0f))
    {
        DAVA::Matrix4 cameraToWorld;
        const DAVA::Vector3 center = liveCameraTransform.GetInverse(cameraToWorld)
        ? cameraToWorld.GetTranslationVector()
        : DAVA::Vector3();
        sceneParser->SetLiveCullCenter(center, liveRadius);
        DAVA::Logger::Info("[BeastManager] live: uploading only entities within %.0f m of the camera", liveRadius);
    }

    parseStartTime = DAVA::SystemTimer::GetMs();
    sceneParser->StartParsing();
    maxLodMaxSwitch = sceneParser->FindMaxLod(sceneForBake);
    sceneParser->SetLodLevel(lodLevel);
    sceneParser->SetSwitchIndex(switchIndex);
    sceneParser->FindAndInitDirectionalLight(sceneForBake);
    sceneParser->ValidateDirectionalLight();
    sceneParser->ParseNodeRecursive(sceneForBake);
    // SceneParser completes asynchronously (landscape color thumbnails),
    // OnSceneParsingCompleted() is invoked when everything is parsed.
}

void BeastManager::OnSceneParsingCompleted()
{
    if (scene == nullptr)
    {
        // BeginScene failed before parsing started, or the bake was cancelled
        // while the landscape thumbnails were still being captured.
        state = STATE_CANCELED;
        return;
    }

    if (sceneParser == nullptr || sceneParser->ShouldCancel())
    {
        state = STATE_CANCELED;
        return;
    }

    // A scene with no light source bakes black. Shipped Blitz scenes often keep their
    // lights as editor-only objects (editor.staticlight.enable = 0) or as identity
    // transforms sitting under the terrain, so Beast gets nothing. A sky light
    // illuminates the whole scene and is the honest neutral: it is what the sky is for.
    if (sceneParser->GetLightsCreated() == 0)
    {
        const DAVA::String skyName = GetBeastName() + "_skyLight";
        ILBLightHandle skyLight = nullptr;
        const ILBMatrix4x4 skyTransform = ConvertDavaMatrix(DAVA::Matrix4::IDENTITY);
        const ILBLinearRGB skyIntensity(0.45f, 0.47f, 0.55f);
        const ILBStatus skyStatus = ILBCreateSkyLight(scene, STRING_TO_BEAST_STRING(skyName), &skyTransform, &skyIntensity, &skyLight);
        if (skyStatus != ILB_ST_SUCCESS || skyLight == nullptr)
        {
            BeastManagerDetails::LogILBFailure("ILBCreateSkyLight", skyStatus);
        }
        else
        {
            DAVA::Logger::Warning("[BeastManager] no light source reached Beast (the scene lights are editor-only, disabled for static light or at the world origin): created a sky light so the bake is not black");
        }
    }

    if (mode == eBeastMode::MODE_LIVE && !CreateLiveCamera())
    {
        // the camera is part of the scene and the scene cannot be changed after
        // EndScene, so a failed camera creation ends the session here
        state = STATE_FAILED;
        return;
    }

    const DAVA::int64 sceneEndStart = DAVA::SystemTimer::GetMs();
    EndScene();
    DAVA::Logger::Info("[BeastManager] scene setup (LOD %d): %.2f s to parse and upload, %.2f s to close the scene",
                       static_cast<DAVA::int32>(lodLevel),
                       static_cast<DAVA::float32>(sceneEndStart - parseStartTime) / 1000.0f,
                       static_cast<DAVA::float32>(DAVA::SystemTimer::GetMs() - sceneEndStart) / 1000.0f);

    if (!GenerateLightmaps())
    {
        DAVA::Logger::Error("[BeastManager] lightmap generation could not start, see the lines above");
        state = STATE_CANCELED;
    }
}

bool BeastManager::BeginScene()
{
    scene = nullptr;

    ILBStatus status = ILBBeginScene(handle, GENERATE_BEAST_NAME(BeastManager), &scene);
    if (status != ILB_ST_SUCCESS || scene == nullptr)
    {
        BeastManagerDetails::LogILBFailure("ILBBeginScene", status);
        DAVA::Logger::Error("[BeastManager] ILBBeginScene returned no scene (status %d). Check that Data.beast/bin holds thebeast.exe + rendertool.xml and that BEAST_LICENSE is valid.",
                            static_cast<DAVA::int32>(status));
        return false;
    }

    // DAVA is Z-up (right handed, OpenGL style). Beast defaults to Y-up, and
    // without this the whole scene is rolled a quarter turn: the camera view comes
    // out upside down and eRnsT's own window sits on its side. The property is a
    // scene setting, so it has to be set before ILBEndScene; setting it afterwards
    // is refused.
    const ILBStatus upStatus = ILBSetSceneUpVector(scene, ILB_UP_POS_Z);
    if (upStatus != ILB_ST_SUCCESS)
    {
        BeastManagerDetails::LogILBFailure("ILBSetSceneUpVector", upStatus);
    }
    // A DAVA unit is a metre, and Beast uses this for its light attenuation and
    // sampling distances.
    BEAST_VERIFY(ILBSetMeterPerWorldUnit(scene, 1.0f));

    return true;
}

void BeastManager::EndScene()
{
    // the handles die with the job's scene, so hold none past it
    bakeTargetEntities.clear();

    if (scene == nullptr)
    {
        return;
    }

    BEAST_VERIFY(ILBEndScene(scene));
}

bool BeastManager::GenerateLightmaps()
{
    if (scene == nullptr)
    {
        DAVA::Logger::Error("[BeastManager] GenerateLightmaps: no Beast scene, the bake cannot continue");
        return false;
    }

    // A live session is not a bake: it has no config XML, no render passes and no
    // output, and it must never reach the target/pass code below.
    if (mode == eBeastMode::MODE_LIVE)
    {
        return GenerateLiveJob();
    }

    // pick the configuration file deployed with the module
    DAVA::String configName = "lightmaps_mode.xml";
    if (mode == eBeastMode::MODE_SPHERICAL_HARMONICS)
    {
        configName = "sh_mode.xml";
    }
    else if (mode == eBeastMode::MODE_PREVIEW)
    {
        configName = "preview_mode.xml";
    }

    DAVA::FilePath configPath = BeastManagerDetails::FindBeastResource("config/" + configName);
    if (!DAVA::FileSystem::Instance()->Exists(configPath))
    {
        DAVA::Logger::Error("[BeastManager] Can't find Beast configuration file: %s", configPath.GetAbsolutePathname().c_str());
        state = STATE_DONE;
        return false;
    }

    BEAST_VERIFY(ILBCreateJob(handle, GENERATE_BEAST_NAME(BeastManager), scene,
                              STRING_TO_BEAST_STRING(configPath.GetAbsolutePathname()), &job));
    if (job == nullptr)
    {
        state = STATE_DONE;
        return false;
    }

    // bake output is written directly into the lightmaps directory
    if (!lightmapsDirectory.empty())
    {
        BEAST_VERIFY(ILBSetJobOutputPath(job, STRING_TO_BEAST_STRING(DAVA::FilePath(lightmapsDirectory).GetAbsolutePathname())));
    }

    const DAVA::List<BeastMeshInstance*>& instances = BeastMeshInstance::GetResourceList();

    if (mode == eBeastMode::MODE_SPHERICAL_HARMONICS)
    {
        BEAST_VERIFY(ILBCreateIlluminationPassSH(job, CONST_STRING_TO_BEAST_STRING("sh"), ILB_IM_FULL, &passSH));

        // one shared vertex target for the whole job
        ILBTargetHandle vertexTarget = nullptr;
        BEAST_VERIFY(ILBCreateVertexTarget(job, GENERATE_BEAST_NAME(BeastManager), &vertexTarget));
        BEAST_VERIFY(ILBAddPassToTarget(vertexTarget, passSH));

        for (BeastMeshInstance* instance : instances)
        {
            BEAST_VERIFY(ILBAddBakeInstance(vertexTarget, instance->GetILBMeshInstance(), nullptr));
        }

        const DAVA::List<BeastPointCloud*>& pointClouds = BeastPointCloud::GetResourceList();
        for (BeastPointCloud* pointCloud : pointClouds)
        {
            ILBTargetEntityHandle pointCloudEntity = nullptr;
            BEAST_VERIFY(ILBAddBakePointCloud(vertexTarget, pointCloud->GetPointCloudHandle(), &pointCloudEntity));
            pointCloud->SetTargets(vertexTarget, pointCloudEntity);
        }

        if (instances.empty() && pointClouds.empty())
        {
            DAVA::Logger::Error("[BeastManager] nothing to bake in SH mode: no Beast mesh instances and no point clouds");
            return false;
        }

        curTaskName = "Beast SH baking";
        curTaskProgress = 0;
    lastLoggedProgress = -1;
    notRunningTicks = 0;
    completionLogged = false;
        BEAST_VERIFY(ILBStartJob(job, ILB_SR_CLOSE_WHEN_DONE, ILB_RD_FORCE_LOCAL));
        return true;
    }

    // MODE_LIGHTMAPS / MODE_PREVIEW:
    //   sm pass = full illumination (lightmap), ao pass = ambient occlusion.
    //   LightmapsPacker expects the results in {lightmapsDirectory}/sm and
    //   {lightmapsDirectory}/ao.
    BEAST_VERIFY(ILBCreateIlluminationPass(job, CONST_STRING_TO_BEAST_STRING("sm"), ILB_IM_FULL, &passLM));
    // maxDistance decides whether this pass is contact occlusion or a vague environment term. At
    // 100 m a 30 cm crevice subtends a sliver of the sampling sphere and the pass returns 238-245
    // there, i.e. nothing: the join between the tower and its plinth then reads as a hard step
    // (54 -> 92) with no darkening. The shipped map darkens by 31% at that join (58 falling to 40,
    // then rising to 96 over 13 rows), which is contact occlusion at roughly half a metre, so the
    // pass looks one metre out now instead of a hundred.
    static const DAVA::float32 BEAST_AO_MAX_DISTANCE = 1.0f;
    static const DAVA::float32 BEAST_AO_CONE_ANGLE = 90.0f;
    BEAST_VERIFY(ILBCreateAmbientOcclusionPass(job, CONST_STRING_TO_BEAST_STRING("ao"), BEAST_AO_MAX_DISTANCE, BEAST_AO_CONE_ANGLE, &passSH));
    DAVA::Logger::Info("[BeastManager] ao pass: maxDistance %.2f m, cone %.1f deg", BEAST_AO_MAX_DISTANCE, BEAST_AO_CONE_ANGLE);

    if (passLM == nullptr)
    {
        DAVA::Logger::Error("[BeastManager] lightmap pass 'sm' could not be created");
        state = STATE_DONE;
        return false;
    }

    // Beast logs the real reason a job refuses to run into its own log, which by
    // default goes nowhere in an embedded build. Route it next to the render
    // output so a "job is not started" can be explained without guessing.
    static bool beastLogConfigured = false;
    if (!beastLogConfigured)
    {
        beastLogConfigured = true;

        DAVA::FilePath logDir = lightmapsDirectory.empty()
                                    ? DAVA::FileSystem::Instance()->GetCurrentWorkingDirectory()
                                    : DAVA::FilePath(lightmapsDirectory);
        DAVA::FileSystem::Instance()->CreateDirectory(logDir, true);

        const DAVA::String errorLog = (logDir + "beast_error.log").GetAbsolutePathname();
        const DAVA::String infoLog = (logDir + "beast_info.log").GetAbsolutePathname();
        beastErrorLogPath = errorLog;
        beastInfoLogPath = infoLog;
        logTargetsActive = true;

        ILBSetLogTarget(ILB_LT_ERROR, ILB_LS_FILE, STRING_TO_BEAST_STRING(errorLog));
        ILBSetLogTarget(ILB_LT_INFO, ILB_LS_FILE, STRING_TO_BEAST_STRING(infoLog));

        DAVA::Logger::Info("[BeastManager] Beast log routed to '%s' and '%s'", errorLog.c_str(), infoLog.c_str());
    }

    DAVA::Logger::Info("[BeastManager] passes ready: sm=%p ao=%p, %u light(s) in scene",
                       static_cast<void*>(passLM),
                       static_cast<void*>(passSH),
                       static_cast<DAVA::uint32>(BeastLight::GetResourceList().size()));

    DAVA::uint32 lightmapInstanceCount = 0;
    DAVA::uint32 targetCount = 0;
    DAVA::Map<DAVA::int32, DAVA::uint32> sizeHistogram;

    // target entities are only valid for the job that created them, and the UV set can only be
    // set once that job is running, so the handles have to survive from here to ILBStartJob
    bakeTargetEntities.clear();
    bakeTargetInstances.clear();
    bakeUVSetApplied = 0;
    bakeUVSetRefused = 0;
    bakeUVRectFitted = 0;
    bakeUVRectRefused = 0;

    for (BeastMeshInstance* instance : instances)
    {
        if (!instance->IsUseLightmap())
        {
            continue;
        }

        ++lightmapInstanceCount;

        DAVA::RenderBatch* batch = instance->GetRenderBatch();
        if (batch == nullptr || IsLightmapGenerated(batch))
        {
            // already baked during one of the previous LOD passes
            continue;
        }

        // A target of size 0 makes ILBCreateTextureTarget fail ("Parameter width must be above 0"),
        // and then every call on the null handle fails too, which buries the real cause. Anything
        // that reaches here without a size is a parser bug: clamp it and say which instance.
        DAVA::int32 targetSize = instance->GetLightmapSize();
        if (targetSize < 16)
        {
            if (zeroSizedTargets < 8)
            {
                DAVA::Logger::Warning("[BeastManager] instance '%s' has lightmap size %d, clamped to 16",
                                      instance->GetName().c_str(), targetSize);
            }
            ++zeroSizedTargets;
            targetSize = 16;
            instance->SetLightmapSize(targetSize);
        }

        TextureTarget* textureTarget = instance->GetTextureTarget();
        if (textureTarget == nullptr)
        {
            textureTarget = new TextureTarget(job, targetSize);
            instance->SetTextureTarget(textureTarget);
        }

        ++sizeHistogram[instance->GetLightmapSize()];

        ILBTargetEntityHandle targetEntity = nullptr;
        BEAST_VERIFY(ILBAddBakeInstance(textureTarget->GetHandle(), instance->GetILBMeshInstance(), &targetEntity));
        if (targetEntity != nullptr)
        {
            bakeTargetEntities.push_back(targetEntity);
            bakeTargetInstances.push_back(instance);

            // before the passes and before ILBStartJob, exactly like the bundled baking example
            ConfigureTargetEntity(targetEntity, instance);
        }
        BEAST_VERIFY(ILBAddPassToTarget(textureTarget->GetHandle(), passLM));
        BEAST_VERIFY(ILBAddPassToTarget(textureTarget->GetHandle(), passSH));
        ++targetCount;
    }

    // ILB answers "Nothing to do in job" (634) when the job has no target, which
    // is what an empty selection looks like. Report the numbers instead of
    // handing Beast an empty job.
    if (targetCount == 0 && bakedTargetsTotal > 0)
    {
        // Later LOD passes exist only to reach instances whose LOD batch was not
        // accepted in the earlier ones. When a pass finds nothing new, the chain is
        // finished: it is not an error, and the lightmaps the earlier passes wrote
        // still have to be packed. Failing here is what threw away the whole bake.
        DAVA::Logger::Info("[BeastManager] LOD %d: nothing new to bake, every marked instance already has a lightmap (baked so far: %d targets). Finishing the chain.",
                           static_cast<DAVA::int32>(lodLevel), bakedTargetsTotal);
        job = nullptr;
        passLM = nullptr;
        passSH = nullptr;
        if (lodLevel < maxLodMaxSwitch.maxLodLevel)
        {
            StartLodPass();
        }
        else
        {
            EndLodPass();
        }
        return true;
    }

    if (targetCount == 0 && resumeExistingBakes && resumeSkippedTargets > 0)
    {
        // Every target already has its page: this is a resumed run that finished the work in
        // an earlier session. Pack what is on disk instead of declaring an empty bake.
        DAVA::Logger::Info("[BeastManager] nothing new to bake: all %d targets were reused from disk. Packing the existing pages.",
                           resumeSkippedTargets);
        job = nullptr;
        passLM = nullptr;
        passSH = nullptr;
        if (lodLevel < maxLodMaxSwitch.maxLodLevel)
        {
            StartLodPass();
        }
        else
        {
            EndLodPass();
        }
        return true;
    }

    if (targetCount == 0)
    {
        DAVA::Logger::Error("[BeastManager] nothing to bake: %u Beast mesh instances, %u marked for lightmaps, 0 targets. "
                            "See the [SceneParser] lines above for the per-batch reason.",
                            static_cast<DAVA::uint32>(instances.size()),
                            lightmapInstanceCount);
        return false;
    }

    {
        // A shipped .sc2 has illumination.lightmapSize stripped along with FLAG_ILLUMINATION_USED,
        // so a material without it resolves to NMaterial::DEFAULT_LIGHTMAP_SIZE (16 px). Print
        // what every target actually got, otherwise a bake can spend an hour producing 16x16.
        DAVA::String sizes;
        for (const auto& entry : sizeHistogram)
        {
            sizes += DAVA::Format("%s%d x %u", sizes.empty() ? "" : ", ", entry.first, entry.second);
        }
        DAVA::Logger::Info("[BeastManager] lightmap target sizes: %s", sizes.c_str());
    }

    bakedTargetsTotal += static_cast<DAVA::int32>(targetCount);
    DAVA::Logger::Info("[BeastManager] LOD %d: submitting %u lightmap targets from %u instances (%d baked before this job)",
                       static_cast<DAVA::int32>(lodLevel), targetCount, lightmapInstanceCount, bakedTargetsTotal - static_cast<DAVA::int32>(targetCount));

    curTaskName = "Beast lightmaps baking";
    curTaskProgress = 0;
    lastLoggedProgress = -1;
    notRunningTicks = 0;
    completionLogged = false;

    ApplyBakeRenderSettings(job);

    ILBStatus startStatus = ILBStartJob(job, ILB_SR_CLOSE_WHEN_DONE, ILB_RD_FORCE_LOCAL);
    if (startStatus != ILB_ST_SUCCESS)
    {
        ILBStringHandle extendedError = nullptr;
        ILBGetExtendErrorInformation(&extendedError);
        DAVA::Logger::Error("[BeastManager] ILBStartJob failed (%d): %s",
                            static_cast<DAVA::int32>(startStatus),
                            ConvertBeastString(extendedError).c_str());
        state = STATE_DONE;
        return false;
    }

    // a job that is already not running right after a successful start aborted
    // immediately - say why instead of polling a dead handle for a minute
    ILBBool startedRunning = 0;
    if (ILBIsJobRunning(job, &startedRunning) == ILB_ST_SUCCESS && startedRunning == 0)
    {
        ILBJobStatus jobStatus = ILB_JS_OTHER_ERROR;
        ILBGetJobResult(job, &jobStatus);
        DAVA::Logger::Error("[BeastManager] the job is not running right after ILBStartJob (job status %d)",
                            static_cast<DAVA::int32>(jobStatus));
        BeastManagerDetails::LogBeastRendererErrors(beastInfoLogPath);
    }

    // Everything the targets need was applied while they were created; this only reports it, so a
    // run where the transform was silently ignored is obvious from the log.
    ReportBakeTargetConfiguration();

    return true;
}

void BeastManager::Update()
{
    if (state != STATE_GENERATING || job == nullptr)
    {
        return;
    }

    if (mode == eBeastMode::MODE_LIVE)
    {
        UpdateLive();
        return;
    }

    // ILBWaitJobDone is the pump, not just a wait: its documented behaviour is
    // "waits until a job is done or until there is progress updates". Autodesk's
    // own example loop (Thirdparty/examples/code/common/utils.h renderJob) calls
    // it on every iteration for exactly this reason. The status is ignored on
    // purpose: Beast reports "Job is not started" for it on local jobs while the
    // job is fine, and the VERIFY wrapper would fill the log with it.
    ILBWaitJobDone(job, 10);

    RetriveProgress();

    // progress also goes to the log, so a stuck bar can be told apart from a
    // stuck job without guessing
    if (curTaskProgress != lastLoggedProgress)
    {
        lastLoggedProgress = curTaskProgress;
        DAVA::Logger::Info("[BeastManager] %s: %d%%", curTaskName.c_str(), curTaskProgress);
    }

    // The exit condition is ILBIsJobRunning, never ILBIsJobCompleted.
    // beastjob.h:383 says a completed job "might still be running", and Autodesk's
    // example logs it as "Job is completed. It might still be running if the user
    // has selected ILB_SR_KEEP_OPEN." Completed means every target has been baked;
    // Beast then keeps the job alive for the collection pass that writes the
    // output, which for 518 targets is the longest part of the run. Reading the
    // result in that window only ever returns "Job is not finished."
    ILBBool completed = 0;
    if (ILBIsJobCompleted(job, &completed) == ILB_ST_SUCCESS && completed != 0 && !completionLogged)
    {
        completionLogged = true;
        DAVA::Logger::Info("[BeastManager] every target is baked; the job is still running while Beast collects the output. "
                           "This stage runs one thebeast.exe per target and is usually the longest.");
    }

    ILBBool running = 0;
    if (ILBIsJobRunning(job, &running) != ILB_ST_SUCCESS)
    {
        return; // the query itself failed: ask again on the next tick
    }

    if (running != 0)
    {
        notRunningTicks = 0;
        resultQueryDeadlineMs = 0;
        return; // still rendering, the result is not readable yet
    }

    // Not running any more: finished, crashed, or the handle died. Give the tool
    // chain a moment to settle before treating it as over.
    if (++notRunningTicks <= NOT_RUNNING_GRACE_TICKS)
    {
        return;
    }

    OnJobCompleted();
}

bool BeastManager::IsJobDone()
{
    return (state == STATE_DONE) || (state == STATE_CANCELED) || (state == STATE_FAILED);
}

bool BeastManager::HasFailed() const
{
    return state == STATE_FAILED;
}

void BeastManager::OnJobCompleted()
{
    // ILBIsJobCompleted is a signal, not a verdict: beastjob.h states that a job
    // which is not running "might not have finished if it was aborted or had
    // errors", and the completion flag flips while the result is still
    // unavailable. Retry inside a bounded window instead of turning a transient
    // query failure into "the bake failed".
    ILBJobStatus jobStatus = ILB_JS_OTHER_ERROR;
    const ILBStatus queryStatus = ILBGetJobResult(job, &jobStatus);

    if (queryStatus != ILB_ST_SUCCESS)
    {
        const DAVA::int64 now = DAVA::SystemTimer::GetMs();
        if (resultQueryDeadlineMs == 0)
        {
            resultQueryDeadlineMs = now + RESULT_QUERY_TIMEOUT_MS;
        }

        if (now < resultQueryDeadlineMs)
        {
            return; // inside the grace window: ask again on the next tick
        }

        // "Job is not finished" also means the job picked up work again (the
        // collection queue is not idle in the middle of a bake). Never call a
        // resuming job a failure.
        ILBBool running = 0;
        if (ILBIsJobRunning(job, &running) == ILB_ST_SUCCESS && running != 0)
        {
            resultQueryDeadlineMs = 0;
            notRunningTicks = 0;
            return;
        }

        ILBStringHandle extendedError = nullptr;
        ILBGetExtendErrorInformation(&extendedError);
        DAVA::Logger::Error("[BeastManager] the job never returned a status: ILBGetJobResult failed for %d ms (Beast: %s). "
                            "Nothing was read back, so nothing is packed; the render output is kept in '%s'.",
                            static_cast<DAVA::int32>(RESULT_QUERY_TIMEOUT_MS),
                            ConvertBeastString(extendedError).c_str(),
                            lightmapsDirectory.c_str());
        BeastManagerDetails::LogBeastRendererErrors(beastInfoLogPath);
        resultQueryDeadlineMs = 0;
        state = STATE_FAILED;
        return;
    }

    resultQueryDeadlineMs = 0;

    if (jobStatus != ILB_JS_SUCCESS)
    {
        DAVA::Logger::Error("[BeastManager] Beast job finished with status %d", static_cast<DAVA::int32>(jobStatus));
        if (jobStatus == ILB_JS_CANCELLED)
        {
            state = STATE_CANCELED;
            return;
        }

        // crash, invalid license, config or command line error: the renderer
        // produced nothing usable, so keep the output instead of packing it away
        BeastManagerDetails::LogBeastRendererErrors(beastInfoLogPath);
        state = STATE_FAILED;
        return;
    }

    SaveResults();

    if (mode == eBeastMode::MODE_LIGHTMAPS && lodLevel < maxLodMaxSwitch.maxLodLevel)
    {
        // bake the next LOD in the chain
        StartLodPass();
    }
    else
    {
        EndLodPass();
    }
}

void BeastManager::StartLodPass()
{
    ++lodLevel;
    state = STATE_GENERATING;
    curTaskName = DAVA::Format("Beast baking LOD %d", lodLevel);
    curTaskProgress = 0;
    lastLoggedProgress = -1;
    notRunningTicks = 0;
    completionLogged = false;

    // the ILB scene is immutable after ILBEndScene: rebuild it for the new LOD
    job = nullptr;
    passLM = nullptr;
    passSH = nullptr;

    sceneParser->ClearScene();
    BeginScene();
    RunLodIteration();
}

void BeastManager::EndLodPass()
{
    if (mode == eBeastMode::MODE_LIGHTMAPS)
    {
        DAVA::Logger::Info("[BeastManager] bake chain finished: %d LOD pass(es), %d lightmap target(s) submitted in total",
                           static_cast<DAVA::int32>(lodLevel) + 1, bakedTargetsTotal);
    }
    job = nullptr;
    passLM = nullptr;
    passSH = nullptr;
    state = STATE_DONE;
    curTaskName = "Beast finished";
    curTaskProgress = 100;
}

void BeastManager::RunLodIteration()
{
    DVASSERT(sceneParser != nullptr);
    sceneParser->SetLodLevel(lodLevel);
    sceneParser->SetSwitchIndex(switchIndex);
    ParseScene(davaScene);
}

void BeastManager::ResetLiveInstanceRegistry()
{
    // Called before the scene walk: clearing it afterwards (as the first version did)
    // threw away every instance the parser had just registered, so nothing synced.
    liveTrackedInstances.clear();
    liveSyncCounter = 0;
}

void BeastManager::RetriveProgress()
{
    ILBBool newActivity = 0;
    ILBBool newProgress = 0;
    BEAST_VERIFY(ILBJobHasNewProgress(job, &newActivity, &newProgress));
    if (newProgress == 0 && newActivity == 0)
    {
        return;
    }

    ILBStringHandle taskNameHandle = nullptr;
    DAVA::int32 progress = 0;
    if (ILBGetJobProgress(job, &taskNameHandle, &progress) == ILB_ST_SUCCESS)
    {
        if (taskNameHandle != nullptr)
        {
            curTaskName = ConvertBeastString(taskNameHandle);
        }
        curTaskProgress = progress;
    }
}

// ---------------------------------------------------------------------------
// Live session (eRnsT)
//
// Autodesk's own example (Thirdparty/examples/code/liveernst) is the contract:
//   scene + camera -> ILBCreateLiveErnstJob -> targets added to the job ->
//   ILBStartJob -> ILBSetErnstGIBakingMode(CAMERA_FRUSTUM) -> drain
//   ILBGetJobUpdate until the caller is done.
// There are no render passes, no output files, and the job never finishes on
// its own unless the renderer dies.
// ---------------------------------------------------------------------------

void BeastManager::SetLiveTargetSize(DAVA::int32 size)
{
    liveTargetSize = DAVA::Max<DAVA::int32>(16, size);
}

void BeastManager::SetLiveShowWindow(bool show)
{
    liveShowWindow = show;
}

void BeastManager::SetUseMaterialIllumination(bool use)
{
    lightmapSelection = use ? eLightmapSelection::Flagged : eLightmapSelection::All;
}

bool BeastManager::UseMaterialIllumination() const
{
    return lightmapSelection == eLightmapSelection::Flagged;
}

void BeastManager::SetBakeQuality(eBakeQuality quality)
{
    bakeQuality = quality;
}

BeastManager::eBakeQuality BeastManager::GetBakeQuality() const
{
    return bakeQuality;
}

// The three knobs Beast exposes for a bake job, from its own job.xml defaults:
//   GISettings/GIEngine  MinSamples 20  MaxSamples 32768  VarianceLimit 0.001
//   GISettings/GIEngine  MinDepth 1     MaxDepth 3
// Those defaults are what a 45 minute bake spends its time on: with fgInterpolationPoints 1
// there is no irradiance cache, so every texel is a full final gather, and the variance limit
// lets a pathological texel keep going to 32768 samples. Capping the tail is the cheap win;
// the fixed per-texel cost (fgRays 600) is not reachable from this API.
void BeastManager::ApplyBakeRenderSettings(ILBJobHandle bakeJob)
{
    if (bakeQuality == eBakeQuality::BeastDefault)
    {
        DAVA::Logger::Info("[BeastManager] bake samples: Beast defaults (MaxSamples 32768, variance 0.001, GI depth 3)");
        return;
    }

    DAVA::int32 maxSamples = 2048;
    DAVA::float32 variance = 0.005f;
    DAVA::int32 maxDepth = 2;
    switch (bakeQuality)
    {
    case eBakeQuality::Test:
        // for "did the atlas land where I think it did" runs: shape and placement are exact,
        // the light is noisy
        maxSamples = 256;
        variance = 0.05f;
        maxDepth = 1;
        break;
    case eBakeQuality::Fast:
        maxSamples = 512;
        variance = 0.02f;
        maxDepth = 1;
        break;
    case eBakeQuality::Clean:
        maxSamples = 8192;
        variance = 0.001f;
        maxDepth = 3;
        break;
    default:
        break;
    }

    const ILBStatus sampleStatus = ILBSetJobRenderSamples(bakeJob, 20, maxSamples, variance);
    const ILBStatus depthStatus = ILBSetJobRenderDepth(bakeJob, 1, maxDepth);
    DAVA::Logger::Info("[BeastManager] bake samples: MaxSamples %d, variance %.4f, GI depth 1..%d (samples status %d, depth status %d)",
                       maxSamples, variance, maxDepth, static_cast<DAVA::int32>(sampleStatus), static_cast<DAVA::int32>(depthStatus));

    sampleCapRefused = (sampleStatus != ILB_ST_SUCCESS);

    if (sampleStatus != ILB_ST_SUCCESS)
    {
        // 5 is ILB_ST_INVALID_OBJECT_STATE, i.e. the job refused the sample budget and keeps
        // Beast's own MaxSamples 32768: the bake then costs what it always cost and the presets
        // mean nothing. Say so instead of printing a number that never took effect.
        DAVA::Logger::Warning("[BeastManager] the sample cap was REFUSED (status %d): this bake runs with Beast's defaults, not %d samples",
                              static_cast<DAVA::int32>(sampleStatus), maxSamples);
    }
}

bool BeastManager::ConfigureTargetEntity(ILBTargetEntityHandle targetEntity, BeastMeshInstance* targetInstance)
{
    if (targetEntity == nullptr)
    {
        return false;
    }

    // The bundled example (Thirdparty/examples/code/baking/baking.cpp:181) sets the UV transform
    // right after ILBAddBakeInstance and before the job runs. Doing it after ILBStartJob returns
    // SUCCESS but has no effect at all: the last run's tiles still had their content sitting at
    // the raw UV coordinates, which is exactly "transform ignored". Same order for the UV set.
    DAVA::String targetUVSet = bakeUVSetName;
    DAVA::RenderBatch* targetBatch = (targetInstance == nullptr) ? nullptr : targetInstance->GetRenderBatch();
    auto rectIt = (targetBatch == nullptr) ? lightmapUVRects.end() : lightmapUVRects.find(targetBatch);
    if (rectIt != lightmapUVRects.end())
    {
        const LightmapUVRect& rect = rectIt->second;
        targetUVSet = DAVA::Format("%d", static_cast<DAVA::int32>(rect.uvSet));

        // No ILBSetUVTransform. Measured from a real bake: the island is painted where uv1 says
        // [0,1] maps onto the whole target, not where a fitted rectangle would put it. Stretching
        // the batch's uv rectangle over the target put 30% of the rock's triangles outside the
        // painted area, which is what the solid black faces were. The shipped map does the same as
        // this line does now: uv1 straight in, and the tile placed by the material transform.
        (void) rect;
    }

    if (!targetUVSet.empty())
    {
        const ILBStatus uvStatus = ILBSetBakeUVSet(targetEntity, STRING_TO_BEAST_STRING(targetUVSet));
        if (uvStatus == ILB_ST_SUCCESS)
        {
            ++bakeUVSetApplied;
        }
        else
        {
            if (bakeUVSetRefused < 3)
            {
                BeastManagerDetails::LogILBFailure(DAVA::Format("ILBSetBakeUVSet '%s'", targetUVSet.c_str()).c_str(), uvStatus);
            }
            ++bakeUVSetRefused;
            return false;
        }
    }

    return true;
}

void BeastManager::ReportBakeTargetConfiguration()
{
    DAVA::Logger::Info("[BeastManager] bake targets: UV set '%s' %d set / %d refused, uv1 used as authored (no rectangle fit)",
                       bakeUVSetName.c_str(), bakeUVSetApplied, bakeUVSetRefused);
}

void BeastManager::SetBakeUVSetName(const DAVA::String& uvName)
{
    bakeUVSetName = uvName;
    DAVA::Logger::Info("[BeastManager] bake UV set: '%s'", bakeUVSetName.c_str());
}

DAVA::String BeastManager::GetBakeUVSetName() const
{
    return bakeUVSetName;
}

void BeastManager::SetLightmapSelection(eLightmapSelection selection)
{
    lightmapSelection = selection;
    DAVA::Logger::Info("[BeastManager] lightmap selection: %s",
                       (selection == eLightmapSelection::All) ? "all, scene rects where present"
                       : (selection == eLightmapSelection::AutoOnly) ? "auto only, rects ignored"
                                                                     : "flagged only");
}

BeastManager::eLightmapSelection BeastManager::GetLightmapSelection() const
{
    return lightmapSelection;
}

bool BeastManager::UseStoredLightmapRects() const
{
    return lightmapSelection != eLightmapSelection::AutoOnly;
}

void BeastManager::SetLiveRenderQuality(DAVA::float32 quality)
{
    liveRenderQuality = DAVA::Max(0.0f, DAVA::Min(quality, 1.0f));
}

void BeastManager::SetLiveViewPerPixel(bool perPixel)
{
    if (liveViewPerPixel == perPixel)
    {
        return;
    }
    liveViewPerPixel = perPixel;

    // Render mode is a camera-target property, so a running session can switch
    // without a restart: lightmap lighting composes the frame from per-instance
    // targets, and with no target data that is a checkerboard, not lighting.
    if (mode == eBeastMode::MODE_LIVE && liveCameraTarget != nullptr)
    {
        const ILBCameraRenderModeType renderMode = perPixel ? ILB_LEC_CAMERA_PER_PIXEL_LIGHTING : ILB_LEC_CAMERA_LIGHT_MAP_LIGHTING;
        const ILBStatus status = ILBCameraTargetSetRenderMode(liveCameraTarget, renderMode);
        DAVA::Logger::Info("[BeastManager] live camera render mode -> %s (status %d)",
                           perPixel ? "per-pixel" : "lightmap lighting", static_cast<DAVA::int32>(status));
    }
}

void BeastManager::SetLiveEventPump(DAVA::Function<void()> pump)
{
    // Kept here as well: the target loop below runs on this thread and blocks the
    // window just like the scene walk does.
    livePump = pump;
    if (sceneParser != nullptr)
    {
        sceneParser->SetEventPump(pump);
    }
}

void BeastManager::SetLiveRadius(DAVA::float32 meters)
{
    liveRadius = DAVA::Max(0.0f, meters);
}

void BeastManager::SetDumpTextures(bool value)
{
    dumpTextures = value;
    if (sceneParser != nullptr)
    {
        sceneParser->SetDumpTextures(value);
    }
}

void BeastManager::SetTextureMaxSize(DAVA::int32 size)
{
    textureMaxSize = DAVA::Max<DAVA::int32>(0, size);
    if (sceneParser != nullptr)
    {
        sceneParser->SetTextureMaxSize(textureMaxSize);
    }
}

void BeastManager::SetLiveMaxTargets(DAVA::int32 count)
{
    liveMaxTargets = DAVA::Max(1, count);
}

namespace
{
// DAVA and Beast do not agree on what a "camera matrix" is.
//
// DAVA stores the VIEW matrix in Camera::GetMatrix(): Matrix4::BuildLookAtMatrix
// fills it as a world-to-camera matrix (rows are the camera basis, translation
// -R*p, Matrix4.h:293). Beast wants the opposite: the sample builds
// translation(position) * cameraOrientation(forward, up), a camera-to-world
// transform, and ConvertDavaMatrix flips rows to columns on the way in, which is
// what a column-major reader needs for a logical matrix.
//
// Feeding the view matrix straight in therefore gave Beast an inverse rotation
// plus a translation that is not a position at all: the camera sat at the origin
// pointing the wrong way, which is why the panel showed a white surface with a
// few edges. Invert first, then convert.
ILBMatrix4x4 MakeBeastCameraMatrix(const DAVA::Matrix4& davaViewMatrix)
{
    DAVA::Matrix4 cameraToWorld;
    if (!davaViewMatrix.GetInverse(cameraToWorld))
    {
        DAVA::Logger::Warning("[BeastManager] live camera: the view matrix is singular, pushing it unconverted");
        return ConvertDavaMatrix(davaViewMatrix);
    }
    return ConvertDavaMatrix(cameraToWorld);
}
} // namespace
void BeastManager::SetLiveCamera(const DAVA::Matrix4& worldTransform, DAVA::float32 horizontalFovRadians, DAVA::float32 pixelAspect)
{
    // Beast takes radians and rejects anything outside (0, pi). DAVA hands out
    // degrees, so the caller converts; the clamp here keeps a bad value from
    // killing the session instead of the single call.
    const DAVA::float32 fov = DAVA::Max(0.01f, DAVA::Min(horizontalFovRadians, 3.14159f));
    const DAVA::float32 aspect = (pixelAspect > 0.0f) ? pixelAspect : 1.0f;

    // A live renderer refines progressively per camera position, so a push from
    // an unchanged view is noise at best: the panel calls this every tick and the
    // editor camera usually has not moved. Only a real change is forwarded.
    bool changed = !liveCameraSent;
    if (!changed)
    {
        for (DAVA::int32 row = 0; row < 4 && !changed; ++row)
        {
            for (DAVA::int32 column = 0; column < 4; ++column)
            {
                if (DAVA::Abs(worldTransform._data[row][column] - liveCameraTransform._data[row][column]) > 1.0e-5f)
                {
                    changed = true;
                    break;
                }
            }
        }
        changed = changed || DAVA::Abs(fov - liveCameraFov) > 1.0e-5f || DAVA::Abs(aspect - liveCameraAspect) > 1.0e-5f;
    }

    liveCameraTransform = worldTransform;
    liveCameraFov = fov;
    liveCameraAspect = aspect;

    if (!changed || liveCamera == nullptr)
    {
        return;
    }

    // A jump of hundreds of units between two ticks is not a pan, it is another
    // view or another camera taking over, and it is worth a line in the log: the
    // panel then jumps with it and the user sees the view teleported.
    // The position lives in the inverse: GetTranslationVector() of a view matrix
    // is -R*p, not where the camera is.
    DAVA::Matrix4 pushedCameraToWorld;
    const DAVA::Vector3 pushedPosition = liveCameraTransform.GetInverse(pushedCameraToWorld)
    ? pushedCameraToWorld.GetTranslationVector()
    : DAVA::Vector3();
    if (liveCameraSent)
    {
        const DAVA::float32 moveDistance = (pushedPosition - liveCameraPushedPosition).Length();
        if (moveDistance > 100.0f && liveCameraJumpLogs < 8)
        {
            ++liveCameraJumpLogs;
            DAVA::Logger::Info("[BeastManager] live camera jumped %.1f units in one tick, from (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f) - another view or camera took over",
                               moveDistance, liveCameraPushedPosition.x, liveCameraPushedPosition.y, liveCameraPushedPosition.z,
                               pushedPosition.x, pushedPosition.y, pushedPosition.z);
        }
    }

    ILBMatrix4x4 matrix = MakeBeastCameraMatrix(liveCameraTransform);
    const ILBStatus transformStatus = ILBSetCameraTransform(liveCamera, &matrix);
    const ILBStatus fovStatus = ILBSetFov(liveCamera, liveCameraFov, liveCameraAspect);
    if (!liveCameraSent)
    {
        // one line to prove the camera sits where the editor camera sits: if these
        // numbers look nothing like the editor's position, the matrix is still wrong
        DAVA::Logger::Info("[BeastManager] live camera: first push from (%.1f, %.1f, %.1f), fov %.1f deg, status %d",
                           pushedPosition.x, pushedPosition.y, pushedPosition.z,
                           liveCameraFov * 57.2957795f, static_cast<DAVA::int32>(transformStatus));
    }
    liveCameraSent = (transformStatus == ILB_ST_SUCCESS);
    liveCameraPushedPosition = pushedPosition;
    ++liveCameraPushes;
    if (!liveCameraSent)
    {
        BeastManagerDetails::LogILBFailure("ILBSetCameraTransform (live)", transformStatus);
    }
    if (fovStatus != ILB_ST_SUCCESS)
    {
        BeastManagerDetails::LogILBFailure("ILBSetFov (live)", fovStatus);
    }
}

bool BeastManager::CreateLiveCamera()
{
    if (liveCamera != nullptr)
    {
        return true;
    }

    if (scene == nullptr)
    {
        liveError = true;
        liveErrorText = "no ILB scene for the live camera";
        return false;
    }

    ILBMatrix4x4 matrix = MakeBeastCameraMatrix(liveCameraTransform);
    const ILBStatus status = ILBCreatePerspectiveCamera(scene, CONST_STRING_TO_BEAST_STRING("beast_live_camera"), &matrix, &liveCamera);
    if (status != ILB_ST_SUCCESS || liveCamera == nullptr)
    {
        BeastManagerDetails::LogILBFailure("ILBCreatePerspectiveCamera", status);
        liveError = true;
        liveErrorText = "ILBCreatePerspectiveCamera failed";
        return false;
    }

    BEAST_VERIFY(ILBSetFov(liveCamera, liveCameraFov, liveCameraAspect));
    return true;
}

bool BeastManager::GenerateLiveJob()
{
    DVASSERT(scene != nullptr);

    // A live renderer writes the reason it does nothing into its own log, and by
    // default that log goes nowhere. Route it beside the exe like the bake path.
    if (!logTargetsActive)
    {
        logTargetsActive = true;
        const DAVA::String workingDir = DAVA::FileSystem::Instance()->GetCurrentWorkingDirectory().GetAbsolutePathname();
        beastErrorLogPath = workingDir + "beast_live_error.log";
        beastInfoLogPath = workingDir + "beast_live_info.log";
        ILBSetLogTarget(ILB_LT_ERROR, ILB_LS_FILE, STRING_TO_BEAST_STRING(beastErrorLogPath));
        ILBSetLogTarget(ILB_LT_INFO, ILB_LS_FILE, STRING_TO_BEAST_STRING(beastInfoLogPath));
        DAVA::Logger::Info("[BeastManager] Beast live log routed to '%s' and '%s'", beastErrorLogPath.c_str(), beastInfoLogPath.c_str());
    }

    const ILBStatus status = ILBCreateLiveErnstJob(handle, GENERATE_BEAST_NAME(BeastManager), scene, &job);
    if (status != ILB_ST_SUCCESS || job == nullptr)
    {
        BeastManagerDetails::LogILBFailure("ILBCreateLiveErnstJob", status);
        liveError = true;
        liveErrorText = "ILBCreateLiveErnstJob failed";
        return false;
    }

    // Autodesk's own live sample (Thirdparty/examples/code/liveernst) uses ONE
    // texture target for the whole session and adds every instance to it through
    // ILBAddBakeInstance, giving each target entity its own resolution with
    // ILBSetBakeResolution. One target per instance would mean thousands of render
    // targets and no benefit.
    liveTargets.clear();
    liveTargetIndexByName.clear();

    if (liveTextureTarget == nullptr)
    {
        const DAVA::String targetName = DAVA::Format("%s_liveTextureTarget", GetBeastName().c_str());
        const ILBStatus targetStatus = ILBCreateTextureTarget(job, STRING_TO_BEAST_STRING(targetName), liveTargetSize, liveTargetSize, &liveTextureTarget);
        if (targetStatus != ILB_ST_SUCCESS || liveTextureTarget == nullptr)
        {
            BeastManagerDetails::LogILBFailure("ILBCreateTextureTarget (live)", targetStatus);
            liveError = true;
            liveErrorText = "ILBCreateTextureTarget failed for the live session";
            return false;
        }
    }

    // The camera target is the view itself. Beast renders the session camera into
    // it and streams ILB_UT_UPDATE_CAMERA_FRAME_BUFFER, and frustum baking mode
    // derives its work from a camera target: with a texture target alone the
    // renderer has no view, reports activity 0 / progress 0 and shows a black
    // window.
    if (liveCamera != nullptr && liveCameraTarget == nullptr)
    {
        const DAVA::String cameraTargetName = DAVA::Format("%s_liveCameraTarget", GetBeastName().c_str());
        const ILBStatus cameraTargetStatus = ILBCreateCameraTarget(job, STRING_TO_BEAST_STRING(cameraTargetName), liveCamera,
                                                                   liveViewWidth, liveViewHeight, &liveCameraTarget);
        if (cameraTargetStatus != ILB_ST_SUCCESS || liveCameraTarget == nullptr)
        {
            BeastManagerDetails::LogILBFailure("ILBCreateCameraTarget (live)", cameraTargetStatus);
            liveCameraTarget = nullptr;
        }
        else
        {
            const ILBCameraRenderModeType renderMode = liveViewPerPixel ? ILB_LEC_CAMERA_PER_PIXEL_LIGHTING : ILB_LEC_CAMERA_LIGHT_MAP_LIGHTING;
            const ILBStatus modeStatus = ILBCameraTargetSetRenderMode(liveCameraTarget, renderMode);
            DAVA::Logger::Info("[BeastManager] live camera target: %dx%d, %s lighting (render mode status %d)",
                               liveViewWidth, liveViewHeight, liveViewPerPixel ? "per-pixel" : "lightmap",
                               static_cast<DAVA::int32>(modeStatus));
        }
    }

    // Only what the camera can see becomes a target. Beast keeps a framebuffer per
    // target entity, and a live frame for 1235 instances at 512 px is a full bake:
    // it produced no update at all in the first minutes. Nearest first, capped.
    const DAVA::List<BeastMeshInstance*>& instances = BeastMeshInstance::GetResourceList();

    // A BaseObject starts at referenceCount 1 and its destructor asserts the count
    // is 0, so it can never live on the stack: that assert is what fired here.
    DAVA::Frustum* frustum = new DAVA::Frustum();
    bool haveFrustum = false;
    DAVA::Camera* camera = davaScene != nullptr ? davaScene->GetCurrentCamera() : nullptr;
    if (camera != nullptr)
    {
        frustum->Build(camera->GetViewProjMatrix(), rhi::DeviceCaps().isZeroBaseClipRange);
        haveFrustum = true;
    }

    struct LiveCandidate
    {
        BeastMeshInstance* instance = nullptr;
        DAVA::float32 distanceSq = 0.0f;
    };

    DAVA::Vector<LiveCandidate> candidates;
    DAVA::int32 culledByFrustum = 0;
    DAVA::int32 withoutMesh = 0;
    DAVA::int32 withoutBox = 0;
    const DAVA::Vector3 cameraPosition = haveFrustum ? camera->GetPosition() : DAVA::Vector3();
    const bool composeFromTargets = !liveViewPerPixel;

    for (BeastMeshInstance* instance : instances)
    {
        if (instance->GetILBMeshInstance() == nullptr)
        {
            ++withoutMesh; // the mesh failed to upload, nothing to light
            continue;
        }

        LiveCandidate candidate;
        candidate.instance = instance;

        DAVA::RenderBatch* batch = instance->GetRenderBatch();
        DAVA::AABBox3 worldBox;
        bool haveBox = false;
        if (batch != nullptr && !batch->GetBoundingBox().IsEmpty())
        {
            DAVA::RenderObject* renderObject = batch->GetRenderObject();
            DAVA::Matrix4* worldTransform = renderObject != nullptr ? renderObject->GetWorldMatrixPtr() : nullptr;
            if (worldTransform != nullptr)
            {
                batch->GetBoundingBox().GetTransformedBox(*worldTransform, worldBox);
            }
            else
            {
                worldBox = batch->GetBoundingBox();
            }
            haveBox = true;
        }

        // Lightmap lighting builds the frame out of the per-instance targets, so an
        // instance without a target is either black or a crash inside Ernst. In that
        // mode nothing is culled here: every instance inside the radius gets a target.
        if (!composeFromTargets && haveFrustum && haveBox && !instance->IsLandscape() && !frustum->IsInside(worldBox))
        {
            ++culledByFrustum;
            continue;
        }

        if (instance->IsLandscape())
        {
            // the ground is a single mesh and everything else stands on it, so it
            // must never lose the target cap to a nearby prop. Negative sorts it
            // ahead of every real distance.
            candidate.distanceSq = -1.0f;
        }
        else if (haveBox)
        {
            candidate.distanceSq = (worldBox.GetCenter() - cameraPosition).SquareLength();
        }
        else
        {
            // no bounding box to sort by: keep it, but never ahead of geometry we
            // can actually place in front of the camera
            candidate.distanceSq = std::numeric_limits<DAVA::float32>::max();
            ++withoutBox;
        }

        candidates.push_back(candidate);
    }

    DAVA::SafeRelease(frustum);

    // nearest first, so the cap keeps what is actually in front of the camera
    std::sort(candidates.begin(), candidates.end(), [](const LiveCandidate& a, const LiveCandidate& b)
              {
                  return a.distanceSq < b.distanceSq;
              });

    // The budget applies in both modes. It was off in lightmap lighting for exactly one
    // build and Ernst died during "loading initial scene" with 972 targets at 512 px
    // (1 GB of RGBA before a single frame): the cap is a memory bound, not a nicety.
    DAVA::int32 cappedByCount = 0;
    if (static_cast<DAVA::int32>(candidates.size()) > liveMaxTargets)
    {
        cappedByCount = static_cast<DAVA::int32>(candidates.size()) - liveMaxTargets;
        candidates.resize(static_cast<std::size_t>(liveMaxTargets));
    }
    if (composeFromTargets)
    {
        DAVA::Logger::Info("[BeastManager] lightmap lighting: %d target(s) for %d instance(s) in the radius (nearest first; the rest stay unbound)",
                           static_cast<DAVA::int32>(candidates.size()), static_cast<DAVA::int32>(candidates.size()) + cappedByCount);
    }

    DAVA::Logger::Info("[BeastManager] live target selection: %d chosen (frustum culled %d, %d over the %d cap, %d without mesh, %d without bounds)",
                       static_cast<DAVA::int32>(candidates.size()), culledByFrustum, cappedByCount, liveMaxTargets, withoutMesh, withoutBox);

    const DAVA::int32 candidateCount = static_cast<DAVA::int32>(candidates.size());

    const DAVA::int32 LIVE_TEXEL_BUDGET = 384 * 1024 * 1024; // 384 MP ~ 1.5 GB of RGBA in Beast
    DAVA::int32 perInstanceSize = liveTargetSize;
    if (candidateCount > 0)
    {
        const DAVA::float32 affordable = sqrtf(static_cast<DAVA::float32>(LIVE_TEXEL_BUDGET) / static_cast<DAVA::float32>(candidateCount));
        if (affordable < static_cast<DAVA::float32>(perInstanceSize))
        {
            // round down to a power of two so the mip chain stays clean
            DAVA::int32 clamped = 64;
            while (clamped * 2 <= static_cast<DAVA::int32>(affordable) && clamped < liveTargetSize)
            {
                clamped *= 2;
            }
            perInstanceSize = clamped;
        }
    }




    // ADD THE INSTANCES BEFORE THE JOB STARTS.
    // Ernst warns "Warning target <name> is empty." and then dies with an access
    // violation while it loads the scene when a texture target has no instance
    // yet, which is exactly what the post-start add produced. Adding instances is
    // legal before the start (only the per-target property sets were refused), so
    // the target is populated for the start and the resolutions are applied after
    // it.
    DAVA::int32 skippedWithoutMesh = withoutMesh;
    DAVA::int32 skippedWithoutTarget = 0;
    for (const LiveCandidate& candidate : candidates)
    {
        BeastMeshInstance* instance = candidate.instance;
        ILBTargetEntityHandle targetEntity = nullptr;
        const ILBStatus addStatus = ILBAddBakeInstance(liveTextureTarget, instance->GetILBMeshInstance(), &targetEntity);
        if (addStatus != ILB_ST_SUCCESS || targetEntity == nullptr)
        {
            if (skippedWithoutTarget < 5)
            {
                DAVA::Logger::Warning("[BeastManager] live session: ILBAddBakeInstance failed for '%s'", instance->GetName().c_str());
                BeastManagerDetails::LogILBFailure("ILBAddBakeInstance (live)", addStatus);
            }
            ++skippedWithoutTarget;
            continue;
        }

        LiveTargetImage entry;
        // exactly the string handed to ILBCreateInstance: it is what
        // ILBGetTargetEntityInstance reports back in an update
        entry.name = instance->GetName();
        // allocated on the first frame Beast pushes for this instance
        entry.width = 0;
        entry.height = 0;
        entry.targetEntity = targetEntity;
        liveTargetIndexByName[entry.name] = static_cast<DAVA::int32>(liveTargets.size());
        liveTargets.push_back(entry);

        if (livePump && (liveTargets.size() % 16) == 0)
        {
            livePump();
        }
    }

    if (liveTargets.empty())
    {
        liveError = true;
        liveErrorText = "the live session has no geometry to light";
        DAVA::Logger::Error("[BeastManager] live session: no target, every instance was skipped (see the [BeastMesh] lines above)");
        return false;
    }

    DAVA::Logger::Info("[BeastManager] live session: adding %d target(s) at %dx%d before the job starts (%d skipped, no mesh; %d skipped, add failed)",
                       static_cast<DAVA::int32>(liveTargets.size()), perInstanceSize, perInstanceSize, skippedWithoutMesh, skippedWithoutTarget);

    // No display window: the editor shows the frames, and a stray Beast window
    // would only compete for the foreground.
    // FORCE_LOCAL, never AUTODETECT: a distributed render cannot stream frames
    // back to the editor, and on this machine AUTODETECT tried to reach a
    // Distribeast node and failed the whole job with
    // "Could not connect to Distribeast node".
    // A live Ernst session never opens Beast's own window. It is a separate renderer
    // that hands the editor a framebuffer; opening its window as well would mean a
    // second viewport nobody asked for. The old "Beast window" switch is gone.
    const ILBShowResults showResults = ILB_SR_NO_DISPLAY;
    if (liveShowWindow)
    {
        DAVA::Logger::Info("[BeastManager] live: the Beast window switch is ignored, a live session runs headless and streams frames to the editor");
    }
    const ILBStatus startStatus = ILBStartJob(job, showResults, ILB_RD_FORCE_LOCAL);
    if (startStatus != ILB_ST_SUCCESS)
    {
        BeastManagerDetails::LogILBFailure("ILBStartJob (live)", startStatus);
        liveError = true;
        liveErrorText = "ILBStartJob failed for the live session";
        return false;
    }

    // Now that the job runs, give each target entity its resolution. Before the
    // start the job refuses this with "Unable to set property
    // LightMapping.Resolution", which left every target on the create resolution.
    DAVA::int32 resolutionFailures = 0;
    for (LiveTargetImage& entry : liveTargets)
    {
        const ILBStatus resolutionStatus = ILBSetBakeResolution(entry.targetEntity, perInstanceSize, perInstanceSize);
        if (resolutionStatus != ILB_ST_SUCCESS)
        {
            ++resolutionFailures;
            if (resolutionFailures <= 5)
            {
                BeastManagerDetails::LogILBFailure("ILBSetBakeResolution (live)", resolutionStatus);
            }
        }
    }
    DAVA::Logger::Info("[BeastManager] live session: %d target(s) at %dx%d (%d requested; %d resolution failure(s))",
                       static_cast<DAVA::int32>(liveTargets.size()), perInstanceSize, perInstanceSize, liveTargetSize, resolutionFailures);

    // render what the camera sees instead of the entire level: this is what
    // makes the session responsive
    // ILBSetJobRenderQuality belongs to bake jobs: on a live Ernst job it answers
    // status 6 (ILB_ST_INVALID_HANDLE) and leaves the defaults in place, which is
    // why the first frame took 80 s. The Ernst knob is the pixel variance
    // threshold: a pixel is done once its variance drops below it, so a higher
    // threshold converges (and shows up) sooner.
    const DAVA::float32 cleanliness = 1.0f - DAVA::Max(0.0f, DAVA::Min(liveRenderQuality, 1.0f));
    const DAVA::float32 varianceThreshold = 0.0025f + 0.25f * cleanliness * cleanliness;
    const ILBStatus varianceStatus = ILBSetErnstVarianceThreshold(job, varianceThreshold);
    DAVA::Logger::Info("[BeastManager] live variance threshold %.4f from quality %.2f (status %d)",
                       varianceThreshold, liveRenderQuality, static_cast<DAVA::int32>(varianceStatus));

    // CAMERA_FRUSTUM only bakes what the camera sees, which is why 143 of 144 targets
    // never received an image and every lightmapped object stayed black.
    // ILB_BM_ENTIRE_LEVEL bakes all targets, prioritised by variance.
    const ILBErnstBakingModeType bakingMode = liveBakeEntireLevel ? ILB_BM_ENTIRE_LEVEL : ILB_BM_CAMERA_FRUSTUM;
    BEAST_VERIFY(ILBSetErnstGIBakingMode(job, bakingMode));
    DAVA::Logger::Info("[BeastManager] live GI baking mode: %s", liveBakeEntireLevel ? "entire level" : "camera frustum");
    BEAST_VERIFY(ILBSetErnstRunningState(job, ILB_ERS_RUN));

    // Kick the worker: pushing the camera again is what tells a live session the
    // view changed, and it costs one call.
    if (liveCamera != nullptr)
    {
        const ILBMatrix4x4 kickMatrix = MakeBeastCameraMatrix(liveCameraTransform);
        ILBSetCameraTransform(liveCamera, &kickMatrix);
        ILBSetFov(liveCamera, liveCameraFov, liveCameraAspect);
    }

    livePaused = false;
    liveUpdatesSinceReport = 0;
    liveCameraFrameUpdates = 0;
    liveTextureUpdates = 0;
    liveCameraPushes = 0;
    liveCameraSent = false;
    liveCameraJumpLogs = 0;
    liveReadFailuresLogged = 0;
    liveMismatchedFrames = 0;
    liveReadChannelsDecided = false;
    liveReadSelection = ILB_CS_RGBA;
    liveReadChannelCount = 4;
    liveLastReportTime = 0.0;
    state = STATE_GENERATING;
    return true;
}

DAVA::int32 BeastManager::GetLiveTargetsWithImage() const
{
    return CountLiveFramesWithImage();
}

bool BeastManager::IsLiveLightmapLighting() const
{
    return (mode == eBeastMode::MODE_LIVE) && !liveViewPerPixel;
}

DAVA::int32 BeastManager::CountLiveFramesWithImage() const
{
    DAVA::int32 count = 0;
    for (const LiveTargetImage& entry : liveTargets)
    {
        if (entry.width > 0 && entry.height > 0)
        {
            ++count;
        }
    }
    return count;
}


namespace
{
// A fatal error inside Ernst writes a dump next to the job it was running. We do
// not hunt for the newest one, we just point at the folder: temp_beast/cache/jobs.
void LogMinidumpLocation()
{
    const DAVA::FilePath jobsRoot = Beast::SceneParser::GetTemporaryFolder() + "cache/jobs/";
    DAVA::Logger::Error("[BeastManager] if the renderer died, it wrote a minidump under '%s' - attach the newest *.dmp", jobsRoot.GetAbsolutePathname().c_str());
}
} // namespace

void BeastManager::ReadLiveFramebuffer(ILBFramebufferHandle framebuffer, DAVA::Vector<DAVA::uint8>& rgba, DAVA::int32& width, DAVA::int32& height)
{
    DAVA::int32 w = 0;
    DAVA::int32 h = 0;
    const ILBStatus resolutionStatus = ILBGetResolution(framebuffer, &w, &h);
    if (resolutionStatus != ILB_ST_SUCCESS || w <= 0 || h <= 0)
    {
        if (liveReadFailuresLogged < 3)
        {
            ++liveReadFailuresLogged;
            DAVA::Logger::Warning("[BeastManager] live framebuffer: ILBGetResolution status %d, %dx%d",
                                  static_cast<DAVA::int32>(resolutionStatus), w, h);
        }
        return;
    }

    // Not every Beast framebuffer has an alpha channel, and asking for one that
    // is not there fails with ILB_ST_INVALID_PARAMETER and writes
    // "Channel A not found" into Beast's error log for every single attempt
    // (5739 lines in one session). The channel count says it up front: four
    // channels means RGBA, three means RGB. The first attempt also teaches the
    // session, so a buffer with four channels that are not RGBA falls back once.
    if (!liveReadChannelsDecided)
    {
        DAVA::int32 channelCount = 0;
        if (ILBGetChannelCount(framebuffer, &channelCount) == ILB_ST_SUCCESS && channelCount > 0)
        {
            if (channelCount < 4)
            {
                liveReadSelection = ILB_CS_RGB;
                liveReadChannelCount = 3;
            }
            liveReadChannelsDecided = true;
            DAVA::Logger::Info("[BeastManager] live framebuffer: %d channel(s), reading %s",
                               channelCount, liveReadChannelCount == 4 ? "RGBA" : "RGB");
        }
    }

    for (DAVA::int32 attempt = 0; attempt < 2; ++attempt)
    {
        const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(liveReadChannelCount);
        rgba.resize(bytes);

        // gamma 2.2: Beast works in linear space and the panel is sRGB
        // Region is inclusive lower, exclusive upper, so 0,0,w,h is the whole buffer
        const ILBStatus readStatus = ILBReadRegionLDR(framebuffer, 0, 0, w, h, liveReadSelection, 2.2f, rgba.data());
        if (readStatus == ILB_ST_SUCCESS)
        {
            break;
        }

        if (attempt == 0 && liveReadSelection == ILB_CS_RGBA)
        {
            // four channels but no alpha in them: read RGB from now on
            liveReadSelection = ILB_CS_RGB;
            liveReadChannelCount = 3;
            liveReadChannelsDecided = true;
            DAVA::Logger::Info("[BeastManager] live framebuffer: RGBA read failed (status %d) at %dx%d, switching to RGB",
                               static_cast<DAVA::int32>(readStatus), w, h);
            continue;
        }

        if (liveReadFailuresLogged < 3)
        {
            ++liveReadFailuresLogged;
            DAVA::Logger::Warning("[BeastManager] live framebuffer: ILBReadRegionLDR status %d at %dx%d",
                                  static_cast<DAVA::int32>(readStatus), w, h);
        }
        return;
    }

    // One layout for the panel: expand RGB to RGBA in place, last pixel first so
    // the destination never overtakes the source.
    if (liveReadChannelCount == 3)
    {
        const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
        rgba.resize(pixelCount * 4u);
        for (size_t i = pixelCount; i-- > 0;)
        {
            const size_t src = i * 3u;
            const size_t dst = i * 4u;
            rgba[dst + 0] = rgba[src + 0];
            rgba[dst + 1] = rgba[src + 1];
            rgba[dst + 2] = rgba[src + 2];
            rgba[dst + 3] = 255;
        }
    }

    width = w;
    height = h;
}

void BeastManager::ReadLiveUpdate(ILBJobUpdateHandle update)
{
    ILBUpdateType type = ILB_UT_UPDATE_TEXTURE;
    if (ILBGetJobUpdateType(update, &type) != ILB_ST_SUCCESS)
    {
        return;
    }

    if (type == ILB_UT_UPDATE_CAMERA_FRAME_BUFFER)
    {
        ++liveCameraFrameUpdates;
        ILBFramebufferHandle framebuffer = nullptr;
        if (ILBGetUpdateFramebuffer(update, &framebuffer) == ILB_ST_SUCCESS && framebuffer != nullptr)
        {
            DAVA::int32 frameWidth = 0;
            DAVA::int32 frameHeight = 0;
            // Beast's own render window reports a different size (640x480, 660x606)
            // than the camera target this session created. Those frames come from a
            // camera that is not ours, so they are dropped instead of shown at the
            // wrong size.
            if (ILBGetResolution(framebuffer, &frameWidth, &frameHeight) == ILB_ST_SUCCESS && frameWidth > 0 && frameHeight > 0 && (frameWidth != liveViewWidth || frameHeight != liveViewHeight))
            {
                ++liveMismatchedFrames;
                if (liveMismatchedFrames <= 3 || (liveMismatchedFrames % 100) == 0)
                {
                    DAVA::Logger::Info("[BeastManager] live camera: dropped %d frame(s) total, latest %dx%d, the session camera target is %dx%d",
                                       static_cast<DAVA::int32>(liveMismatchedFrames), frameWidth, frameHeight, liveViewWidth, liveViewHeight);
                }
                return;
            }

            ReadLiveFramebuffer(framebuffer, liveCameraImage, liveCameraImageWidth, liveCameraImageHeight);
            liveCameraImageChanged = liveCameraImageWidth > 0;
        }
        return;
    }

    if (type != ILB_UT_UPDATE_TEXTURE)
    {
        return;
    }

    ILBFramebufferHandle framebuffer = nullptr;
    if (ILBGetUpdateFramebuffer(update, &framebuffer) != ILB_ST_SUCCESS || framebuffer == nullptr)
    {
        return;
    }

    ILBTargetEntityHandle entity = nullptr;
    if (ILBGetFramebufferTargetEntity(framebuffer, &entity) != ILB_ST_SUCCESS || entity == nullptr)
    {
        return;
    }

    ILBStringHandle nameHandle = nullptr;
    if (ILBGetTargetEntityInstance(entity, &nameHandle) != ILB_ST_SUCCESS || nameHandle == nullptr)
    {
        return;
    }

    const DAVA::String name = ConvertBeastString(nameHandle);
    auto it = liveTargetIndexByName.find(name);
    if (it == liveTargetIndexByName.end())
    {
        // the instance name Beast reports must be the one we created it with;
        // if it is not, say so instead of silently dropping every frame
        if (unmatchedLiveNames.size() < 8 && unmatchedLiveNames.insert(name).second)
        {
            DAVA::Logger::Warning("[BeastManager] live update for unknown instance '%s'", name.c_str());
        }
        return;
    }

    ++liveTextureUpdates;

    LiveTargetImage& entry = liveTargets[static_cast<size_t>(it->second)];
    ReadLiveFramebuffer(framebuffer, entry.rgba, entry.width, entry.height);
}

void BeastManager::UpdateLive()
{
    if (job == nullptr)
    {
        return;
    }

    // Objects moved in the editor are re-pushed here: Beast holds a snapshot.
    SyncLiveInstances();

    // 0 ms: the pump has to return so the editor keeps its frame rate
    ILBWaitJobDone(job, 0);

    // Drain the queue. Bounded per tick so a fast renderer cannot starve the UI.
    DAVA::int32 drained = 0;
    for (;;)
    {
        ILBJobUpdateHandle update = nullptr;
        ILBBool hasUpdate = 0;
        const ILBStatus pumpStatus = ILBGetJobUpdate(job, &hasUpdate, &update);
        liveLastPumpStatus = static_cast<DAVA::int32>(pumpStatus);
        if (pumpStatus != ILB_ST_SUCCESS || hasUpdate == 0 || update == nullptr)
        {
            break;
        }

        ReadLiveUpdate(update);
        ILBDestroyUpdate(update);
        ++liveUpdatesSinceReport;

        if (++drained >= LIVE_UPDATES_PER_TICK)
        {
            break;
        }
    }

    // say something while nothing arrives: "0 frames" with no explanation is
    // indistinguishable from a hang
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const DAVA::float64 now = static_cast<DAVA::float64>(nowMs) / 1000.0;
    if (liveLastReportTime == 0.0)
    {
        liveLastReportTime = now;
    }
    if (now - liveLastReportTime >= 10.0)
    {
        ILBBool isRunning = 0;
        ILBIsJobRunning(job, &isRunning);
        // ILBJobHasNewProgress is bake-job only: on a live Ernst job it answers
        // "Tried to get progress on a job not supporting it" (151 lines per run).
        // The camera and lightmap counters below say the same thing, truthfully.
        DAVA::Logger::Info("[BeastManager] live: %d update(s) in the last %.0f s (%d camera, %d lightmap), %d camera push(es), %d target(s), job running=%d, activity=%d, progress=%d, targets with image=%d, last pump status=%d",
                           liveUpdatesSinceReport, now - liveLastReportTime,
                           liveCameraFrameUpdates, liveTextureUpdates, liveCameraPushes,
                           static_cast<DAVA::int32>(liveTargets.size()), static_cast<DAVA::int32>(isRunning),
                           CountLiveFramesWithImage(), liveLastPumpStatus);
        liveCameraFrameUpdates = 0;
        liveTextureUpdates = 0;
        liveCameraPushes = 0;

        // A silent session plus a failing pump is not "slow", it is broken, and
        // Beast's own log says why.
        if (liveUpdatesSinceReport == 0 && liveLastPumpStatus != ILB_ST_SUCCESS)
        {
            BeastManagerDetails::LogILBFailure("ILBGetJobUpdate (live)", static_cast<ILBStatus>(liveLastPumpStatus));
        }
        liveUpdatesSinceReport = 0;
        liveLastReportTime = now;
    }

    ILBBool running = 0;
    if (ILBIsJobRunning(job, &running) == ILB_ST_SUCCESS && running == 0 && !livePaused)
    {
        // A live session has no natural end: if it stopped, it crashed or was
        // closed. Report it instead of pretending the view is still live.
        ILBJobStatus jobStatus = ILB_JS_OTHER_ERROR;
        if (ILBGetJobResult(job, &jobStatus) == ILB_ST_SUCCESS && jobStatus == ILB_JS_SUCCESS)
        {
            liveErrorText = "the live session ended";
        }
        else
        {
            liveErrorText = DAVA::Format("the live session ended with status %d", static_cast<DAVA::int32>(jobStatus));
            BeastManagerDetails::LogBeastRendererErrors(beastInfoLogPath);
            // A fatal error inside Ernst writes a minidump next to the job. Say
            // where it is instead of leaving the user to hunt for it.
            LogMinidumpLocation();
        }

        DAVA::Logger::Error("[BeastManager] %s", liveErrorText.c_str());
        liveError = true;
        state = STATE_FAILED;
    }
}

void BeastManager::StopLive()
{
    liveTrackedInstances.clear();
    if (job != nullptr)
    {
        ILBCancelJob(job);

        for (DAVA::int32 i = 0; i < 500; ++i)
        {
            ILBBool running = 0;
            if (ILBIsJobRunning(job, &running) != ILB_ST_SUCCESS || running == 0)
            {
                break;
            }
            DAVA::Thread::Sleep(10);
        }
        job = nullptr;
    }

    // job lifetime owns its targets
    liveTextureTarget = nullptr;
    liveCameraTarget = nullptr;

    if (state == STATE_GENERATING)
    {
        state = STATE_DONE;
    }
}

void BeastManager::SetLivePaused(bool paused)
{
    livePaused = paused;

    // an Ernst property only exists on a live job: poking a bake job with it
    // returns "job not supporting it"
    if (job != nullptr && mode == eBeastMode::MODE_LIVE)
    {
        const ILBStatus status = ILBSetErnstRunningState(job, paused ? ILB_ERS_PAUSE : ILB_ERS_RUN);
        if (status != ILB_ST_SUCCESS)
        {
            ILBStringHandle extendedError = nullptr;
            ILBGetExtendErrorInformation(&extendedError);
            DAVA::Logger::Error("[BeastManager] ILBSetErnstRunningState(%s) failed: %s",
                                paused ? "pause" : "run", ConvertBeastString(extendedError).c_str());
        }
    }
}

bool BeastManager::IsLivePaused() const
{
    return livePaused;
}

bool BeastManager::IsLiveRunning() const
{
    if (job == nullptr || liveError)
    {
        return false;
    }

    ILBBool running = 0;
    return ILBIsJobRunning(const_cast<ILBJobHandle>(job), &running) == ILB_ST_SUCCESS && running != 0;
}

bool BeastManager::HasLiveError() const
{
    return liveError;
}

const DAVA::String& BeastManager::GetLiveError() const
{
    return liveErrorText;
}

bool BeastManager::GetLiveCameraImage(DAVA::Vector<DAVA::uint8>& rgba, DAVA::int32& width, DAVA::int32& height) const
{
    if (liveCameraImage.empty() || liveCameraImageWidth <= 0 || liveCameraImageHeight <= 0)
    {
        return false;
    }

    rgba = liveCameraImage;
    width = liveCameraImageWidth;
    height = liveCameraImageHeight;
    return true;
}

bool BeastManager::TakeLiveCameraImageChanged()
{
    const bool changed = liveCameraImageChanged;
    liveCameraImageChanged = false;
    return changed;
}

DAVA::int32 BeastManager::GetLiveTargetCount() const
{
    return static_cast<DAVA::int32>(liveTargets.size());
}

static DAVA::Matrix4 LiveWorldTransform(DAVA::Entity* entity)
{
    // dava.revanced keeps transforms on TransformComponent; DAVA::Entity has no
    // GetWorldTransform() in this fork.
    if (entity == nullptr)
    {
        return DAVA::Matrix4::IDENTITY;
    }
    const DAVA::TransformComponent* transformComponent = entity->GetComponent<DAVA::TransformComponent>();
    if (transformComponent == nullptr)
    {
        return DAVA::Matrix4::IDENTITY;
    }
    return DAVA::TransformUtils::ToMatrix(transformComponent->GetWorldTransform());
}

void BeastManager::SetLiveBakeEntireLevel(bool entireLevel)
{
    liveBakeEntireLevel = entireLevel;
}

void BeastManager::RegisterLiveInstance(DAVA::Entity* entity, ILBInstanceHandle instance)
{
    if ((entity == nullptr) || (instance == nullptr) || (liveTrackedInstances.size() >= 4096))
    {
        return;
    }

    LiveTrackedInstance tracked;
    tracked.entity = entity;
    tracked.instance = instance;
    tracked.lastTransform = LiveWorldTransform(entity);
    liveTrackedInstances.push_back(tracked);
}

void BeastManager::SyncLiveInstances()
{
    if ((mode != MODE_LIVE) || liveTrackedInstances.empty())
    {
        return;
    }

    // Every tick would be 1000 matrix compares at 30 Hz; every 4th is 7 Hz of object
    // moves, which is smoother than the drag itself.
    if (++liveSyncCounter % 4 != 0)
    {
        return;
    }

    DAVA::uint32 moved = 0;
    for (LiveTrackedInstance& tracked : liveTrackedInstances)
    {
        if (tracked.entity == nullptr || tracked.instance == nullptr)
        {
            continue;
        }

        const DAVA::Matrix4 world = LiveWorldTransform(tracked.entity);
        if (memcmp(world._data, tracked.lastTransform._data, sizeof(world._data)) == 0)
        {
            continue;
        }

        const ILBMatrix4x4 beastTransform = ConvertDavaMatrix(world);
        const ILBStatus status = ILBSetInstanceTransform(tracked.instance, &beastTransform);
        if (status != ILB_ST_SUCCESS)
        {
            continue;
        }

        tracked.lastTransform = world;
        ++moved;
    }

    if (moved > 0)
    {
        DAVA::Logger::Info("[BeastManager] live: pushed %d moved instance transform(s)", static_cast<DAVA::int32>(moved));
    }
}

DAVA::int32 BeastManager::GetLiveDroppedFrameCount() const
{
    return static_cast<DAVA::int32>(liveMismatchedFrames);
}

const DAVA::String& BeastManager::GetLiveTargetName(DAVA::int32 index) const
{
    static const DAVA::String empty;
    if (index < 0 || index >= static_cast<DAVA::int32>(liveTargets.size()))
    {
        return empty;
    }
    return liveTargets[static_cast<size_t>(index)].name;
}

bool BeastManager::GetLiveTargetImage(DAVA::int32 index, DAVA::Vector<DAVA::uint8>& rgba, DAVA::int32& width, DAVA::int32& height) const
{
    if (index < 0 || index >= static_cast<DAVA::int32>(liveTargets.size()))
    {
        return false;
    }

    const LiveTargetImage& entry = liveTargets[static_cast<size_t>(index)];
    if (entry.rgba.empty())
    {
        return false;
    }

    rgba = entry.rgba;
    width = entry.width;
    height = entry.height;
    return true;
}

bool BeastManager::HasProducedOutput() const
{
    return producedOutput;
}

void BeastManager::DisableLogTargets()
{
    if (!logTargetsActive)
    {
        return;
    }

    ILBSetLogTarget(ILB_LT_ERROR, ILB_LS_NULL, nullptr);
    ILBSetLogTarget(ILB_LT_INFO, ILB_LS_NULL, nullptr);
    logTargetsActive = false;
}

int32 BeastManager::GetCurTaskProcess() const
{
    return curTaskProgress;
}

const DAVA::String& BeastManager::GetCurTaskName() const
{
    return curTaskName;
}

void BeastManager::Cancel()
{
    if (state != STATE_GENERATING)
    {
        return;
    }

    if (job != nullptr)
    {
        BEAST_VERIFY(ILBCancelJob(job));
    }

    // note: SceneParser has no public cancel entry point in the published
    // module; ShouldCancel() flips to true on its own when the landscape
    // thumbnail pass fails, and OnSceneParsingCompleted() checks it.
    state = STATE_CANCELED;
}

bool BeastManager::WasCancelled() const
{
    return (state == STATE_CANCELED);
}

void BeastManager::UpdateAtlas(DAVA::Vector<LightmapAtlasingData>* atlasData)
{
    if (atlasData == nullptr || atlasData->empty())
    {
        DAVA::Logger::Warning("[BeastManager] UpdateAtlas: no atlasing entries, nothing to bind");
        return;
    }

    // How the shipped content does it (rudniki.sc2, lightmapped material instances):
    //
    //   properties: uvOffset (Vector2), uvScale (Vector2)
    //   textures:   lightmap -> rudniki.sc2_lightmaps/texture3.tex
    //
    // and materials-vp.sl:754 turns that into
    //
    //   output.varTexCoord1 = uvScale * input.texcoord1.xy + uvOffset;
    //
    // so the atlas placement is a MATERIAL property pair, not something baked into the
    // mesh: the geometry keeps its tile-local lightmap UVs and stays shared between
    // instances. No PolygonGroup surgery here, just one material instance per lightmapped
    // batch with the tile rectangle and the page on it.
    //
    // Two passes come out of a bake: "sm" (the illumination page the engine samples through
    // the lightmap slot) and "ao" (its own page, and this engine version has no second
    // lightmap slot to put it in). Binding both means the ao page lands last and wins,
    // which is exactly the flat grey result a finished bake used to produce. Only the
    // surface pass is bound.
    DAVA::int32 bound = 0;
    DAVA::int32 skippedAO = 0;
    DAVA::int32 unknownInstance = 0;
    DAVA::int32 noMaterial = 0;
    DAVA::int32 missingAtlas = 0;
    DAVA::int32 degenerateRect = 0;
    DAVA::Map<DAVA::String, DAVA::int32> pageUsage;

    // The packer assembles each page from the per-instance images and leaves the gaps between
    // tiles at zero. Measured on a rudniki page: 11.2% of texels pure black against 0.0% in the
    // shipped one, and any mesh sampling a gap renders as a flat void - that is the lighthouse
    // door. The per-instance fill never reached here because it runs before packing. Fill each
    // page once, from its own painted neighbours, before any texture is created from it.
    std::set<DAVA::String> filledPages;

    const DAVA::FastName lightmapSlot(DAVA::NMaterialTextureName::TEXTURE_LIGHTMAP);
    const DAVA::FastName uvOffsetName("uvOffset");
    const DAVA::FastName uvScaleName("uvScale");

    for (const LightmapAtlasingData& entry : *atlasData)
    {
        const DAVA::String atlasPath = entry.textureName.GetAbsolutePathname();
        if (atlasPath.find("/ao/") != DAVA::String::npos || atlasPath.find("\\ao\\") != DAVA::String::npos)
        {
            ++skippedAO;
            continue;
        }

        if (filledPages.insert(atlasPath).second)
        {
            const DAVA::FilePath pagePng = DAVA::FilePath::CreateWithNewExtension(entry.textureName, ".png");
            DAVA::ScopedPtr<DAVA::Image> page(DAVA::ImageSystem::LoadSingleMip(pagePng));
            if (page && (page->format == DAVA::PixelFormat::FORMAT_RGBA8888))
            {
                BeastManagerDetails::FillUnpaintedTexels(page->data, static_cast<DAVA::int32>(page->width), static_cast<DAVA::int32>(page->height), 8);
                const bool saved = DAVA::ImageSystem::Save(pagePng, page.get(), DAVA::PixelFormat::FORMAT_RGBA8888) == DAVA::eErrorCode::SUCCESS;
                DAVA::Logger::Info("[BeastManager] atlas page filled: %s (%ux%u) %s", pagePng.GetFilename().c_str(),
                                   page->width, page->height, saved ? "written" : "NOT written");
            }
            else
            {
                DAVA::Logger::Warning("[BeastManager] atlas page %s could not be filled (missing or not RGBA8888)", pagePng.GetAbsolutePathname().c_str());
            }
        }

        DAVA::RenderBatch* batch = nullptr;
        auto byStableKey = lightmapKeyToBatch.find(entry.meshInstanceName);
        if (byStableKey != lightmapKeyToBatch.end())
        {
            batch = byStableKey->second;
        }
        else
        {
            // older runs named their descriptors after the batch pointer; keep those atlases usable
            batch = static_cast<DAVA::RenderBatch*>(BeastResource<DAVA::RenderBatch>::StringToPointer(entry.meshInstanceName));
        }
        if (batch == nullptr)
        {
            ++unknownInstance;
            continue;
        }

        // A zero-width or zero-height rect collapses every UV of the batch onto one texel, and
        // that texel is atlas padding, which is black: the whole batch then renders as a solid
        // black shape on the model. Name those batches instead of letting a binding bug look
        // like a failed bake.
        if ((entry.uvScale.x <= 0.0f) || (entry.uvScale.y <= 0.0f))
        {
            if (degenerateRect < 12)
            {
                DAVA::Logger::Warning("[BeastManager] atlas entry with an empty rect: '%s' offset (%.4f, %.4f) scale (%.4f, %.4f) page %s",
                                      entry.meshInstanceName.c_str(), entry.uvOffset.x, entry.uvOffset.y, entry.uvScale.x, entry.uvScale.y, atlasPath.c_str());
            }
            ++degenerateRect;
        }
        ++pageUsage[atlasPath];

        DAVA::NMaterial* source = batch->GetMaterial();
        if (source == nullptr)
        {
            ++noMaterial;
            continue;
        }

        DAVA::Texture* atlas = nullptr;
        auto cached = atlasTextures.find(atlasPath);
        if (cached != atlasTextures.end())
        {
            atlas = cached->second;
        }
        else
        {
            atlas = DAVA::Texture::CreateFromFile(entry.textureName);
            atlasTextures[atlasPath] = atlas;
        }

        if (atlas == nullptr)
        {
            ++missingAtlas;
            continue;
        }

        DAVA::NMaterial* instance = source->Clone();
        if (instance == nullptr)
        {
            ++noMaterial;
            continue;
        }

        // These Blitz materials ship with the SETUP_LIGHTMAP flag set, and with that flag the
        // fragment shader never samples the lightmap: materials-fp.sl:348-378 replaces the sample
        // with a 1-texel 0.75/0.25 grey checkerboard, and that checker (lit, then multiplied by the
        // material colour and the light) is what read as solid black blocks on the tower and as
        // black over the rocks. It has to be cleared for the atlas to be used at all.
        // NMaterial::SetFlag asserts when the flag is not already in the material's local set
        // (NMaterial.cpp:575), and the last run died right there with "config.localFlags.find(flagName)
        // != config.localFlags.end()" - so the flag was never cleared and the checkerboard stayed on
        // screen. Add it locally when it isn't there: a local value overrides the template.
        if (instance->HasLocalFlag(DAVA::NMaterialFlagName::FLAG_SETUPLIGHTMAP))
        {
            instance->SetFlag(DAVA::NMaterialFlagName::FLAG_SETUPLIGHTMAP, 0);
        }
        else
        {
            instance->AddFlag(DAVA::NMaterialFlagName::FLAG_SETUPLIGHTMAP, 0);
        }

        // Clone copies the local properties, and the shipped lightmap materials already
        // carry uvOffset/uvScale, so AddProperty on the clone hits DVASSERT "property
        // already exists". Same for the lightmap slot. Overwrite what is there, add only
        // what is missing, and rebuild the property if the existing one is not a float2.
        DAVA::float32 offset[2] = { entry.uvOffset.x, entry.uvOffset.y };
        DAVA::float32 scale[2] = { entry.uvScale.x, entry.uvScale.y };

        // The material transform is the mechanism the shipped map itself uses: its materials carry
        // uvScale/uvOffset and its meshes keep their own lightmap UVs, so this is the path that is
        // known to work in this renderer. Rewriting the mesh UVs (v10.45-10.48) never took effect on
        // screen, so it is gone and the numbers are logged instead so they can be checked.
        // The bake writes uv1 as it is (no ILBSetUVTransform), so placing the tile is nothing but
        // the tile's fraction of the page plus its origin. Those two numbers are already in scale
        // and offset, straight from the descriptor. The shipped map's materials carry exactly the
        // same shape of value (uvScale 0.0625 = a 256 tile on a 4096 page, uvOffset = tile origin),
        // which is why this is the arrangement that renders correctly.

        if (uvRectLogged < 8)
        {
            // The shipped value matters as much as ours: the original map's materials already carry
            // a uvScale/uvOffset, and those are what the engine demonstrably renders correctly.
            const DAVA::float32* shippedScale = source->GetLocalPropValue(uvScaleName);
            const DAVA::float32* shippedOffset = source->GetLocalPropValue(uvOffsetName);
            DAVA::Logger::Info("[BeastManager] bind '%s': tile %.4f,%.4f +%.4f,%.4f | uvScale %.4f,%.4f uvOffset %.4f,%.4f | shipped uvScale %s uvOffset %s",
                               entry.meshInstanceName.c_str(), entry.uvOffset.x, entry.uvOffset.y, entry.uvScale.x, entry.uvScale.y,
                               scale[0], scale[1], offset[0], offset[1],
                               (shippedScale != nullptr) ? DAVA::Format("%.4f,%.4f", shippedScale[0], shippedScale[1]).c_str() : "none",
                               (shippedOffset != nullptr) ? DAVA::Format("%.4f,%.4f", shippedOffset[0], shippedOffset[1]).c_str() : "none");
            ++uvRectLogged;
        }

        struct PropPair
        {
            const DAVA::FastName* name;
            const DAVA::float32* value;
        };
        const PropPair properties[] = { { &uvOffsetName, offset }, { &uvScaleName, scale } };

        for (const PropPair& property : properties)
        {
            const bool wrongShape = instance->HasLocalProperty(*property.name) &&
            (instance->GetLocalPropType(*property.name) != rhi::ShaderProp::TYPE_FLOAT2 || instance->GetLocalPropArraySize(*property.name) != 1);

            if (wrongShape)
            {
                instance->RemoveProperty(*property.name);
            }

            if (instance->HasLocalProperty(*property.name))
            {
                instance->SetPropertyValue(*property.name, property.value);
            }
            else
            {
                instance->AddProperty(*property.name, property.value, rhi::ShaderProp::TYPE_FLOAT2);
            }
        }

        if (instance->HasLocalTexture(lightmapSlot))
        {
            instance->SetTexture(lightmapSlot, atlas);
        }
        else
        {
            instance->AddTexture(lightmapSlot, atlas);
        }

        // materials-fp.sl:355 hides the tile border with a per-texel checker driven by this
        // value:  frac(floor(texCoord1.x * lightmapSize) / 2). The shader default is 1.0, so a
        // material that never got it dithered at the wrong rate along every seam, which is one
        // of the ways a tile edge turns into a hard brightness step. The tile size in texels is
        // exactly what it wants, and the descriptor already carries it.
        const DAVA::FastName lightmapSizeName(DAVA::NMaterialParamName::PARAM_LIGHTMAP_SIZE);
        if (atlas != nullptr && entry.uvScale.x > 0.0f)
        {
            const DAVA::float32 tilePixels = entry.uvScale.x * static_cast<DAVA::float32>(atlas->GetWidth());
            if (instance->HasLocalProperty(lightmapSizeName) && instance->GetLocalPropType(lightmapSizeName) != rhi::ShaderProp::TYPE_FLOAT1)
            {
                instance->RemoveProperty(lightmapSizeName);
            }
            if (instance->HasLocalProperty(lightmapSizeName))
            {
                instance->SetPropertyValue(lightmapSizeName, &tilePixels);
            }
            else
            {
                instance->AddProperty(lightmapSizeName, &tilePixels, rhi::ShaderProp::TYPE_FLOAT1, 1);
            }
        }

        // the material had no lightmap texture when its variants were built, so both the
        // texture bindings and the compiled variants are stale now
        instance->InvalidateTextureBindings();
        instance->InvalidateRenderVariants();

        batch->SetMaterial(instance);
        appliedMaterialInstances.push_back(instance);
        generatedLightmaps[batch] = true;
        ++bound;
    }

    DAVA::Logger::Info("[BeastManager] UpdateAtlas: %d entries, %d bound, %d pages loaded, %d ao entries skipped, %d unknown instances, %d without material, %d missing atlas, %d empty rects, %d mesh UV sets rewritten",
                       static_cast<DAVA::int32>(atlasData->size()), bound, static_cast<DAVA::int32>(atlasTextures.size()), skippedAO, unknownInstance, noMaterial, missingAtlas, degenerateRect, uvRewrittenCount);

    for (const auto& page : pageUsage)
    {
        DAVA::Logger::Info("[BeastManager] lightmap page %s: %d batches", page.first.c_str(), page.second);
    }

    if (bound == 0 && !atlasData->empty())
    {
        DAVA::Logger::Error("[BeastManager] UpdateAtlas bound nothing: the bake is on disk but the scene is unchanged");
    }
}

bool BeastManager::IsLightmapGenerated(DAVA::RenderBatch* batch)
{
    auto found = generatedLightmaps.find(batch);
    return (found != generatedLightmaps.end()) && found->second;
}

void BeastManager::SetLightmapGenerated(DAVA::RenderBatch* batch)
{
    generatedLightmaps[batch] = true;
}

bool BeastManager::IsResumingExistingBakes() const
{
    return resumeExistingBakes;
}

void BeastManager::RewriteLightmapUVs(DAVA::RenderBatch* batch, const LightmapAtlasingData& entry)
{
    if (batch == nullptr)
    {
        return;
    }

    if (uvRewritten.find(batch) != uvRewritten.end())
    {
        return;
    }

    auto rectIt = lightmapUVRects.find(batch);
    if (rectIt == lightmapUVRects.end())
    {
        return;
    }

    const LightmapUVRect& rect = rectIt->second;
    if ((rect.size.x <= 1e-5f) || (rect.size.y <= 1e-5f))
    {
        return;
    }

    DAVA::PolygonGroup* pg = batch->GetPolygonGroup();
    if ((pg == nullptr) || (pg->textureCoordCount <= rect.uvSet) || (pg->vertexCount <= 0))
    {
        return;
    }

    // texcoord1 becomes atlas space: island rectangle -> whole tile (the same mapping the bake was
    // given through ILBSetUVTransform), then the tile placed on the page. Doing this on the mesh
    // means the shader samples where the texels are no matter which variant it compiled to.
    DAVA::Vector2 uv;
    DAVA::Vector2 atlasUV;
    for (DAVA::int32 i = 0; i < pg->vertexCount; ++i)
    {
        pg->GetTexcoord(rect.uvSet, i, uv);
        atlasUV.x = ((uv.x - rect.offset.x) / rect.size.x) * entry.uvScale.x + entry.uvOffset.x;
        atlasUV.y = ((uv.y - rect.offset.y) / rect.size.y) * entry.uvScale.y + entry.uvOffset.y;
        pg->SetTexcoord(rect.uvSet, i, atlasUV);
    }

    uvRewritten.insert(batch);
    ++uvRewrittenCount;
}

void BeastManager::RegisterLightmapTarget(const DAVA::String& instanceKey, const DAVA::String& stableKey, DAVA::RenderBatch* batch)
{
    lightmapStableKeys[instanceKey] = stableKey;
    lightmapKeyToBatch[stableKey] = batch;
}

void BeastManager::RegisterLightmapUVRect(DAVA::RenderBatch* batch, const DAVA::Vector2& uvOffset, const DAVA::Vector2& uvSize, DAVA::int32 uvSet)
{
    if (batch == nullptr)
    {
        return;
    }

    LightmapUVRect& rect = lightmapUVRects[batch];
    rect.offset = uvOffset;
    rect.size = uvSize;
    rect.uvSet = uvSet;
}

DAVA::String BeastManager::GetLightmapStableKey(const DAVA::String& instanceKey) const
{
    auto found = lightmapStableKeys.find(instanceKey);
    return found == lightmapStableKeys.end() ? DAVA::String() : found->second;
}

bool BeastManager::HasBakedResult(const DAVA::String& stableKey) const
{
    if (stableKey.empty() || lightmapsDirectory.empty())
    {
        return false;
    }

    const bool baked = DAVA::FileSystem::Instance()->IsFile(DAVA::FilePath(lightmapsDirectory) + "sm/" + stableKey + ".png");
    if (baked)
    {
        ++resumeSkippedTargets;
    }

    return baked;
}

bool BeastManager::HasBakedLandscapeResult() const
{
    return !lightmapsDirectory.empty() && DAVA::FileSystem::Instance()->IsFile(DAVA::FilePath(lightmapsDirectory) + "landscape.png");
}

void BeastManager::ClearLightmapsDirectory()
{
    if (lightmapsDirectory.empty())
    {
        return;
    }

    DAVA::FilePath basePath(lightmapsDirectory);
    DAVA::FileSystem::Instance()->DeleteDirectory(basePath + "sm/", true);
    DAVA::FileSystem::Instance()->DeleteDirectory(basePath + "ao/", true);
}

void BeastManager::SaveResults()
{
    if (mode == eBeastMode::MODE_SPHERICAL_HARMONICS)
    {
        // SH results are vertex data: copy the coefficients back onto the
        // SpeedTree objects registered by the SceneParser.
        const DAVA::List<BeastPointCloud*>& pointClouds = BeastPointCloud::GetResourceList();
        for (BeastPointCloud* pointCloud : pointClouds)
        {
            ILBTargetHandle target = pointCloud->GetCloudTarget();
            ILBTargetEntityHandle targetEntity = pointCloud->GetEntityTarget();
            if (target == nullptr || targetEntity == nullptr)
            {
                continue;
            }

            ILBFramebufferHandle vertexBuffer = nullptr;
            if (ILBGetVertexbuffer(target, passSH, targetEntity, &vertexBuffer) != ILB_ST_SUCCESS || vertexBuffer == nullptr)
            {
                continue;
            }

            DAVA::int32 width = 0;
            DAVA::int32 height = 0;
            BEAST_VERIFY(ILBGetResolution(vertexBuffer, &width, &height));
            const DAVA::int32 floatCount = width * height;
            if (floatCount <= 0)
            {
                BEAST_VERIFY(ILBDestroyFramebuffer(vertexBuffer));
                continue;
            }

            DAVA::Vector<DAVA::float32> coefficients(static_cast<size_t>(floatCount), 0.0f);
            BEAST_VERIFY(ILBReadRegionHDR(vertexBuffer, 0, 0, width, height, ILB_CS_RGBA, coefficients.data()));
            BEAST_VERIFY(ILBDestroyFramebuffer(vertexBuffer));

            const DAVA::int32 entityCount = pointCloud->GetEntityCount();
            for (DAVA::int32 e = 0; e < entityCount; ++e)
            {
                DAVA::Entity* entity = pointCloud->GetEntity(e);
                DAVA::RenderObject* renderObject = DAVA::GetRenderObject(entity);
                if (renderObject != nullptr && renderObject->GetType() == DAVA::RenderObject::TYPE_SPEED_TREE)
                {
                    DAVA::SpeedTreeObject* speedTree = static_cast<DAVA::SpeedTreeObject*>(renderObject);
                    DAVA::Array<DAVA::float32, DAVA::SpeedTreeObject::HARMONICS_BUFFER_CAPACITY> sh = {};
                    const DAVA::int32 copyCount = DAVA::Min<DAVA::int32>(static_cast<DAVA::int32>(sh.size()), floatCount);
                    for (DAVA::int32 i = 0; i < copyCount; ++i)
                    {
                        sh[static_cast<size_t>(i)] = coefficients[static_cast<size_t>(i)];
                    }
                    speedTree->SetSphericalHarmonics(sh);
                }
            }
        }
        return;
    }

    // MODE_LIGHTMAPS / MODE_PREVIEW: read every framebuffer of every
    // lightmapped instance target and store it as
    //   {lightmapsDirectory}/sm/{instanceName}.png
    //   {lightmapsDirectory}/ao/{instanceName}.png
    // exactly where LightmapsPacker expects to find them.
    if (lightmapsDirectory.empty())
    {
        return;
    }

    DAVA::FilePath basePath(lightmapsDirectory);
    DAVA::FileSystem::Instance()->CreateDirectory(basePath + "sm/", true);
    DAVA::FileSystem::Instance()->CreateDirectory(basePath + "ao/", true);

    DAVA::int32 writtenFramebuffers = 0;
    DAVA::int32 failedFramebuffers = 0;
    DAVA::int32 targetsWithoutFramebuffers = 0;
    DAVA::int32 aoComposited = 0;

    const DAVA::List<BeastMeshInstance*>& instances = BeastMeshInstance::GetResourceList();
    for (BeastMeshInstance* instance : instances)
    {
        if (!instance->IsUseLightmap())
        {
            continue;
        }

        TextureTarget* textureTarget = instance->GetTextureTarget();
        if (textureTarget == nullptr)
        {
            continue;
        }

        const bool isLandscape = instance->IsLandscape();
        DAVA::String instanceName = instance->GetMeshInstancePointerString();
        if (isLandscape)
        {
            instanceName = "landscape";
        }
        else
        {
            // pointer strings die with the process, so a page written under one can never be
            // matched to the scene after a restart. Write under the stable key instead.
            const DAVA::String stableKey = GetLightmapStableKey(instanceName);
            if (!stableKey.empty())
            {
                instanceName = stableKey;
            }
        }

        // BeastRunner::PackLightmaps() moves "{lightmapsDirectory}/landscape.png"
        // out of the directory before packing, so the landscape lightmap is
        // written to the top level; regular instances go into sm/ and ao/,
        // which ResourcePacker2D turns into the "sm" and "ao" atlases.
        const char* passNames[2] = { (isLandscape ? "" : "sm/"), (isLandscape ? "" : "ao/") };
        ILBRenderPassHandle passes[2] = { passLM, passSH };

        for (DAVA::int32 passIndex = 0; passIndex < 2; ++passIndex)
        {
            if (passes[passIndex] == nullptr)
            {
                continue;
            }

            // the landscape keeps a single lightmap: skip the AO pass
            if (isLandscape && passIndex == 1)
            {
                continue;
            }

            DAVA::int32 framebufferCount = 0;
            if (ILBGetFramebufferCount(textureTarget->GetHandle(), &framebufferCount) != ILB_ST_SUCCESS || framebufferCount <= 0)
            {
                ++targetsWithoutFramebuffers;
                continue;
            }

            for (DAVA::int32 fbIndex = 0; fbIndex < framebufferCount; ++fbIndex)
            {
                ILBFramebufferHandle framebuffer = nullptr;
                if (ILBGetFramebuffer(textureTarget->GetHandle(), passes[passIndex], fbIndex, &framebuffer) != ILB_ST_SUCCESS
                    || framebuffer == nullptr)
                {
                    continue;
                }

                // The pass was already selected by handle above, so probing it
                // again only asks Beast a question it answers with "Not available
                // on this frame buffer" for buffers it does not tie to a pass.
                // The previous version treated that answer as fatal and destroyed
                // the framebuffer, which quietly threw away every lightmap of the
                // run (1035 destroyed buffers, nothing packed).
                DAVA::FilePath pngPath = basePath + DAVA::String(passNames[passIndex]) + instanceName + ".png";
                if (BeastManagerDetails::SaveFramebufferAsPNG(framebuffer, pngPath))
                {
                    producedOutput = true;
                    ++writtenFramebuffers;
                }
                else
                {
                    ++failedFramebuffers;
                }
            }
        }

        if (!isLandscape)
        {
            // ao beside sm, same base name: fold it in so the single bound lightmap carries the
            // contact shadows the bake actually produced
            const DAVA::FilePath smPng = basePath + DAVA::String("sm/") + instanceName + ".png";
            const DAVA::FilePath aoPng = basePath + DAVA::String("ao/") + instanceName + ".png";
            if (BeastManagerDetails::MultiplyAOIntoLightmap(smPng, aoPng))
            {
                ++aoComposited;
            }
        }

        if (instance->GetRenderBatch() != nullptr)
        {
            SetLightmapGenerated(instance->GetRenderBatch());
        }
    }

    // One line instead of one line per buffer per pass: enough to tell a real
    // bake from an empty one without drowning the console.
    DAVA::Logger::Info("[BeastManager] lightmap readback: %d written, %d failed, %d targets without framebuffers, %d with ao composited",
                       writtenFramebuffers, failedFramebuffers, targetsWithoutFramebuffers, aoComposited);
}
} // namespace Beast
