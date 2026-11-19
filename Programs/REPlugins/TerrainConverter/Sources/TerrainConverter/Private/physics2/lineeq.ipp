/******************************************************************************
BigWorld Technology
Copyright BigWorld Pty, Ltd.
All Rights Reserved. Commercial in confidence.

WARNING: This computer program is protected by copyright law and international
treaties. Unauthorized use, reproduction or distribution of this program, or
any portion of this program, may result in the imposition of civil and
criminal penalties as provided by law.
******************************************************************************/

// lineeq.ipp - reconstructed file adapted to DAVA types (the original was
// never published with the TerrainConverter plugin). Method names follow the
// published lineeq.hpp declarations: DAVA::Vector2 + PascalCase DAVA API.

/**
 *	Line-segment constructor
 */
inline LineEq::LineEq(const DAVA::Vector2& v0, const DAVA::Vector2& v1,
                      bool needToNormalise)
{
    normal_.x = v0.y - v1.y;
    normal_.y = v1.x - v0.x;
    if (needToNormalise)
        normal_.Normalize();

    d_ = normal_.DotProduct(v0);
}

/**
 *	This method intersects this line with the other, returning the
 *	distance along the line that the intersection occurs.
 */
inline float LineEq::intersect(const LineEq& other) const
{
    const DAVA::Vector2& on = other.normal_;
    return (other.d_ - d_ * on.DotProduct(normal_)) /
    on.CrossProduct(normal_);
}

/**
 *	Return the point that corresponds to the given parameter
 */
inline DAVA::Vector2 LineEq::param(float t) const
{
    return DAVA::Vector2(
                          normal_.x * d_ + normal_.y * t,
                          normal_.y * d_ - normal_.x * t);
}

/**
 *	Return the parameter of the given point projected onto the line
 */
inline float LineEq::project(const DAVA::Vector2& point) const
{
    DAVA::Vector2 dir(normal_.y, -normal_.x); // also unit length
    return dir.DotProduct(point - normal_ * d_);
}

/**
 *	Returns whether or not the given line is a minimum cutter of our line.
 */
inline bool LineEq::isMinCutter(const LineEq& cutter) const
{
    return normal_.CrossProduct(cutter.normal_) > 0.f;
}

/**
 *	Returns whether or not this line is parallel with the other
 */
inline bool LineEq::isParallel(const LineEq& other) const
{
    return fabs(normal_.CrossProduct(other.normal_)) < 0.001f;
}

/**
 *	Returns whether or not the given point lies in front of (in the positive
 *	half-space of) this line. Adapted from the BigWorld ancestor
 *	lib/math/lineeq.ipp. This was the fourth of the plugin's unresolved
 *	externals: lineeq.hpp declares it unconditionally (line 42) and includes
 *	this file at the bottom, but the shipped stub never defined it.
 */
inline bool LineEq::isInFrontOf(const DAVA::Vector2& point) const
{
    return normal_.DotProduct(point) > d_;
}
