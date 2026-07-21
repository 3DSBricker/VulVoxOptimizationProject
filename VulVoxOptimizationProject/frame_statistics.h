#pragma once

#include <cstdint>

namespace vulvox
{
    // Values describe the most recently completed frame. GPU time is available
    // after its fence signals, so it can be up to MAX_FRAMES_IN_FLIGHT frames old.
    struct Frame_Statistics
    {
        // Presentation-loop metrics, sampled over a half-second window.
        double frames_per_second = 0.0;
        double average_frame_time_ms = 0.0;
        double cpu_frame_time_ms = 0.0;
        double gpu_frame_time_ms = 0.0;
        uint64_t draw_calls = 0;
        uint64_t vertices = 0;
        uint64_t indices = 0;
        uint32_t queue_submits = 0;
        uint32_t host_buffer_uploads = 0;
        uint32_t pipeline_binds = 0;
        uint32_t descriptor_set_binds = 0;
    };
}
