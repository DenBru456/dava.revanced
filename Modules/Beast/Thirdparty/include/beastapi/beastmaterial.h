

/*
Copyright 2014 Autodesk, Inc.  All rights reserved.
Use of this software is subject to the terms of the Autodesk license agreement
provided at the time of installation or download, or which otherwise
accompanies this software in either electronic or hard copy form.
*/

#ifndef BEASTMATERIAL_H
#define BEASTMATERIAL_H
#include "beastapitypes.h"

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

typedef enum {
        ILB_CC_DIFFUSE = 0,
        ILB_CC_SPECULAR,
        ILB_CC_EMISSIVE,
        ILB_CC_TRANSPARENCY,
        ILB_CC_SHININESS,
        ILB_CC_REFLECTION,
        ILB_CC_NONDIRECTIONAL,
        ILB_CC_NORMAL,
        ILB_CC_TOTAL_CHANNELS
} ILBMaterialChannel;

typedef enum {
        ILB_WM_REPEAT = 0,
        ILB_WM_MIRROR,
        ILB_WM_CLAMP,
        ILB_WM_TOTAL_MODES
} ILBMaterialChannelWrapMode;

ILB_DLL_FUNCTION ILBStatus ILBCreateMaterial(ILBSceneHandle scene,
        ILBConstString name,
        ILBMaterialHandle* material);

ILB_DLL_FUNCTION ILBStatus ILBFindMaterial(ILBSceneHandle scene,
                                                                         ILBConstString name,
                                                                         ILBMaterialHandle* material);

ILB_DLL_FUNCTION ILBStatus ILBSetMaterialColor(ILBMaterialHandle material,
                                                                         ILBMaterialChannel channel,
                                                                         const ILBLinearRGBA* color);

ILB_DLL_FUNCTION ILBStatus ILBSetMaterialScale(ILBMaterialHandle material,
                                                                                           ILBMaterialChannel channel,
                                                                                           float scale);

ILB_DLL_FUNCTION ILBStatus ILBSetMaterialTexture(ILBMaterialHandle material,
                                                                           ILBMaterialChannel channel,
                                                                           ILBTextureHandle texture);
ILB_DLL_FUNCTION ILBStatus ILBSetMaterialTextureWrapMode(ILBMaterialHandle material,
        ILBMaterialChannel channel,
        ILBMaterialChannelWrapMode wrapModeU,
        ILBMaterialChannelWrapMode wrapModeV,
        ILBTextureHandle texture);


ILB_DLL_FUNCTION ILBStatus ILBSetMaterialUseVertexColors(ILBMaterialHandle material,
                                                                                           ILBMaterialChannel channel);

ILB_DLL_FUNCTION ILBStatus ILBSetChannelUVLayer(ILBMaterialHandle material,
                                                                                ILBMaterialChannel channel,
                                                                                ILBConstString uvLayerName);

ILB_DLL_FUNCTION ILBStatus ILBSetAlphaAsTransparency(ILBMaterialHandle material,
                                                                           ILBBool alphaAsTransparency);


ILB_DLL_FUNCTION ILBStatus ILBSetPrimaryGICorrection(ILBMaterialHandle material,
                                                                           float intensity,
                                                                           float saturation);

ILB_DLL_FUNCTION ILBStatus ILBSetSecondaryGICorrection(ILBMaterialHandle material,
                                                                           float intensity,
                                                                           float saturation);


ILB_DLL_FUNCTION ILBStatus ILBSetGIScale(ILBMaterialHandle material,
                                                                           float diffuseBoost,
                                                                           float emissiveScale,
                                                                           float specularScale);

ILB_DLL_FUNCTION ILBStatus ILBSetMaterialDisplayName(ILBMaterialHandle material,
                                                                                         ILBConstString displayName);



ILB_DLL_FUNCTION ILBStatus ILBCreateShader(ILBManagerHandle beastManager,
                                                                                        ILBConstString uniqueName,
                                                                                        ILBConstString fileName,
                                                                                        ILBShaderHandle* target);

ILB_DLL_FUNCTION ILBStatus ILBFindShader(ILBManagerHandle beastManager,
        ILBConstString uniqueName,
        ILBShaderHandle* target);

ILB_DLL_FUNCTION ILBStatus ILBSetShader(ILBMaterialHandle material,
                                                                                ILBShaderHandle shader);

ILB_DLL_FUNCTION ILBStatus ILBSetShaderParamFloat(ILBMaterialHandle material,
                                                                                                        ILBConstString param,
                                                                                                        float value);

ILB_DLL_FUNCTION ILBStatus ILBSetShaderParamInt(ILBMaterialHandle material,
                                                                                ILBConstString param,
                                                                                int32 value);

ILB_DLL_FUNCTION ILBStatus ILBSetShaderParamColor(ILBMaterialHandle material,
                                                                                                        ILBConstString param,
                                                                                                        ILBLinearRGB* value);

ILB_DLL_FUNCTION ILBStatus ILBSetShaderParamTexture(ILBMaterialHandle material,
                                                                                                                ILBConstString param,
                                                                                                                ILBTextureHandle value);

ILB_DLL_FUNCTION ILBStatus ILBSetShaderParamUV(ILBMaterialHandle material,
                                                                                                                ILBConstString param,
                                                                                                                ILBConstString value);


#ifdef __cplusplus
}
#endif // __cplusplus


#endif // BEASTMATERIAL_H
