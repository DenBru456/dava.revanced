

/*
Copyright 2014 Autodesk, Inc.  All rights reserved.
Use of this software is subject to the terms of the Autodesk license agreement
provided at the time of installation or download, or which otherwise
accompanies this software in either electronic or hard copy form.
*/


#ifndef BEASTTARGETENTITY_H
#define BEASTTARGETENTITY_H

#include "beastapitypes.h"
#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

typedef enum {
        ILB_TT_TEXTURE = 0,

        ILB_TT_VERTEX,
} ILBTargetEntityType;


        ILB_DLL_FUNCTION ILBStatus ILBSetBakeUVSet(ILBTargetEntityHandle target,
                ILBConstString uvName);


        ILB_DLL_FUNCTION ILBStatus ILBSetUVTransform(ILBTargetEntityHandle target,
                const ILBVec2* offset,
                const ILBVec2* scale);

        ILB_DLL_FUNCTION ILBStatus ILBSetBakeResolution(ILBTargetEntityHandle target,
                int32 width,
                int32 height);

        ILB_DLL_FUNCTION ILBStatus ILBSetTexelScale(ILBTargetEntityHandle target,
                float scale);

        ILB_DLL_FUNCTION ILBStatus ILBChangeTargetEntityType(ILBTargetEntityHandle entity,
                ILBTargetEntityType type);

        ILB_DLL_FUNCTION ILBStatus ILBGetAtlasInformation(ILBTargetEntityHandle te,
                int32* framebufferIndex,
                ILBVec2* offset,
                ILBVec2* scale);

        ILB_DLL_FUNCTION ILBStatus ILBGetNormalizationData(ILBTargetEntityHandle entity,
                ILBRenderPassHandle pass,
                int channel,
                float* minValue,
                float* maxValue);


        ILB_DLL_FUNCTION ILBStatus ILBGetTargetEntityType(ILBTargetEntityHandle entity,
                ILBTargetEntityType* type);

        ILB_DLL_FUNCTION ILBStatus ILBGetTargetEntityResolution(ILBTargetEntityHandle entity,
                int32* width,
                int32* height);

        ILB_DLL_FUNCTION ILBStatus ILBGetTargetEntityInstance(ILBTargetEntityHandle entity,
                ILBStringHandle* name);

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus


#endif // BEASTTARGETENTITY_H