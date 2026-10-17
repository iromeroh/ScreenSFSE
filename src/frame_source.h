#pragma once

#include <cstdint>

// Something that produces RGBA frames (DXGI R8G8B8A8 byte order) at the screen texture's size.
// copyLatest() is called from the render thread's submission hook: it must be quick and never block on decoding.
class FrameSource
{
public:
    virtual ~FrameSource() = default;

    // Copies the frame to show now into dst (rows of dstPitch bytes) if it differs from lastSeq.
    // Returns that frame's sequence number (never 0), or 0 when there is nothing new.
    virtual std::uint64_t copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch) = 0;

    // A screen texture became live (true) or none is left (false); lets a source idle while nothing shows it.
    virtual void onSurfaceLive(bool live) { (void)live; }
};
