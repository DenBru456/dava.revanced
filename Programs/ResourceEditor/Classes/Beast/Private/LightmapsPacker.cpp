#if defined(__DAVAENGINE_BEAST__)

#include "Classes/Beast/Private/LightmapsPacker.h"
#include "FileSystem/File.h"
#include "Utils/StringFormat.h"
#include <set>

namespace LightmapsPackerDetails
{
struct PassFolder
{
    const char* folder;
    const char* name;
};

// Beast bakes one illumination pass at a time and ResourcePacker2D mirrors the input
// layout into the output directory, so each pass ends up in its own subfolder holding its
// atlas and the per-instance sprite descriptors:
//
//   <output>/sm/texture0.tex, texture0.dx11.dds, <PointerString>.txt, ...
//   <output>/ao/texture0.tex, texture0.dx11.dds, <PointerString>.txt, ...
//
// The stock implementation scanned <output> itself with a non-recursive FileList, found
// no .txt at all, and handed BeastManager::UpdateAtlas an empty vector, so nothing was
// ever bound and a finished bake changed nothing in the editor. Each pass has to be
// scanned on its own, and the atlas name inside a descriptor is relative to that pass
// folder ("texture0.tex"), so the folder has to be put back in front of it: texture0.tex
// exists in both sm/ and ao/.
static const PassFolder passFolders[] = {
    { "sm/", "surface" },
    { "ao/", "ao" },
};
} // namespace LightmapsPackerDetails

void LightmapsPacker::ParseSpriteDescriptors()
{
    DAVA::UnorderedMap<DAVA::String, DAVA::Vector2> texturesSize;
    std::set<DAVA::String> seenKeys;
    DAVA::int32 duplicates = 0;
    DAVA::String receipt;

    for (const LightmapsPackerDetails::PassFolder& pass : LightmapsPackerDetails::passFolders)
    {
        DAVA::FilePath passDir = outputDir + pass.folder;
        if (!DAVA::FileSystem::Instance()->IsDirectory(passDir))
        {
            DAVA::Logger::Info("[LightmapsPacker] %s: no such folder (%s), pass skipped", pass.name, passDir.GetAbsolutePathname().c_str());
            continue;
        }

        DAVA::FileList* fileList = new DAVA::FileList(passDir);

        DAVA::int32 itemsCount = fileList->GetCount();
        DAVA::int32 parsedInPass = 0;
        for (DAVA::int32 i = 0; i < itemsCount; ++i)
        {
            const DAVA::FilePath& filePath = fileList->GetPathname(i);
            if (fileList->IsDirectory(i) || !filePath.IsEqualToExtension(".txt"))
            {
                continue;
            }

            Beast::LightmapAtlasingData data;

            // the descriptor is named after the mesh instance, which in this module is the
            // pointer string of the DAVA::RenderBatch that produced it
            // (BeastResource::PointerToString), so UpdateAtlas can turn it back into a batch
            data.meshInstanceName = filePath.GetBasename();

            DAVA::File* file = DAVA::File::Create(filePath, DAVA::File::OPEN | DAVA::File::READ);
            if (file == nullptr)
            {
                DAVA::Logger::Error("[LightmapsPacker] cannot open sprite descriptor %s", filePath.GetAbsolutePathname().c_str());
                continue;
            }

            DAVA::char8 buf[512] = {};
            file->ReadLine(buf, sizeof(buf)); // textures count

            DAVA::uint32 readSize = file->ReadLine(buf, sizeof(buf)); // texture name
            DAVA::String textureBasename(buf, readSize);
            while (!textureBasename.empty() && (textureBasename.back() == '\r' || textureBasename.back() == '\n'))
            {
                textureBasename.pop_back();
            }

            DAVA::FilePath originalTextureName = passDir + textureBasename;
            data.textureName = originalTextureName;

            file->ReadLine(buf, sizeof(buf)); // image size
            file->ReadLine(buf, sizeof(buf)); // frames count
            file->ReadLine(buf, sizeof(buf)); // frame rect: x y dx dy ...

            DAVA::int32 x = 0, y = 0, dx = 0, dy = 0;
            sscanf(buf, "%d %d %d %d", &x, &y, &dx, &dy);

            const DAVA::String absoluteFileName = originalTextureName.GetAbsolutePathname();
            if (texturesSize.count(absoluteFileName) == 0)
            {
                texturesSize.emplace(absoluteFileName, GetTextureSize(originalTextureName));
            }

            const DAVA::Vector2 textureSize = texturesSize[absoluteFileName];
            if (textureSize.x <= 0.0f || textureSize.y <= 0.0f)
            {
                // unreadable atlas: a NaN tileset is worse than a missing lightmap
                DAVA::Logger::Error("[LightmapsPacker] atlas %s has no readable size", absoluteFileName.c_str());
                file->Release();
                continue;
            }

            // same maths the engine's own lightmap loader uses for packed sprites:
            // uv = uv * uvScale + uvOffset, in texels over atlas pixels
            data.uvOffset = DAVA::Vector2(static_cast<DAVA::float32>(x) / textureSize.x, static_cast<DAVA::float32>(y) / textureSize.y);
            data.uvScale = DAVA::Vector2(static_cast<DAVA::float32>(dx) / textureSize.x, static_cast<DAVA::float32>(dy) / textureSize.y);

            file->Release();

            // A descriptor is the only record of where this tile lives in the atlas, so it must
            // survive. Deleting it (which this did) meant that a resumed run repacked nothing,
            // UpdateAtlas saw "no atlasing entries", and the shipped scene kept sampling pages
            // that no longer matched. Dedupe by instance key instead of deleting: a stale
            // descriptor from an earlier pass has the same key and is simply skipped.
            if (seenKeys.find(data.meshInstanceName) != seenKeys.end())
            {
                ++duplicates;
                continue;
            }
            seenKeys.insert(data.meshInstanceName);

            atlasingData.push_back(data);
            ++parsedInPass;
            receipt += DAVA::Format("%s;%s;%s;%d;%d;%d;%d\n", pass.folder, data.meshInstanceName.c_str(), absoluteFileName.c_str(), x, y, dx, dy);
        }

        fileList->Release();

        if (duplicates > 0)
        {
            DAVA::Logger::Info("[LightmapsPacker] %s: %d stale descriptors skipped (same instance key already parsed)", pass.name, duplicates);
        }
        DAVA::Logger::Info("[LightmapsPacker] %s: %d sprite descriptors from %s", pass.name, parsedInPass, passDir.GetAbsolutePathname().c_str());
    }

    // A plain text receipt of every tile we parsed. The descriptors next to the pages are
    // Beast's own output and can be regenerated or lost; this line per tile is what lets a
    // finished run be re-bound without touching the renderer again.
    if (!receipt.empty())
    {
        DAVA::FilePath receiptPath = outputDir + "atlas-descriptors.csv";
        DAVA::File* receiptFile = DAVA::File::Create(receiptPath, DAVA::File::WRITE | DAVA::File::CREATE);
        if (receiptFile != nullptr)
        {
            receiptFile->Write(receipt.c_str(), static_cast<DAVA::uint32>(receipt.size()));
            receiptFile->Release();
            DAVA::Logger::Info("[LightmapsPacker] repack receipt: %s", receiptPath.GetAbsolutePathname().c_str());
        }
    }

    if (atlasingData.empty())
    {
        DAVA::Logger::Error("[LightmapsPacker] no sprite descriptors under %s, nothing can be bound", outputDir.GetAbsolutePathname().c_str());
    }
}

DAVA::Vector2 LightmapsPacker::GetTextureSize(const DAVA::FilePath& filePath)
{
    DAVA::Vector2 ret;

    DAVA::FilePath sourceTexturePathname = DAVA::FilePath::CreateWithNewExtension(filePath, DAVA::TextureDescriptor::GetLightmapTextureExtension());

    DAVA::ScopedPtr<DAVA::Image> image(DAVA::ImageSystem::LoadSingleMip(sourceTexturePathname));
    if (image)
    {
        ret.x = static_cast<DAVA::float32>(image->GetWidth());
        ret.y = static_cast<DAVA::float32>(image->GetHeight());
    }

    return ret;
}

DAVA::Vector<Beast::LightmapAtlasingData>* LightmapsPacker::GetAtlasingData()
{
    return &atlasingData;
}

void LightmapsPacker::CreateDescriptors()
{
    // atlases are written into the per-pass folders and ResourcePacker2D writes their .tex
    // beside them, so only loose pngs at the top level still need a descriptor
    DAVA::FileList* fileList = new DAVA::FileList(outputDir);

    DAVA::int32 itemsCount = fileList->GetCount();
    for (DAVA::int32 i = 0; i < itemsCount; ++i)
    {
        const DAVA::FilePath& filePath = fileList->GetPathname(i);
        if (fileList->IsDirectory(i) || !filePath.IsEqualToExtension(".png"))
        {
            continue;
        }

        DAVA::TextureDescriptor* descriptor = new DAVA::TextureDescriptor();
        descriptor->Save(DAVA::TextureDescriptor::GetDescriptorPathname(filePath));
        delete descriptor;
    }

    fileList->Release();
}

#endif //#if defined(__DAVAENGINE_BEAST__)
