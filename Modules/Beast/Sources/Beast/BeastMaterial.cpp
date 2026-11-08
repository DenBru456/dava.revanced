#include "Beast/BeastMaterial.h"
#include "Beast/BeastTexture.h"
#include "Beast/BeastDebug.h"
#include "Beast/SceneParser.h"
#include <beastapi/beastmaterial.h>
#include <Render/Material/NMaterial.h>
#include <Math/MathDefines.h>

namespace Beast
{
BeastMaterial::BeastMaterial(const DAVA::String& name, BeastManager* manager)
    : BeastResource(name, manager)
    ,
    material(0)
{
}

void BeastMaterial::InitWithTextureAndDavaMaterial(BeastTexture* beastTexture, DAVA::NMaterial* davaMaterial)
{
    if (0 == material)
    {
        BEAST_VERIFY(ILBCreateMaterial(manager->GetILBScene(), STRING_TO_BEAST_STRING(resourceName), &material));

        if (beastTexture)
        {
            BEAST_VERIFY(ILBSetMaterialTexture(material, ILB_CC_DIFFUSE, beastTexture->GetILBTexture()));
            // Blitz materials tile their albedo through a uvScale material param, not
            // through the UVs, so Beast has to be told the same factor or the texture
            // is stretched once across the whole surface.
            BEAST_VERIFY(ILBSetMaterialTextureWrapMode(material, ILB_CC_DIFFUSE, ILB_WM_REPEAT, ILB_WM_REPEAT,
                                                       beastTexture->GetILBTexture()));
            const DAVA::float32* uvScale = (davaMaterial != nullptr) ? davaMaterial->GetEffectivePropValue(DAVA::FastName("uvScale")) : nullptr;
            if (uvScale != nullptr && uvScale[0] > 0.0f && !FLOAT_EQUAL(uvScale[0], 1.0f))
            {
                BEAST_VERIFY(ILBSetMaterialScale(material, ILB_CC_DIFFUSE, uvScale[0]));
            }
        }
        else
        {
            // No albedo at all: Beast would fill the surface with its checkerboard,
            // which is what those black/grey rocks were. A flat colour from the
            // material (or mid grey) reads as untextured, not as an error.
            ILBLinearRGBA flat(0.5f, 0.5f, 0.5f, 1.0f);
            const DAVA::float32* flatColor = (davaMaterial != nullptr) ? davaMaterial->GetEffectivePropValue(DAVA::FastName("flatColor")) : nullptr;
            if (flatColor != nullptr)
            {
                flat = ILBLinearRGBA(flatColor[0], flatColor[1], flatColor[2], flatColor[3]);
            }
            BEAST_VERIFY(ILBSetMaterialColor(material, ILB_CC_DIFFUSE, &flat));
        }

        BEAST_VERIFY(ILBSetChannelUVLayer(material, ILB_CC_DIFFUSE, CONST_STRING_TO_BEAST_STRING("0")));

        if (SceneParser::IsMaterialTemplateContainString(davaMaterial, "Alphatest")
            || SceneParser::IsMaterialTemplateContainString(davaMaterial, "Alphablend")
            || SceneParser::IsMaterialTemplateContainString(davaMaterial, "SpeedTreeLeaf"))
        {
            BEAST_VERIFY(ILBSetAlphaAsTransparency(material, true));
        }
    }
}

void BeastMaterial::AttachNormalMap(BeastTexture* beastTexture)
{
    if (material)
    {
        BEAST_VERIFY(ILBSetMaterialTexture(material, ILB_CC_NORMAL, beastTexture->GetILBTexture()));
        BEAST_VERIFY(ILBSetChannelUVLayer(material, ILB_CC_NORMAL, CONST_STRING_TO_BEAST_STRING("2")));
    }
}
}
