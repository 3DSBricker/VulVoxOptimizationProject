#pragma once

#include <functional>
#include <vulkan/vulkan.h>
// Zorg dat je de juiste includes voor glfw, imgui en je vulkan_instance hier hebt staan

namespace vulvox
{
    class ImGui_Context
    {
    public:
        // VkRenderPass vervangen door VkFormat
        ImGui_Context(GLFWwindow* glfw_window, vulvox::Vulkan_Instance& vulkan_instance, VkFormat color_attachment_format, const int max_swapchain_images);
        ~ImGui_Context();

        void set_imgui_callback(std::function<void()>& callback);

        void set_dark_theme();
        void set_light_theme();

        void start_imgui_frame();
        void render_and_end_imgui_frame(VkCommandBuffer current_command_buffer);

    private:
        std::function<void()> imgui_callback;

        // Allocating a separate descriptor pool for imgui is just way easier to manage
        VkDescriptorPool descriptor_pool;

        // Device that created this imgui context
        VkDevice device = VK_NULL_HANDLE;
    };
}