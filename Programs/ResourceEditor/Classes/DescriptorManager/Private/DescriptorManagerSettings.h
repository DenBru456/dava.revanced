#pragma once

#include <TArc/DataProcessing/PropertiesHolder.h>

#include <Base/BaseTypes.h>

// Persistent settings of the Descriptor Manager, stored through the TArc properties system (PropertiesItem supports Vector<String> natively)
class DescriptorManagerSettings
{
public:
    // what to do automatically when a project is opened
    static constexpr DAVA::int32 STARTUP_NOTHING = 0;
    static constexpr DAVA::int32 STARTUP_GENERATE = 1;
    static constexpr DAVA::int32 STARTUP_UPDATE = 2;

    // what the module may do with source texture files that are not on disk; Generate only ever creates the .tex
    enum eSourceCreate : DAVA::int32
    {
        // only sources already present under the plain name are used
        SOURCE_EXISTING = 0,
        // if no plain source exists, materialize one from the best variant
        SOURCE_DEFAULT = 1,
        // never read or write source files: descriptors only, pixels come from the runtime fallbacks
        SOURCE_OFF = 2
    };

    // Priorities -> Time: how file age decides between several files of the same family
    // NONE ignores age, CURRENT prefers the file already on disk, NEW prefers the newest
    enum eTimePriority : DAVA::int32
    {
        TIME_NONE = 0,
        TIME_CURRENT = 1,
        TIME_NEW = 2
    };

    DAVA::int32 timePriority = TIME_NONE;

    // Priorities -> Format: order in which source files of one texture are preferred; only the order is editable
    DAVA::Vector<DAVA::String> formatPriority = { ".png", ".tga", ".jpg", ".jpeg", ".dds", ".pvr", ".webp", ".hdr" };

    // Priorities -> GPU: order used when several family-prefixed variants exist, and the runtime load fallback order
    DAVA::Vector<DAVA::String> familyPriority = { "dx11", "PowerVR_Android", "PowerVR_iOS", "tegra", "mali", "adreno" };

    // a scene referencing a texture with no .tex loads a pink placeholder; ON generates the descriptor when the scene opens
    bool generateMissingOnSceneOpen = true;

    bool reactToMissingSource = false;
    DAVA::int32 startupAction = STARTUP_NOTHING;
    bool updateIgnoresHealthy = true;
    DAVA::int32 sourceCreate = SOURCE_OFF;
    bool overwriteExistingSource = false;
    // extension of a source file the module has to create (.png or .tga); other extensions mean decode and re-encode
    DAVA::String defaultSourceExtension = ".png";

    // every source format Dava supports, in the default order
    static const DAVA::Vector<DAVA::String>& AllSourceFormats();
    // every device GPU family name, in the default order
    static const DAVA::Vector<DAVA::String>& AllGPUFamilies();

    void Load(const DAVA::PropertiesItem& item);
    void Save(DAVA::PropertiesItem& item) const;
};
