#pragma once

#include "Beast/BeastTypes.h"
#include "Beast/BeastResource.h"

namespace DAVA
{
class Texture;
class Image;
class FilePath;
}

namespace Beast
{
class BeastTexture : public BeastResource<BeastTexture>
{
public:
    void InitWithTexture(DAVA::Texture* davaTexture);
    void InitWithFile(const DAVA::FilePath& filePath);
    // Upload decoded pixels straight into Beast. The game ships .tex packed for a
    // GPU, not .png, so any file handed to Beast is one we encoded ourselves first:
    // ILBBeginTexture + ILBAddPixelDataLDR skips that round trip entirely.
    // Image must be FORMAT_RGBA8888. Returns false if Beast refused it.
    bool InitWithPixels(const DAVA::Image* image);
    ILBTextureHandle GetILBTexture();

private:
    BeastTexture(const DAVA::String& name, BeastManager* manager);
    ILBTextureHandle texture = nullptr;

    friend class BeastResource<BeastTexture>;
};
}
