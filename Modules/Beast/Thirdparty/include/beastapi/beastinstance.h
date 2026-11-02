/*
Copyright 2014 Autodesk, Inc.  All rights reserved.
Use of this software is subject to the terms of the Autodesk license agreement
provided at the time of installation or download, or which otherwise 
accompanies this software in either electronic or hard copy form.
*/

/** \file 
 * The instance definitions
 * Reconstructed from the Beast 2015 documentation (beastinstance_8h.html):
 * the header was never published with the dava.engine repository - the
 * Thirdparty/include/beastapi directory is an incomplete subset of the SDK.
 * Enum values and signatures below are the exact documented ones.
 */
#ifndef BEASTINSTANCE_H
#define BEASTINSTANCE_H

#include "beastapitypes.h"
#include "beastlightsource.h"

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

typedef uint32 ILBRenderStatsMask;

/**
 * Selects if the render stats should be enabled or disabled.
 */
typedef enum {
    ILB_RSOP_DISABLE = 0,
    ILB_RSOP_ENABLE
} ILBRenderStatOperation;

/**
 * Beast render stats. Combine with the or operator as an ILBRenderStatsMask.
 */
typedef enum {
    ILB_RS_VISIBLE_FOR_EYE = 0x00000001,
    ILB_RS_VISIBLE_FOR_REFLECTIONS = 0x00000002,
    ILB_RS_VISIBLE_FOR_REFRACTIONS = 0x00000004,
    ILB_RS_VISIBLE_FOR_GI = 0x00000008,

    ILB_RS_CAST_SHADOWS = 0x00000010,
    ILB_RS_RECEIVE_SHADOWS = 0x00000020,

    ILB_RS_CAST_GI = 0x00000040,
    ILB_RS_RECEIVE_GI = 0x00000080,

    ILB_RS_CAST_OCCLUSION = 0x00000100,
    ILB_RS_RECEIVE_OCCLUSION = 0x00000200,

    ILB_RS_DOUBLE_SIDED = 0x00000400,
    ILB_RS_OPPOSITE = 0x00000800,

    ILB_RS_SELF_OCCLUSION = 0x00001000,
    ILB_RS_SHADOW_BIAS = 0x00002000,

    ILB_RS_PRIMARY_VISIBILITY = ILB_RS_VISIBLE_FOR_EYE,
    ILB_RS_VISIBLE_IN_REFLECTIONS = ILB_RS_VISIBLE_FOR_REFLECTIONS,
    ILB_RS_VISIBLE_IN_REFRACTIONS = ILB_RS_VISIBLE_FOR_REFRACTIONS,
    ILB_RS_VISIBLE_IN_FINAL_GATHER = ILB_RS_VISIBLE_FOR_GI
} ILBRenderStats;

ILB_DLL_FUNCTION ILBStatus ILBAddInstanceLightLinks(ILBInstanceHandle instance,
        ILBLightLinkMode mode,
        const ILBLightHandle* lightSources,
        int32 count);

ILB_DLL_FUNCTION ILBStatus ILBAddLODInstance(ILBInstanceHandle lowRes,
        ILBInstanceHandle highRes);

ILB_DLL_FUNCTION ILBStatus ILBCreateInstance(ILBSceneHandle scene,
        ILBMeshHandle mesh,
        ILBConstString name,
        const ILBMatrix4x4* transform,
        ILBInstanceHandle* instance);

ILB_DLL_FUNCTION ILBStatus ILBDeleteInstance(ILBInstanceHandle instance);

ILB_DLL_FUNCTION ILBStatus ILBSetInstanceDisplayName(ILBInstanceHandle instance,
        ILBConstString displayName);

ILB_DLL_FUNCTION ILBStatus ILBSetInstanceMesh(ILBInstanceHandle instance,
        ILBMeshHandle mesh);

ILB_DLL_FUNCTION ILBStatus ILBSetInstanceMotionTransforms(ILBInstanceHandle instance,
        const ILBMatrix4x4* transforms,
        int32 count);

ILB_DLL_FUNCTION ILBStatus ILBSetInstanceTransform(ILBInstanceHandle instance,
        const ILBMatrix4x4* transform);

ILB_DLL_FUNCTION ILBStatus ILBSetMaterialOverrides(ILBInstanceHandle instance,
        ILBMaterialHandle* materials,
        int32 materialCount);

ILB_DLL_FUNCTION ILBStatus ILBSetRenderStats(ILBInstanceHandle instance,
        ILBRenderStatsMask stats,
        ILBRenderStatOperation operation);

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus

#endif // BEASTINSTANCE_H
