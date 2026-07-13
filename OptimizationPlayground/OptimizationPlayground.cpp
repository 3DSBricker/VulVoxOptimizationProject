#include "pch.h"
#include "scene.h"

#include "imgui.h"

int main()
{
    constexpr uint32_t width = 1024;
    constexpr uint32_t height = 720;

    float slider_value = 0.5f;

    try
    {
        vulvox::Renderer renderer;

        renderer.init(width, height, glm::radians(45.0f), 0.1f, 1000.0f);

        renderer.init_imgui();

        Scene scene(renderer);
        


        auto previous_frame_time = std::chrono::high_resolution_clock::now();
        while (!renderer.should_close())
        {
            auto frame_time = std::chrono::high_resolution_clock::now();
            float delta_time = std::chrono::duration<float, std::chrono::seconds::period>(frame_time - previous_frame_time).count();
            previous_frame_time = frame_time;

            //Required to update input state
            glfwPollEvents();

            scene.update(delta_time);

            renderer.start_draw();
            renderer.set_imgui_callback([&renderer, &slider_value]() {
                const auto& stats = renderer.get_frame_statistics();
                ImGui::Begin("Demo Window");
                ImGui::Text("Hello, ImGui!");
                ImGui::Text(renderer.get_memory_statistics().c_str());
                ImGui::SeparatorText("Frame profiler");
                ImGui::Text("FPS: %.1f | Avg frame: %.2f ms", stats.frames_per_second, stats.average_frame_time_ms);
                ImGui::Text("CPU render: %.3f ms", stats.cpu_frame_time_ms);
                ImGui::Text("GPU frame: %.3f ms", stats.gpu_frame_time_ms);
                ImGui::Text("Draw calls: %llu", static_cast<unsigned long long>(stats.draw_calls));
                ImGui::Text("Vertices: %llu | Indices: %llu", static_cast<unsigned long long>(stats.vertices), static_cast<unsigned long long>(stats.indices));
                ImGui::Text("Queue submits: %u | Host buffer uploads: %u", stats.queue_submits, stats.host_buffer_uploads);
                ImGui::Text("Pipeline binds: %u | Descriptor binds: %u", stats.pipeline_binds, stats.descriptor_set_binds);
                ImGui::SliderFloat("Slider", &slider_value, 0.0f, 1.0f);
                ImGui::End();
                });


            scene.draw();
            renderer.end_draw();
        }

        renderer.destroy();
    }
    catch (const std::exception& ex)
    {
        std::cout << ex.what() << std::endl;
    }
}
