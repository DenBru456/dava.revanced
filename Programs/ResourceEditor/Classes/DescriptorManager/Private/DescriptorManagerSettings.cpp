#include "Classes/DescriptorManager/Private/DescriptorManagerSettings.h"

#include <Render/GPUFamilyDescriptor.h>
#include <Render/RenderBase.h>

#include <algorithm>

using namespace DAVA;

namespace DescriptorManagerSettingsDetails
{
// keep known entries in the stored order, then append the missing ones: the tables always hold the full set
void Normalize(Vector<String>& stored, const Vector<String>& all)
{
    Vector<String> result;
    for (const String& value : stored)
    {
        if (std::find(all.begin(), all.end(), value) != all.end())
        {
            if (std::find(result.begin(), result.end(), value) == result.end())
            {
                result.push_back(value);
            }
        }
    }
    for (const String& value : all)
    {
        if (std::find(result.begin(), result.end(), value) == result.end())
        {
            result.push_back(value);
        }
    }
    stored = result;
}
} // namespace DescriptorManagerSettingsDetails

const Vector<String>& DescriptorManagerSettings::AllSourceFormats()
{
    static const Vector<String> formats = { ".png", ".tga", ".jpg", ".jpeg", ".dds", ".pvr", ".webp", ".hdr" };
    return formats;
}

const Vector<String>& DescriptorManagerSettings::AllGPUFamilies()
{
    static const Vector<String> families = { "dx11", "PowerVR_Android", "PowerVR_iOS", "tegra", "mali", "adreno" };
    return families;
}

void DescriptorManagerSettings::Load(const PropertiesItem& item)
{
    formatPriority = item.Get<Vector<String>>("formatPriority", formatPriority);
    familyPriority = item.Get<Vector<String>>("familyPriority", familyPriority);
    timePriority = item.Get<int32>("timePriority", TIME_NONE);
    generateMissingOnSceneOpen = item.Get<bool>("generateMissingOnSceneOpen", true);
    reactToMissingSource = item.Get<bool>("reactToMissingSource", false);
    startupAction = item.Get<int32>("startupAction", STARTUP_NOTHING);
    // older nodes stored the flag under the Regenerate name
    updateIgnoresHealthy = item.Get<bool>("updateIgnoresHealthy", item.Get<bool>("regenerateIgnoresHealthy", true));
    sourceCreate = item.Get<int32>("sourceCreate", SOURCE_OFF);
    overwriteExistingSource = item.Get<bool>("overwriteExistingSource", false);
    defaultSourceExtension = item.Get<String>("defaultSourceExtension", String(".png"));

    DescriptorManagerSettingsDetails::Normalize(formatPriority, AllSourceFormats());
    DescriptorManagerSettingsDetails::Normalize(familyPriority, AllGPUFamilies());

    if (defaultSourceExtension.empty() == true)
    {
        defaultSourceExtension = ".png";
    }
    if (startupAction < STARTUP_NOTHING || startupAction > STARTUP_UPDATE)
    {
        startupAction = STARTUP_NOTHING;
    }
    if (timePriority < TIME_NONE || timePriority > TIME_NEW)
    {
        timePriority = TIME_NONE;
    }
    if (sourceCreate < SOURCE_EXISTING || sourceCreate > SOURCE_OFF)
    {
        sourceCreate = SOURCE_OFF;
    }
}

void DescriptorManagerSettings::Save(PropertiesItem& item) const
{
    item.Set("formatPriority", Any(formatPriority));
    item.Set("familyPriority", Any(familyPriority));
    item.Set("timePriority", Any(timePriority));
    item.Set("generateMissingOnSceneOpen", Any(generateMissingOnSceneOpen));
    item.Set("reactToMissingSource", Any(reactToMissingSource));
    item.Set("startupAction", Any(startupAction));
    item.Set("updateIgnoresHealthy", Any(updateIgnoresHealthy));
    item.Set("sourceCreate", Any(sourceCreate));
    item.Set("overwriteExistingSource", Any(overwriteExistingSource));
    item.Set("defaultSourceExtension", Any(defaultSourceExtension));
}
