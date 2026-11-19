#include "REPlatform/Scene/Utils/DvplPacker.h"

#include <Compression/LZ4Compressor.h>
#include <Compression/ZipCompressor.h>
#include <FileSystem/File.h>
#include <FileSystem/FileSystem.h>
#include <FileSystem/Private/PackFormatSpec.h>
#include <Engine/Engine.h>
#include <Engine/EngineContext.h>
#include <Logger/Logger.h>
#include <Utils/CRC32.h>

namespace DAVA
{
namespace DvplPackerDetails
{
bool Compress(Compressor::Type type, const Vector<uint8>& in, Vector<uint8>& out)
{
    switch (type)
    {
    case Compressor::Type::None:
        out = in;
        return true;
    case Compressor::Type::Lz4:
        return LZ4Compressor().Compress(in, out);
    case Compressor::Type::Lz4HC:
        return LZ4HCCompressor().Compress(in, out);
    case Compressor::Type::RFC1951:
        return ZipCompressor().Compress(in, out);
    case Compressor::Type::Lz4Stream:
        return LZ4StreamCompressor::LZ4StreamCompress(in, out);
    default:
        return false;
    }
}
} // namespace DvplPackerDetails

bool DvplPacker::PackFile(const FilePath& source, Compressor::Type type, bool deleteSource)
{
    FileSystem* fs = GetEngineContext()->fileSystem;

    if (!fs->Exists(source) || source.IsDirectoryPathname())
    {
        Logger::Error("[DvplPacker] '%s' is not a file, nothing to pack", source.GetAbsolutePathname().c_str());
        return false;
    }

    // Read the plain file. The path is the plain one, so File::Create does not take the
    // .dvpl fallback here.
    Vector<uint8> plain;
    {
        ScopedPtr<File> in(File::Create(source, File::OPEN | File::READ));
        if (!in)
        {
            Logger::Error("[DvplPacker] cannot open '%s'", source.GetAbsolutePathname().c_str());
            return false;
        }
        const uint64 size = in->GetSize();
        plain.resize(static_cast<size_t>(size));
        if (size > 0 && in->Read(plain.data(), static_cast<uint32>(size)) != size)
        {
            Logger::Error("[DvplPacker] short read on '%s'", source.GetAbsolutePathname().c_str());
            return false;
        }
    }

    Vector<uint8> packed;
    if (!DvplPackerDetails::Compress(type, plain, packed))
    {
        Logger::Error("[DvplPacker] compression type %u failed on '%s'", static_cast<uint32>(type), source.GetAbsolutePathname().c_str());
        return false;
    }

    PackFormat::LitePack::Footer footer;
    footer.sizeUncompressed = static_cast<uint32>(plain.size());
    footer.sizeCompressed = static_cast<uint32>(packed.size());
    footer.crc32Compressed = CRC32::ForBuffer(packed.data(), packed.size());
    footer.type = type;
    footer.packMarkerLite = PackFormat::FILE_MARKER_LITE;

    FilePath target(source);
    target.ReplaceExtension(source.GetExtension() + ".dvpl");

    {
        ScopedPtr<File> out(File::Create(target, File::CREATE | File::WRITE));
        if (!out)
        {
            Logger::Error("[DvplPacker] cannot create '%s'", target.GetAbsolutePathname().c_str());
            return false;
        }
        if (!packed.empty() && out->Write(packed.data(), static_cast<uint32>(packed.size())) != packed.size())
        {
            Logger::Error("[DvplPacker] short write on '%s'", target.GetAbsolutePathname().c_str());
            return false;
        }
        if (out->Write(&footer, sizeof(footer)) != sizeof(footer))
        {
            Logger::Error("[DvplPacker] cannot write the footer of '%s'", target.GetAbsolutePathname().c_str());
            return false;
        }
    }

    if (deleteSource)
    {
        // Verify before deleting: the footer must be the last 20 bytes and the payload must
        // match the CRC stored in it. CRC32::ForDVPLFileContent skips the footer for us.
        const uint32 readBack = CRC32::ForDVPLFileContent(target);
        if (readBack != footer.crc32Compressed)
        {
            Logger::Error("[DvplPacker] '%s' failed verification (crc %08x, expected %08x), the plain file is kept",
                          target.GetAbsolutePathname().c_str(), readBack, footer.crc32Compressed);
            return false;
        }
        if (!fs->DeleteFile(source))
        {
            Logger::Warning("[DvplPacker] packed '%s' but could not delete the plain file", source.GetAbsolutePathname().c_str());
        }
    }

    Logger::Info("[DvplPacker] %s -> %s (%u -> %u bytes, type %u)", source.GetFilename().c_str(), target.GetFilename().c_str(),
                 footer.sizeUncompressed, footer.sizeCompressed, static_cast<uint32>(type));
    return true;
}
} // namespace DAVA
