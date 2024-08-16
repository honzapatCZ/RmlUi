#include "FlaxFileInterface.h"

#include <Engine/Content/BinaryAsset.h>
#include <Engine/Content/Content.h>
#include <Engine/Core/Collections/Dictionary.h>
#include <Engine/Core/Math/Math.h>
#include <Engine/Core/Log.h>
#include <Engine/Core/Types/Span.h>
#include "Engine/Platform/File.h"

// We assume only .rml and .rcss files are accessed through this interface,
// so we only support loading specific BinaryAsset files.

namespace
{
    // Store spans of binary assets to provide random access to the data
    Dictionary<BinaryAsset*, Span<byte>> AssetSpans;
}

Rml::FileHandle FlaxFileInterface::Open(const Rml::String& path)
{
    String assetPath = String(path.c_str());
    LOG(Info, "Opening asset: {0}", assetPath);
    File* file = File::Open(assetPath, FileMode::OpenExisting, FileAccess::Read);

    if (file == nullptr)
        return Rml::FileHandle();

    return (Rml::FileHandle)file;
}

void FlaxFileInterface::Close(Rml::FileHandle file)
{
    auto asset = (File*)file;
    asset->Close();
}

size_t FlaxFileInterface::Read(void* buffer, size_t size, Rml::FileHandle file)
{
    auto asset = (File*)file;
    uint32 readSize = 0;
    asset->Read(buffer, (uint32)size, &readSize);
    return (size_t)readSize;
}

bool FlaxFileInterface::Seek(Rml::FileHandle file, long offset, int origin)
{
    auto asset = (File*)file;
    uint32 start = 0;
    uint32 end = start + asset->GetSize();
    uint32 ptr;
    if (origin == SEEK_CUR)
        ptr = asset->GetPosition()  + offset;
    else if (origin == SEEK_SET)
        ptr = start + offset;
    else if (origin == SEEK_END)
        ptr = end + offset;
    else
        ptr = 0;

    ASSERT(ptr >= start);
    ASSERT(ptr < end);
    
    asset->SetPosition(ptr);

    return true;
}

size_t FlaxFileInterface::Tell(Rml::FileHandle file)
{
    auto asset = (File*)file;
    uint32 start = 0;
    size_t offset = asset->GetPosition() - start;
    return offset;
}

size_t FlaxFileInterface::Length(Rml::FileHandle file)
{
    auto asset = (File*)file;
    return asset->GetSize();
}

bool FlaxFileInterface::LoadFile(const Rml::String& path, Rml::String& out_data)
{
    auto asset = (File*)Open(path);
    if (asset == nullptr)
        return false;

    out_data.resize(Length((Rml::FileHandle)asset));
    size_t read = Read(&out_data[0], out_data.length(), (Rml::FileHandle)asset);
    Close((Rml::FileHandle)asset);

    if (read != out_data.length())
        return false;

    return true;
}