#pragma once

#include <cstdint>

namespace vulvox
{
    struct Renderer_Configuration
    {
        // Total transient instance/texture-index/UV data available to one frame.
        // 128 MiB supports roughly one million matrices, or fewer instances when
        // texture-index and UV streams are also used.
        uint64_t instance_upload_arena_bytes = 128ull * 1024ull * 1024ull;
        bool enable_back_face_culling = true;
    };
}
