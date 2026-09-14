#pragma once

#include <algorithm>
#include <clap/process.h>

namespace lumipaint {

// Clear host-owned buffers, including when no MIDI events arrive. Declaring a
// silent bus without writing its samples can send stale buffer contents to audio.
inline void clearSilentOutput(const clap_process_t *process)
{
    for (uint32_t bus = 0; bus < process->audio_outputs_count; ++bus)
    {
        auto &output = process->audio_outputs[bus];
        for (uint32_t channel = 0; channel < output.channel_count; ++channel)
        {
            if (output.data32)
                std::fill_n(output.data32[channel], process->frames_count, 0.0f);
            if (output.data64)
                std::fill_n(output.data64[channel], process->frames_count, 0.0);
        }
        output.constant_mask = output.channel_count >= 64
            ? UINT64_MAX : (uint64_t{1} << output.channel_count) - 1;
    }
}

} // namespace lumipaint
