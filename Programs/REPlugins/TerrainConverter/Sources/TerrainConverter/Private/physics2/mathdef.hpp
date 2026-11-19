#ifndef MATHDEF_HPP
#define MATHDEF_HPP

// mathdef.hpp - reconstructed file (the original was never published in the
// public dava.engine repository; the TerrainConverter plugin's physics2
// folder shipped without it). Provides the small math surface actually used
// by the plugin's physics2 sources:
//   - almostZero/almostEqual in the global namespace (for the de-namespaced
//     adapted files like planeeq.cpp); BW-namespaced versions already exist
//     in cstdmf/stdmf.hpp and are picked up by files inside
//     BW_BEGIN_NAMESPACE (vector3.ipp)
//   - Math::clamp template (used from vector3.ipp)
//   - Outcode/VertexIndex types and the X/Y/Z_AXIS constants (boundbox,
//     worldtri) - values from BigWorld's mathdef.hpp
#include <math.h>
#include <Base/BaseTypes.h>

inline bool almostZero(const float f, const float epsilon = 0.0004f)
{
    return -epsilon < f && f < epsilon;
}

inline bool almostEqual(const float f1, const float f2, const float epsilon = 0.0004f)
{
    return fabs(f1 - f2) < epsilon;
}

typedef DAVA::uint32 Outcode;
typedef DAVA::uint32 VertexIndex;

const Outcode OUTCODE_LEFT = 0x1;
const Outcode OUTCODE_RIGHT = 0x2;
const Outcode OUTCODE_BOTTOM = 0x4;
const Outcode OUTCODE_TOP = 0x8;
const Outcode OUTCODE_NEAR = 0x10;
const Outcode OUTCODE_FAR = 0x20;
const Outcode OUTCODE_MASK = OUTCODE_LEFT | OUTCODE_RIGHT | OUTCODE_BOTTOM | OUTCODE_TOP | OUTCODE_NEAR | OUTCODE_FAR;
const VertexIndex NO_VERTEX_INDEX = 0xffffffff;

enum
{
    X_AXIS = 0,
    Y_AXIS = 1,
    Z_AXIS = 2
};

namespace Math
{
template <class TYPE>
inline TYPE clamp(TYPE minValue, TYPE value, TYPE maxValue)
{
    return (value < minValue) ? minValue : ((value > maxValue) ? maxValue : value);
}

template <class TYPE>
inline TYPE clamp(TYPE magnitude, TYPE value)
{
    return clamp(-magnitude, value, magnitude);
}
} // namespace Math

#endif // MATHDEF_HPP
