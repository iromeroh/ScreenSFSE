#pragma once

#include "frame_source.h"

#include <string>

// What the screen controller drives: a frame source with transport controls and stereo gains.
// MediaPlayer plays files (seekable, known length); LivePlayer plays live streams (no seek, no length).
class Playback : public FrameSource
{
public:
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual bool playing() const = 0;
    // Everything has played (a file's end; a live stream that ended or failed).
    virtual bool finished() const = 0;
    virtual double positionSeconds() const = 0;
    // 0 when unknown (live).
    virtual double durationSeconds() const = 0;
    virtual std::string lastError() const = 0;
    // Output gains for the left and right speakers (0..1), e.g. from distance and direction.
    virtual void setGains(float left, float right) = 0;
    virtual bool live() const { return false; }
    // Nothing ever arrived (a stream that stalled before its first data): a failure to start, however long it took.
    virtual bool stalled() const { return false; }
};
