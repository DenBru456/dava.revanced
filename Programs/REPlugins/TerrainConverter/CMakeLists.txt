cmake_minimum_required (VERSION 3.0)

# ---------------------------------------------------------------------------
# TerrainConverter as a STATIC module merged into ResourceEditor.exe.
#
# Why: built as MODULE_TYPE PLUGIN the module produced a DLL that statically
# linked its OWN copy of the engine. TArc's START_PLUGIN_DECLARATION stitches
# the EngineContext into the DLL (SetEngineContext + master/slave DBs), so
# light engine calls work - but C++ Type identity stays per-module. Any
# component Type created in the DLL is a different Type object than the one
# the exe's ComponentManager seeded with user data, so EntityFamily ->
# ComponentManager::GetRuntimeComponentId -> Type::GetUserData asserts the
# moment Converter::Do() runs `new Scene()`.
#
# As a STATIC module the plugin's objects are linked whole-archive into
# ResourceEditor.exe: one engine, one registry, one Type identity.
# TArcPluginManager never sees it (no DLL to scan), so the GUI module is
# registered by StaticModuleRegistrar.cpp via DECL_TARC_MODULE instead.
# ---------------------------------------------------------------------------

set(MODULE_NAME TerrainConverter)
set(MODULE_TYPE STATIC)
set(PLATFORMS_SUPPORTED WIN)

set(FIND_PACKAGE QT5)
set(FIND_MODULE QtHelpers TArc QtTools REPlatform)

set(SOURCES_DIR "${CMAKE_CURRENT_LIST_DIR}/Sources/TerrainConverter")
set(LIBS_DIR "${CMAKE_CURRENT_LIST_DIR}/Libs")

# The DLL plugin entry (START_PLUGIN_DECLARATION ... END_PLUGIN_DECLARATION)
# lives at the bottom of Private/TerainConverterModule.cpp in this tree, so
# it cannot be erased as a file. Wrap that block in the module cpp:
#     #if defined(DAVA_IMPLEMENT_PLUGIN_MODULE)
#     START_PLUGIN_DECLARATION()
#     ...
#     END_PLUGIN_DECLARATION()
#     #endif
# The template defines DAVA_IMPLEMENT_PLUGIN_MODULE ONLY for PLUGIN builds,
# so the block (and its DAVAMain stub, which would collide with the exe's)
# disappears from the STATIC build while the file still builds as a plugin.

set(HPP_FILES_RECURSE Sources/TerrainConverter/*.hpp Sources/TerrainConverter/*.h)
set(CPP_FILES_RECURSE Sources/TerrainConverter/*.cpp Sources/TerrainConverter/*.c)

set(INCLUDES_PRIVATE
                     "${LIBS_DIR}/Includes"
                     "${LIBS_DIR}/Includes/zip"
                     "${SOURCES_DIR}"
                     "${SOURCES_DIR}/Private"
                     "${SOURCES_DIR}/Private/physics2"
                     "${DAVA_ENGINE_DIR}"
                     "${DAVA_THIRD_PARTY_ROOT_PATH}/zlib"
                     "${DAVA_THIRD_PARTY_ROOT_PATH}/bullet")

# Keep the two TArc-facing compile units OUT of the unity packs: the
# bwentity/cstdmf header chain leaks `#define interface struct` (rpcndr.h via
# COM headers), which breaks ClientModule.h when packed together with them.
# The masks below are regex-matched against the file path.
set(UNITY_IGNORE_LIST
                     "StaticModuleRegistrar[.]cpp"
                     "TerainConverterModule[.]cpp")

# STATIC modules do not inherit the app's definitions (PLUGIN did via
# GLOBAL_DEFINITIONS), so restate the ones the plugin sources expect.

set(DEFINITIONS_PRIVATE_WIN
                     -DQT_OPENGL_LIB -DQT_GUI_LIB -DQT_NETWORK_LIB -DQT_CORE_LIB
                     -DQT_THREAD_SUPPORT
                     -D_CRT_SECURE_NO_DEPRECATIONS
                     -D_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS
                     -D__DAVAENGINE_RESOURCEEDITOR__
                     -DDAVA_QT
                     -D__DAVAENGINE_QT__)
set(DEFINITIONS_PRIVATE -D__DAVAENGINE_QT__)

# minizip stays a separate C-language target so the unity/jumbo build cannot
# compile the C sources as C++ (unchanged from the plugin-era CMakeLists).
add_library(terrainconverter_minizip STATIC
    "${LIBS_DIR}/Includes/zip/zip.c"
    "${LIBS_DIR}/Includes/zip/ioapi.c")
set_target_properties(terrainconverter_minizip PROPERTIES LINKER_LANGUAGE C)
target_include_directories(terrainconverter_minizip PRIVATE
    "${LIBS_DIR}/Includes/zip"
    "${DAVA_THIRD_PARTY_ROOT_PATH}/zlib")

set(WIN_DEBUG_LIBS terrainconverter_minizip)
set(WIN_RELEASE_LIBS terrainconverter_minizip)

set(STATIC_LIBRARIES_WIN64_RELEASE "${WIN_RELEASE_LIBS}")
set(STATIC_LIBRARIES_WIN64_DEBUG "${WIN_DEBUG_LIBS}")

setup_main_module()

# The app references NO TerrainConverter symbols, so without this the MSVC
# linker drops every object from the static lib - including
# StaticModuleRegistrar.obj, whose ModuleInitializer static objects
# self-register the GUI and console modules into TArc's ModuleCollection.
# Result: exe links, but GetGuiModules()/GetConsoleModules() are empty and
# no menu/settings/-mapConvert ever appear. Force whole-archive linkage on
# every consumer so the registration objects survive the link.
if(MSVC)
    set_property(TARGET TerrainConverter APPEND PROPERTY
        INTERFACE_LINK_OPTIONS "/WHOLEARCHIVE:$<TARGET_FILE:TerrainConverter>")
else()
    set_property(TARGET TerrainConverter APPEND PROPERTY
        INTERFACE_LINK_OPTIONS "-Wl,--whole-archive,$<TARGET_FILE:TerrainConverter>,--no-whole-archive")
endif()
