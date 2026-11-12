#include <cstdlib>
#include <iostream>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>

#include "application.hpp"
#include "graphics_internal.hpp"

int main() {
	if (!glfwInit()) {
		std::cerr << "Failed to initialize GLFW\n";
		return EXIT_FAILURE;
	}

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWwindow* window = glfwCreateWindow(1280, 720, "Lab 2 - Camera and Lighting", nullptr, nullptr);
	if (!window) {
		std::cerr << "Failed to create window\n";
		glfwTerminate();
		return EXIT_FAILURE;
	}

	glfwSetFramebufferSizeCallback(window, [](GLFWwindow*, int width, int height) {
		if (width > 0 && height > 0)
			graphics::internal::resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
	});

	if (!ImGui::CreateContext() || !ImGui_ImplGlfw_InitForVulkan(window, true)) {
		std::cerr << "Failed to initialize ImGui\n";
		glfwDestroyWindow(window);
		glfwTerminate();
		return EXIT_FAILURE;
	}

	bool graphics_ready = graphics::internal::initialize(window);
	if (!graphics_ready) {
		std::cerr << "Failed to initialize graphics\n";
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		glfwDestroyWindow(window);
		glfwTerminate();
		return EXIT_FAILURE;
	}

	if (!application::initialize()) {
		std::cerr << "Failed to initialize Lab 2\n";
		application::shutdown();
		graphics::internal::shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		glfwDestroyWindow(window);
		glfwTerminate();
		return EXIT_FAILURE;
	}

	while (!glfwWindowShouldClose(window)) {
		glfwPollEvents();
		const auto frame = graphics::internal::prepare();
		if (!frame.command_buffer)
			continue;
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
		application::update(glfwGetTime());
		ImGui::Render();
		application::render(frame);
		graphics::internal::submitAndPresent();
	}

	application::shutdown();
	graphics::internal::shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(window);
	glfwTerminate();
	return EXIT_SUCCESS;
}
