#include "Beast/BeastTexture.h"
#include "Beast/BeastDebug.h"
#include "Beast/BeastManager.h"

#include <Render/Texture.h>
#include <Render/TextureDescriptor.h>
#include <Render/Image/Image.h>
#include <Logger/Logger.h>
#include <cstddef>
#include <cstring>

namespace Beast
{
BeastTexture::BeastTexture(const DAVA::String& name, BeastManager* manager)
    : BeastResource(name, manager)
{
}

void BeastTexture::InitWithTexture(DAVA::Texture* davaTexture)
{
    DVASSERT(nullptr != davaTexture);
    InitWithFile(davaTexture->GetDescriptor()->GetSourceTexturePathname());
}

void BeastTexture::InitWithFile(const DAVA::FilePath& filePath)
{
    if (nullptr == texture)
    {
        DVASSERT(!filePath.IsEmpty());
        BEAST_VERIFY(ILBReferenceTexture(manager->GetILBManager(), STRING_TO_BEAST_STRING(resourceName),
                                         STRING_TO_BEAST_STRING(filePath.GetAbsolutePathname()), &texture));
    }
}

bool BeastTexture::InitWithPixels(const DAVA::Image* image)
{
    if (texture != nullptr)
    {
        return true;
    }
    if (image == nullptr || image->data == nullptr || image->width == 0 || image->height == 0)
    {
        return false;
    }
    if (image->format != DAVA::FORMAT_RGBA8888)
    {
        DAVA::Logger::Warning("[BeastTexture] '%s' must be RGBA8888 to upload, got format %d",
                              resourceName.c_str(), static_cast<DAVA::int32>(image->format));
        return false;
    }

    // Textures are remembered per manager by name, so another batch using the same
    // material must not create it a second time.
    ILBTextureHandle cached = nullptr;
    if ((ILBFindTexture(manager->GetILBManager(), STRING_TO_BEAST_STRING(resourceName), &cached) == ILB_ST_SUCCESS) && (cached != nullptr))
    {
        texture = cached;
        return true;
    }

    const DAVA::int32 width = static_cast<DAVA::int32>(image->width);
    const DAVA::int32 height = static_cast<DAVA::int32>(image->height);
    const ILBStatus began = ILBBeginTexture(manager->GetILBManager(), STRING_TO_BEAST_STRING(resourceName),
                                           width, height, ILB_PF_RGBA_BYTE, &texture);
    if (began != ILB_ST_SUCCESS || texture == nullptr)
    {
        DAVA::Logger::Warning("[BeastTexture] ILBBeginTexture failed for '%s' (%dx%d)", resourceName.c_str(), width, height);
        texture = nullptr;
        return false;
    }

    // ILBAddPixelDataLDR wants the lines from the bottom up, left to right. The
    // engine's images are top-down, so the rows are reversed once here instead of
    // being sent a row at a time.
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    DAVA::Vector<DAVA::uint8> flipped(rowBytes * static_cast<size_t>(height));
    const DAVA::uint8* source = static_cast<const DAVA::uint8*>(image->data);
    for (DAVA::int32 row = 0; row < height; ++row)
    {
        const DAVA::uint8* sourceRow = source + rowBytes * static_cast<size_t>(height - 1 - row);
        std::memcpy(&flipped[rowBytes * static_cast<size_t>(row)], sourceRow, rowBytes);
    }

    // BEAST_VERIFY is a statement block, not an expression, so the status is
    // checked here instead of wrapped in an if.
    const ILBStatus added = ILBAddPixelDataLDR(texture, flipped.data(), width * height);
    if (added != ILB_ST_SUCCESS)
    {
        DAVA::Logger::Warning("[BeastTexture] ILBAddPixelDataLDR failed (%d) for '%s' (%dx%d)",
                              static_cast<DAVA::int32>(added), resourceName.c_str(), width, height);
        texture = nullptr;
        return false;
    }
    const ILBStatus ended = ILBEndTexture(texture);
    if (ended != ILB_ST_SUCCESS)
    {
        DAVA::Logger::Warning("[BeastTexture] ILBEndTexture failed (%d) for '%s'", static_cast<DAVA::int32>(ended), resourceName.c_str());
        texture = nullptr;
        return false;
    }
    return true;
}

ILBTextureHandle BeastTexture::GetILBTexture()
{
    return texture;
}
}
