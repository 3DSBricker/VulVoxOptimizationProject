#pragma once

#include "vulkan_buffer.h"
#include "vulkan_instance.h"

#include <vector>
#include <cassert>
#include <cstring>

namespace vulvox
{
    class Vulkan_Buffer_Manager
    {
    public:

        Vulkan_Buffer_Manager() = default;

        // Initialize buffer manager.
        // default_buffers_per_frame: how many instance sub-buffers to preallocate per-frame (helps avoid runtime VMA allocations)
        // default_max_instances: maximum number of instances expected per instance-buffer (preallocates buffer size = sizeof(mat4) * default_max_instances)
        void init(Vulkan_Instance* vulkan_instance, uint32_t frames_in_flight, VkDeviceSize instance_arena_size);
        void destroy();

        /// <summary>
        /// Set the amount of extra memory (in instance sizes) that is allocated when resizing a buffer.
        /// </summary>
        void set_growth_factor(const uint32_t growth_factor);

        /// <summary>
        /// Call this at the start of every frame to reset the instance buffer usage counter.
        /// </summary>
        // Reset per-frame allocation cursor. Must be called each frame with the current_frame index.
        void begin_frame(const uint32_t current_frame);

        /// <summary>
        /// Retrieve the uniform buffer for the current swap chain image.
        /// </summary>
        Buffer& get_uniform_buffer(const uint32_t current_frame);

        /// <summary>
        /// Get index of an instance buffer with at least instance_size * instance_count capacity.
        /// </summary>
        /// <param name="current_frame">Index of the current swap_chain image.</param>
        /// <returns>Reference to an available buffer with at least instance_size * instance_count capacity.</returns>
        size_t get_instance_buffer(const uint32_t current_frame, const VkDeviceSize instance_size, const VkDeviceSize instance_count);

        Buffer& get_instance_buffer(const size_t buffer_index);

        /// <summary>
        /// Reference to allocated region in an instance buffer.
        /// buffer_index: index into instance_buffers
        /// offset: byte offset within the buffer where data was copied
        /// </summary>
        struct InstanceBufferRef
        {
            size_t buffer_index;
            VkDeviceSize offset;
        };

        /// <summary>
        /// Copy data to a per-frame instance buffer and returns the buffer index + byte offset.
        /// This avoids creating new buffers at runtime by using large preallocated, persistently-mapped per-frame buffers.
        /// </summary>
        template<typename T>
        InstanceBufferRef copy_to_instance_buffer(Vulkan_Instance& instance, const uint32_t current_frame, const std::vector<T>& data)
        {
            // Ensure current_frame is valid
            assert(current_frame < instance_buffers.size());

            VkDeviceSize data_size = static_cast<VkDeviceSize>(data.size()) * sizeof(T);

            // Align to 16 bytes to satisfy std140-like alignment needs
            const VkDeviceSize alignment = 16;
            VkDeviceSize aligned_offset = (instance_buffer_offsets[current_frame] + (alignment - 1)) & ~(alignment - 1);

            // Never recreate a buffer while it is referenced by the command buffer:
            // that would invalidate earlier commands in this same frame.
            VkDeviceSize required_size = aligned_offset + data_size;
            if (required_size > instance_buffers[current_frame].size)
            {
                throw std::runtime_error("Per-frame instance upload arena exhausted. Increase Renderer_Configuration::instance_upload_arena_bytes.");
            }

            // Copy into mapped memory
            if (instance_buffers[current_frame].allocation_info.pMappedData != nullptr)
            {
                std::memcpy(static_cast<char*>(instance_buffers[current_frame].allocation_info.pMappedData) + aligned_offset, data.data(), data_size);
            }

            // Update cursor
            instance_buffer_offsets[current_frame] = aligned_offset + data_size;

            // Count this host->GPU upload for profiling
            if (current_frame < instance_uploads.size())
            {
                instance_uploads[current_frame]++;
            }

            return InstanceBufferRef{ current_frame, aligned_offset };
        }

        /// <summary>
        /// Retrieve the number of per-frame host uploads (copy_to_instance_buffer calls) for a given frame index.
        /// </summary>
        uint32_t get_frame_uploads(const uint32_t frame) const
        {
            if (frame < instance_uploads.size()) return instance_uploads[frame];
            return 0;
        }

    private:

        void create_uniform_buffers();

        /// <summary>
        ///Create an instance buffer thats accessable from both the host and device.
        /// </summary>
        /// <param name="instance_buffer_size">Size in sizeof(instance_type) * instance_count.</param>
        Buffer create_instance_buffer(const VkDeviceSize instance_buffer_size);

        /// <summary>
        /// Create extra instance buffers with given buffer size if current amount of instance buffers is less than the requested amount.
        /// </summary>
        void expand_instance_buffers(uint32_t required_buffers_per_frame, VkDeviceSize instance_buffer_size);

        Vulkan_Instance* vulkan_instance = nullptr;

        uint32_t swap_chain_image_count = 0;
        uint32_t instance_buffer_requests = 0; // retained for compatibility

        //Uniform buffers, data available across shaders
        std::vector<Buffer> uniform_buffers;

        //Instance buffers, one per swap-chain image (persistently mapped big buffers)
        std::vector<Buffer> instance_buffers;

        // Per-frame byte cursor into the instance buffer
        std::vector<VkDeviceSize> instance_buffer_offsets;

        // Number of persistently-mapped instance-buffer writes in each frame.
        std::vector<uint32_t> instance_uploads;

        // Configured default max instances per per-frame buffer (used at init)
        uint32_t default_max_instances = 0;
    };

}
