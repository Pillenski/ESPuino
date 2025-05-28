#pragma once

class Audio;

namespace AudioPlayer_TrackCtrl {

    /**
     * Execute the full track‑control command set that used to live inside the
     * monolithic AudioPlayer_Task() loop.
     *
     * @param cmd   One of the Track-Control command constants (see values.h).
     * @param audio Pointer to the global Audio instance. Must be valid.
     */
    void executeTrackCommand(Audio* audio, uint8_t cmd);

}   // namespace AudioPlayer_TrackCtrl