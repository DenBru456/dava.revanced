#pragma once

#include "Beast/BeastTypes.h"

#include <Base/BaseTypes.h>
#include <Logger/Logger.h>
#include "Utils/UTF8Utils.h"

namespace Beast
{
// Every ILB call in this module receives its name through STRING_TO_BEAST_STRING, which builds a
// DAVA::String and hands the temporary's characters to Beast. A DAVA::String built from a NULL
// const char* calls strlen(NULL) and takes the whole editor down inside ucrtbase, with a stack that
// names nothing: rcx = 0, the zero byte masks (0x7EFEFEFEFEFEFEFF / 0x8101010101010100) in r8/r11.
// These overloads keep the conversion but turn a null into a log line naming the call site.
template <typename T>
inline DAVA::String BeastSafeString(const T& value, DAVA::int32 line)
{
    return DAVA::String(value);
}

inline DAVA::String BeastSafeString(const DAVA::String& value, DAVA::int32 line)
{
    return value;
}

inline DAVA::String BeastSafeString(const char* value, DAVA::int32 line)
{
    if (value == nullptr)
    {
        DAVA::Logger::Error("[Beast] a null string was passed to a Beast call at %s:%d (converted to an empty name)",
                            __FILE__, static_cast<DAVA::int32>(line));
        return DAVA::String();
    }
    return DAVA::String(value);
}

#define DECLARE_BEAST_NAME(name) \
	static DAVA::String GetBeastName() { return DAVA::String(#name); }

#if defined(ILB_STRING_UTF16)
#define GENERATE_BEAST_NAME(type) DAVA::UTF8Utils::EncodeToWideString(BeastNameGenerator::GenerateName<type>()).c_str()
#define STRING_TO_BEAST_STRING(value) DAVA::UTF8Utils::EncodeToWideString(Beast::BeastSafeString(value, __LINE__)).c_str()
#define CONST_STRING_TO_BEAST_STRING(value) L##value
#else
#define GENERATE_BEAST_NAME(type) BeastNameGenerator::GenerateName<type>().c_str()
#define STRING_TO_BEAST_STRING(value) value.c_str()
#define CONST_STRING_TO_BEAST_STRING(value) value
#endif

class BeastNameGenerator
{
public:
    template <typename T>
    static DAVA::String GenerateName();

private:
    static DAVA::int32 nameIndex;
};

template <typename T>
DAVA::String BeastNameGenerator::GenerateName()
{
    DAVA::String name = DAVA::Format("%s_%d", T::GetBeastName().c_str(), nameIndex);
    nameIndex++;

    return name;
}
}