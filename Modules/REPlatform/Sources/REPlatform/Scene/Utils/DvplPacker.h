#pragma once

#include <Base/BaseTypes.h>
#include <Compression/Compressor.h>
#include <FileSystem/FilePath.h>

namespace DAVA
{
// Packs a file into the DVPL "lite" container the engine reads transparently
// (File::Create falls back to <name>.dvpl): compressed payload followed by a
// 20 byte footer {sizeUncompressed, sizeCompressed, crc32(payload), type, "DVPL"}.
// Byte-for-byte what Programs/ResourceDVPL writes.
class DvplPacker
{
public:
    // Writes <source>.dvpl. When deleteSource is true the plain file is removed
    // only after the .dvpl was written AND read back with a matching CRC, so a
    // failed pack never costs the user their scene.
    static bool PackFile(const FilePath& source, Compressor::Type type, bool deleteSource);
};
} // namespace DAVA
