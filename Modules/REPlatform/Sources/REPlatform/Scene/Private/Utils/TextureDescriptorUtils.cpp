#include "REPlatform/Scene/Utils/ImageTools.h"
#include "REPlatform/Scene/Utils/Preset.h"
#include "REPlatform/Scene/Utils/RETextureDescriptorUtils.h"

#include <FileSystem/FileList.h>
#include <FileSystem/KeyedArchive.h>
#include <Functional/Function.h>
#include <Logger/Logger.h>
#include <Render/GPUFamilyDescriptor.h>
#include <Render/Image/ImageSystem.h>

namespace DAVA
{
namespace RETextureDescriptorUtils
{
namespace Internal
{
bool IsCorrectDirectory(FileList* fileList, const uint32 fileIndex)
{
    if (fileList->IsDirectory(fileIndex))
    {
        String name = fileList->GetFilename(fileIndex);
        if (0 != CompareCaseInsensitive(String(".svn"), name) && !fileList->IsNavigationDirectory(fileIndex))
        {
            return true;
        }
    }

    return false;
}

template <typename FileActionFunctor, typename... Args>
void RecursiveWalk(FileActionFunctor fileAction, const FilePath& folderPath, Args... args)
{
    ScopedPtr<FileList> fileList(new FileList(folderPath));
    for (uint32 fi = 0; fi < fileList->GetCount(); ++fi)
    {
        const FilePath& pathname = fileList->GetPathname(fi);
        if (IsCorrectDirectory(fileList, fi))
        {
            RecursiveWalk(fileAction, pathname, args...);
        }
        else
        {
            fileAction(pathname, args...);
        }
    }
}

bool CreateOrUpdateDescriptor(const FilePath& texturePath, const KeyedArchive* preset)
{
    const String sourceExtension = texturePath.GetExtension();
    const ImageFormat sourceFormat = ImageSystem::GetImageFormatForExtension(sourceExtension);

    if (sourceFormat == IMAGE_FORMAT_UNKNOWN || false == TextureDescriptor::IsSupportedSourceFormat(sourceFormat))
    {
        return false;
    }

    // a .pvr is a last-resort source: an existing same-basename .dds is kept,
    // otherwise this pass rewrites a freshly adopted dds back to pvr (the walk visits both)
    if (sourceFormat == IMAGE_FORMAT_PVR)
    {
        FilePath ddsSibling = FilePath::CreateWithNewExtension(texturePath, ".dds");
        if (FileSystem::Instance()->Exists(ddsSibling))
        {
            Logger::Info("[texdescriptor] skipped %s (same-basename .dds preferred as source)",
                         texturePath.GetAbsolutePathname().c_str());
            return true;
        }
    }

    ImageInfo info = ImageSystem::GetImageInfo(texturePath);

    FilePath descriptorPath = TextureDescriptor::GetDescriptorPathname(texturePath);

    std::unique_ptr<TextureDescriptor> descriptor;
    bool descriptorChanged = false;

    if (FileSystem::Instance()->Exists(descriptorPath))
    {
        descriptor.reset(TextureDescriptor::CreateFromFile(descriptorPath));
    }

    // declared source before any rewrite, for the log below
    String previousSourceExtension = (descriptor != nullptr) ? descriptor->dataSettings.sourceFileExtension : String();

    if (!descriptor)
    {
        descriptor.reset(new TextureDescriptor());
        descriptor->pathname = descriptorPath;
        descriptor->dataSettings.sourceFileFormat = sourceFormat;
        descriptor->dataSettings.sourceFileExtension = sourceExtension;

        descriptor->compression[eGPUFamily::GPU_ORIGIN].imageFormat = sourceFormat;
        descriptor->compression[eGPUFamily::GPU_ORIGIN].format = info.format;

        descriptorChanged = true;
    }
    else
    {
        if (sourceFormat != descriptor->dataSettings.sourceFileFormat)
        {
            // re-point only when the declared master is missing from disk:
            // the walk also visits compiled siblings (.dds/.pvr)
            const FilePath declaredSource = descriptor->GetSourceTexturePathname();
            if (false == FileSystem::Instance()->Exists(declaredSource))
            {
                descriptor->dataSettings.sourceFileFormat = sourceFormat;
                descriptor->dataSettings.sourceFileExtension = sourceExtension;

                descriptor->compression[eGPUFamily::GPU_ORIGIN].imageFormat = sourceFormat;
                descriptor->compression[eGPUFamily::GPU_ORIGIN].format = info.format;

                descriptorChanged = true;
            }
        }
    }

    if (preset != nullptr && Preset::ApplyTexturePreset(descriptor.get(), preset))
    {
        descriptorChanged = true;
    }

    // family-infixed files (x.dx11.pvr) are repair sources for main descriptors:
    // shipped content often keeps pixel data only under GPU-infixed names
    {
        const eGPUFamily familyGPU = GPUFamilyDescriptor::GetGPUForPathname(texturePath);
        if (GPUFamilyDescriptor::IsGPUForDevice(familyGPU))
        {
            const String& gpuPrefix = GPUFamilyDescriptor::GetGPUPrefix(familyGPU);
            const String filename = texturePath.GetFilename();
            const size_t infixPos = filename.rfind(gpuPrefix);
            if (infixPos != String::npos)
            {
                String mainFilename = filename;
                mainFilename.erase(infixPos, gpuPrefix.size());

                FilePath mainTexturePath = texturePath;
                mainTexturePath.ReplaceFilename(mainFilename);
                FilePath mainDescriptorPath = TextureDescriptor::GetDescriptorPathname(mainTexturePath);

                std::unique_ptr<TextureDescriptor> mainDescriptor;
                if (FileSystem::Instance()->Exists(mainDescriptorPath))
                {
                    mainDescriptor.reset(TextureDescriptor::CreateFromFile(mainDescriptorPath));
                }

                const bool masterGone =
                (mainDescriptor != nullptr) &&
                (false == FileSystem::Instance()->Exists(mainDescriptor->GetSourceTexturePathname()));

                if (masterGone)
                {
                    // prefer the dx11 family file; another family only when no dx11 sibling exists
                    bool shouldRepair = (familyGPU == eGPUFamily::GPU_DX11);
                    if (!shouldRepair)
                    {
                        const String& dx11Prefix = GPUFamilyDescriptor::GetGPUPrefix(eGPUFamily::GPU_DX11);
                        const String dx11Filename = mainFilename;
                        const size_t dotPos = dx11Filename.rfind('.');
                        const String dx11Sibling = dx11Filename.substr(0, dotPos) + dx11Prefix + dx11Filename.substr(dotPos);
                        FilePath dx11SiblingPath = texturePath;
                        dx11SiblingPath.ReplaceFilename(dx11Sibling);
                        shouldRepair = (false == FileSystem::Instance()->Exists(dx11SiblingPath));
                    }

                    // the copy target must not exist yet
                    if (shouldRepair && (false == FileSystem::Instance()->Exists(mainTexturePath)))
                    {
                        if (FileSystem::Instance()->CopyFile(texturePath, mainTexturePath, false))
                        {
                            Logger::Info("[texdescriptor] repaired %s from family file %s",
                                         mainDescriptorPath.GetAbsolutePathname().c_str(),
                                         texturePath.GetAbsolutePathname().c_str());
                            CreateOrUpdateDescriptor(mainTexturePath, preset);
                        }
                        else
                        {
                            Logger::Error("[texdescriptor] failed to copy %s -> %s",
                                          texturePath.GetAbsolutePathname().c_str(),
                                          mainTexturePath.GetAbsolutePathname().c_str());
                        }
                    }
                }
            }
        }
    }

    if (descriptorChanged)
    {
        if (previousSourceExtension.empty())
        {
            Logger::Info("[texdescriptor] created %s (source: %s)",
                         descriptorPath.GetAbsolutePathname().c_str(), sourceExtension.c_str());
        }
        else if (previousSourceExtension != sourceExtension)
        {
            Logger::Info("[texdescriptor] re-pointed %s (%s -> %s)",
                         descriptorPath.GetAbsolutePathname().c_str(),
                         previousSourceExtension.c_str(), sourceExtension.c_str());
        }
        else
        {
            Logger::Info("[texdescriptor] updated %s (source stays %s)",
                         descriptorPath.GetAbsolutePathname().c_str(), sourceExtension.c_str());
        }

        descriptor->Save(descriptorPath);
    }
    else
    {
        Logger::FrameworkDebug("[texdescriptor] unchanged %s", descriptorPath.GetAbsolutePathname().c_str());
    }

    return true;
}

void CreateDescriptorsForFolder(const FilePath& folderPath, const KeyedArchive* preset)
{
    auto fileAction = DAVA::MakeFunction(&Internal::CreateOrUpdateDescriptor);
    RecursiveWalk(fileAction, folderPath, preset);
}

void SetPreset(const FilePath& descriptorPath, const KeyedArchive* preset, bool toConvert, TextureConverter::eConvertQuality quality)
{
    if (false == TextureDescriptor::IsDescriptorExtension(descriptorPath.GetExtension()))
    {
        return;
    }

    std::unique_ptr<TextureDescriptor> descriptor(TextureDescriptor::CreateFromFile(descriptorPath));
    if (!descriptor)
    {
        return;
    }

    bool applied = Preset::ApplyTexturePreset(descriptor.get(), preset);
    if (applied)
    {
        if (toConvert)
        {
            for (uint8 gpu = 0; gpu < GPU_FAMILY_COUNT; ++gpu)
            {
                DAVA::eGPUFamily eGPU = static_cast<eGPUFamily>(gpu);
                ImageTools::ConvertImage(descriptor.get(), eGPU, quality);
            }
        }

        descriptor->Save();
    }
}

void SetPresetForFolder(const FilePath& folder, const KeyedArchive* preset, bool toConvert, TextureConverter::eConvertQuality quality)
{
    auto fileAction = DAVA::MakeFunction(&Internal::SetPreset);
    RecursiveWalk(fileAction, folder, preset, toConvert, quality);
}

} // namespace Internal

void ResaveDescriptorsForFolder(const FilePath& folderPath)
{
    auto fileAction = DAVA::MakeFunction(&ResaveDescriptor);
    Internal::RecursiveWalk(fileAction, folderPath);
}

void ResaveDescriptor(const FilePath& descriptorPath)
{
    if (TextureDescriptor::IsDescriptorExtension(descriptorPath.GetExtension()))
    {
        std::unique_ptr<TextureDescriptor> descriptor(TextureDescriptor::CreateFromFile(descriptorPath));
        DVASSERT(descriptor);
        descriptor->Save();
    }
}

void CreateDescriptorsForFolder(const DAVA::FilePath& folder, const DAVA::FilePath& presetPath)
{
    if (false == FileSystem::Instance()->Exists(folder))
    {
        Logger::Error("[texdescriptor] folder does not exist: %s", folder.GetAbsolutePathname().c_str());
        return;
    }

    Logger::Info("[texdescriptor] scanning folder: %s", folder.GetAbsolutePathname().c_str());
    ScopedPtr<KeyedArchive> preset(Preset::LoadArchive(presetPath));
    Internal::CreateDescriptorsForFolder(folder, preset);
    Logger::Info("[texdescriptor] scan finished: %s", folder.GetAbsolutePathname().c_str());
}

bool CreateOrUpdateDescriptor(const DAVA::FilePath& texturePath, const DAVA::FilePath& presetPath)
{
    ScopedPtr<KeyedArchive> preset(Preset::LoadArchive(presetPath));
    return Internal::CreateOrUpdateDescriptor(texturePath, preset);
}

void SetCompressionParamsForFolder(const FilePath& folderPath, const DAVA::Map<DAVA::eGPUFamily, DAVA::TextureDescriptor::Compression>& compressionParams, bool convertionEnabled, bool force, DAVA::TextureConverter::eConvertQuality quality, bool generateMipMaps)
{
    auto fileAction = DAVA::MakeFunction(&SetCompressionParams);
    Internal::RecursiveWalk(fileAction, folderPath, compressionParams, convertionEnabled, force, quality, generateMipMaps);
}

void SetCompressionParams(const FilePath& descriptorPath, const DAVA::Map<DAVA::eGPUFamily, DAVA::TextureDescriptor::Compression>& compressionParams, bool convertionEnabled, bool force, DAVA::TextureConverter::eConvertQuality quality, bool generateMipMaps)
{
    if (false == TextureDescriptor::IsDescriptorExtension(descriptorPath.GetExtension()))
    {
        return;
    }

    std::unique_ptr<TextureDescriptor> descriptor(TextureDescriptor::CreateFromFile(descriptorPath));
    if (!descriptor)
    {
        return;
    }

    descriptor->dataSettings.SetGenerateMipmaps(generateMipMaps);

    DVASSERT(descriptor->compression);

    for (const auto& compressionParam : compressionParams)
    {
        eGPUFamily gpu = compressionParam.first;

        if (force || (descriptor->compression[gpu].format == FORMAT_INVALID))
        {
            PixelFormat dstFormat = static_cast<PixelFormat>(compressionParam.second.format);
            if (dstFormat == FORMAT_PVR2 || dstFormat == FORMAT_PVR4)
            {
                DAVA::FilePath path = descriptor->GetSourceTexturePathname();
                ImageInfo imgInfo = ImageSystem::GetImageInfo(descriptor->GetSourceTexturePathname());
                if (imgInfo.width != imgInfo.height)
                {
                    DAVA::Logger::Error("Can't set %s compression for non-squared texture %s",
                                        GlobalEnumMap<PixelFormat>::Instance()->ToString(dstFormat),
                                        path.GetAbsolutePathname().c_str());
                    continue;
                }
            }

            descriptor->compression[gpu] = compressionParam.second;

            if (convertionEnabled)
            {
                ImageTools::ConvertImage(descriptor.get(), gpu, quality);
            }
        }
    }

    descriptor->Save();
}

void SetPresetForFolder(const FilePath& folder, const FilePath& presetPath, bool toConvert, TextureConverter::eConvertQuality quality)
{
    ScopedPtr<KeyedArchive> preset(Preset::LoadArchive(presetPath));
    if (preset)
    {
        Internal::SetPresetForFolder(folder, preset, toConvert, quality);
    }
}

void SetPreset(const FilePath& descriptorPath, const FilePath& presetPath, bool toConvert, TextureConverter::eConvertQuality quality)
{
    ScopedPtr<KeyedArchive> preset(Preset::LoadArchive(presetPath));
    if (preset)
    {
        Internal::SetPreset(descriptorPath, preset, toConvert, quality);
    }
}

void SavePreset(const DAVA::Vector<DAVA::FilePath>& descriptors, const DAVA::Vector<DAVA::FilePath>& presets)
{
    if (descriptors.size() != presets.size())
    {
        Logger::Error("Descriptors size differs from presets size");
        return;
    }

    size_t count = descriptors.size();
    for (size_t i = 0; i < count; ++i)
    {
        std::unique_ptr<TextureDescriptor> descriptor(TextureDescriptor::CreateFromFile(descriptors[i]));
        if (!descriptor)
        {
            Logger::Error("Cannot create descriptor from file %s", descriptors[i].GetStringValue().c_str());
            continue;
        }

        ScopedPtr<KeyedArchive> presetArchive(new KeyedArchive());
        if (descriptor->SerializeToPreset(presetArchive) == false)
        {
            Logger::Error("Can't create preset from descriptor");
            continue;
        }

        FileSystem::Instance()->CreateDirectory(presets[i].GetDirectory(), true);
        if (Preset::SaveArchive(presetArchive, presets[i]) == false)
        {
            Logger::Error("Can't save preset as %s", presets[i].GetStringValue().c_str());
            continue;
        }
    }
}
} // namespace RETextureDescriptorUtils
} // namespace DAVA
