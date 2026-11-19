#ifndef CSTD_MF_DEBUG_HPP
#define CSTD_MF_DEBUG_HPP

// cstdmf/debug.hpp - reconstructed minimal file. The original was never
// published with the TerrainConverter plugin. The plugin's sources only use
// MF_ASSERT and DECLARE_DEBUG_COMPONENT2 from this header, so this
// reconstruction provides those, plus the common BW message macros as
// no-ops. Everything is header-only: no symbols from the prebuilt cstdmf.lib
// are referenced, so it cannot cause link mismatches.

#include <stdlib.h>

#ifdef _DEBUG
#define MF_ASSERT(cond) \
    do { if (!(cond)) { abort(); } } while (false)
#else
#define MF_ASSERT(cond) ((void)0)
#endif

#define MF_ASSERT_DEV(cond) MF_ASSERT(cond)
#define MF_VERIFY(cond) MF_ASSERT(cond)
#define MF_ASSERT_DEBUG(cond) MF_ASSERT(cond)

#define DECLARE_DEBUG_COMPONENT(group)
#define DECLARE_DEBUG_COMPONENT2(group, priority)

#define DEBUG_MSG(...) ((void)0)
#define INFO_MSG(...) ((void)0)
#define NOTICE_MSG(...) ((void)0)
#define WARNING_MSG(...) ((void)0)
#define ERROR_MSG(...) ((void)0)
#define CRITICAL_MSG(...) ((void)0)

#endif // CSTD_MF_DEBUG_HPP
