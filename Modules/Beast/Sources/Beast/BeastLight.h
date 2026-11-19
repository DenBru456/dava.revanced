#pragma once

#include "Beast/BeastTypes.h"
#include "Beast/BeastResource.h"
#include "Beast/BeastManager.h"

#include <Base/BaseTypes.h>

namespace DAVA
{
class Entity;
class Light;
}

namespace Beast
{
class BeastLight : public BeastResource<BeastLight>
{
public:
    void InitWithLight(DAVA::Entity* node, DAVA::Light* lightObject);
    void UpdateLightParamsFromHandle(ILBLightHandle lightHandle);

    ILBLightHandle GetILBLight();

    // One shadow, one light. A shipped scene enables every light it contains for the static bake and
    // carries no editor.staticlight.castshadows at all, so Beast's default (true) makes all of them
    // cast: two directional suns draw two shadow sets over the same wall, and a sky light traces the
    // hemisphere and darkens every recess from every direction. Only the strongest static directional
    // keeps its shadows; sky and ambient never cast, and they are never even created as tracing lights.
    static void ApplySolitaryShadowPolicy();

    // KeyedArchive::GetFloat DVASSERTs on "type == TYPE_FLOAT" instead of coercing, so a property the
    // editor happened to save as a bool or an int takes the whole bake down with a dialog. Scene values
    // are authored data; read the variant, convert, fall back.
    static DAVA::float32 ReadFloatProperty(DAVA::Entity* node, const DAVA::String& key, DAVA::float32 defaultValue);

private:
    friend class BeastResource<BeastLight>;
    BeastLight(const DAVA::String& name, BeastManager* manager);

    void CreateSkyLight();
    void CreateDirectionalLight();
    void CreateSpotLight();
    void CreatePointLight();
    void CreateAmbientLight();
    void SetIntensity();
    bool GetCastShadows();
    bool IsDynamicLight();
    bool IsStaticLightEnabled();
    DAVA::Color GetAmbientColor();
    DAVA::float32 GetShadowPriority();
    void SetShadowParameters();

    const ILBLinearRGB& GetColor() const;
    const ILBMatrix4x4& GetMatrix() const;
    DAVA::int32 GetShadowSamples();
    DAVA::float32 GetShadowAngle();
    DAVA::float32 GetShadowRadius();
    DAVA::float32 GetFalloffCutoff();
    DAVA::float32 GetFalloffExponent();
    DAVA::float32 GetConeAngle();
    DAVA::float32 GetConePenumbraAngle();
    DAVA::float32 GetConePenumbraExponent();

    DAVA::float32 GetFloat(const DAVA::String& key, DAVA::float32 defaultValue);

private:
    ILBLightHandle light = nullptr;
    DAVA::Light* lightObject = nullptr;
    DAVA::Entity* ownerNode = nullptr;
    ILBMatrix4x4 matrix;
    ILBLinearRGB linearRGB;
};
}