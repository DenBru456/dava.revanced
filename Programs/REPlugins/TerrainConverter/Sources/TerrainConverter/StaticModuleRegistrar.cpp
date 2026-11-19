// TerrainConverter static registration (dava.revanced x64 port).
//
// As a STATIC module there is no PlugIns/*.dll for TArcPluginManager to scan,
// so the module registers itself the same way the editor's own modules do:
// a ModuleInitializer static object per module class, which adds the class's
// ReflectedType into TArc's ModuleCollection. Routing is automatic inside
// ModuleInitializer: std::is_base_of<ClientModule> -> AddGuiModule (TArc Core
// then constructs/drives it: menus, settings, Library context action), else
// AddConsoleModule (REApplication::CreateConsoleModules matches it by
// M::CommandName - our "-mapConvert" console module).
//
// NOTE: the classes live in TerainConverterModule.h and are named
// TerrainConverterGUIModule (GUI) and TerrainConverterConsoleModule
// (console). There is no class named TerainConverterModule - that spelling
// only exists in the legacy FILE names (TerainConverterModule.cpp/.h).

// The Windows RPC/COM header chain (rpcndr.h, pulled in via objbase.h/ole2.h
// by some header in the bwentity/cstdmf tree) does `#define interface struct`.
// TArc's ClientModule API uses `interface` as a parameter name
// (RegisterInterface(TInterface* interface)), so the macro turns it into
// `RegisterInterface(TInterface* struct)` -> the whole header fails to parse.
// Unity packs make this order-dependent; neutralize it before ANY TArc
// include in every TArc-facing compile unit.
#ifdef interface
#undef interface
#endif

#include "TArc/Core/ClientModule.h"
#include "TArc/Utils/ModuleCollection.h"

#include "TerainConverterModule.h" // declares TerrainConverterGUIModule + TerrainConverterConsoleModule

DECL_TARC_MODULE(TerrainConverterGUIModule);
DECL_TARC_MODULE(TerrainConverterConsoleModule);
