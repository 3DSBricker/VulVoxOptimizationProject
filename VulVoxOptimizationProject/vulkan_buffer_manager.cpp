#include "pch.h"
#include "vulkan_buffer_manager.h"

namespace vulvox
{
    void Vulkan_Buffer_Manager::init(Vulkan_Instance* vulkan_instance, const uint32_t swap_chain_image_count, const VkDeviceSize instance_arena_size)
    {
        this->vulkan_instance = vulkan_instance;
        this->swap_chain_image_count = swap_chain_image_count;
        this->default_max_instances = 0;

        create_uniform_buffers();

        // Create one large, persistently-mapped instance buffer per frame.
        instance_buffers.resize(swap_chain_image_count);
        instance_buffer_offsets.assign(swap_chain_image_count, 0);
        instance_uploads.assign(swap_chain_image_count, 0);

        for (size_t i = 0; i < swap_chain_image_count; i++)
        {
            instance_buffers[i] = create_instance_buffer(instance_arena_size);
        }
    }

    void Vulkan_Buffer_Manager::set_growth_factor(const uint32_t growth_factor)
    {
        (void)growth_factor;
    }

    void Vulkan_Buffer_Manager::destroy()
    {
        // Destroy all buffers
        for (auto& buffer : uniform_buffers)
        {
            buffer.destroy(vulkan_instance->allocator);
        }
        uniform_buffers.clear();

        for (auto& buffer : instance_buffers)
        {
            buffer.destroy(vulkan_instance->allocator);

        }
        instance_buffers.clear();
    }

    void Vulkan_Buffer_Manager::begin_frame(const uint32_t current_frame)
    {
        // Reset the allocation cursor for the current frame; next allocations will start from 0
        assert(current_frame < instance_buffer_offsets.size());
        instance_buffer_offsets[current_frame] = 0;
        instance_uploads[current_frame] = 0;
        instance_buffer_requests = 0; //retain for compatibility
    }

    Buffer& Vulkan_Buffer_Manager::get_uniform_buffer(const uint32_t current_frame)
    {
        assert(current_frame < uniform_buffers.size());
        return uniform_buffers[current_frame];
    }

    /// <summary>
    /// Get an instance buffer with at least instance_size * instance_count capacity.
    /// </summary>
    /// <param name="current_frame">Index of the current swap_chain image.</param>
    /// <returns>Reference to an available buffer with at least instance_size * instance_count capacity.</returns>
    size_t Vulkan_Buffer_Manager::get_instance_buffer(const uint32_t current_frame, const VkDeviceSize instance_size, const VkDeviceSize instance_count)
    {
        //Check if there are buffers left or if we need to create a new set
        uint32_t buffers_per_frame = static_cast<uint32_t>(instance_buffers.size()) / swap_chain_image_count;

        if (uint32_t current_buffer_usage = instance_buffer_requests + 1; current_buffer_usage > buffers_per_frame)
        {
            expand_instance_buffers(current_buffer_usage, instance_size * instance_count);
        }

        uint32_t buffer_index = (instance_buffer_requests * swap_chain_image_count) + current_frame;
        instance_buffer_requests++;
        assert(buffer_index < instance_buffers.size());

        //Check if the buffer size has enough capacity 
        if (VkDeviceSize buffer_size = instance_size * instance_count; buffer_size > instance_buffers[buffer_index].size)
        {
            throw std::runtime_error("Per-frame instance upload arena exhausted. Increase Renderer_Configuration::instance_upload_arena_bytes.");
        }

        return buffer_index;
    }

    Buffer& Vulkan_Buffer_Manager::get_instance_buffer(const size_t buffer_index)
    {
        return instance_buffers[buffer_index];
    }

    //Create an instance buffer thats accessable from both the host and device
    void Vulkan_Buffer_Manager::create_uniform_buffers()
    {
        VkDeviceSize buffer_size = sizeof(MVP);

        uniform_buffers.resize(swap_chain_image_count);

        for (size_t i = 0; i < swap_chain_image_count; i++)
        {
            //Also maps pointer to memory so we can write to the buffer, this pointer is persistant throughout the applications lifetime.
            uniform_buffers[i].create(*vulkan_instance, buffer_size,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        }
    }

    Buffer Vulkan_Buffer_Manager::create_instance_buffer(const VkDeviceSize instance_buffer_size)
    {
        VkDeviceSize instance_data_buffer_size = instance_buffer_size;

        Buffer instance_buffer;
        instance_buffer.create(*vulkan_instance, instance_data_buffer_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);

        return instance_buffer;
    }

    void Vulkan_Buffer_Manager::expand_instance_buffers(uint32_t required_buffers_per_frame, VkDeviceSize instance_buffer_size)
    {
        uint32_t required_buffer_count = required_buffers_per_frame * swap_chain_image_count;

        if (instance_buffers.size() >= required_buffer_count)
        {
            return;
        }

        throw std::runtime_error("Instance buffer expansion during a frame is disabled. Increase Renderer_Configuration::instance_upload_arena_bytes.");
    }
}
