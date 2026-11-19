#pragma once

// ConverterVisitorElement.h - patched file: added the forward declaration
// of ConverterVisitorBase so this header compiles when included on its own
// (BoundingBoxData.h includes it without including ConverterVisitorBase.h).
// Everything else is the original published content.

class ConverterVisitorBase;

class ConverterVisitorElement
{
public:
    virtual ~ConverterVisitorElement() = default;

    virtual void Accept(ConverterVisitorBase& v) = 0;
};
