#include "pch.h"
#include "vulkan_engine.h"

namespace vulvox
{
    const int Vulkan_Engine::MAX_FRAMES_IN_FLIGHT = 2;


    Vulkan_Engine::Vulkan_Engine() : swap_chain(&vulkan_instance)
    {
    }

    Vulkan_Engine::~Vulkan_Engine()
    {
    }

    void Vulkan_Engine::init_window(uint32_t width, uint32_t height)
    {
        this->width = width;
        this->height = height;

        std::cout << "Init window.." << std::endl;

        glfwInit();

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

        window = glfwCreateWindow(width, height, "Vulkan", nullptr, nullptr);

        //Pass pointer of this to glfw so we can retrieve the object instance in the callback function
        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(window, framebuffer_resize_callback);

        std::cout << "Window initialized." << std::endl;
    }

    void Vulkan_Engine::init_vulkan(const Renderer_Configuration& configuration)
    {
        this->configuration = configuration;
        std::cout << "Init vulkan.." << std::endl;

        vulkan_instance.init_instance();
        vulkan_instance.init_surface(window);
        vulkan_instance.init_device();
        vulkan_instance.init_allocator();

        swap_chain.create_swap_chain(window, vulkan_instance.surface);
        mvp_handler.set_aspect_ratio(static_cast<float>(width) / static_cast<float>(height));

        create_mvp_descriptor_set_layout();
        create_texture_descriptor_set_layout();
        create_graphics_pipeline();
        command_pool = Vulkan_Command_Pool(&vulkan_instance, MAX_FRAMES_IN_FLIGHT);
        create_depth_resources();

        // Initialize buffer manager with preallocation to avoid runtime VMA allocations
        // growth_factor: extra space when resizing; default_buffers_per_frame: number of instance buffers to preallocate per frame
        // default_max_instances: max instances per instance-buffer
        buffer_manager.init(&vulkan_instance, MAX_FRAMES_IN_FLIGHT, configuration.instance_upload_arena_bytes);

        create_descriptor_pool();
        create_descriptor_sets();
        create_sync_objects();
        create_timestamp_queries();

        std::cout << "Vulkan initialized." << std::endl;
    }

    void Vulkan_Engine::init(uint32_t width, uint32_t height, const Renderer_Configuration& configuration)
    {
        init_window(width, height);
        init_vulkan(configuration);
        is_initialized = true;
        fps_sample_start = std::chrono::steady_clock::now();
    }

    void Vulkan_Engine::init_imgui()
    {
        // NOTE: ImGui_Context::ImGui_Context used to take a VkRenderPass. With dynamic rendering
        // there is no render pass object anymore, so this now needs to take the color/depth
        // attachment formats and set ImGui_ImplVulkan_InitInfo::UseDynamicRendering = true plus
        // PipelineRenderingCreateInfo instead. imgui_context.h/.cpp weren't part of this upload,
        // so I couldn't patch them - update that constructor to match this call site (see chat).
        imgui_context = std::make_unique<ImGui_Context>(window, vulkan_instance, swap_chain.image_format, MAX_FRAMES_IN_FLIGHT);
    }

    void Vulkan_Engine::disable_imgui()
    {
        imgui_context.reset();
    }

    ImGui_Context* Vulkan_Engine::get_imgui_context() const
    {
        return imgui_context.get();
    }

    void Vulkan_Engine::destroy()
    {
        if (!is_initialized)
        {
            return;
        }

        //Wait until all operations are completed before cleanup
        vkDeviceWaitIdle(vulkan_instance.device);

        if (imgui_context)
        {
            imgui_context.reset();
        }

        cleanup_swap_chain();

        vkDestroyPipeline(vulkan_instance.device, instance_plane_pipeline, nullptr);
        vkDestroyPipeline(vulkan_instance.device, vertex_pipeline, nullptr);
        vkDestroyPipeline(vulkan_instance.device, instance_pipeline, nullptr);
        vkDestroyPipeline(vulkan_instance.device, instance_tex_array_pipeline, nullptr);

        vkDestroyPipelineLayout(vulkan_instance.device, pipeline_layout, nullptr);

        //Descriptor sets will be destroyed with the pool
        vkDestroyDescriptorPool(vulkan_instance.device, descriptor_pool, nullptr);

        //Texture cleanup
        for (auto& [name, texture] : textures)
        {
            texture.destroy();
        }

        textures.clear();

        for (auto& [name, texture_array] : texture_arrays)
        {
            texture_array.destroy();
        }

        texture_arrays.clear();

        //Cleanup descriptor set layout and buffers
        vkDestroyDescriptorSetLayout(vulkan_instance.device, mvp_descriptor_set_layout, nullptr);
        vkDestroyDescriptorSetLayout(vulkan_instance.device, texture_descriptor_set_layout, nullptr);

        //Clear all the models and their (vertex & index) buffers
        for (auto& [name, model] : models)
        {
            model.destroy();
        }

        models.clear();

        buffer_manager.destroy();

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            vkDestroySemaphore(vulkan_instance.device, image_available_semaphores.at(i), nullptr);
            vkDestroySemaphore(vulkan_instance.device, render_finished_semaphores.at(i), nullptr);
            vkDestroyFence(vulkan_instance.device, in_flight_fences.at(i), nullptr);
        }

        if (timestamp_query_pool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(vulkan_instance.device, timestamp_query_pool, nullptr);
            timestamp_query_pool = VK_NULL_HANDLE;
        }

        command_pool.destroy();

        vulkan_instance.cleanup_allocator();
        vulkan_instance.cleanup_device();
        vulkan_instance.cleanup_surface();
        vulkan_instance.cleanup_instance();

        glfwDestroyWindow(window);

        glfwTerminate();
    }

    GLFWwindow* Vulkan_Engine::get_glfw_window_ptr()
    {
        return window;
    }

    MVP_Handler& Vulkan_Engine::get_mvp_handler()
    {
        return mvp_handler;
    }

    void Vulkan_Engine::resize_window(const uint32_t new_width, const uint32_t new_height)
    {
        this->width = new_width;
        this->height = new_height;

        //Will also call the resize callback so the render engine will recreate the swapchain
        glfwSetWindowSize(get_glfw_window_ptr(), new_width, new_height);
    }

    void Vulkan_Engine::load_model(const std::string& model_name, const std::filesystem::path& path)
    {
        if (models.contains(model_name))
        {
            std::cout << "Attempted to load model " << model_name << " but a model with the same name was already loaded. Path was: " << path << std::endl;
            return;
        }

        auto [model_it, succeeded] = models.try_emplace(model_name, &vulkan_instance, command_pool, path);

        if (!succeeded)
        {
            std::string error_string = "Failed to load model " + model_name + " with path: " + path.string();
            throw std::runtime_error(error_string);
        }
    }

    void Vulkan_Engine::load_mesh(const std::string& mesh_name, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
    {
        if (models.contains(mesh_name))
        {
            throw std::runtime_error("A model or mesh named '" + mesh_name + "' is already loaded.");
        }

        models.emplace(std::piecewise_construct,
            std::forward_as_tuple(mesh_name),
            std::forward_as_tuple(&vulkan_instance, command_pool, vertices, indices));
    }

    void Vulkan_Engine::load_texture(const std::string& texture_name, const std::filesystem::path& path)
    {
        if (textures.contains(texture_name))
        {
            std::cout << "Attempted to load texture " << texture_name << " but a texture with the same name was already loaded. Path was: " << path << std::endl;
            return;
        }

        auto [texture_it, succeeded] = textures.try_emplace(texture_name, Image::create_texture_image(vulkan_instance, command_pool, path));

        if (!succeeded)
        {
            std::string error_string = "Failed to load texture " + texture_name + " with path: " + path.string();
            throw std::runtime_error(error_string);
        }


        auto [texture_descriptor_it, descriptor_succeeded] = texture_descriptor_sets.try_emplace(texture_name, create_texture_descriptor_set(texture_it->second));

        if (!descriptor_succeeded)
        {
            throw std::runtime_error("Failed to allocate descriptor sets! (map allocation failed)");
        }
    }

    void Vulkan_Engine::load_texture_array(const std::string& texture_name, const std::vector<std::filesystem::path>& paths)
    {
        if (texture_arrays.contains(texture_name))
        {
            std::cout << "Attempted to load texture array " << texture_name << " but a texture array with the same name was already loaded." << std::endl;
            return;
        }

        auto [texture_it, succeeded] = texture_arrays.try_emplace(texture_name, Image::create_texture_array_image(vulkan_instance, command_pool, paths));

        if (!succeeded)
        {
            std::string error_string = "Failed to load texture array " + texture_name;
            throw std::runtime_error(error_string);
        }

        auto [texture_descriptor_it, descriptor_succeeded] = texture_array_descriptor_sets.try_emplace(texture_name, create_texture_descriptor_set(texture_it->second));

        if (!descriptor_succeeded)
        {
            throw std::runtime_error("Failed to allocate descriptor sets! (map allocation failed)");
        }

    }

    void Vulkan_Engine::unload_model(const std::string& name)
    {
    }

    void Vulkan_Engine::unload_texture(const std::string& name)
    {
    }

    void Vulkan_Engine::unload_texture_array(const std::string& name)
    {
    }

    void Vulkan_Engine::start_draw()
    {
        cpu_frame_start = std::chrono::steady_clock::now();
        vkWaitForFences(vulkan_instance.device, 1, &in_flight_fences[current_frame], VK_TRUE, UINT64_MAX);
        read_gpu_timestamp(current_frame);

        //Ask the swapchain for a render image to target
        VkResult result = vkAcquireNextImageKHR(vulkan_instance.device, swap_chain.swap_chain, UINT64_MAX, image_available_semaphores[current_frame], nullptr, &current_image_index);

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            //Swap chain in not compatible with the current window size, recreate
            recreate_swap_chain();
            return;
        }
        else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            throw std::runtime_error("Failed to acquire swap chain image!");
        }

        //Reset fence *after* confirming the swapchain is valid (prevents deadlock)
        vkResetFences(vulkan_instance.device, 1, &in_flight_fences[current_frame]);

        //Tell our memory allocator a new frame has started (useful for optimization)
        //The 2nd argument is the current swapchain index (this is badly documented online)
        vmaSetCurrentFrameIndex(vulkan_instance.allocator, current_frame);

        //Reset the per-frame instance buffer cursor
        buffer_manager.begin_frame(current_frame);
        reset_command_state_cache();
        frame_statistics.draw_calls = 0;
        frame_statistics.vertices = 0;
        frame_statistics.indices = 0;
        frame_statistics.queue_submits = 0;
        frame_statistics.pipeline_binds = 0;
        frame_statistics.descriptor_set_binds = 0;

        //Update global variables (camera etc.)
        update_uniform_buffer();

        //Start recording a new command buffer for rendering
        current_command_buffer = command_pool.reset_command_buffer(current_frame);

        // Start recording the command buffer and wait for draw calls
        start_record_command_buffer();

        if (imgui_context)
        {
            imgui_context->start_imgui_frame();
        }

    }

    void Vulkan_Engine::end_draw()
    {
        if (imgui_context)
        {
            imgui_context->render_and_end_imgui_frame(current_command_buffer);
        }

        //Complete the command buffer before submitting it and presenting the image
        end_record_command_buffer();

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        //Which semaphore to wait for before execution and at which stage
        std::array<VkSemaphore, 1> wait_semaphores = { image_available_semaphores[current_frame] };
        std::array<VkPipelineStageFlags, 1> wait_stages = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submit_info.waitSemaphoreCount = static_cast<uint32_t>(wait_semaphores.size());
        submit_info.pWaitSemaphores = wait_semaphores.data();
        submit_info.pWaitDstStageMask = wait_stages.data();

        //Which semaphore to signal when the command buffer is done
        std::array<VkSemaphore, 1> signal_semaphores = { render_finished_semaphores[current_frame] };
        submit_info.signalSemaphoreCount = static_cast<uint32_t>(signal_semaphores.size());
        submit_info.pSignalSemaphores = signal_semaphores.data();

        //Link command buffer
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &current_command_buffer;

        //Submit the command buffer so the GPU starts executing it
        if (vkQueueSubmit(vulkan_instance.graphics_queue, 1, &submit_info, in_flight_fences[current_frame]) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to submit draw command buffer!");
        }
        frame_statistics.queue_submits++;

        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = signal_semaphores.data(); //Wait for semaphore before we can present

        std::array<VkSwapchainKHR, 1> swap_chains = { swap_chain.swap_chain };
        present_info.swapchainCount = static_cast<uint32_t>(swap_chains.size());
        present_info.pSwapchains = swap_chains.data();
        present_info.pImageIndices = &current_image_index;

        //Dont need to collect the draw results because the present function returns them as well
        present_info.pResults = nullptr;

        //Present the result on screen
        VkResult result = vkQueuePresentKHR(vulkan_instance.present_queue, &present_info);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebuffer_resized)
        {
            framebuffer_resized = false;

            //Swap chain in not compatible with the current window size, recreate
            recreate_swap_chain();
        }
        else if (result != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to present swap chain image!");
        }

        //Rotate to next frame resources
        frame_statistics.host_buffer_uploads = 1 + buffer_manager.get_frame_uploads(current_frame);
        frame_statistics.cpu_frame_time_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpu_frame_start).count();
        ++fps_sample_frame_count;
        const double sample_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - fps_sample_start).count();
        if (sample_seconds >= 0.5)
        {
            frame_statistics.frames_per_second = fps_sample_frame_count / sample_seconds;
            frame_statistics.average_frame_time_ms = 1000.0 / frame_statistics.frames_per_second;
            fps_sample_start = std::chrono::steady_clock::now();
            fps_sample_frame_count = 0;
        }
        current_frame = (current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    void Vulkan_Engine::draw_model(const std::string& model_name, const std::string& texture_name, const glm::mat4& model_matrix)
    {
        if (!models.contains(model_name))
        {
            std::cout << "No model with name " << model_name << " is loaded, skipping draw call." << std::endl;
            return;
        }

        if (!textures.contains(texture_name))
        {
            std::cout << "No texture with name " << texture_name << " is loaded, skipping draw call." << std::endl;
            return;
        }

        //Set the vertex buffers
        std::array<VkDeviceSize, 1> offsets = { 0 };
        bind_vertex_buffer(0, models.at(model_name).vertex_buffer.buffer, 0);

        //Set the index buffers
        bind_index_buffer(models.at(model_name).index_buffer.buffer);

        //Bind the uniform buffers
        //Bind set 0, the MVP buffer
        bind_descriptor_set(0, descriptor_sets.tri_descriptor_set[current_frame]);
        //Bind set 1, the texture
        bind_descriptor_set(1, texture_descriptor_sets.at(texture_name));

        //Set the push constants (model matrix)
        vkCmdPushConstants(current_command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &model_matrix);

        //Bind to graphics pipeline: The shaders and configuration used to the render the object
        bind_pipeline(vertex_pipeline);

        //Draw command, set vertex and instance counts (we're not using instancing here) and indices
        vkCmdDrawIndexed(current_command_buffer, models.at(model_name).index_count, 1, 0, 0, 0);
        frame_statistics.draw_calls++;
        frame_statistics.indices += models.at(model_name).index_count;
    }

    void Vulkan_Engine::draw_mesh(const std::string& mesh_name, const std::string& texture_name, const glm::mat4& model_matrix)
    {
        draw_model(mesh_name, texture_name, model_matrix);
    }

    void Vulkan_Engine::draw_model_with_texture_array(const std::string& model_name, const std::string& texture_array_name, const int texture_index, const glm::mat4& model_matrix)
    {
        if (true)
        {
            std::cout << "draw_model_with_texture_array is not yet supported." << std::endl;
            return;
        }

        if (!models.contains(model_name))
        {
            std::cout << "No model with name " << model_name << " is loaded, skipping draw call." << std::endl;
            return;
        }

        if (!texture_arrays.contains(texture_array_name))
        {
            std::cout << "No texture array with name " << texture_array_name << " is loaded, skipping draw call." << std::endl;
            return;
        }

        std::array<VkDeviceSize, 1> offsets = { 0 };

        //Bind the uniform buffers
        //Bind set 0, the MVP buffer
        bind_descriptor_set(0, descriptor_sets.tri_descriptor_set[current_frame]);
        //Bind set 1, the texture
        bind_descriptor_set(1, texture_array_descriptor_sets.at(texture_array_name));

        //Binding point 0 - mesh vertex buffer
        bind_vertex_buffer(0, models.at(model_name).vertex_buffer.buffer, 0);

        ////Binding point 1 - instance data buffer
        //vkCmdBindVertexBuffers(current_command_buffer, 1, 1, &instance_data_buffers[current_frame].buffer, offsets.data());

        ////Binding point 2 - texture array index buffer
        //vkCmdBindVertexBuffers(current_command_buffer, 2, 1, &instance_texture_index_buffers[current_frame].buffer, offsets.data());

        //Set the push constants (model matrix)
        vkCmdPushConstants(current_command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &model_matrix);

        //Bind index buffer
        bind_index_buffer(models.at(model_name).index_buffer.buffer);

        bind_pipeline(vertex_pipeline);

        //Draw command, set vertex and instance counts (we're not using instancing here) and indices
        vkCmdDrawIndexed(current_command_buffer, models.at(model_name).index_count, 1, 0, 0, 0);
        frame_statistics.draw_calls++;
        frame_statistics.indices += models.at(model_name).index_count;
    }

    void Vulkan_Engine::draw_instanced(const std::string& model_name, const std::string& texture_name, const std::vector<glm::mat4>& model_matrices)
    {
        if (!models.contains(model_name))
        {
            std::cout << "No model with name " << model_name << " is loaded, skipping draw call." << std::endl;
            return;
        }

        if (!textures.contains(texture_name))
        {
            std::cout << "No texture with name " << texture_name << " is loaded, skipping draw call." << std::endl;
            return;
        }

        std::array<VkDeviceSize, 1> offsets = { 0 };


        auto model_matrices_ref = buffer_manager.copy_to_instance_buffer(vulkan_instance, current_frame, model_matrices);

        //Bind the uniform buffers
        //Bind set 0, the MVP buffer
        bind_descriptor_set(0, descriptor_sets.instance_descriptor_set[current_frame]);
        //Bind set 1, the texture
        bind_descriptor_set(1, texture_descriptor_sets.at(texture_name));

        //Binding point 0 - mesh vertex buffer
        bind_vertex_buffer(0, models.at(model_name).vertex_buffer.buffer, 0);

        //Binding point 1 - instance data buffer (with offset returned by the buffer manager)
        VkDeviceSize instance_offset = model_matrices_ref.offset;
        VkBuffer instance_buf = buffer_manager.get_instance_buffer(model_matrices_ref.buffer_index).buffer;
        bind_vertex_buffer(1, instance_buf, instance_offset);

        //Bind index buffer
        bind_index_buffer(models.at(model_name).index_buffer.buffer);

        bind_pipeline(instance_pipeline);

        //Render instances
        uint32_t instance_count = static_cast<uint32_t>(model_matrices.size());
        vkCmdDrawIndexed(current_command_buffer, models.at(model_name).index_count, instance_count, 0, 0, 0);
        frame_statistics.draw_calls++;
        frame_statistics.indices += static_cast<uint64_t>(models.at(model_name).index_count) * instance_count;
    }

    void Vulkan_Engine::draw_instanced_with_texture_array(const std::string& model_name, const std::string& texture_array_name, const std::vector<glm::mat4>& model_matrices, const std::vector<uint32_t>& texture_indices)
    {
        if (!models.contains(model_name))
        {
            std::cout << "No model with name " << model_name << " is loaded, skipping draw call." << std::endl;
            return;
        }

        if (!texture_arrays.contains(texture_array_name))
        {
            std::cout << "No texture array with name " << texture_array_name << " is loaded, skipping draw call." << std::endl;
            return;
        }

        std::array<VkDeviceSize, 1> offsets = { 0 };

        auto model_matrices_ref = buffer_manager.copy_to_instance_buffer(vulkan_instance, current_frame, model_matrices);
        auto texture_index_ref = buffer_manager.copy_to_instance_buffer(vulkan_instance, current_frame, texture_indices);

        //Bind the uniform buffers
        //Bind set 0, the MVP buffer
        bind_descriptor_set(0, descriptor_sets.instance_descriptor_set[current_frame]);
        //Bind set 1, the textures
        bind_descriptor_set(1, texture_array_descriptor_sets.at(texture_array_name));

        //Binding point 0 - mesh vertex buffer
        bind_vertex_buffer(0, models.at(model_name).vertex_buffer.buffer, 0);

        //Binding point 1 - instance data buffer
        VkDeviceSize instance_offset = model_matrices_ref.offset;
        VkBuffer instance_buf = buffer_manager.get_instance_buffer(model_matrices_ref.buffer_index).buffer;
        bind_vertex_buffer(1, instance_buf, instance_offset);

        //Binding point 2 - texture array index buffer
        VkDeviceSize tex_idx_offset = texture_index_ref.offset;
        VkBuffer tex_idx_buf = buffer_manager.get_instance_buffer(texture_index_ref.buffer_index).buffer;
        bind_vertex_buffer(2, tex_idx_buf, tex_idx_offset);

        //Bind index buffer
        bind_index_buffer(models.at(model_name).index_buffer.buffer);

        bind_pipeline(instance_tex_array_pipeline);

        //Render instances
        uint32_t instance_count = static_cast<uint32_t>(model_matrices.size());
        vkCmdDrawIndexed(current_command_buffer, models.at(model_name).index_count, instance_count, 0, 0, 0);
        frame_statistics.draw_calls++;
        frame_statistics.indices += static_cast<uint64_t>(models.at(model_name).index_count) * instance_count;
    }

    void Vulkan_Engine::draw_planes(const std::string& texture_array_name, const std::vector<glm::mat4>& model_matrices, const std::vector<uint32_t>& texture_indices, const std::vector<glm::vec4>& min_max_uvs)
    {
        if (!texture_arrays.contains(texture_array_name))
        {
            std::cout << "No texture array with name " << texture_array_name << " is loaded, skipping draw call." << std::endl;
            return;
        }

        auto model_matrices_ref = buffer_manager.copy_to_instance_buffer(vulkan_instance, current_frame, model_matrices);
        auto texture_index_ref = buffer_manager.copy_to_instance_buffer(vulkan_instance, current_frame, texture_indices);
        auto min_max_uv_ref = buffer_manager.copy_to_instance_buffer(vulkan_instance, current_frame, min_max_uvs);

        std::array<VkDeviceSize, 1> offsets = { 0 };

        //Bind the uniform buffers
        //Bind set 0, the MVP buffer
        bind_descriptor_set(0, descriptor_sets.instance_descriptor_set[current_frame]);
        //Bind set 1, the textures
        bind_descriptor_set(1, texture_array_descriptor_sets.at(texture_array_name));

        //Binding point 1 - instance data buffer
        VkDeviceSize instance_offset = model_matrices_ref.offset;
        VkBuffer instance_buf = buffer_manager.get_instance_buffer(model_matrices_ref.buffer_index).buffer;
        bind_vertex_buffer(1, instance_buf, instance_offset);

        //Binding point 2 - texture array index buffer
        VkDeviceSize tex_idx_offset = texture_index_ref.offset;
        VkBuffer tex_idx_buf = buffer_manager.get_instance_buffer(texture_index_ref.buffer_index).buffer;
        bind_vertex_buffer(2, tex_idx_buf, tex_idx_offset);

        //Binding point 3 - texture min max uvs
        VkDeviceSize uv_offset = min_max_uv_ref.offset;
        VkBuffer uv_buf = buffer_manager.get_instance_buffer(min_max_uv_ref.buffer_index).buffer;
        bind_vertex_buffer(3, uv_buf, uv_offset);

        bind_pipeline(instance_plane_pipeline);

        //Render instances
        uint32_t instance_count = static_cast<uint32_t>(model_matrices.size());
        vkCmdDraw(current_command_buffer, 6, instance_count, 0, 0);
        frame_statistics.draw_calls++;
        frame_statistics.vertices += 6ull * instance_count;
    }

    bool Vulkan_Engine::initialized() const
    {
        return is_initialized;
    }

    std::string Vulkan_Engine::get_memory_statistics() const
    {
        return vulkan_instance.get_memory_statistics();
    }

    const Frame_Statistics& Vulkan_Engine::get_frame_statistics() const
    {
        return frame_statistics;
    }

    void Vulkan_Engine::reset_command_state_cache()
    {
        command_state_cache = {};
    }

    void Vulkan_Engine::bind_pipeline(const VkPipeline pipeline)
    {
        if (command_state_cache.pipeline == pipeline)
        {
            return;
        }

        vkCmdBindPipeline(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        command_state_cache.pipeline = pipeline;
        ++frame_statistics.pipeline_binds;
    }

    void Vulkan_Engine::bind_descriptor_set(const uint32_t set_index, const VkDescriptorSet descriptor_set)
    {
        if (command_state_cache.descriptor_sets[set_index] == descriptor_set)
        {
            return;
        }

        vkCmdBindDescriptorSets(current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, set_index, 1, &descriptor_set, 0, nullptr);
        command_state_cache.descriptor_sets[set_index] = descriptor_set;
        ++frame_statistics.descriptor_set_binds;
    }

    void Vulkan_Engine::bind_vertex_buffer(const uint32_t binding, const VkBuffer buffer, const VkDeviceSize offset)
    {
        if (command_state_cache.vertex_buffers[binding] == buffer && command_state_cache.vertex_offsets[binding] == offset)
        {
            return;
        }

        vkCmdBindVertexBuffers(current_command_buffer, binding, 1, &buffer, &offset);
        command_state_cache.vertex_buffers[binding] = buffer;
        command_state_cache.vertex_offsets[binding] = offset;
    }

    void Vulkan_Engine::bind_index_buffer(const VkBuffer buffer)
    {
        if (command_state_cache.index_buffer == buffer)
        {
            return;
        }

        vkCmdBindIndexBuffer(current_command_buffer, buffer, 0, VK_INDEX_TYPE_UINT32);
        command_state_cache.index_buffer = buffer;
    }

    void Vulkan_Engine::update_uniform_buffer()
    {
        Buffer& uniform_buffer = buffer_manager.get_uniform_buffer(current_frame);
        uniform_buffer.copy_to_buffer(vulkan_instance, mvp_handler.model_view_projection);
    }

    void Vulkan_Engine::recreate_swap_chain()
    {
        int new_width = 0;
        int new_height = 0;
        glfwGetFramebufferSize(window, &new_width, &new_height);

        while (new_width == 0 || new_height == 0) {
            glfwGetFramebufferSize(window, &new_width, &new_height);
            glfwWaitEvents();
        }

        width = new_width;
        height = new_height;

        vkDeviceWaitIdle(vulkan_instance.device);

        cleanup_swap_chain();

        swap_chain.create_swap_chain(window, vulkan_instance.surface);

        mvp_handler.set_aspect_ratio(static_cast<float>(width) / static_cast<float>(height));

        create_depth_resources(); //Depend on depth image. No framebuffers to recreate anymore (dynamic rendering).
    }

    void Vulkan_Engine::cleanup_swap_chain()
    {
        //Destroy objects that depend on the swap chain
        depth_image.destroy();

        //Destroy the swap chain
        swap_chain.cleanup_swap_chain();
    }

    void Vulkan_Engine::create_graphics_pipeline()
    {
        //Define push constants, the model matrix for single rendering is updated using push constants
        VkPushConstantRange push_constant_range{};
        push_constant_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push_constant_range.offset = 0;
        push_constant_range.size = sizeof(glm::mat4);

        //Define global variables (like a MVP matrix)
        //These are defined in a seperate pipeline layout
        std::array<VkDescriptorSetLayout, 2> set_layouts = { mvp_descriptor_set_layout, texture_descriptor_set_layout };

        VkPipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size()); //Amount of descriptor set layouts (uniform buffer layouts)
        pipeline_layout_info.pSetLayouts = set_layouts.data();
        pipeline_layout_info.pushConstantRangeCount = 1; //Small uniform data, used for the model matrix
        pipeline_layout_info.pPushConstantRanges = &push_constant_range;

        if (vkCreatePipelineLayout(vulkan_instance.device, &pipeline_layout_info, nullptr, &pipeline_layout) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create pipeline layout!");
        }

        //Load compiled SPIR-V shader files 
        std::filesystem::path vert_shader_filepath("../shaders/vert.spv");
        std::filesystem::path frag_shader_filepath("../shaders/frag.spv");

        std::filesystem::path instance_vert_shader_filepath("../shaders/instance_vert.spv");
        std::filesystem::path instance_frag_shader_filepath("../shaders/instance_frag.spv");

        std::filesystem::path instance_tex_array_vert_shader_filepath("../shaders/instance_tex_array_vert.spv");
        std::filesystem::path instance_tex_array_frag_shader_filepath("../shaders/instance_tex_array_frag.spv");

        std::filesystem::path instance_plane_vert_shader_filepath("../shaders/instance_plane_vert.spv");

        Vulkan_Shader vert_shader{ vulkan_instance.device, vert_shader_filepath, "main", VK_SHADER_STAGE_VERTEX_BIT };
        Vulkan_Shader frag_shader{ vulkan_instance.device, frag_shader_filepath, "main", VK_SHADER_STAGE_FRAGMENT_BIT };
        Vulkan_Shader instance_vert_shader{ vulkan_instance.device, instance_vert_shader_filepath, "main", VK_SHADER_STAGE_VERTEX_BIT };
        Vulkan_Shader instance_frag_shader{ vulkan_instance.device, instance_frag_shader_filepath, "main", VK_SHADER_STAGE_FRAGMENT_BIT };
        Vulkan_Shader instance_vert_tex_array_shader{ vulkan_instance.device, instance_tex_array_vert_shader_filepath, "main", VK_SHADER_STAGE_VERTEX_BIT };
        Vulkan_Shader instance_frag_tex_array_shader{ vulkan_instance.device, instance_tex_array_frag_shader_filepath, "main", VK_SHADER_STAGE_FRAGMENT_BIT };
        Vulkan_Shader instance_plane_vert_shader{ vulkan_instance.device, instance_plane_vert_shader_filepath, "main", VK_SHADER_STAGE_VERTEX_BIT };

        //Describes the configuration of the vertices the triangles and lines use
        VkPipelineInputAssemblyStateCreateInfo input_assembly_info{};
        input_assembly_info.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; //Triangle from every three vertices, without reuse
        input_assembly_info.primitiveRestartEnable = VK_FALSE; //Break up lines and triangles in strip mode
        input_assembly_info.flags = 0;

        //Viewport size
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = (float)swap_chain.extent.width;
        viewport.height = (float)swap_chain.extent.height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        //Visible part of the viewport (whole viewport)
        VkRect2D scissor{};
        scissor.offset = { 0,0 };
        scissor.extent = swap_chain.extent;

        //It is usefull to have a non-static viewport and scissor size
        std::vector<VkDynamicState> dynamic_states =
        {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };

        VkPipelineDynamicStateCreateInfo dynamic_state_info{};
        dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_info.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
        dynamic_state_info.pDynamicStates = dynamic_states.data();

        VkPipelineViewportStateCreateInfo viewport_state_info{};
        viewport_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_info.viewportCount = 1;
        viewport_state_info.scissorCount = 1;
        viewport_state_info.flags = 0;

        //Setup rasterizer stage
        VkPipelineRasterizationStateCreateInfo rasterizer_info{};
        rasterizer_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer_info.depthClampEnable = VK_FALSE; //Discard fragments outside of near/far plane (instead of clamping)
        rasterizer_info.rasterizerDiscardEnable = VK_FALSE; //If true, discards all geometery
        rasterizer_info.polygonMode = VK_POLYGON_MODE_FILL; //Full render, switch for wireframe or points (among others)
        rasterizer_info.lineWidth = 1.0f; //wider requires wideLines GPU feature
        rasterizer_info.cullMode = configuration.enable_back_face_culling ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        rasterizer_info.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; //Counter clockwise vertex order determines the front (we flip the y axis)
        rasterizer_info.depthBiasEnable = VK_FALSE;
        rasterizer_info.depthBiasConstantFactor = 0.0f;
        rasterizer_info.depthBiasClamp = 0.0f;
        rasterizer_info.depthBiasSlopeFactor = 0.0f;
        rasterizer_info.flags = 0;

        //Multisample / anti-aliasing stage (disabled)
        VkPipelineMultisampleStateCreateInfo multisampling_info{};
        multisampling_info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling_info.sampleShadingEnable = VK_FALSE;
        multisampling_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        multisampling_info.minSampleShading = 1.0f; // Optional
        multisampling_info.pSampleMask = nullptr; // Optional
        multisampling_info.alphaToCoverageEnable = VK_FALSE; // Optional
        multisampling_info.alphaToOneEnable = VK_FALSE; // Optional
        multisampling_info.flags = 0;

        //Enable depth testing
        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = VK_TRUE; //Discard fragment is they fail depth test
        depth_stencil.depthWriteEnable = VK_TRUE; //Write passed depth tests to the depth buffer
        depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL; //Write closer results to the depth buffer
        depth_stencil.depthBoundsTestEnable = VK_FALSE;
        depth_stencil.minDepthBounds = 0.0f; // Optional
        depth_stencil.maxDepthBounds = 1.0f; // Optional
        depth_stencil.stencilTestEnable = VK_FALSE;
        depth_stencil.front = {}; // Optional
        depth_stencil.back = {}; // Optional
        //depth_stencil.back.compareOp = VK_COMPARE_OP_ALWAYS; //Back face culling


        //Handle color blending of the fragments (for example alpha blending) (disabled)
        VkPipelineColorBlendAttachmentState color_blend_attachement_info{};
        color_blend_attachement_info.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        color_blend_attachement_info.blendEnable = VK_TRUE; //Blend activated for transparency
        //use standard blend equation:
        //Color: SrcColor * SrcAlpha + DstColor * (1 - SrcAlpha)
        color_blend_attachement_info.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        color_blend_attachement_info.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color_blend_attachement_info.colorBlendOp = VK_BLEND_OP_ADD;
        color_blend_attachement_info.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; //Keep source alpha
        color_blend_attachement_info.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        //color_blend_attachement_info.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        //color_blend_attachement_info.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color_blend_attachement_info.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo color_blending_info{};
        color_blending_info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blending_info.logicOpEnable = VK_FALSE; //Disabled
        color_blending_info.logicOp = VK_LOGIC_OP_COPY; //Optional
        color_blending_info.attachmentCount = 1;
        color_blending_info.pAttachments = &color_blend_attachement_info;
        color_blending_info.blendConstants[0] = 0.0f; //Optional
        color_blending_info.blendConstants[1] = 0.0f; //Optional
        color_blending_info.blendConstants[2] = 0.0f; //Optional
        color_blending_info.blendConstants[3] = 0.0f; //Optional


        //Some parts of the pipeline can be dynamic, we define these parts here
        //std::array<VkPipelineShaderStageCreateInfo, 2> shader_stages_info = { vert_shader_stage_info, frag_shader_stage_info };
        std::array<VkPipelineShaderStageCreateInfo, 2> shader_stages_info;

        //Describe the format of the vertex and instance data
        std::vector<VkVertexInputBindingDescription> binding_descriptions =
        {
            //Binding point 0: Mesh vertex layout description at per-vertex rate
            Vertex::get_binding_description(0),
            //Binding point 1: Instanced data at per-instance rate
            Instance_Data::get_binding_description(1),
            //Binding point 2: Texture array index
            Texture_Array_Index_Binding::get_binding_description(2),
        };

        //Vertex attribute bindings
        //Note that the shader declaration for per-vertex and per-instance attributes is the same, the different input rates are only stored in the bindings:
            //	layout (location = 0) in vec3 in_position;		Per-Vertex
            //	...
            //	layout (location = 3) in vec3 instance_position;	Per-Instance
        std::vector<VkVertexInputAttributeDescription> attribute_descriptions;

        //Per-vertex attributes
        //These are advanced for each vertex fetched by the vertex shader
        for (const auto& attribute_desc : Vertex::get_attribute_descriptions(0))
        {
            attribute_descriptions.push_back(attribute_desc);
        }

        //Per-Instance attributes
        //These are advanced for each instance rendered
        for (const auto& attribute_desc : Instance_Data::get_attribute_descriptions(1))
        {
            attribute_descriptions.push_back(attribute_desc);
        }

        attribute_descriptions.push_back(Texture_Array_Index_Binding::get_attribute_description(2));

        VkPipelineVertexInputStateCreateInfo vertex_input_state_info{};
        vertex_input_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        vertex_input_state_info.pVertexBindingDescriptions = binding_descriptions.data(); //spacing between data and per vertex or per instance
        vertex_input_state_info.pVertexAttributeDescriptions = attribute_descriptions.data(); //attribute type, which bindings to load, and offset

        //Dynamic rendering (Vulkan 1.3 core): pipelines declare the attachment formats they'll be
        //used with directly, instead of being tied to a VkRenderPass + subpass index. This struct
        //is chained via pNext and must stay alive for all vkCreateGraphicsPipelines calls below.
        VkFormat color_attachment_format = swap_chain.image_format;
        VkFormat depth_attachment_format = vulkan_instance.find_depth_format();

        VkPipelineRenderingCreateInfo pipeline_rendering_info{};
        pipeline_rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        pipeline_rendering_info.colorAttachmentCount = 1;
        pipeline_rendering_info.pColorAttachmentFormats = &color_attachment_format;
        pipeline_rendering_info.depthAttachmentFormat = depth_attachment_format;

        //Combine the pipeline stages, we change the input and shader stages for the instance and vertex pipelines
        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &pipeline_rendering_info;
        pipeline_info.pVertexInputState = &vertex_input_state_info; //Differ for the instance and vertex rendering
        pipeline_info.pInputAssemblyState = &input_assembly_info;
        pipeline_info.pViewportState = &viewport_state_info;
        pipeline_info.pRasterizationState = &rasterizer_info;
        pipeline_info.pMultisampleState = &multisampling_info;
        pipeline_info.pDepthStencilState = &depth_stencil;
        pipeline_info.pColorBlendState = &color_blending_info;
        pipeline_info.pDynamicState = &dynamic_state_info;

        pipeline_info.stageCount = 2; //Vert & Frag shader stages
        pipeline_info.pStages = shader_stages_info.data(); //Differ for the instance and vertex rendering

        pipeline_info.layout = pipeline_layout;
        pipeline_info.renderPass = VK_NULL_HANDLE; //No render pass with dynamic rendering
        pipeline_info.subpass = 0;
        //This pipeline doesn't derive from another 
        //(set VK_PIPELINE_CREATE_DERIVATIVE_BIT, if we want to derive from another)    
        pipeline_info.basePipelineHandle = nullptr;
        pipeline_info.basePipelineIndex = -1;

        ///Per instance pipeline
        VkPipelineShaderStageCreateInfo instance_vert_shader_stage_info = instance_vert_shader.get_shader_stage_create_info();
        VkPipelineShaderStageCreateInfo instance_frag_shader_stage_info = instance_frag_shader.get_shader_stage_create_info();

        //Use instance vert and frag shaders
        shader_stages_info[0] = instance_vert_shader_stage_info;
        shader_stages_info[1] = instance_frag_shader_stage_info;

        //The instance pipeline uses the input bindings and attribute descriptions except for the texture array index
        vertex_input_state_info.vertexBindingDescriptionCount = 2;
        vertex_input_state_info.vertexAttributeDescriptionCount = 7;

        if (vkCreateGraphicsPipelines(vulkan_instance.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &instance_pipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create instance graphics pipeline!");
        }

        //Per instance pipeline with texture array support
        VkPipelineShaderStageCreateInfo instance_texture_array_vert_shader_stage_info = instance_vert_tex_array_shader.get_shader_stage_create_info();
        VkPipelineShaderStageCreateInfo instance_texture_array_frag_shader_stage_info = instance_frag_tex_array_shader.get_shader_stage_create_info();

        //Use instance vert and frag shaders
        shader_stages_info[0] = instance_texture_array_vert_shader_stage_info;
        shader_stages_info[1] = instance_texture_array_frag_shader_stage_info;

        //The instance pipeline uses all the input bindings and attribute descriptions
        vertex_input_state_info.vertexBindingDescriptionCount = static_cast<uint32_t>(binding_descriptions.size());
        vertex_input_state_info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribute_descriptions.size());

        if (vkCreateGraphicsPipelines(vulkan_instance.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &instance_tex_array_pipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create instance with tex array graphics pipeline!");
        }

        ///Per vertex pipeline
        VkPipelineShaderStageCreateInfo vert_shader_stage_info = vert_shader.get_shader_stage_create_info();
        VkPipelineShaderStageCreateInfo frag_shader_stage_info = frag_shader.get_shader_stage_create_info();

        //Use vertex vert and frag shaders
        shader_stages_info[0] = vert_shader_stage_info;
        shader_stages_info[1] = frag_shader_stage_info;

        //The vertex pipeline only uses the non-instanced input bindings and attribute descriptions
        //(we pass the same list but only look at the vertex specific ones)
        vertex_input_state_info.vertexBindingDescriptionCount = 1;
        vertex_input_state_info.vertexAttributeDescriptionCount = 3;

        if (vkCreateGraphicsPipelines(vulkan_instance.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &vertex_pipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create vertex graphics pipeline!");
        }

        ///Plane pipeline

        //Instance data format for planes
        std::vector<VkVertexInputBindingDescription> plane_binding_descriptions =
        {
            //Binding point 1: Instanced data at per-instance rate
            Instance_Data::get_binding_description(1),
            //Binding point 2: Texture array index
            Texture_Array_Index_Binding::get_binding_description(2),
            //Binding point 3: Instanced plane data (min max uvs)
            Plane_Instance_Data::get_binding_description(3)
        };

        //Per instance attributes for plane rendering
        std::vector<VkVertexInputAttributeDescription> plane_attribute_descriptions;

        for (const auto& i : Instance_Data::get_attribute_descriptions(1))
        {
            plane_attribute_descriptions.push_back(i);
        }

        plane_attribute_descriptions.push_back(Texture_Array_Index_Binding::get_attribute_description(2));


        for (const auto& i : Plane_Instance_Data::get_attribute_descriptions(3))
        {
            plane_attribute_descriptions.push_back(i);
        }

        VkPipelineShaderStageCreateInfo plane_vert_shader_stage_info = instance_plane_vert_shader.get_shader_stage_create_info();
        //Re-use the frag shader for instance with texture arrays
        VkPipelineShaderStageCreateInfo plane_frag_shader_stage_info = instance_frag_tex_array_shader.get_shader_stage_create_info();

        //The plane shader uses different bindings than the other shaders so we set them here
        vertex_input_state_info.pVertexBindingDescriptions = plane_binding_descriptions.data(); //spacing between data and per vertex or per instance
        vertex_input_state_info.pVertexAttributeDescriptions = plane_attribute_descriptions.data(); //attribute type, which bindings to load, and offset

        shader_stages_info[0] = plane_vert_shader_stage_info;
        shader_stages_info[1] = plane_frag_shader_stage_info;

        vertex_input_state_info.vertexBindingDescriptionCount = static_cast<uint32_t>(plane_binding_descriptions.size());
        vertex_input_state_info.vertexAttributeDescriptionCount = static_cast<uint32_t>(plane_attribute_descriptions.size());

        if (vkCreateGraphicsPipelines(vulkan_instance.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &instance_plane_pipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create plane graphics pipeline!");
        }
    }

    void Vulkan_Engine::create_depth_resources()
    {
        VkFormat depth_format = vulkan_instance.find_depth_format();

        depth_image.create_image(
            &vulkan_instance,
            swap_chain.extent.width, swap_chain.extent.height,
            depth_format,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT,
            VMA_MEMORY_USAGE_AUTO);

        depth_image.create_image_view();

        //With dynamic rendering there's no render pass to do this for us anymore, so we transition
        //the depth image into its attachment layout once, right after creation. Because every frame
        //clears it (LOAD_OP_CLEAR) and doesn't care about preserving it (STORE_OP_DONT_CARE), it
        //never needs to leave this layout again - no per-frame depth barrier required.
        VkCommandBuffer command_buffer = command_pool.begin_single_time_commands();

        Image::cmd_transition_image_layout(command_buffer, depth_image.image, depth_image.aspect_flags, 1,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        command_pool.end_single_time_commands(command_buffer);
        depth_image.current_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    /// <summary>
    /// This function creates a descriptor pool that holds the descriptor sets.
    /// Because this renderers usecase will not involve a lot of models we just create a single large pool.
    /// </summary>
    void Vulkan_Engine::create_descriptor_pool()
    {
        std::array<VkDescriptorPoolSize, 2> pool_sizes{};

        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        pool_sizes[0].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) * 2;

        //Limits for a common GPU are >1mil, for our use we probably wont exceed 512 textures 
        //(if we do we could consider making a manager class in the future)
        pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_sizes[1].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) * 512;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        pool_info.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) + 510; // Just allocate a bunch so we can load multiple models, can make dynamic later.
        pool_info.flags = 0;

        if (vkCreateDescriptorPool(vulkan_instance.device, &pool_info, nullptr, &descriptor_pool) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create descriptor pool!");
        }
    }

    /// <summary>
    /// Creates the descriptor sets that describes the layout of the mvp matrix data binding in our pipeline.
    /// </summary>
    void Vulkan_Engine::create_mvp_descriptor_set_layout()
    {
        //Layout binding for the uniform buffer
        VkDescriptorSetLayoutBinding mvp_layout_binding{};
        mvp_layout_binding.binding = 0; //Same as in shader
        mvp_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        mvp_layout_binding.descriptorCount = 1;
        mvp_layout_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; //This buffer is (only) referenced in the vertex stage
        mvp_layout_binding.pImmutableSamplers = nullptr; //Optional

        //Bind the layout binding in the single descriptor set layout
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1;
        layout_info.pBindings = &mvp_layout_binding;

        if (vkCreateDescriptorSetLayout(vulkan_instance.device, &layout_info, nullptr, &mvp_descriptor_set_layout) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to created descriptor set layout!");
        }
    }

    /// <summary>
    /// Creates the descriptor sets that describes the layout of the texture binding in our pipeline.
    /// </summary>
    void Vulkan_Engine::create_texture_descriptor_set_layout()
    {
        //Layout binding for the image sampler
        VkDescriptorSetLayoutBinding sampler_layout_binding{};
        sampler_layout_binding.binding = 1; //Same as in shader
        sampler_layout_binding.descriptorCount = 1;
        sampler_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_layout_binding.pImmutableSamplers = nullptr;
        sampler_layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; //Connect to fragment shader stage

        //Bind the layout binding in the single descriptor set layout
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1;
        layout_info.pBindings = &sampler_layout_binding;

        if (vkCreateDescriptorSetLayout(vulkan_instance.device, &layout_info, nullptr, &texture_descriptor_set_layout) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to created descriptor set layout!");
        }
    }

    /// <summary>
    /// Allocates the mvp descriptor sets for all shaders.
    /// The MVP buffer will never change (only its data will) so we write it here as well.
    /// </summary>
    void Vulkan_Engine::create_descriptor_sets()
    {
        std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, mvp_descriptor_set_layout);

        VkDescriptorSetAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate_info.descriptorPool = descriptor_pool;
        allocate_info.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
        allocate_info.pSetLayouts = layouts.data(); //mvp uniform layout

        ///Triangle descriptor sets
        descriptor_sets.tri_descriptor_set.resize(MAX_FRAMES_IN_FLIGHT);

        if (VkResult result = vkAllocateDescriptorSets(vulkan_instance.device, &allocate_info, descriptor_sets.tri_descriptor_set.data()); result != VK_SUCCESS)
        {
            std::string error_string{ string_VkResult(result) };
            throw std::runtime_error("Failed to allocate descriptor sets! " + error_string);
        }

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            VkDescriptorBufferInfo buffer_info{};
            buffer_info.buffer = buffer_manager.get_uniform_buffer(i).buffer;
            buffer_info.offset = 0;
            buffer_info.range = sizeof(MVP);

            //Tri shaders
            VkWriteDescriptorSet descriptor_write{};

            //Descriptor for the MVP uniform
            descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptor_write.dstSet = descriptor_sets.tri_descriptor_set[i]; //Binding to update
            descriptor_write.dstBinding = 0; //Binding index equal to shader binding index
            descriptor_write.dstArrayElement = 0; //Index of array data to update, no array so zero
            descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptor_write.descriptorCount = 1; //A single buffer info struct
            descriptor_write.pBufferInfo = &buffer_info; //Data and layout of buffer
            descriptor_write.pImageInfo = nullptr; //Optional, only used for image data
            descriptor_write.pTexelBufferView = nullptr; //Optional, only used for buffers views (for tex buffers)

            //Apply updates, Only write descriptor, no copy, so copy variable is 0
            vkUpdateDescriptorSets(vulkan_instance.device, 1, &descriptor_write, 0, nullptr);
        }

        ///Instance descriptor sets
        descriptor_sets.instance_descriptor_set.resize(MAX_FRAMES_IN_FLIGHT);

        if (VkResult result = vkAllocateDescriptorSets(vulkan_instance.device, &allocate_info, descriptor_sets.instance_descriptor_set.data()); result != VK_SUCCESS)
        {
            std::string error_string{ string_VkResult(result) };
            throw std::runtime_error("Failed to allocate descriptor sets! " + error_string);
        }

        //TODO: Do we need this or can we just use the above one for all shaders?
        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            VkDescriptorBufferInfo buffer_info{};
            buffer_info.buffer = buffer_manager.get_uniform_buffer(i).buffer;
            buffer_info.offset = 0;
            buffer_info.range = sizeof(MVP);

            VkWriteDescriptorSet descriptor_write{};

            //Descriptor for the MVP uniform
            descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptor_write.dstSet = descriptor_sets.instance_descriptor_set[i]; //Binding to update
            descriptor_write.dstBinding = 0; //Binding index equal to shader binding index
            descriptor_write.dstArrayElement = 0; //Index of array data to update, no array so zero
            descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptor_write.descriptorCount = 1; //A single buffer info struct
            descriptor_write.pBufferInfo = &buffer_info; //Data and layout of buffer
            descriptor_write.pImageInfo = nullptr; //Optional, only used for image data
            descriptor_write.pTexelBufferView = nullptr; //Optional, only used for buffers views (for tex buffers)

            //Apply updates
            vkUpdateDescriptorSets(vulkan_instance.device, 1, &descriptor_write, 0, nullptr);
        }
    }

    /// <summary>
    /// Allocates a new descriptor for the texture sampler uniform in the triangle shader to point to the given texture
    /// </summary>
    /// <param name="texture_name">Name of the texture to create a descriptor for</param>
    VkDescriptorSet Vulkan_Engine::create_texture_descriptor_set(const Image& texture)
    {
        VkDescriptorSet new_descriptor_set{};

        VkDescriptorSetAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate_info.descriptorPool = descriptor_pool;
        allocate_info.descriptorSetCount = 1;
        allocate_info.pSetLayouts = &texture_descriptor_set_layout; //texture uniform layout

        ///Triangle descriptor sets
        descriptor_sets.tri_descriptor_set.resize(MAX_FRAMES_IN_FLIGHT);
        if (vkAllocateDescriptorSets(vulkan_instance.device, &allocate_info, &new_descriptor_set) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate descriptor sets!");
        }

        VkDescriptorImageInfo image_info{};
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        image_info.imageView = texture.image_view;
        image_info.sampler = texture.sampler;

        VkWriteDescriptorSet descriptor_write{};

        //Descriptor for the image sampler uniform
        descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptor_write.dstSet = new_descriptor_set; //Binding to update
        descriptor_write.dstBinding = 1; //Binding index equal to shader binding index
        descriptor_write.dstArrayElement = 0; //Index of array data to update, no array so zero
        descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptor_write.descriptorCount = 1; //A single buffer info struct
        descriptor_write.pImageInfo = &image_info; //This is an image sampler so we provide an image data description

        //Apply updates, only write descriptor, no copy, so copy variable is 0
        vkUpdateDescriptorSets(vulkan_instance.device, 1, &descriptor_write, 0, nullptr);

        return new_descriptor_set;
    }

    void Vulkan_Engine::create_sync_objects()
    {
        image_available_semaphores.resize(MAX_FRAMES_IN_FLIGHT);
        render_finished_semaphores.resize(MAX_FRAMES_IN_FLIGHT);
        in_flight_fences.resize(MAX_FRAMES_IN_FLIGHT);

        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT; //Start in signaled state for first frame

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            if (vkCreateSemaphore(vulkan_instance.device, &semaphore_info, nullptr, &image_available_semaphores[i]) != VK_SUCCESS ||
                vkCreateSemaphore(vulkan_instance.device, &semaphore_info, nullptr, &render_finished_semaphores[i]) != VK_SUCCESS ||
                vkCreateFence(vulkan_instance.device, &fence_info, nullptr, &in_flight_fences[i]) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create semaphores!");
            }
        }
    }

    void Vulkan_Engine::create_timestamp_queries()
    {
        VkQueryPoolCreateInfo query_pool_info{};
        query_pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        query_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_pool_info.queryCount = MAX_FRAMES_IN_FLIGHT * 2;

        if (vkCreateQueryPool(vulkan_instance.device, &query_pool_info, nullptr, &timestamp_query_pool) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create timestamp query pool!");
        }
    }

    void Vulkan_Engine::read_gpu_timestamp(const uint32_t frame)
    {
        if (timestamp_query_pool == VK_NULL_HANDLE)
        {
            return;
        }

        std::array<uint64_t, 4> results{};
        const VkResult result = vkGetQueryPoolResults(
            vulkan_instance.device,
            timestamp_query_pool,
            frame * 2,
            2,
            sizeof(results),
            results.data(),
            sizeof(uint64_t) * 2,
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

        // A newly-created query has no result yet. Keep the last valid GPU time.
        if (result == VK_SUCCESS && results[1] != 0 && results[3] != 0)
        {
            const auto properties = vulkan_instance.get_physical_device_properties();
            frame_statistics.gpu_frame_time_ms =
                static_cast<double>(results[2] - results[0]) * properties.limits.timestampPeriod / 1'000'000.0;
        }
    }

    void Vulkan_Engine::transition_swap_chain_image_to_color_attachment()
    {
        // NOTE: assumes Vulkan_Swap_Chain exposes the raw per-image VkImage handles as
        // `swap_chain.images`, parallel to `swap_chain.image_views`. That file wasn't part of
        // this upload - if the member is named differently, adjust this one line.
        Image::cmd_transition_image_layout(current_command_buffer, swap_chain.images[current_image_index], VK_IMAGE_ASPECT_COLOR_BIT, 1,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }

    void Vulkan_Engine::transition_swap_chain_image_to_present()
    {
        Image::cmd_transition_image_layout(current_command_buffer, swap_chain.images[current_image_index], VK_IMAGE_ASPECT_COLOR_BIT, 1,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE);
    }

    void Vulkan_Engine::start_record_command_buffer()
    {
        //Start recording a command buffer
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = 0;
        begin_info.pInheritanceInfo = nullptr;

        if (vkBeginCommandBuffer(current_command_buffer, &begin_info) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to begin recording command buffer!");
        }

        const uint32_t query_index = current_frame * 2;
        vkCmdResetQueryPool(current_command_buffer, timestamp_query_pool, query_index, 2);
        vkCmdWriteTimestamp2(current_command_buffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamp_query_pool, query_index);

        //Dynamic rendering (Vulkan 1.3 core) replaces vkCmdBeginRenderPass/VkFramebuffer: we
        //explicitly transition the swap chain image ourselves instead of the render pass doing it
        //implicitly through its initial/final layout. The depth image was already transitioned
        //once in create_depth_resources() and never needs to move again.
        transition_swap_chain_image_to_color_attachment();

        VkRenderingAttachmentInfo color_attachment_info{};
        color_attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment_info.imageView = swap_chain.image_views[current_image_index];
        color_attachment_info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment_info.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment_info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment_info.clearValue.color = { { 0.0f, 0.0f, 0.0f, 1.0f } }; //Clear image to black

        VkRenderingAttachmentInfo depth_attachment_info{};
        depth_attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment_info.imageView = depth_image.image_view;
        depth_attachment_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment_info.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment_info.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; //We dont care what the gpu does with the data after determining the depth
        depth_attachment_info.clearValue.depthStencil = { 1.0f, 0 }; //Clear depth image to 1.0 (far plane)

        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea.offset = { 0, 0 };
        rendering_info.renderArea.extent = swap_chain.extent;
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment_info;
        rendering_info.pDepthAttachment = &depth_attachment_info;

        vkCmdBeginRendering(current_command_buffer, &rendering_info);

        //We set viewport and scissor to dynamic earlier, so we define them now
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(swap_chain.extent.width);
        viewport.height = static_cast<float>(swap_chain.extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor{};
        scissor.offset = { 0,0 };
        scissor.extent = swap_chain.extent;

        vkCmdSetViewport(current_command_buffer, 0, 1, &viewport);
        vkCmdSetScissor(current_command_buffer, 0, 1, &scissor);
    }

    void Vulkan_Engine::end_record_command_buffer()
    {
        vkCmdEndRendering(current_command_buffer);

        //Hand the swap chain image back to the present layout ourselves - the render pass used to
        //do this via its finalLayout, dynamic rendering leaves it entirely up to us.
        transition_swap_chain_image_to_present();

        vkCmdWriteTimestamp2(current_command_buffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamp_query_pool, current_frame * 2 + 1);

        if (vkEndCommandBuffer(current_command_buffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to record command buffer!");
        }
    }

    VkShaderModule Vulkan_Engine::create_shader_module(const std::vector<char>& bytecode)
    {
        VkShaderModuleCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create_info.codeSize = bytecode.size();
        create_info.pCode = reinterpret_cast<const uint32_t*> (bytecode.data());

        VkShaderModule shader_module;
        if (vkCreateShaderModule(vulkan_instance.device, &create_info, nullptr, &shader_module) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create shader module!");
        }

        return shader_module;
    }

    bool Vulkan_Engine::has_stencil_component(VkFormat format) const
    {
        return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
    }
}
