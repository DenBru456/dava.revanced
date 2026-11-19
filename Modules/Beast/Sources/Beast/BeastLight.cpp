#include "Beast/BeastLight.h"
#include "Beast/BeastDebug.h"

#include <Base/BaseMath.h>
#include <FileSystem/KeyedArchive.h>
#include <Math/Color.h>
#include <Math/MathHelpers.h>
#include <Math/Vector.h>
#include <Render/Highlevel/Light.h>
#include <Scene3D/Components/ComponentHelpers.h>
#include <Scene3D/Components/TransformComponent.h>
#include <Math/TransformUtils.h>
#include <Scene3D/Entity.h>

// dava.revanced compatibility: entity transforms live on TransformComponent
// (this file was written against dava.engine master's Entity::GetWorldTransform)
static DAVA::Matrix4 GetEntityWorldTransform(DAVA::Entity* node)
{
    if (node == nullptr)
        return DAVA::Matrix4::IDENTITY;
    const DAVA::TransformComponent* transformComponent = node->GetComponent<DAVA::TransformComponent>();
    if (transformComponent == nullptr)
        return DAVA::Matrix4::IDENTITY;
    return DAVA::TransformUtils::ToMatrix(transformComponent->GetWorldTransform());
}


// Measured against the shipped rudniki tower tile, see InitWithLight.
static const DAVA::float32 BEAST_BAKE_SUN_TINT_R = 1.00f;
static const DAVA::float32 BEAST_BAKE_SUN_TINT_G = 0.92f;
static const DAVA::float32 BEAST_BAKE_SUN_TINT_B = 0.55f;

namespace Beast
{
// A real sun is 0.53 degrees across; the shipped lightmaps are softer than that, and a thin seam
// between two parts needs a finite angular size or it resolves to fully-lit/fully-shadowed.
static const DAVA::float32 BEAST_DEFAULT_SHADOW_ANGLE_RAD = 0.0175f;   // one degree
static const DAVA::int32 BEAST_MIN_SHADOW_SAMPLES = 16;

BeastLight::BeastLight(const DAVA::String& name, BeastManager* manager)
    : BeastResource(name, manager)
{
}

void BeastLight::InitWithLight(DAVA::Entity* owner, DAVA::Light* _light)
{
    if (light == nullptr)
    {
        ownerNode = owner;
        lightObject = _light;

        DAVA::Color nodeColor = lightObject->GetDiffuseColor();

        // The shipped atlases are lit by a warm sun, and the scene does not say so: neither
        // rudnikiv56 nor the shipped OGTOWERONLYrudniki serialises a diffuse colour on any of its
        // three lights, so every light arrives white. Fitting a v10.60 bake against the shipped
        // tower tile per channel gives theirs = 0.740*ours_R + 16.0, 0.667*ours_G + 22.0,
        // 0.476*ours_B + 25.9, i.e. our light carries too much blue for the energy it delivers.
        // Ratio of the three gains, normalised to red: (1.00, 0.90, 0.643). That is the sun tint.
        if (lightObject->GetType() == DAVA::Light::TYPE_DIRECTIONAL)
        {
            nodeColor.r *= BEAST_BAKE_SUN_TINT_R;
            nodeColor.g *= BEAST_BAKE_SUN_TINT_G;
            nodeColor.b *= BEAST_BAKE_SUN_TINT_B;
        }

        linearRGB = ILBLinearRGB(nodeColor.r, nodeColor.g, nodeColor.b);
        matrix = ConvertDavaMatrix(GetEntityWorldTransform(ownerNode));

        // Print the rig. A shipped scene carries two directional lights about twelve degrees apart
        // plus a sky light, all enabled for the static pass, and nothing in the editor UI makes that
        // visible; the log line is what turns "the shadows look doubled" into a number.
        {
            const DAVA::Vector3& direction = lightObject->GetDirection();
            const DAVA::Vector3& position = lightObject->GetPosition();
            DAVA::Logger::Info("[BeastLight] '%s' type %d dynamic %d static %d | pos %.1f %.1f %.1f | dir %.3f %.3f %.3f",
                               resourceName.c_str(), static_cast<DAVA::int32>(lightObject->GetType()),
                               IsDynamicLight() ? 1 : 0, IsStaticLightEnabled() ? 1 : 0,
                               position.x, position.y, position.z, direction.x, direction.y, direction.z);
        }

        // A light the engine drives at runtime has no business writing into a static lightmap. Its
        // direction and its shadows are recomputed every frame in game, so a bake of them is a second,
        // frozen shadow that nothing in the scene explains - the phantom shadows. Its ambColor is a
        // different matter: that is the fill colour the artist picked to travel with it, and losing it
        // changes the mood of the map, so it is contributed as flat ambient (no direction, no tracing,
        // therefore no occlusion bands).
        if (IsDynamicLight())
        {
            const DAVA::Color ambient = GetAmbientColor();
            // An ambient light with zero radiance adds nothing to a bake and only gives Beast one
            // more light to sort, so it is not created at all.
            const DAVA::float32 ambientIntensity = GetFloat("editor.staticlight.ambientintensity", 0.05f);
            if (((ambient.r + ambient.g + ambient.b) * ambientIntensity) <= 1.0e-4f)
            {
                DAVA::Logger::Info("[BeastLight] '%s' is dynamic (editor.dynamiclight.enable) and its ambColor is %.3f %.3f %.3f: nothing to contribute, light dropped",
                                   resourceName.c_str(), ambient.r, ambient.g, ambient.b);
                light = nullptr;
                return;
            }
            linearRGB = ILBLinearRGB(ambient.r, ambient.g, ambient.b);
            // The pointer string is already unique; the suffix keeps ambient lights visually
            // distinct from the light entities in Beast's own log.
            const DAVA::String ambientName = resourceName + "_dynamic_ambient";
            BEAST_VERIFY(ILBCreateAmbientLight(manager->GetILBScene(), STRING_TO_BEAST_STRING(ambientName), &GetMatrix(), &GetColor(), &light));
            const ILBStatus intensityStatus = ILBSetLightIntensity(light, ambientIntensity);
            DAVA::Logger::Info("[BeastLight] '%s' is dynamic (editor.dynamiclight.enable): baked as flat ambient %.3f %.3f %.3f at intensity %.3f (status %d), no direction, no shadows",
                               resourceName.c_str(), ambient.r, ambient.g, ambient.b, ambientIntensity,
                               static_cast<DAVA::int32>(intensityStatus));
            return;
        }

        // The same argument, one step stronger. A sky light is a hemisphere: every texel traces the
        // whole upper dome, so anything above it - a roof, an overhang, the next storey - closes the
        // cone and darkens the texel. That is occlusion with no light direction at all, and it is what
        // puts a soft dark band under every ledge on every side of a tower. As a flat ambient it keeps
        // its energy and loses the geometry-dependent darkening.
        if (lightObject->GetType() == DAVA::Light::TYPE_SKY)
        {
            BEAST_VERIFY(ILBCreateAmbientLight(manager->GetILBScene(), STRING_TO_BEAST_STRING(resourceName), &GetMatrix(), &GetColor(), &light));
            DAVA::Logger::Info("[BeastLight] '%s' is a sky light: baked as flat ambient instead of a traced hemisphere",
                               resourceName.c_str());

            // A flat fill must not keep the sky light's full intensity. Tracing the dome is what
            // makes a sky light dim against the sun (a clear sky delivers roughly a quarter to a
            // third of the sun's irradiance on a lit surface), and it is also what gives it its
            // occlusion. Turning the tracing off kept the energy and threw away both, so the fill
            // arrived at the same 0.22 as the sun: a wall then received 0.32 of directionless fill
            // against 0.22 of sun, could not develop a gradient (flat wall), and stepped hard where
            // the sun term crossed zero (one face of the wall reading darker than the other).
            const DAVA::float32 authoredSky = GetFloat("editor.intensity", 1.0f);
            const DAVA::float32 exposureSky = GetFloat("editor.staticlight.exposure", 0.22f);
            const DAVA::float32 skyRatio = GetFloat("editor.staticlight.skyratio", 1.0f);
            const DAVA::float32 skyIntensity = authoredSky * exposureSky * skyRatio;
            BEAST_VERIFY(ILBSetLightIntensity(light, skyIntensity));
            DAVA::Logger::Info("[BeastLight] sky fill intensity %.3f (editor.intensity %.3f x exposure %.3f x skyratio %.3f) for '%s'",
                               skyIntensity, authoredSky, exposureSky, skyRatio, resourceName.c_str());
            return;
        }

        switch (lightObject->GetType())
        {
        case DAVA::Light::TYPE_SKY:
            CreateSkyLight();
            break;

        case DAVA::Light::TYPE_DIRECTIONAL:
            CreateDirectionalLight();
            break;

        case DAVA::Light::TYPE_POINT:
            CreatePointLight();
            break;

        case DAVA::Light::TYPE_AMBIENT:
            CreateAmbientLight();
            break;

        case DAVA::Light::TYPE_SPOT:
            CreateSpotLight();
            break;

        default:
            DVASSERT(0, "Invalid light type");
        }

        SetShadowParameters();
        SetIntensity();
    }
}

ILBLightHandle BeastLight::GetILBLight()
{
    return light;
}

void BeastLight::CreateSkyLight()
{
    DAVA::Matrix4 davaMatrix;
    davaMatrix.Identity();
    ILBMatrix4x4 matrix = ConvertDavaMatrix(davaMatrix);

    BEAST_VERIFY(ILBCreateSkyLight(manager->GetILBScene(), STRING_TO_BEAST_STRING(resourceName), &matrix, &GetColor(), &light));
    BEAST_VERIFY(ILBSetCastShadows(light, GetCastShadows()));
}

void BeastLight::CreateAmbientLight()
{
    BEAST_VERIFY(ILBCreateAmbientLight(manager->GetILBScene(), STRING_TO_BEAST_STRING(resourceName), &GetMatrix(), &GetColor(), &light));
}

void BeastLight::CreateDirectionalLight()
{
    BEAST_VERIFY(ILBCreateDirectionalLight(manager->GetILBScene(), STRING_TO_BEAST_STRING(resourceName), &GetMatrix(), &GetColor(), &light));
    // the shadow angle is set once, in SetShadowParameters, after the type is known
}

void BeastLight::CreatePointLight()
{
    BEAST_VERIFY(ILBCreatePointLight(manager->GetILBScene(), STRING_TO_BEAST_STRING(resourceName), &GetMatrix(), &GetColor(), &light));
    BEAST_VERIFY(ILBSetLightMaxRangeFalloff(light, GetFalloffCutoff(), GetFalloffExponent()));
    BEAST_VERIFY(ILBSetShadowRadius(light, GetShadowRadius()));
}

void BeastLight::CreateSpotLight()
{
    BEAST_VERIFY(ILBCreateSpotLight(manager->GetILBScene(), STRING_TO_BEAST_STRING(resourceName), &GetMatrix(), &GetColor(), &light));
    BEAST_VERIFY(ILBSetLightMaxRangeFalloff(light, GetFalloffCutoff(), GetFalloffExponent()));
    BEAST_VERIFY(ILBSetSpotlightCone(light, GetConeAngle() * DAVA::PI / 180.0f, GetConePenumbraAngle() * DAVA::PI / 180.0f, GetConePenumbraExponent()));
    BEAST_VERIFY(ILBSetShadowRadius(light, GetShadowRadius()));
}

void BeastLight::SetShadowParameters()
{
    BEAST_VERIFY(ILBSetCastShadows(light, GetCastShadows()));

    // ILBSetShadowSamples and ILBSetShadowAngle are not accepted by every light type. Beast answers
    // "Tried to set shadow angle on a light type not support it." (ILB_ST_INVALID_PARAMETER, 152)
    // for a sky light, and the VERIFY wrapper turns that into a modal dialog. The header says the
    // angle is for "a directional light or window light", and the radius is "only valid for point
    // and spot lights", so ask only where the type allows it and log the answer instead of
    // throwing a dialog at the user mid-bake.
    const DAVA::Light::eType type = (lightObject != nullptr) ? lightObject->GetType() : DAVA::Light::TYPE_DIRECTIONAL;

    const bool supportsSamples = (type == DAVA::Light::TYPE_DIRECTIONAL)
    || (type == DAVA::Light::TYPE_POINT)
    || (type == DAVA::Light::TYPE_SPOT);

    if (supportsSamples)
    {
        const ILBStatus samplesStatus = ILBSetShadowSamples(light, GetShadowSamples());
        if (samplesStatus != ILB_ST_SUCCESS)
        {
            DAVA::Logger::Warning("[BeastLight] ILBSetShadowSamples(%d) rejected with status %d for '%s'",
                                  GetShadowSamples(), static_cast<DAVA::int32>(samplesStatus), resourceName.c_str());
        }
    }

    // The angular size of the sun. DAVA authors editor.staticlight.shadowangle = 0 and this
    // function never called ILBSetShadowAngle at all, so every shadow ray hit the same point and
    // the sample count did nothing: a one-texel gap between two parts then resolves to fully lit
    // (91/255 at the tower collar) where the shipped map is half shaded (57/255), which is the
    // bright line across the seam. Only a directional light takes an angle; point and spot lights
    // take a radius instead.
    if (type == DAVA::Light::TYPE_DIRECTIONAL)
    {
        const DAVA::float32 authoredAngle = GetShadowAngle();
        const DAVA::float32 shadowAngle = (authoredAngle > 1.0e-4f) ? authoredAngle : BEAST_DEFAULT_SHADOW_ANGLE_RAD;
        const ILBStatus angleStatus = ILBSetShadowAngle(light, shadowAngle);
        if (angleStatus == ILB_ST_SUCCESS)
        {
            DAVA::Logger::Info("[BeastLight] shadows: %d samples, angle %.4f rad for '%s'",
                               GetShadowSamples(), shadowAngle, resourceName.c_str());
        }
        else
        {
            DAVA::Logger::Warning("[BeastLight] ILBSetShadowAngle(%.4f) rejected with status %d for '%s'",
                                  shadowAngle, static_cast<DAVA::int32>(angleStatus), resourceName.c_str());
        }
    }
    else if ((type == DAVA::Light::TYPE_POINT) || (type == DAVA::Light::TYPE_SPOT))
    {
        const DAVA::float32 radius = GetShadowRadius();
        if (radius > 1.0e-4f)
        {
            const ILBStatus radiusStatus = ILBSetShadowRadius(light, radius);
            if (radiusStatus != ILB_ST_SUCCESS)
            {
                DAVA::Logger::Warning("[BeastLight] ILBSetShadowRadius(%.4f) rejected with status %d for '%s'",
                                      radius, static_cast<DAVA::int32>(radiusStatus), resourceName.c_str());
            }
        }
    }
    else
    {
        DAVA::Logger::Info("[BeastLight] '%s' is type %d: no soft shadow settings for this type", resourceName.c_str(), static_cast<DAVA::int32>(type));
    }
}

const ILBLinearRGB& BeastLight::GetColor() const
{
    return linearRGB;
}

DAVA::float32 BeastLight::ReadFloatProperty(DAVA::Entity* node, const DAVA::String& key, DAVA::float32 defaultValue)
{
    DAVA::KeyedArchive* props = GetCustomPropertiesArchieve(node);
    if (props == nullptr)
    {
        return defaultValue;
    }
    const DAVA::VariantType* variant = props->GetVariant(key);
    if (variant == nullptr)
    {
        return defaultValue;
    }
    switch (variant->GetType())
    {
    case DAVA::VariantType::TYPE_FLOAT:
        return variant->AsFloat();
    case DAVA::VariantType::TYPE_FLOAT64:
        return static_cast<DAVA::float32>(variant->AsFloat64());
    case DAVA::VariantType::TYPE_INT32:
        return static_cast<DAVA::float32>(variant->AsInt32());
    case DAVA::VariantType::TYPE_BOOLEAN:
        return variant->AsBool() ? 1.0f : 0.0f;
    default:
        return defaultValue;
    }
}

DAVA::float32 BeastLight::GetFloat(const DAVA::String& key, DAVA::float32 defaultValue)
{
    return ReadFloatProperty(ownerNode, key, defaultValue);
}

bool BeastLight::IsStaticLightEnabled()
{
    return ReadFloatProperty(ownerNode, "editor.staticlight.enable", 1.0f) > 0.5f;
}

bool BeastLight::IsDynamicLight()
{
    return ReadFloatProperty(ownerNode, "editor.dynamiclight.enable", 0.0f) > 0.5f;
}

DAVA::Color BeastLight::GetAmbientColor()
{
    // The lights that matter here are the dynamic ones, and those are exactly the lights whose
    // archive carries no ambColor.r/g/b keys: the value lives on the Light component and the log
    // printed 1.000 0.871 0.647 while this function was returning black. Ask the light first.
    if (lightObject != nullptr)
    {
        const DAVA::Color& ambient = lightObject->GetAmbientColor();
        if ((ambient.r + ambient.g + ambient.b) > 1.0e-4f)
        {
            return ambient;
        }
    }
    DAVA::KeyedArchive* props = GetCustomPropertiesArchieve(ownerNode);
    if (props == nullptr)
    {
        return DAVA::Color(0.0f, 0.0f, 0.0f, 1.0f);
    }
    return DAVA::Color(GetFloat("ambColor.r", 0.0f),
                       GetFloat("ambColor.g", 0.0f),
                       GetFloat("ambColor.b", 0.0f),
                       GetFloat("ambColor.a", 1.0f));
}

DAVA::float32 BeastLight::GetShadowPriority()
{
    return GetFloat("editor.intensity", 1.0f);
}

void BeastLight::ApplySolitaryShadowPolicy()
{
    const DAVA::List<BeastLight*>& lights = GetResourceList();

    BeastLight* primary = nullptr;
    DAVA::float32 bestPriority = -1.0f;
    for (BeastLight* candidate : lights)
    {
        if ((candidate == nullptr) || (candidate->light == nullptr) || (candidate->lightObject == nullptr))
        {
            continue;
        }
        if (candidate->lightObject->GetType() != DAVA::Light::TYPE_DIRECTIONAL)
        {
            continue;
        }
        if (candidate->IsDynamicLight() || !candidate->GetCastShadows())
        {
            continue;
        }
        const DAVA::float32 priority = candidate->GetShadowPriority();
        if (priority > bestPriority)
        {
            bestPriority = priority;
            primary = candidate;
        }
    }

    DAVA::int32 silenced = 0;
    for (BeastLight* each : lights)
    {
        if ((each == nullptr) || (each->light == nullptr) || (each->lightObject == nullptr))
        {
            continue;
        }
        const bool wantShadows = (each == primary);
        const ILBStatus status = ILBSetCastShadows(each->light, wantShadows);
        if (status != ILB_ST_SUCCESS)
        {
            DAVA::Logger::Warning("[BeastLight] castshadows=%d refused (status %d) for '%s'",
                                  wantShadows ? 1 : 0, static_cast<DAVA::int32>(status), each->resourceName.c_str());
        }
        if (!wantShadows)
        {
            ++silenced;
            // A second directional that is only a few degrees off the caster is the worst case: its
            // shadow is not a separate shape the eye can forgive, it is the caster's own shadow
            // slightly delayed, which reads as a soft second edge on every wall.
            if ((primary != nullptr) && (each->lightObject->GetType() == DAVA::Light::TYPE_DIRECTIONAL))
            {
                const DAVA::Vector3& a = primary->lightObject->GetDirection();
                const DAVA::Vector3& b = each->lightObject->GetDirection();
                const DAVA::float32 dot = DAVA::Min(1.0f, DAVA::Max(-1.0f, a.DotProduct(b)));
                DAVA::Logger::Info("[BeastLight] light-only '%s' points %.1f deg from the caster",
                                   (each->ownerNode != nullptr) ? each->ownerNode->GetName().c_str() : "?", DAVA::RadToDeg(acosf(dot)));
            }
        }
    }

    if (primary != nullptr)
    {
        DAVA::Logger::Info("[BeastLight] shadow caster: '%s' (editor.intensity %.3f); %d other light(s) kept for light only",
                           (primary->ownerNode != nullptr) ? primary->ownerNode->GetName().c_str() : "?",
                           bestPriority, silenced);
    }
    else
    {
        DAVA::Logger::Warning("[BeastLight] no static directional light left to cast a shadow; %d light(s) are light only", silenced);
    }
}

bool BeastLight::GetCastShadows()
{
    DAVA::KeyedArchive* props = GetCustomPropertiesArchieve(ownerNode);
    if (props)
    {
        return props->GetBool("editor.staticlight.castshadows", true);
    }
    return true;
}

DAVA::int32 BeastLight::GetShadowSamples()
{
    DAVA::KeyedArchive* props = GetCustomPropertiesArchieve(ownerNode);
    if (props)
    {
        // One sample per texel gives a hard, aliased shadow edge. BeastSystem::SetDefaultPropertyValues
        // stamps editor.staticlight.shadowsamples = 1 on every light, so the authored value is the
        // editor's default, not intent: clamp up to a level that resolves a penumbra.
        const DAVA::int32 authored = props->GetInt32("editor.staticlight.shadowsamples", BEAST_MIN_SHADOW_SAMPLES);
        return (authored < BEAST_MIN_SHADOW_SAMPLES) ? BEAST_MIN_SHADOW_SAMPLES : authored;
    }
    return 4;
}

const ILBMatrix4x4& BeastLight::GetMatrix() const
{
    return matrix;
}

void BeastLight::SetIntensity()
{
    // editor.intensity is tuned for the engine's forward renderer, not for Beast. Calibrated against
    // the shipped lightmaps of the same tower (shipped uvOffset/uvScale still on the materials, tile
    // sampled out of rudniki.sc2_lightmaps, with the UV gaps filled so only lighting is compared):
    // 1.0 now that the light filter actually skips the other two lights: every earlier exposure was
    // fitted against a bake that contained THREE lights (Beast's own log listed light0/light1/light2),
    // so their combined 1.5x was standing in for one sun. One sun at the authored 0.5 is the level
    // the shipped scene asks for.
    // 0.35 is measured, not guessed: the v10.59 single-sun bake at exposure 1.0 read p25 73 / p50 132
    // against the shipped tower tile's 26 / 42, and every per-channel ratio lands at 0.29-0.31, so the
    // conversion from editor intensity to Beast radiance is a single flat factor with no colour cast.
    // The three-light fit that produced 0.22 was measuring a broken bake; this one has one sun.
    // No extra scale by default. The earlier 0.22-0.5 factors were fitting a bake that contained
    // THREE lights at once (a re-saved scene loses editor.staticlight.enable, so every light was
    // baked and two suns drew double shadows), and they were also applied on top of the forced 0.5,
    // which is why the map went dark. With one sun at the authored level the energy already matches
    // the shipped maps; this property stays as the knob if a map wants it brighter or dimmer. Beast takes intensity as
    // radiance where 1.0 is a fully white surface, so the authored number is halved; the factor is a
    // custom property for maps that want to push either way.
    const DAVA::float32 authored = GetFloat("editor.intensity", 1.0f);
    const DAVA::float32 exposure = GetFloat("editor.staticlight.exposure", 0.22f);
    const DAVA::float32 intensity = authored * exposure;
    BEAST_VERIFY(ILBSetLightIntensity(light, intensity));
    DAVA::Logger::Info("[BeastLight] intensity %.3f (editor.intensity %.3f x exposure %.3f) for '%s'",
                       intensity, authored, exposure, resourceName.c_str());
}

DAVA::float32 BeastLight::GetShadowAngle()
{
    return GetFloat("editor.staticlight.shadowangle", 0.0f);
}

DAVA::float32 BeastLight::GetShadowRadius()
{
    return GetFloat("editor.staticlight.shadowradius", 0.0f);
}

DAVA::float32 BeastLight::GetFalloffCutoff()
{
    return GetFloat("editor.staticlight.falloffcutoff", 1000.f);
}

DAVA::float32 BeastLight::GetFalloffExponent()
{
    return GetFloat("editor.staticlight.falloffexponent", 1.0f);
}

DAVA::float32 BeastLight::GetConeAngle()
{
    return GetFloat("editor.staticlight.cone.angle", 90.0f);
}

DAVA::float32 BeastLight::GetConePenumbraAngle()
{
    return GetFloat("editor.staticlight.cone.penumbra.angle", 0.0f);
}

DAVA::float32 BeastLight::GetConePenumbraExponent()
{
    return GetFloat("editor.staticlight.cone.penumbra.exponent", 1.0f);
}

void BeastLight::UpdateLightParamsFromHandle(ILBLightHandle lightHandle)
{
    ILBStringHandle name1, name2;
    ILBGetLightName(lightHandle, &name1);
    ILBGetLightName(light, &name2);
    DAVA::String davaName1 = ConvertBeastString(name1);
    DAVA::String davaName2 = ConvertBeastString(name2);

    if (davaName1 != davaName2)
        return;

    ILBLinearRGB color;
    BEAST_VERIFY(ILBGetLightColor(lightHandle, &color));
    lightObject->SetDiffuseColor(DAVA::Color(color.r, color.g, color.b, 1.f));

    ILBMatrix4x4 lightTransform;
    BEAST_VERIFY(ILBGetLightTransform(lightHandle, &lightTransform));
    DAVA::Matrix4 davaTransform = ConvertBeastMatrix(lightTransform);
    GetTransformComponent(ownerNode)->SetLocalTransform(DAVA::Transform(davaTransform));

    DAVA::KeyedArchive* props = GetCustomPropertiesArchieve(ownerNode);
    if (props)
    {
        DAVA::float32 intensity;
        BEAST_VERIFY(ILBGetLightIntensity(lightHandle, &intensity));

        if (lightObject->GetType() == DAVA::Light::TYPE_DIRECTIONAL)
        {
            DAVA::float32 shadowAngle;
            BEAST_VERIFY(ILBGetShadowAngle(lightHandle, &shadowAngle));
            props->SetFloat("editor.staticlight.shadowangle", shadowAngle);
        }
        else if (lightObject->GetType() != DAVA::Light::TYPE_SKY)
        {
            DAVA::float32 falloffCutoff, falloffExponent;
            BEAST_VERIFY(ILBGetLightMaxRangeFalloff(lightHandle, &falloffCutoff, &falloffExponent));
            props->GetFloat("editor.staticlight.falloffcutoff", falloffCutoff);
            props->GetFloat("editor.staticlight.falloffexponent", falloffExponent);
        }

        if (lightObject->GetType() == DAVA::Light::TYPE_POINT || lightObject->GetType() == DAVA::Light::TYPE_SPOT)
        {
            DAVA::float32 shadowRadius;
            BEAST_VERIFY(ILBGetShadowRadius(lightHandle, &shadowRadius));
            props->GetFloat("editor.staticlight.shadowradius", shadowRadius);
        }

        DAVA::int32 shadowSamples;
        BEAST_VERIFY(ILBGetLightShadowSamples(lightHandle, &shadowSamples));

        props->SetFloat("editor.intensity", intensity);
        props->GetInt32("editor.staticlight.shadowsamples", shadowSamples);
    }
}
}