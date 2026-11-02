#include "Classes/DescriptorManager/DescriptorManagerModule.h"
#include "Classes/DescriptorManager/Private/DescriptorManagerSettings.h"
#include "Classes/DescriptorManager/Private/DescriptorManagerSettingsDialog.h"

#include "REPlatform/DataNodes/ProjectManagerData.h"
#include "REPlatform/DataNodes/SceneData.h"
#include "REPlatform/Scene/SceneEditor2.h"
#include "REPlatform/Scene/SceneHelper.h"
#include "REPlatform/Scene/Systems/SelectionSystem.h"
#include "REPlatform/Scene/Systems/StructureSystem.h"
#include "REPlatform/Scene/Utils/ImageTools.h"
#include "REPlatform/Scene/Utils/RETextureDescriptorUtils.h"

#include <REPlatform/Commands/RECommandNotificationObject.h>

#include <TArc/Core/ContextAccessor.h>
#include <TArc/Core/Deprecated.h>
#include <TArc/WindowSubSystem/ActionUtils.h>
#include <TArc/WindowSubSystem/QtAction.h>
#include <TArc/WindowSubSystem/UI.h>

#include <Base/RefPtr.h>
#include <Base/ScopedPtr.h>
#include <FileSystem/File.h>
#include <FileSystem/FileList.h>
#include <FileSystem/FileSystem.h>
#include <Logger/Logger.h>
#include <Reflection/ReflectionRegistrator.h>
#include <Render/GPUFamilyDescriptor.h>
#include <Render/Image/ImageSystem.h>
#include <Render/Material/NMaterial.h>
#include <Render/RenderBase.h>
#include <Render/Texture.h>
#include <Render/TextureDescriptor.h>
#include <Render/TextureDescriptorUtils.h>
#include <TextureCompression/TextureConverter.h>

#include <TArc/Utils/ModuleCollection.h>

#include <Reflection/ReflectedTypeDB.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>

#include <QAction>
#include <QElapsedTimer>
#include <QList>
#include <QTimer>

namespace
{
// one pass per command burst (paste, Add Scene)
constexpr int SCENE_PASS_DELAY_MS = 400;

struct Report
{
    DAVA::uint32 created = 0;
    DAVA::uint32 updated = 0;
    DAVA::uint32 relinked = 0;
    DAVA::uint32 sourcesCreated = 0;
    DAVA::uint32 skipped = 0;
    DAVA::uint32 failedMissingSource = 0;
    DAVA::Vector<DAVA::String> failedPaths;

    void Log(const DAVA::String& action) const
    {
        using namespace DAVA;
        Logger::Info("[DescriptorManager] %s: created %u, updated %u, relinked %u, sources created %u, skipped %u, no source %u",
                     action.c_str(), created, updated, relinked, sourcesCreated, skipped, failedMissingSource);
        for (const String& path : failedPaths)
        {
            Logger::Info("[DescriptorManager]   no source found for %s", path.c_str());
        }
    }
};

DAVA::String ToLower(const DAVA::String& s)
{
    DAVA::String result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return result;
}

bool IsImageExtension(const DAVA::String& extension)
{
    return DAVA::ImageSystem::GetImageFormatForExtension(extension) != DAVA::IMAGE_FORMAT_UNKNOWN;
}

// descriptor path without the .tex extension
DAVA::String DescriptorStem(const DAVA::FilePath& texPath)
{
    const DAVA::uint32 texExtensionLength = 4; // ".tex"
    const DAVA::String absolute = texPath.GetAbsolutePathname();
    return absolute.size() > texExtensionLength ? absolute.substr(0, absolute.size() - texExtensionLength) : absolute;
}

// face index of a cube side suffix (_px .. _nz), -1 when absent
DAVA::int32 FaceSuffixIndex(const DAVA::String& lowerBasename)
{
    using namespace DAVA;
    for (int32 face = 0; face < Texture::CUBE_FACE_COUNT; ++face)
    {
        const String& suffix = Texture::FACE_NAME_SUFFIX[face];
        if (lowerBasename.size() > suffix.size() &&
            lowerBasename.compare(lowerBasename.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            return face;
        }
    }
    return -1;
}

// face files of the cube stem; facesOut has CUBE_FACE_COUNT slots, returns how many were found
DAVA::int32 FindCubeFaces(const DAVA::FilePath& texPath, DAVA::Vector<DAVA::FilePath>& facesOut)
{
    using namespace DAVA;

    facesOut.clear();
    facesOut.resize(Texture::CUBE_FACE_COUNT, FilePath());

    const String stem = DescriptorStem(texPath);
    int32 found = 0;
    for (int32 face = 0; face < Texture::CUBE_FACE_COUNT; ++face)
    {
        const String& suffix = Texture::FACE_NAME_SUFFIX[face];
        for (const String& ext : DescriptorManagerSettings::AllSourceFormats())
        {
            const FilePath candidate(stem + suffix + ext);
            if (FileSystem::Instance()->Exists(candidate) == true)
            {
                facesOut[face] = candidate;
                ++found;
                break;
            }
        }
    }
    return found;
}

const DAVA::Vector<DAVA::String>& DefaultRuntimeFormats()
{
    static const DAVA::Vector<DAVA::String> formats = { ".dds", ".pvr" };
    return formats;
}

// format entries naming a runtime container; empty table means .dds then .pvr
DAVA::Vector<DAVA::String> RuntimeFormatsFrom(const DescriptorManagerSettings* settings)
{
    DAVA::Vector<DAVA::String> formats;
    for (const DAVA::String& extension : settings->formatPriority)
    {
        if (extension == ".dds" || extension == ".pvr")
        {
            formats.push_back(extension);
        }
    }
    return formats.empty() == true ? DefaultRuntimeFormats() : formats;
}

DAVA::ImageFormat ContainerImageFormat(const DAVA::FilePath& path)
{
    return ToLower(path.GetExtension()) == ".dds" ? DAVA::IMAGE_FORMAT_DDS : DAVA::IMAGE_FORMAT_PVR;
}

// device families in Priorities -> GPU order
DAVA::Vector<DAVA::eGPUFamily> BuildFamilyOrder(const DescriptorManagerSettings* settings)
{
    using namespace DAVA;

    Vector<eGPUFamily> order;
    for (const String& name : settings->familyPriority)
    {
        const eGPUFamily gpu = GPUFamilyDescriptor::GetGPUByName(name);
        if (gpu >= 0 && gpu < eGPUFamily::GPU_DEVICE_COUNT && std::find(order.begin(), order.end(), gpu) == order.end())
        {
            order.push_back(gpu);
        }
    }
    // families missing from older settings go last
    for (int32 gpu = 0; gpu < eGPUFamily::GPU_DEVICE_COUNT; ++gpu)
    {
        const eGPUFamily family = static_cast<eGPUFamily>(gpu);
        if (std::find(order.begin(), order.end(), family) == order.end())
        {
            order.push_back(family);
        }
    }
    return order;
}

// both foo.dx11.dds and foodx11.dds spell the dx11 variant
DAVA::Vector<DAVA::String> PrefixSpellings(const DAVA::String& prefix)
{
    using namespace DAVA;
    Vector<String> spellings;
    if (prefix.empty() == true)
    {
        return spellings;
    }
    spellings.push_back(prefix);
    spellings.push_back(prefix[0] == '.' ? prefix.substr(1) : String(".") + prefix);
    return spellings;
}

// first existing source for a descriptor, in format table order
DAVA::FilePath FindSourceFile(const DAVA::FilePath& descriptorPath, const DAVA::Vector<DAVA::String>& priority)
{
    using namespace DAVA;
    for (const String& ext : priority)
    {
        const FilePath candidate = FilePath::CreateWithNewExtension(descriptorPath, ext);
        if (FileSystem::Instance()->Exists(candidate))
        {
            return candidate;
        }
    }
    return FilePath();
}

// families carrying a runtime prefix, in priority order
DAVA::Vector<DAVA::String> BuildGPUPostfixes(const DescriptorManagerSettings* settings)
{
    using namespace DAVA;
    Vector<String> postfixes;
    for (eGPUFamily gpu : BuildFamilyOrder(settings))
    {
        const String prefix = ToLower(GPUFamilyDescriptor::GetGPUPrefix(gpu));
        if (prefix.empty() == false)
        {
            postfixes.push_back(prefix);
        }
    }
    return postfixes;
}

// strip a known family prefix from a lowered basename
void StripFamilyPrefix(DAVA::String& base, const DAVA::Vector<DAVA::String>& postfixes)
{
    using namespace DAVA;
    for (const String& postfix : postfixes)
    {
        for (const String& spelling : PrefixSpellings(postfix))
        {
            if (base.size() > spelling.size() && base.compare(base.size() - spelling.size(), spelling.size(), spelling) == 0)
            {
                base.erase(base.size() - spelling.size());
                return;
            }
        }
    }
}

// variant file that exists and parses; a payload-less DDS is not a variant
bool ContainerReadable(const DAVA::FilePath& path)
{
    using namespace DAVA;
    return path.IsEmpty() == false &&
    FileSystem::Instance()->Exists(path) == true &&
    ImageSystem::GetImageInfo(path).format != FORMAT_INVALID;
}

DAVA::FilePath FindRuntimeVariant(const DAVA::String& stem, DAVA::eGPUFamily gpu, const DAVA::Vector<DAVA::String>& formats)
{
    using namespace DAVA;
    const String postfix = ToLower(GPUFamilyDescriptor::GetGPUPrefix(gpu));
    if (postfix.empty() == true)
    {
        return FilePath();
    }
    for (const String& spelling : PrefixSpellings(postfix))
    {
        for (const String& extension : formats)
        {
            const FilePath candidate(stem + spelling + extension);
            if (ContainerReadable(candidate) == true)
            {
                return candidate;
            }
        }
    }
    return FilePath();
}

// per-GPU compression entries from the variant files present; gpusNeedingConversion collects families that got a default
void FillRuntimeCompression(DAVA::TextureDescriptor* descriptor, DAVA::Vector<DAVA::int32>* gpusNeedingConversion = nullptr)
{
    using namespace DAVA;

    const String stem = DescriptorStem(descriptor->pathname);

    for (int32 gpu = 0; gpu < eGPUFamily::GPU_FAMILY_COUNT; ++gpu)
    {
        if (gpu == eGPUFamily::GPU_ORIGIN)
        {
            continue;
        }
        if (descriptor->compression[gpu].format != FORMAT_INVALID)
        {
            continue; // keep whatever the descriptor already defines
        }
        const FilePath variant = FindRuntimeVariant(stem, static_cast<eGPUFamily>(gpu), DefaultRuntimeFormats());
        if (variant.IsEmpty() == true)
        {
            continue;
        }
        const ImageInfo info = ImageSystem::GetImageInfo(variant);
        if (info.format != FORMAT_INVALID)
        {
            descriptor->compression[gpu].format = info.format;
            descriptor->compression[gpu].imageFormat = ContainerImageFormat(variant);
        }
    }

    // no default format for a family without a container: a failed encode leaves a header-only DDS
    (void)gpusNeedingConversion;
}

// best family-prefixed variant: GPU table order first, format table second
DAVA::FilePath FindFamilyVariant(const DAVA::FilePath& texPath, const DescriptorManagerSettings* settings)
{
    using namespace DAVA;
    const String stem = DescriptorStem(texPath);
    const Vector<String> formats = RuntimeFormatsFrom(settings);

    for (eGPUFamily gpu : BuildFamilyOrder(settings))
    {
        const FilePath variant = FindRuntimeVariant(stem, gpu, formats);
        if (variant.IsEmpty() == false)
        {
            return variant;
        }
    }
    return FilePath();
}

// plain-named source built from a family variant: byte copy on equal extension, decode and re-encode otherwise
DAVA::FilePath MaterializeSource(const DAVA::FilePath& texPath, const DAVA::FilePath& variant,
                                 const DescriptorManagerSettings* settings, Report& report)
{
    using namespace DAVA;

    const String stem = DescriptorStem(texPath);

    const String variantExt = ToLower(variant.GetExtension());
    String targetExt = variantExt;
    const String wantedExt = ToLower(settings->defaultSourceExtension);
    if (wantedExt.empty() == false && wantedExt != variantExt)
    {
        targetExt = wantedExt;
    }

    const FilePath plain(stem + targetExt);
    if (FileSystem::Instance()->Exists(plain) == true && settings->overwriteExistingSource == false)
    {
        Logger::Info("[DescriptorManager] source already present, kept: %s", plain.GetAbsolutePathname().c_str());
        return plain;
    }

    bool written = false;
    if (targetExt == variantExt)
    {
        written = FileSystem::Instance()->CopyFile(variant, plain, true);
    }
    else
    {
        Image* image = ImageSystem::LoadSingleMip(variant);
        if (image != nullptr)
        {
            written = (ImageSystem::Save(plain, image) == eErrorCode::SUCCESS);
            SafeRelease(image);
        }
        if (written == false)
        {
            Logger::Warning("[DescriptorManager] cannot re-encode %s as %s, copying the variant unchanged",
                            variant.GetAbsolutePathname().c_str(), targetExt.c_str());
            written = FileSystem::Instance()->CopyFile(variant, plain, true);
        }
    }

    if (written == false)
    {
        Logger::Error("[DescriptorManager] cannot write source %s", plain.GetAbsolutePathname().c_str());
        return FilePath();
    }

    ++report.sourcesCreated;
    Logger::Info("[DescriptorManager] created source %s (from %s)",
                 plain.GetAbsolutePathname().c_str(), variant.GetAbsolutePathname().c_str());
    return plain;
}

// shipped .tex files are in export form: one baked family and format, immune to the view GPU;
// SaveDescriptorSmart keeps that form

bool RuntimeFileExistsForGPU(const DAVA::TextureDescriptor* descriptor, DAVA::eGPUFamily gpu)
{
    return FindRuntimeVariant(DescriptorStem(descriptor->pathname), gpu, DefaultRuntimeFormats()).IsEmpty() == false;
}

// family to pin the descriptor to, GPU_INVALID for full form
DAVA::eGPUFamily DetectExportPin(const DAVA::TextureDescriptor* descriptor)
{
    using namespace DAVA;
    // an export-form file keeps its baked family
    if (descriptor->IsCompressedFile() == true)
    {
        const int32 gpuVal = static_cast<int32>(descriptor->gpu);
        return (gpuVal >= 0 && gpuVal < eGPUFamily::GPU_DEVICE_COUNT) ? descriptor->gpu : GPU_INVALID;
    }
    // runtime data for one device family: save in export form
    eGPUFamily pinned = GPU_INVALID;
    int32 familiesWithData = 0;
    for (int32 gpu = 0; gpu < eGPUFamily::GPU_DEVICE_COUNT; ++gpu)
    {
        const eGPUFamily g = static_cast<eGPUFamily>(gpu);
        if (descriptor->compression[g].format == FORMAT_INVALID)
        {
            continue;
        }
        if (RuntimeFileExistsForGPU(descriptor, g) == false)
        {
            continue;
        }
        ++familiesWithData;
        pinned = g;
    }
    return (familiesWithData == 1) ? pinned : GPU_INVALID;
}

// save in the game form: export-pinned for one device family or an already pinned file;
// call after pending conversions
void SaveDescriptorSmart(DAVA::TextureDescriptor* descriptor)
{
    using namespace DAVA;
    const eGPUFamily pin = DetectExportPin(descriptor);
    if (pin == GPU_INVALID)
    {
        descriptor->Save();
        return;
    }
    // refill the pinned compression entry from the runtime file before saving
    TextureDescriptor::Compression& c = descriptor->compression[pin];
    if (c.format == FORMAT_INVALID || c.imageFormat == IMAGE_FORMAT_UNKNOWN)
    {
        const FilePath variant = FindRuntimeVariant(DescriptorStem(descriptor->pathname), pin, DefaultRuntimeFormats());
        if (variant.IsEmpty() == false)
        {
            const ImageInfo info = ImageSystem::GetImageInfo(variant);
            if (info.format != FORMAT_INVALID)
            {
                c.format = info.format;
                c.imageFormat = ContainerImageFormat(variant);
            }
        }
    }
    Logger::Info("[DescriptorManager] saving %s in game export form (pinned to %s)",
                 descriptor->pathname.GetFilename().c_str(),
                 GPUFamilyDescriptor::GetGPUName(pin).c_str());
    descriptor->Export(descriptor->pathname, pin);
}

// point a descriptor at a source file and refresh its per-GPU entries
void RelinkDescriptorTo(DAVA::TextureDescriptor* descriptor, const DAVA::FilePath& source, DAVA::Vector<DAVA::int32>* gpusNeedingConversion = nullptr)
{
    using namespace DAVA;
    const String ext = source.GetExtension();
    descriptor->dataSettings.sourceFileExtension = ext;
    descriptor->dataSettings.sourceFileFormat = ImageSystem::GetImageFormatForExtension(ext);
    descriptor->compression[eGPUFamily::GPU_ORIGIN].imageFormat = descriptor->dataSettings.sourceFileFormat;
    descriptor->compression[eGPUFamily::GPU_ORIGIN].format = ImageSystem::GetImageInfo(source).format;
    FillRuntimeCompression(descriptor, gpusNeedingConversion);
    // the caller saves after pending conversions
}

// descriptor named after texPath sourcing the given file; false for an unusable source format
bool CreateDescriptorFromSource(const DAVA::FilePath& texPath, const DAVA::FilePath& sourceFile, DAVA::Vector<DAVA::int32>* gpusNeedingConversion = nullptr)
{
    using namespace DAVA;
    const String ext = sourceFile.GetExtension();
    const ImageFormat format = ImageSystem::GetImageFormatForExtension(ext);
    // a .pvr counts as a source only when no other file exists
    const bool acceptable = TextureDescriptor::IsSupportedSourceFormat(format) || TextureDescriptor::IsSupportedCompressedFormat(format);
    if (format == IMAGE_FORMAT_UNKNOWN || acceptable == false)
    {
        return false;
    }

    ImageInfo info = ImageSystem::GetImageInfo(sourceFile);

    std::unique_ptr<TextureDescriptor> descriptor(new TextureDescriptor());
    descriptor->pathname = texPath;
    descriptor->dataSettings.sourceFileFormat = format;
    descriptor->dataSettings.sourceFileExtension = ext;
    descriptor->compression[eGPUFamily::GPU_ORIGIN].imageFormat = format;
    descriptor->compression[eGPUFamily::GPU_ORIGIN].format = info.format;
    FillRuntimeCompression(descriptor.get(), gpusNeedingConversion);
    SaveDescriptorSmart(descriptor.get());
    return true;
}

// source file for a descriptor:
// 1. plain-named file, format table order
// 2. any other source mode: best variant, materialized under the plain name
// 3. Off: variant referenced as-is, variantOnly reports it
DAVA::FilePath ResolveSource(const DAVA::FilePath& texPath, const DescriptorManagerSettings* settings, Report& report,
                             bool* variantOnly = nullptr)
{
    using namespace DAVA;

    if (variantOnly != nullptr)
    {
        *variantOnly = false;
    }

    if (settings->sourceCreate == DescriptorManagerSettings::SOURCE_OFF)
    {
        const FilePath plain = FindSourceFile(texPath, settings->formatPriority);
        if (plain.IsEmpty() == false)
        {
            return plain;
        }

        const FilePath variant = FindFamilyVariant(texPath, settings);
        if (variantOnly != nullptr && variant.IsEmpty() == false)
        {
            *variantOnly = true;
        }
        return variant;
    }

    const FilePath plain = FindSourceFile(texPath, settings->formatPriority);
    if (plain.IsEmpty() == false)
    {
        return plain;
    }

    if (settings->sourceCreate == DescriptorManagerSettings::SOURCE_EXISTING)
    {
        return FilePath();
    }

    const FilePath variant = FindFamilyVariant(texPath, settings);
    if (variant.IsEmpty() == true)
    {
        return FilePath();
    }
    return MaterializeSource(texPath, variant, settings, report);
}

// cube stem: face bitmask and the on-disk spelling
struct CubeFaces
{
    DAVA::String realStem;
    DAVA::uint8 mask = 0;
};

// enumerate one directory of the folder scan; false when the stack is empty
bool EnumerateOneDirectory(DAVA::Vector<DAVA::FilePath>& stack,
                           DAVA::Map<DAVA::String, DAVA::FilePath>& groups,
                           DAVA::Map<DAVA::String, CubeFaces>& cubeFaces,
                           DAVA::Vector<DAVA::FilePath>& descriptorsOut,
                           const DAVA::Vector<DAVA::String>& gpuPostfixes)
{
    using namespace DAVA;

    if (stack.empty() == true)
    {
        return false;
    }
    const FilePath current = stack.back();
    stack.pop_back();

    ScopedPtr<FileList> fileList(new FileList(current));
    for (uint32 fi = 0; fi < fileList->GetCount(); ++fi)
    {
        if (fileList->IsDirectory(fi))
        {
            if (fileList->IsNavigationDirectory(fi) == false)
            {
                stack.push_back(fileList->GetPathname(fi));
            }
            continue;
        }

        const FilePath path = fileList->GetPathname(fi);
        const String ext = ToLower(path.GetExtension());
        if (ext == ".tex")
        {
            descriptorsOut.push_back(path);
            continue;
        }
        if (IsImageExtension(ext) == false)
        {
            continue;
        }

        String base = ToLower(path.GetBasename());
        String baseReal = path.GetBasename(); // materials reference the real spelling
        const String dir = path.GetDirectory().GetStringValue();

        // GPU postfixes only on compressed runtime artifacts
        if (ext == ".dds" || ext == ".pvr")
        {
            const size_t foldedLength = base.size();
            StripFamilyPrefix(base, gpuPostfixes);
            baseReal.resize(baseReal.size() - (foldedLength - base.size()));
        }

        // a face file belongs to its stem's cube
        const int32 face = FaceSuffixIndex(base);
        if (face >= 0)
        {
            const size_t suffixLength = Texture::FACE_NAME_SUFFIX[face].size();
            const String cubeStem = base.substr(0, base.size() - suffixLength);
            const String key = dir + cubeStem;
            auto it = cubeFaces.find(key);
            if (it == cubeFaces.end())
            {
                CubeFaces entry;
                entry.realStem = dir + baseReal.substr(0, baseReal.size() - suffixLength);
                entry.mask = static_cast<uint8>(1 << face);
                cubeFaces.emplace(key, entry);
            }
            else
            {
                it->second.mask = static_cast<uint8>(it->second.mask | (1 << face));
            }
            continue;
        }

        // one descriptor per texture name; first hit wins
        groups.emplace(dir + base, FilePath(dir + baseReal + ".tex"));
    }
    return true;
}

// texture paths referenced by the scene or selection
void CollectSceneTexturePaths(DAVA::SceneEditor2* scene, DAVA::Entity* onlyForEntity, DAVA::Vector<DAVA::FilePath>& pathsOut)
{
    using namespace DAVA;

    SceneHelper::TextureCollector collector;
    if (onlyForEntity != nullptr)
    {
        SceneHelper::EnumerateEntityTextures(scene, onlyForEntity, collector);
    }
    else
    {
        SceneHelper::EnumerateSceneTextures(scene, collector);
    }

    for (auto& pair : collector.GetTextures())
    {
        const FilePath& texturePath = pair.first;
        if (texturePath.IsEmpty() == false)
        {
            pathsOut.push_back(TextureDescriptor::GetDescriptorPathname(texturePath));
        }
    }
}

// scene of the active context, nullptr when none is open
DAVA::SceneEditor2* ActiveScene()
{
    using namespace DAVA;
    auto* context = Deprecated::GetActiveContext();
    if (context == nullptr)
    {
        return nullptr;
    }
    SceneData* data = context->GetData<SceneData>();
    return data != nullptr ? data->GetScene().Get() : nullptr;
}

// textures of the scope; true when something was selected
bool CollectScopeTextures(DAVA::SceneEditor2* scene, bool selectedOnly, DAVA::Vector<DAVA::FilePath>& pathsOut)
{
    using namespace DAVA;

    if (selectedOnly == false)
    {
        CollectSceneTexturePaths(scene, nullptr, pathsOut);
        return false;
    }

    const SelectableGroup& selection = scene->GetSystem<SelectionSystem>()->GetSelection();
    for (const Selectable& item : selection.GetContent())
    {
        Entity* entity = item.AsEntity();
        if (entity != nullptr)
        {
            CollectSceneTexturePaths(scene, entity, pathsOut);
        }
    }
    return selection.GetSize() > 0;
}

// file the engine loads for a family: plain source for ORIGIN, prefixed container otherwise
DAVA::FilePath RuntimePathForGPU(DAVA::TextureDescriptor* descriptor, DAVA::int32 gpu)
{
    using namespace DAVA;
    if (gpu == eGPUFamily::GPU_ORIGIN)
    {
        return descriptor->GetSourceTexturePathname();
    }
    return descriptor->CreateMultiMipPathnameForGPU(static_cast<eGPUFamily>(gpu));
}

// descriptor declaring a missing or unreadable container: rebuild, never convert
bool DescriptorHasBrokenEntry(DAVA::TextureDescriptor* descriptor)
{
    using namespace DAVA;
    for (int32 gpu = 0; gpu < eGPUFamily::GPU_FAMILY_COUNT; ++gpu)
    {
        if (gpu == eGPUFamily::GPU_ORIGIN || descriptor->compression[gpu].format == FORMAT_INVALID)
        {
            continue;
        }
        if (ContainerReadable(RuntimePathForGPU(descriptor, gpu)) == false)
        {
            return true;
        }
    }
    return false;
}

// per-GPU conversion; existing readable containers are never re-encoded
void ReconvertDescriptor(DAVA::TextureDescriptor* descriptor)
{
    using namespace DAVA;
    for (int32 gpu = 0; gpu < eGPUFamily::GPU_FAMILY_COUNT; ++gpu)
    {
        if (gpu == eGPUFamily::GPU_ORIGIN || descriptor->compression[gpu].format == FORMAT_INVALID)
        {
            continue;
        }
        if (ContainerReadable(RuntimePathForGPU(descriptor, gpu)) == true)
        {
            continue;
        }
        if (ContainerReadable(descriptor->GetSourceTexturePathname()) == false)
        {
            Logger::Warning("[DescriptorManager] %s: no readable source to convert, GPU %s left as is",
                            descriptor->pathname.GetAbsolutePathname().c_str(),
                            GPUFamilyDescriptor::GetGPUName(static_cast<eGPUFamily>(gpu)));
            continue;
        }
        ImageTools::ConvertImage(descriptor, static_cast<eGPUFamily>(gpu), TextureConverter::ECQ_DEFAULT);
        // an encode that produced nothing usable is removed
        const FilePath produced = RuntimePathForGPU(descriptor, gpu);
        if (ContainerReadable(produced) == false && FileSystem::Instance()->Exists(produced) == true)
        {
            Logger::Error("[DescriptorManager] conversion for GPU %s wrote an unreadable %s, removed",
                          GPUFamilyDescriptor::GetGPUName(static_cast<eGPUFamily>(gpu)),
                          produced.GetAbsolutePathname().c_str());
            FileSystem::Instance()->DeleteFile(produced);
        }
    }
}

// Generate: missing descriptors only
void ProcessGenerateOne(const DAVA::FilePath& texPath, const DescriptorManagerSettings* settings, Report& report)
{
    using namespace DAVA;

    if (FileSystem::Instance()->Exists(texPath) == true)
    {
        return; // Generate = missing descriptors only
    }

    // six faces describe a cube
    Vector<FilePath> faces;
    const int32 faceCount = FindCubeFaces(texPath, faces);
    if (faceCount >= 2)
    {
        if (faceCount != Texture::CUBE_FACE_COUNT)
        {
            Logger::Warning("[DescriptorManager] %s looks like a cube map but only %d of %d faces exist, skipped",
                            texPath.GetAbsolutePathname().c_str(), faceCount, Texture::CUBE_FACE_COUNT);
            ++report.failedMissingSource;
            report.failedPaths.push_back(texPath.GetAbsolutePathname());
            return;
        }

        // equal size and format, canonical face names, then CLAMP + mipmaps + mask 0xFF
        if (TextureDescriptorUtils::CreateDescriptorCube(texPath, faces) == true)
        {
            ++report.created;
            Logger::Info("[DescriptorManager] created cube %s (%d faces)", texPath.GetAbsolutePathname().c_str(), faceCount);
        }
        else
        {
            ++report.failedMissingSource;
            report.failedPaths.push_back(texPath.GetAbsolutePathname());
        }
        return;
    }

    bool variantOnly = false;
    const FilePath source = ResolveSource(texPath, settings, report, &variantOnly);
    if (source.IsEmpty() == true)
    {
        ++report.failedMissingSource;
        report.failedPaths.push_back(texPath.GetAbsolutePathname());
        return;
    }

    // source mode Off: descriptor built from the variant, GPU table filled from the variant files
    const ImageFormat sourceFormat = ImageSystem::GetImageFormatForExtension(source.GetExtension());
    if (variantOnly == true || TextureDescriptor::IsSupportedSourceFormat(sourceFormat) == false)
    {
        if (CreateDescriptorFromSource(texPath, source) == true)
        {
            ++report.created;
            Logger::Info("[DescriptorManager] created %s (source variant %s, no copy)",
                         texPath.GetAbsolutePathname().c_str(),
                         source.GetAbsolutePathname().c_str());
        }
        else
        {
            ++report.failedMissingSource;
            report.failedPaths.push_back(texPath.GetAbsolutePathname());
        }
        return;
    }

    if (RETextureDescriptorUtils::CreateOrUpdateDescriptor(source) == true)
    {
        ++report.created;
        Logger::Info("[DescriptorManager] created %s (source %s)",
                     texPath.GetAbsolutePathname().c_str(),
                     source.GetAbsolutePathname().c_str());
    }
    else
    {
        ++report.failedMissingSource;
        report.failedPaths.push_back(texPath.GetAbsolutePathname());
    }
}

// Update: existing descriptors only, relinks and rebuilds, never creates
void ProcessUpdateOne(const DAVA::FilePath& texPath, const DescriptorManagerSettings* settings, Report& report)
{
    using namespace DAVA;

    if (FileSystem::Instance()->Exists(texPath) == false)
    {
        ++report.skipped; // Update does not create missing descriptors
        return;
    }

    std::unique_ptr<TextureDescriptor> descriptor(TextureDescriptor::CreateFromFile(texPath));

    if (descriptor != nullptr && descriptor->IsCubeMap() == true)
    {
        // Update leaves cubes to Generate: relinking would drop the face mask
        ++report.skipped;
        Logger::FrameworkDebug("[DescriptorManager] update skipped cube %s", texPath.GetAbsolutePathname().c_str());
        return;
    }

    // family pointing at a missing or unreadable container: rebuild from the containers that exist
    if (descriptor != nullptr && DescriptorHasBrokenEntry(descriptor.get()) == true)
    {
        const FilePath anchor = FindFamilyVariant(texPath, settings);
        if (anchor.IsEmpty() == false && CreateDescriptorFromSource(texPath, anchor) == true)
        {
            ++report.relinked;
            Logger::Warning("[DescriptorManager] repaired %s: pinned container was unreadable, rebuilt from %s",
                            texPath.GetAbsolutePathname().c_str(),
                            anchor.GetAbsolutePathname().c_str());
            return;
        }
    }

    // source mode Off writes nothing next to the data
    if (settings->sourceCreate == DescriptorManagerSettings::SOURCE_OFF)
    {
        ++report.skipped;
        return;
    }

    const FilePath currentSource = (descriptor != nullptr) ? descriptor->GetSourceTexturePathname() : FilePath();
    const bool healthy = (descriptor != nullptr) && (currentSource.IsEmpty() == false) && FileSystem::Instance()->Exists(currentSource);

    if (healthy == true && settings->updateIgnoresHealthy == true)
    {
        ++report.skipped;
        return;
    }

    if (descriptor == nullptr)
    {
        // corrupt descriptor: rebuild from the best source
        const FilePath source = ResolveSource(texPath, settings, report);
        if (source.IsEmpty() == true)
        {
            ++report.failedMissingSource;
            report.failedPaths.push_back(texPath.GetAbsolutePathname());
            return;
        }
        if (CreateDescriptorFromSource(texPath, source) == false)
        {
            ++report.failedMissingSource;
            report.failedPaths.push_back(texPath.GetAbsolutePathname());
            return;
        }
        ++report.relinked;
        Logger::Info("[DescriptorManager] rebuilt corrupt descriptor %s from %s",
                     texPath.GetAbsolutePathname().c_str(),
                     source.GetAbsolutePathname().c_str());
        return;
    }

    if (healthy == false)
    {
        const FilePath source = ResolveSource(texPath, settings, report);
        if (source.IsEmpty() == true)
        {
            ++report.failedMissingSource;
            report.failedPaths.push_back(texPath.GetAbsolutePathname());
            return;
        }

        Vector<int32> gpusToConvert;
        RelinkDescriptorTo(descriptor.get(), source, &gpusToConvert);
        Logger::Info("[DescriptorManager] relinked %s to %s",
                     texPath.GetAbsolutePathname().c_str(),
                     descriptor->GetSourceTexturePathname().GetAbsolutePathname().c_str());

        // runtime files for families that only got a default
        for (int32 gpu : gpusToConvert)
        {
            ImageTools::ConvertImage(descriptor.get(), static_cast<eGPUFamily>(gpu), TextureConverter::ECQ_DEFAULT);
        }
        ++report.relinked;
    }

    // resave and reconvert every configured family
    ReconvertDescriptor(descriptor.get());
    SaveDescriptorSmart(descriptor.get());
    ++report.updated;
}

// pass the GPU table to the engine load fallback
void ApplyEngineFamilyFallback(const DescriptorManagerSettings* settings)
{
#if defined(DAVA_TEXTURE_FAMILY_FALLBACK)
    using namespace DAVA;

    Vector<eGPUFamily> order = BuildFamilyOrder(settings);
    order.push_back(eGPUFamily::GPU_ORIGIN); // last resort: the plain source
    Texture::SetGPUFallbackOrder(order);

    String text;
    for (eGPUFamily gpu : order)
    {
        if (text.empty() == false)
        {
            text += " -> ";
        }
        text += GPUFamilyDescriptor::GetGPUName(gpu);
    }
    Logger::Info("[DescriptorManager] runtime texture fallback order: %s", text.c_str());
#else
    Logger::Warning("[DescriptorManager] engine has no runtime family fallback (apply engine patch 0015): "
                    "a descriptor pinned to a family whose runtime file is missing still renders pink");
#endif
}
} // namespace

class DescriptorManagerReactor : public DAVA::LoggerOutput
{
public:
    explicit DescriptorManagerReactor(const DescriptorManagerSettings* settings_)
        : settings(settings_)
    {
        DAVA::Logger::AddCustomOutput(this);
    }

    ~DescriptorManagerReactor()
    {
        DAVA::Logger::RemoveCustomOutput(this);
    }

    // run on project open
    void ForgetHandledPaths()
    {
        handledPaths.clear();
    }

private:
    void Output(DAVA::Logger::eLogLevel ll, const DAVA::char8* text) override
    {
        using namespace DAVA;
        if (ll != Logger::LEVEL_ERROR || settings == nullptr || settings->reactToMissingSource == false)
        {
            return;
        }
        if (settings->sourceCreate == DescriptorManagerSettings::SOURCE_OFF)
        {
            return; // source work is switched off on purpose
        }

        static const Vector<String> markers = { "Cannot create image from ", "Cannot create descriptor from " };
        for (const String& marker : markers)
        {
            String msg(text);
            const size_t pos = msg.find(marker);
            if (pos == String::npos)
            {
                continue;
            }

            String pathText = msg.substr(pos + marker.size());
            // trim whitespace and quotes
            while (pathText.empty() == false && (isspace(pathText.back()) != 0 || pathText.back() == '\'' || pathText.back() == '\"'))
            {
                pathText.pop_back();
            }
            if (pathText.empty() == true)
            {
                return;
            }

            FilePath sourcePath(pathText);
            FilePath texPath = TextureDescriptor::GetDescriptorPathname(sourcePath);
            const String texPathText = texPath.GetAbsolutePathname();
            if (std::find(handledPaths.begin(), handledPaths.end(), texPathText) != handledPaths.end())
            {
                return; // the on-disk fix for this path is already in place
            }
            handledPaths.push_back(texPathText);

            Logger::Info("[DescriptorManager] resolving %s (missing source: %s)",
                         texPath.GetFilename().c_str(), pathText.c_str());

            Report report;
            const FilePath source = ResolveSource(texPath, settings, report);
            if (source.IsEmpty() == true)
            {
                String tried;
                for (const String& ext : settings->formatPriority)
                {
                    tried += ext + " ";
                }
                Logger::Warning("[DescriptorManager] no source for %s (format order: %s, then GPU order)",
                                texPathText.c_str(), tried.c_str());
                return;
            }

            if (source.GetExtension() == ".pvr")
            {
                Logger::Warning("[DescriptorManager] %s is a PVR source; PVRTC has no CPU decoder, "
                                "CPU-side consumers (density maps, converter) will not read it",
                                texPathText.c_str());
            }

            // force-relink: the engine reported this descriptor broken
            std::unique_ptr<TextureDescriptor> descriptor(TextureDescriptor::CreateFromFile(texPath));
            if (descriptor == nullptr)
            {
                descriptor.reset(new TextureDescriptor());
                descriptor->pathname = texPath;
            }
            Vector<int32> gpusToConvert;
            RelinkDescriptorTo(descriptor.get(), source, &gpusToConvert);
            Logger::Info("[DescriptorManager] %s now sources %s",
                         texPath.GetFilename().c_str(),
                         source.GetAbsolutePathname().c_str());

            for (int32 gpu : gpusToConvert)
            {
                Logger::Info("[DescriptorManager] converting runtime files for %s (GPU index %d)",
                             texPath.GetFilename().c_str(), gpu);
                ImageTools::ConvertImage(descriptor.get(), static_cast<eGPUFamily>(gpu), TextureConverter::ECQ_DEFAULT);
            }

            SaveDescriptorSmart(descriptor.get());
            return;
        }
    }

    const DescriptorManagerSettings* settings = nullptr;
    DAVA::Vector<DAVA::String> handledPaths;
};

// scene connection state and verified descriptors; kept out of the header because a RefPtr
// member needs the complete type
struct DescriptorManagerModule::SceneStructureWatcher
{
    // scene identity only: a strong reference would outlive its context
    DAVA::SceneEditor2* scene = nullptr;

    // descriptors already verified this session
    std::set<DAVA::String> verified;

    // descriptors that stay pink: one attempt, then no retries
    std::set<DAVA::String> failedPink;
};

// state of the incremental project scan
struct DescriptorManagerModule::ScanJobData
{
    bool update = false;
    bool collecting = true;

    DAVA::Vector<DAVA::String> gpuPostfixes;
    DAVA::Vector<DAVA::FilePath> dirStack;
    DAVA::Map<DAVA::String, DAVA::FilePath> groupMap;
    // cube stem -> face bitmask and on-disk spelling
    DAVA::Map<DAVA::String, CubeFaces> cubeFaceMap;
    DAVA::Vector<DAVA::FilePath> groups;
    DAVA::Vector<DAVA::FilePath> descriptors;

    DAVA::uint32 nextGroup = 0;
    DAVA::uint32 nextDescriptor = 0;
    Report report;

    QElapsedTimer total;
    DAVA::int64 lastProgressMs = 0;
    DAVA::String action;
};

DescriptorManagerModule::DescriptorManagerModule()
{
}

DescriptorManagerModule::~DescriptorManagerModule()
{
    StopScanJob(nullptr);
    if (scanTimer != nullptr)
    {
        delete scanTimer;
        scanTimer = nullptr;
    }
    if (scenePassTimer != nullptr)
    {
        scenePassTimer->stop();
        delete scenePassTimer;
        scenePassTimer = nullptr;
    }
    if (reactor != nullptr)
    {
        reactor.reset();
    }
}

void DescriptorManagerModule::PostInit()
{
    using namespace DAVA;

    ContextAccessor* accessor = GetAccessor();

    // load settings
    settings.reset(new DescriptorManagerSettings());
    PropertiesItem properties = accessor->CreatePropertiesNode("DescriptorManager");
    settings->Load(properties);
    // the time tie-break lives in the engine resolver
    DAVA::TextureDescriptor::SetTimePriority(settings->timePriority);

    // the reactor checks reactToMissingSource per error
    reactor.reset(new DescriptorManagerReactor(settings.get()));

    // hand the GPU table to the engine as fallback order
    ApplyEngineFamilyFallback(settings.get());

    // run Generate/Update when a project opens
    binder.reset(new FieldBinder(accessor));
    FieldDescriptor projectField;
    projectField.type = ReflectedTypeDB::Get<ProjectManagerData>();
    projectField.fieldName = FastName(ProjectManagerData::ProjectPathProperty.c_str());
    binder->BindField(projectField, [this](const Any& v)
                      {
                          if (v.IsEmpty() == false && v.CanCast<FilePath>())
                          {
                              OnProjectOpened();
                              if (reactor != nullptr)
                              {
                                  reactor->ForgetHandledPaths();
                              }
                          } });

    // scenes opened later: data ships images without .tex
    FieldDescriptor sceneField;
    sceneField.type = ReflectedTypeDB::Get<SceneData>();
    sceneField.fieldName = FastName(SceneData::scenePropertyName);
    binder->BindField(sceneField, [this](const Any& value)
                      {
                          if (value.CanCast<SceneData::TSceneType>() == false)
                          {
                              // no scene in the active context any more
                              if (structureWatcher != nullptr)
                              {
                                  structureWatcher->scene = nullptr;
                                  structureWatcher->verified.clear();
                                  structureWatcher->failedPink.clear();
                              }
                              return;
                          }
                          OnSceneOpened(value.Cast<SceneData::TSceneType>().Get()); });

    // Tools -> Descriptor Manager
    UI* ui = GetUI();

    const auto addEntry = [this, ui](const char* text, std::function<void()> handler)
    {
        ActionPlacementInfo placementInfo(CreateMenuPoint(QList<QString>() << "Tools" << "Descriptor Manager"));
        QtAction* action = new QtAction(GetAccessor(), QString::fromUtf8(text));
        connections.AddConnection(action, &QAction::triggered, handler);
        ui->AddAction(DAVA::mainWindowKey, placementInfo, action);
    };

    addEntry("Generate (All)", [this]()
             { RunGenerate(Scope::All); });
    addEntry("Generate (Scene)", [this]()
             { RunGenerate(Scope::Scene); });
    addEntry("Generate (Selected)", [this]()
             { RunGenerate(Scope::Selected); });
    addEntry("Update (All)", [this]()
             { RunUpdate(Scope::All); });
    addEntry("Update (Scene)", [this]()
             { RunUpdate(Scope::Scene); });
    addEntry("Update (Selected)", [this]()
             { RunUpdate(Scope::Selected); });
    addEntry("Settings...", [this]()
             { ShowSettingsDialog(); });
}

void DescriptorManagerModule::OnProjectOpened()
{
    using namespace DAVA;
    if (settings->startupAction == DescriptorManagerSettings::STARTUP_GENERATE)
    {
        Logger::Info("[DescriptorManager] startup: Generate (All)");
        RunGenerate(Scope::All);
    }
    else if (settings->startupAction == DescriptorManagerSettings::STARTUP_UPDATE)
    {
        Logger::Info("[DescriptorManager] startup: Update (All)");
        RunUpdate(Scope::All);
    }
}

// a texture without .tex loads as a pink placeholder
void DescriptorManagerModule::OnSceneOpened(DAVA::SceneEditor2* scene)
{
    // connect first so a later Add Scene is handled
    WatchSceneStructure(scene);

    if (settings->generateMissingOnSceneOpen == false)
    {
        return;
    }

    // deferred: reloads inside the scene open call are too early
    ScheduleSceneTexturePass();
}

void DescriptorManagerModule::WatchSceneStructure(DAVA::SceneEditor2* scene)
{
    using namespace DAVA;

    if (scene == nullptr)
    {
        return;
    }

    if (structureWatcher == nullptr)
    {
        structureWatcher.reset(new SceneStructureWatcher());
    }

    if (structureWatcher->scene == scene)
    {
        return;
    }

    // another scene means another verified set
    structureWatcher->verified.clear();
    structureWatcher->failedPink.clear();
    structureWatcher->scene = scene;

    StructureSystem* system = scene->GetSystem<StructureSystem>();
    if (system != nullptr)
    {
        // emitted one frame after the scene is merged
        system->structureChangedSignal.Connect(this, &DescriptorManagerModule::OnSceneStructureChanged);
    }

    // the command path does not depend on the structure flag
    scene->commandExecuted.Connect(this, &DescriptorManagerModule::OnSceneCommandExecuted);
}

void DescriptorManagerModule::OnSceneStructureChanged(DAVA::SceneEditor2* scene)
{
    if (settings->generateMissingOnSceneOpen == false)
    {
        return;
    }

    ScheduleSceneTexturePass();
}

void DescriptorManagerModule::OnSceneCommandExecuted(DAVA::SceneEditor2* scene, const DAVA::RECommandNotificationObject& commandNotification)
{
    if (settings->generateMissingOnSceneOpen == false)
    {
        return;
    }

    ScheduleSceneTexturePass();
}

void DescriptorManagerModule::ScheduleSceneTexturePass()
{
    if (scenePassTimer == nullptr)
    {
        scenePassTimer = new QTimer(nullptr); // ClientModule is not a QObject: manual ownership
        scenePassTimer->setSingleShot(true);
        connections.AddConnection(scenePassTimer, &QTimer::timeout, [this]()
                                  { RunScheduledSceneTexturePass(); });
    }

    // the idle timer doubles as the debounce
    scenePassTimer->start(SCENE_PASS_DELAY_MS);
}

void DescriptorManagerModule::RunScheduledSceneTexturePass()
{
    using namespace DAVA;

    // scene resolved when the pass runs, never held
    SceneEditor2* scene = ActiveScene();
    if (scene == nullptr)
    {
        return;
    }

    RunSceneTexturePass(scene, "Scene content");
}

namespace
{
// TextureCollector keeps only paths under the project path
void CollectMaterialTextures(DAVA::Entity* node, DAVA::Map<DAVA::FilePath, DAVA::Vector<DAVA::Texture*>>& textures)
{
    using namespace DAVA;

    if (node == nullptr)
    {
        return;
    }

    List<NMaterial*> materials;
    node->GetDataNodes(materials);

    Set<MaterialTextureInfo*> infos;
    for (NMaterial* root : materials)
    {
        if (root == nullptr || root->IsRuntime() == true)
        {
            continue;
        }

        // walk the material parent chain: CollectLocalTextures reports one object only
        const int32 maxDepth = 16;
        int32 depth = 0;
        for (NMaterial* material = root; material != nullptr && depth < maxDepth; material = material->GetParent(), ++depth)
        {
            infos.clear();
            material->CollectLocalTextures(infos);
            for (MaterialTextureInfo* info : infos)
            {
                if (info == nullptr || info->path.IsEmpty() == true)
                {
                    continue;
                }

                // the same path can be bound by several materials
                Vector<Texture*>& bound = textures[info->path];
                if (std::find(bound.begin(), bound.end(), info->texture) == bound.end())
                {
                    bound.push_back(info->texture);
                }
            }
        }
    }
}
} // namespace

void DescriptorManagerModule::RunSceneTexturePass(DAVA::SceneEditor2* scene, const char* reason)
{
    using namespace DAVA;

    if (scene == nullptr || scene->IsLoaded() == false)
    {
        return;
    }

    if (structureWatcher == nullptr)
    {
        structureWatcher.reset(new SceneStructureWatcher());
    }
    std::set<String>& verified = structureWatcher->verified;
    std::set<String>& failedPink = structureWatcher->failedPink;

    Map<FilePath, Vector<Texture*>> textures;
    CollectMaterialTextures(scene, textures);

    Report report;
    uint32 missing = 0;
    uint32 reloaded = 0;
    uint32 stillPink = 0;

    for (auto& pair : textures)
    {
        const FilePath& texturePath = pair.first;
        const FilePath descriptorPath = TextureDescriptor::GetDescriptorPathname(texturePath);
        if (descriptorPath.IsEmpty() == true)
        {
            continue;
        }

        // only textures seen since the last pass are stat'ed
        const String verifiedKey = descriptorPath.GetAbsolutePathname();
        if (verified.find(verifiedKey) != verified.end() || failedPink.find(verifiedKey) != failedPink.end())
        {
            continue;
        }

        // only pink textures are reloaded
        bool anyPink = false;
        for (Texture* texture : pair.second)
        {
            if (texture != nullptr && texture->IsPinkPlaceholder() == true)
            {
                anyPink = true;
                break;
            }
        }

        if (FileSystem::Instance()->Exists(descriptorPath) == true)
        {
            if (anyPink == false)
            {
                verified.insert(verifiedKey);
                continue;
            }
        }
        else
        {
            ++missing;

            // ProcessGenerateOne honours the source mode
            ProcessGenerateOne(descriptorPath, settings.get(), report);
            if (FileSystem::Instance()->Exists(descriptorPath) == false)
            {
                continue;
            }
        }

        // reload in place: materials already reference the objects
        std::unique_ptr<TextureDescriptor> descriptor(TextureDescriptor::CreateFromFile(descriptorPath));
        if (descriptor == nullptr)
        {
            Logger::Error("[DescriptorManager] cannot read the descriptor just written: %s", verifiedKey.c_str());
            continue;
        }

        bool fixedSomething = false;
        uint32 pinkHere = 0;
        for (Texture* texture : pair.second)
        {
            if (texture == nullptr || texture->IsPinkPlaceholder() == false)
            {
                continue;
            }

            texture->GetDescriptor()->Initialize(descriptor.get());
            texture->ReloadAs(eGPUFamily::GPU_ORIGIN);

            if (texture->IsPinkPlaceholder() == true)
            {
                ++stillPink;
                ++pinkHere;
            }
            else
            {
                ++reloaded;
                fixedSomething = true;
            }
        }

        if (pinkHere > 0 && fixedSomething == false)
        {
            // still pink after reload: report once
            failedPink.insert(verifiedKey);
            Logger::Error("[DescriptorManager] descriptor exists but the texture stays pink (no source or source mode Off): %s", verifiedKey.c_str());
        }
        else
        {
            verified.insert(verifiedKey);
        }
    }

    // stay quiet when there was nothing to do
    if (missing > 0 || reloaded > 0 || stillPink > 0)
    {
        Logger::Info("[DescriptorManager] %s: %u texture path(s) in scene, %u without descriptor, %u created, %u texture(s) reloaded, %u still pink",
                     reason, static_cast<uint32>(textures.size()), missing, report.created, reloaded, stillPink);
    }

    if (report.created == 0)
    {
        return;
    }

    report.Log(reason);
}

void DescriptorManagerModule::ShowSettingsDialog()
{
    DescriptorManagerSettingsDialog dialog(settings.get());
    dialog.exec();

    DAVA::PropertiesItem properties = GetAccessor()->CreatePropertiesNode("DescriptorManager");
    settings->Save(properties);

    // the GPU table doubles as the fallback order
    ApplyEngineFamilyFallback(settings.get());
}

// start the incremental project scan
void DescriptorManagerModule::StartScanJob(bool update)
{
    using namespace DAVA;

    ProjectManagerData* data = Deprecated::GetDataNode<ProjectManagerData>();
    const FilePath dataSourcePath = (data != nullptr) ? data->GetDataSource3DPath() : FilePath();
    if (dataSourcePath.IsEmpty() == true)
    {
        Logger::Error("[DescriptorManager] no project is opened");
        return;
    }

    StopScanJob("superseded by a new scan");

    scanJob.reset(new ScanJobData());
    scanJob->update = update;
    scanJob->action = (update == true) ? "Update" : "Generate";
    scanJob->gpuPostfixes = BuildGPUPostfixes(settings.get());
    scanJob->dirStack.push_back(dataSourcePath);
    scanJob->total.start();

    Logger::Info("[DescriptorManager] %s (All) started (incremental: the editor stays responsive)",
                 scanJob->action.c_str());

    if (scanTimer == nullptr)
    {
        scanTimer = new QTimer(nullptr); // ClientModule is not a QObject: manual ownership
        scanTimer->setSingleShot(false);
        scanTimer->setInterval(10);
        connections.AddConnection(scanTimer, &QTimer::timeout, [this]()
                                  { ProcessScanSlice(); });
    }
    scanTimer->start();
}

void DescriptorManagerModule::StopScanJob(const char* reason)
{
    using namespace DAVA;
    if (scanTimer != nullptr)
    {
        scanTimer->stop();
    }
    if (scanJob != nullptr)
    {
        if (reason != nullptr && reason[0] != 0)
        {
            Logger::Info("[DescriptorManager] %s (All) aborted (%s)",
                         scanJob->action.c_str(), reason);
        }
        scanJob.reset();
    }
}

// about 30 ms of scan work per tick
void DescriptorManagerModule::ProcessScanSlice()
{
    using namespace DAVA;

    if (scanJob == nullptr)
    {
        if (scanTimer != nullptr)
        {
            scanTimer->stop();
        }
        return;
    }

    ScanJobData& job = *scanJob;
    QElapsedTimer slice;
    slice.start();

    for (;;)
    {
        if (slice.elapsed() >= 30)
        {
            break;
        }

        if (job.collecting == true)
        {
            if (EnumerateOneDirectory(job.dirStack, job.groupMap, job.cubeFaceMap, job.descriptors, job.gpuPostfixes) == false)
            {
                job.collecting = false;
                job.groups.reserve(job.groupMap.size() + job.cubeFaceMap.size());
                for (auto& pair : job.groupMap)
                {
                    job.groups.push_back(pair.second);
                }
                job.groupMap.clear(); // release the map before the heavy passes

                // a full face set becomes one cube descriptor, a lone face stays 2D
                uint32 cubes = 0;
                for (auto& pair : job.cubeFaceMap)
                {
                    const uint8 mask = pair.second.mask;
                    const String& realStem = pair.second.realStem;
                    if (mask == 0xFF)
                    {
                        job.groups.push_back(FilePath(realStem + ".tex"));
                        ++cubes;
                    }
                    else if (mask != 0 && (mask & (mask - 1)) == 0)
                    {
                        int32 onlyFace = 0;
                        while (onlyFace < Texture::CUBE_FACE_COUNT && (mask & (1 << onlyFace)) == 0)
                        {
                            ++onlyFace;
                        }
                        if (onlyFace < Texture::CUBE_FACE_COUNT)
                        {
                            job.groups.push_back(FilePath(realStem + Texture::FACE_NAME_SUFFIX[onlyFace] + ".tex"));
                        }
                    }
                }
                job.cubeFaceMap.clear();

                Logger::Info("[DescriptorManager] %s (All): folder scan finished (%u descriptors, %u image groups, %u cubes)",
                             job.action.c_str(),
                             static_cast<uint32>(job.descriptors.size()),
                             static_cast<uint32>(job.groups.size()),
                             cubes);
            }
            continue;
        }

        // Generate walks image groups, Update walks descriptors
        if (job.update == false && job.nextGroup < job.groups.size())
        {
            ProcessGenerateOne(job.groups[job.nextGroup], settings.get(), job.report);
            ++job.nextGroup;
            continue;
        }

        if (job.nextDescriptor < job.descriptors.size())
        {
            if (job.update == true)
            {
                ProcessUpdateOne(job.descriptors[job.nextDescriptor], settings.get(), job.report);
            }
            ++job.nextDescriptor;
            continue;
        }

        // scan finished
        job.report.Log(job.action);
        Logger::Info("[DescriptorManager] %s (All) finished in %.1f s",
                     job.action.c_str(), job.total.elapsed() / 1000.0);
        StopScanJob(nullptr);
        return;
    }

    // progress at most every 2 s
    if (job.total.elapsed() - job.lastProgressMs >= 2000)
    {
        job.lastProgressMs = job.total.elapsed();
        if (job.collecting == true)
        {
            Logger::Info("[DescriptorManager] %s (All): scanning folders (%u descriptors so far)",
                         job.action.c_str(),
                         static_cast<uint32>(job.descriptors.size()));
        }
        else
        {
            // count only the list being walked
            const uint32 done = job.update == true ? job.nextDescriptor : job.nextGroup + job.nextDescriptor;
            const uint32 total = static_cast<uint32>(job.update == true ? job.descriptors.size() : job.groups.size() + job.descriptors.size());
            Logger::Info("[DescriptorManager] %s (All): processed %u / %u (%.0f%%)",
                         job.action.c_str(), done, total,
                         total > 0 ? 100.0 * double(done) / double(total) : 100.0);
        }
    }
}

void DescriptorManagerModule::RunGenerate(Scope scope)
{
    using namespace DAVA;

    if (scope == Scope::All)
    {
        // time slices keep the editor responsive on large folders
        StartScanJob(false);
        return;
    }

    SceneEditor2* scene = ActiveScene();
    if (scene == nullptr)
    {
        Logger::Error("[DescriptorManager] no scene is opened");
        return;
    }

    Vector<FilePath> texPaths;
    const bool hadSelection = CollectScopeTextures(scene, scope == Scope::Selected, texPaths);
    if (scope == Scope::Selected && hadSelection == true && texPaths.empty() == true)
    {
        Logger::Info("[DescriptorManager] selection has no textures");
    }

    Logger::Info("[DescriptorManager] Generate (%s) started (missing descriptors only)", ScopeName(scope));

    Report report;
    for (const FilePath& texPath : texPaths)
    {
        ProcessGenerateOne(texPath, settings.get(), report);
    }
    report.Log("Generate");
}

void DescriptorManagerModule::RunUpdate(Scope scope)
{
    using namespace DAVA;

    if (scope == Scope::All)
    {
        StartScanJob(true);
        return;
    }

    SceneEditor2* scene = ActiveScene();
    if (scene == nullptr)
    {
        Logger::Error("[DescriptorManager] no scene is opened");
        return;
    }

    Vector<FilePath> texPaths;
    const bool hadSelection = CollectScopeTextures(scene, scope == Scope::Selected, texPaths);
    if (scope == Scope::Selected && hadSelection == true && texPaths.empty() == true)
    {
        Logger::Info("[DescriptorManager] selection has no textures");
    }

    Logger::Info("[DescriptorManager] Update (%s) started (ignore healthy: %s)",
                 ScopeName(scope), settings->updateIgnoresHealthy ? "yes" : "no");

    Report report;
    for (const FilePath& texPath : texPaths)
    {
        ProcessUpdateOne(texPath, settings.get(), report);
    }
    report.Log("Update");
}

const char* DescriptorManagerModule::ScopeName(Scope scope)
{
    switch (scope)
    {
    case Scope::Scene:
        return "Scene";
    case Scope::Selected:
        return "Selected";
    default:
        return "All";
    }
}

DAVA_VIRTUAL_REFLECTION_IMPL(DescriptorManagerModule)
{
    DAVA::ReflectionRegistrator<DescriptorManagerModule>::Begin()
    .ConstructorByPointer()
    .End();
}

DECL_TARC_MODULE(DescriptorManagerModule);
