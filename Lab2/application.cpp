#include "application.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <imgui.h>
#include <vulkan/vulkan.h>

namespace application {
namespace {

struct Vertex {
	glm::vec3 position;
	glm::vec3 normal;
};

struct alignas(16) SceneUniform {
	glm::mat4 view_projection;
	glm::mat4 model;
	glm::vec4 camera_position;
	glm::vec4 light_direction;
	glm::vec4 light_color_intensity;
	glm::vec4 ambient_color_intensity;
	glm::vec4 albedo;
	glm::vec4 specular_shininess;
};

VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
VkPipeline pipeline = VK_NULL_HANDLE;
VkBuffer vertex_buffer = VK_NULL_HANDLE;
VmaAllocation vertex_allocation = VK_NULL_HANDLE;
VkBuffer index_buffer = VK_NULL_HANDLE;
VmaAllocation index_allocation = VK_NULL_HANDLE;
VkBuffer uniform_buffer = VK_NULL_HANDLE;
VmaAllocation uniform_allocation = VK_NULL_HANDLE;
SceneUniform* mapped_uniform = nullptr;
uint32_t index_count = 0;

glm::vec3 camera_position{4.0f, 3.0f, 4.0f};
float camera_yaw = -135.0f;
float camera_pitch = -20.0f;
float field_of_view = 60.0f;
glm::vec3 model_position{0.0f};
glm::vec3 model_rotation{0.0f};
glm::vec3 model_scale{1.0f};
glm::vec3 light_direction{-0.5f, -1.0f, -0.3f};
glm::vec3 light_color{1.0f};
float light_intensity = 1.0f;
glm::vec3 ambient_color{0.2f};
float ambient_intensity = 0.25f;
glm::vec3 albedo{0.7f, 0.35f, 0.18f};
glm::vec3 specular_color{1.0f};
float shininess = 32.0f;

void check_vk(VkResult result, const char* operation) {
	if (result != VK_SUCCESS)
		throw std::runtime_error(operation);
}

std::vector<char> read_binary(const char* path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file)
		throw std::runtime_error(std::string("Failed to open shader: ") + path);
	const auto end = file.tellg();
	if (end <= 0)
		throw std::runtime_error(std::string("Shader is empty: ") + path);
	std::vector<char> bytes(static_cast<size_t>(end));
	file.seekg(0);
	if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
		throw std::runtime_error(std::string("Failed to read shader: ") + path);
	return bytes;
}

VkShaderModule create_shader_module(const std::vector<char>& code) {
	VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	info.codeSize = code.size();
	info.pCode = reinterpret_cast<const uint32_t*>(code.data());
	VkShaderModule module = VK_NULL_HANDLE;
	check_vk(vkCreateShaderModule(graphics::internal::context.device, &info, nullptr, &module),
	         "Failed to create shader module");
	return module;
}

void create_cube(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
	const glm::vec3 corners[] = {
		{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
		{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1},
	};
	const int faces[][4] = {
		{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
		{3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5},
	};
	const glm::vec3 normals[] = {
		{0, 0, -1}, {0, 0, 1}, {0, -1, 0},
		{0, 1, 0}, {-1, 0, 0}, {1, 0, 0},
	};
	for (uint32_t face = 0; face < 6; ++face) {
		const uint32_t base = static_cast<uint32_t>(vertices.size());
		for (int corner = 0; corner < 4; ++corner)
			vertices.push_back({corners[faces[face][corner]], normals[face]});
		indices.insert(indices.end(), {
			base, base + 1, base + 2, base, base + 2, base + 3,
		});
	}
}

void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation, void** mapped = nullptr) {
	VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	buffer_info.size = size;
	buffer_info.usage = usage;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocation_info{};
	allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
	if (mapped)
		allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo result{};
	check_vk(vmaCreateBuffer(graphics::internal::context.allocator, &buffer_info, &allocation_info, &buffer, &allocation, mapped ? &result : nullptr), "Failed to create Lab 2 buffer");
	if (mapped)
		*mapped = result.pMappedData;
}

void create_geometry() {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	create_cube(vertices, indices);
	index_count = static_cast<uint32_t>(indices.size());

	void* mapped = nullptr;
	create_buffer(vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer, vertex_allocation, &mapped);
	std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(Vertex));
	check_vk(vmaFlushAllocation(graphics::internal::context.allocator, vertex_allocation, 0, VK_WHOLE_SIZE),
	         "Failed to flush Lab 2 vertex buffer");

	create_buffer(indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer, index_allocation, &mapped);
	std::memcpy(mapped, indices.data(), indices.size() * sizeof(uint32_t));
	check_vk(vmaFlushAllocation(graphics::internal::context.allocator, index_allocation, 0, VK_WHOLE_SIZE),
	         "Failed to flush Lab 2 index buffer");
}

void create_descriptors() {
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo layout_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	layout_info.bindingCount = 1;
	layout_info.pBindings = &binding;
	check_vk(vkCreateDescriptorSetLayout(graphics::internal::context.device, &layout_info, nullptr, &descriptor_layout), "Failed to create Lab 2 descriptor set layout");

	VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
	VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;
	check_vk(vkCreateDescriptorPool(graphics::internal::context.device, &pool_info, nullptr, &descriptor_pool), "Failed to create Lab 2 descriptor pool");

	VkDescriptorSetAllocateInfo allocate_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	allocate_info.descriptorPool = descriptor_pool;
	allocate_info.descriptorSetCount = 1;
	allocate_info.pSetLayouts = &descriptor_layout;
	check_vk(vkAllocateDescriptorSets(graphics::internal::context.device, &allocate_info, &descriptor_set), "Failed to allocate Lab 2 descriptor set");

	create_buffer(sizeof(SceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, uniform_buffer, uniform_allocation, reinterpret_cast<void**>(&mapped_uniform));
	VkDescriptorBufferInfo buffer_info{uniform_buffer, 0, sizeof(SceneUniform)};
	VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	write.dstSet = descriptor_set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	write.pBufferInfo = &buffer_info;
	vkUpdateDescriptorSets(graphics::internal::context.device, 1, &write, 0, nullptr);
}

void create_pipeline() {
	const auto vertex_code = read_binary("shaders/shader.vert.spv");
	const auto fragment_code = read_binary("shaders/shader.frag.spv");
	const VkShaderModule vertex_module = create_shader_module(vertex_code);
	const VkShaderModule fragment_module = create_shader_module(fragment_code);

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex_module;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment_module;
	stages[1].pName = "main";

	VkVertexInputBindingDescription vertex_binding{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attributes[] = {
		{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
		{1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
	};
	VkPipelineVertexInputStateCreateInfo vertex_input{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &vertex_binding;
	vertex_input.vertexAttributeDescriptionCount = 2;
	vertex_input.pVertexAttributeDescriptions = attributes;

	VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rasterization{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depth.depthTestEnable = VK_TRUE;
	depth.depthWriteEnable = VK_TRUE;
	depth.depthCompareOp = VK_COMPARE_OP_LESS;
	VkPipelineColorBlendAttachmentState blend_attachment{};
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;
	const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, };
	VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamic_states;

	VkPipelineLayoutCreateInfo layout_info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &descriptor_layout;
	check_vk(vkCreatePipelineLayout(graphics::internal::context.device, &layout_info, nullptr, &pipeline_layout),  "Failed to create Lab 2 pipeline layout");

	VkGraphicsPipelineCreateInfo pipeline_info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipeline_info.stageCount = 2;
	pipeline_info.pStages = stages;
	pipeline_info.pVertexInputState = &vertex_input;
	pipeline_info.pInputAssemblyState = &assembly;
	pipeline_info.pViewportState = &viewport;
	pipeline_info.pRasterizationState = &rasterization;
	pipeline_info.pMultisampleState = &multisample;
	pipeline_info.pDepthStencilState = &depth;
	pipeline_info.pColorBlendState = &blend;
	pipeline_info.pDynamicState = &dynamic;
	pipeline_info.layout = pipeline_layout;
	pipeline_info.renderPass = graphics::internal::context.render_pass;
	check_vk(vkCreateGraphicsPipelines(graphics::internal::context.device, VK_NULL_HANDLE,  1, &pipeline_info, nullptr, &pipeline), "Failed to create Lab 2 graphics pipeline");
	vkDestroyShaderModule(graphics::internal::context.device, vertex_module, nullptr);
	vkDestroyShaderModule(graphics::internal::context.device, fragment_module, nullptr);
}

glm::vec3 camera_forward() {
	const float yaw = glm::radians(camera_yaw);
	const float pitch = glm::radians(camera_pitch);
	return glm::normalize(glm::vec3(std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch)));
}

void update_uniform() {
	const auto extent = graphics::internal::context.swapchain_extent;
	const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
	glm::mat4 projection = glm::perspective(glm::radians(field_of_view), aspect, 0.1f, 100.0f);
	projection[1][1] *= -1.0f;
	const glm::mat4 view = glm::lookAt(camera_position, camera_position + camera_forward(), glm::vec3(0, 1, 0));
	glm::mat4 model = glm::translate(glm::mat4(1.0f), model_position);
	model = glm::rotate(model, glm::radians(model_rotation.x), {1, 0, 0});
	model = glm::rotate(model, glm::radians(model_rotation.y), {0, 1, 0});
	model = glm::rotate(model, glm::radians(model_rotation.z), {0, 0, 1});
	model = glm::scale(model, model_scale);

	mapped_uniform->view_projection = projection * view;
	mapped_uniform->model = model;
	mapped_uniform->camera_position = glm::vec4(camera_position, 1.0f);
	const glm::vec3 direction = glm::length(light_direction) > 0.0001f ? glm::normalize(light_direction) : glm::vec3(0, -1, 0);
	mapped_uniform->light_direction = glm::vec4(direction, 0.0f);
	mapped_uniform->light_color_intensity = glm::vec4(light_color, light_intensity);
	mapped_uniform->ambient_color_intensity = glm::vec4(ambient_color, ambient_intensity);
	mapped_uniform->albedo = glm::vec4(albedo, 1.0f);
	mapped_uniform->specular_shininess = glm::vec4(specular_color, shininess);
	check_vk(vmaFlushAllocation(graphics::internal::context.allocator, uniform_allocation, 0, sizeof(SceneUniform)), "Failed to flush Lab 2 uniform buffer");
}

} // namespace

bool initialize() {
	try {
		create_geometry();
		create_descriptors();
		create_pipeline();
		update_uniform();
		return true;
	} catch (const std::exception& error) {
		fprintf(stderr, "Lab 2 initialization failed: %s\n", error.what());
		return false;
	}
}

void shutdown() {
	VkDevice device = graphics::internal::context.device;
	VmaAllocator allocator = graphics::internal::context.allocator;
	if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
	if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
	if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
	if (descriptor_layout) vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
	if (vertex_buffer) vmaDestroyBuffer(allocator, vertex_buffer, vertex_allocation);
	if (index_buffer) vmaDestroyBuffer(allocator, index_buffer, index_allocation);
	if (uniform_buffer) vmaDestroyBuffer(allocator, uniform_buffer, uniform_allocation);
	pipeline = VK_NULL_HANDLE;
	pipeline_layout = VK_NULL_HANDLE;
	descriptor_pool = VK_NULL_HANDLE;
	descriptor_layout = VK_NULL_HANDLE;
	vertex_buffer = VK_NULL_HANDLE;
	index_buffer = VK_NULL_HANDLE;
	uniform_buffer = VK_NULL_HANDLE;
}

void update(double) {
	ImGui::Begin("Lab 2 — Camera and Lighting");
	ImGui::SeparatorText("Camera");
	ImGui::SliderFloat3("Position", &camera_position.x, -10.0f, 10.0f);
	ImGui::SliderFloat("Yaw", &camera_yaw, -180.0f, 180.0f);
	ImGui::SliderFloat("Pitch", &camera_pitch, -89.0f, 89.0f);
	ImGui::SliderFloat("Field of view", &field_of_view, 20.0f, 100.0f);

	ImGui::SeparatorText("Directional light");
	ImGui::SliderFloat3("Direction", &light_direction.x, -1.0f, 1.0f);
	ImGui::ColorEdit3("Light color", &light_color.x);
	ImGui::SliderFloat("Light intensity", &light_intensity, 0.0f, 5.0f);

	ImGui::SeparatorText("Material");
	ImGui::ColorEdit3("Albedo", &albedo.x);
	ImGui::ColorEdit3("Specular color", &specular_color.x);
	ImGui::SliderFloat("Shininess", &shininess, 1.0f, 256.0f);
	ImGui::ColorEdit3("Ambient color", &ambient_color.x);
	ImGui::SliderFloat("Ambient intensity", &ambient_intensity, 0.0f, 2.0f);

	ImGui::SeparatorText("Model");
	ImGui::SliderFloat3("Position", &model_position.x, -5.0f, 5.0f);
	ImGui::SliderFloat3("Rotation", &model_rotation.x, -180.0f, 180.0f);
	ImGui::SliderFloat3("Scale", &model_scale.x, 0.1f, 3.0f);
	ImGui::End();
	update_uniform();
}

void render(const graphics::internal::FrameData& frame) {
	const VkExtent2D extent = graphics::internal::context.swapchain_extent;
	VkCommandBuffer command = frame.command_buffer;
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	check_vk(vkBeginCommandBuffer(command, &begin), "Failed to begin Lab 2 command buffer");

	VkClearValue clear_values[2]{};
	clear_values[0].color = {{0.04f, 0.05f, 0.08f, 1.0f}};
	clear_values[1].depthStencil = {1.0f, 0};
	VkRenderPassBeginInfo render_pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	render_pass.renderPass = graphics::internal::context.render_pass;
	render_pass.framebuffer = frame.framebuffer;
	render_pass.renderArea.extent = extent;
	render_pass.clearValueCount = 2;
	render_pass.pClearValues = clear_values;
	vkCmdBeginRenderPass(command, &render_pass, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport viewport{};
	viewport.width = static_cast<float>(extent.width);
	viewport.height = static_cast<float>(extent.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(command, 0, 1, &viewport);
	VkRect2D scissor{};
	scissor.extent = extent;
	vkCmdSetScissor(command, 0, 1, &scissor);
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
	                        0, 1, &descriptor_set, 0, nullptr);
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(command, 0, 1, &vertex_buffer, &offset);
	vkCmdBindIndexBuffer(command, index_buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(command, index_count, 1, 0, 0, 0);
	vkCmdEndRenderPass(command);
	check_vk(vkEndCommandBuffer(command), "Failed to record Lab 2 command buffer");
}

} // namespace application
