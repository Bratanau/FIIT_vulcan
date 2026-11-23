#include "application.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <imgui.h>
#include <vulkan/vulkan.h>

namespace application {
namespace {

struct Vertex {
	glm::vec3 position;
	glm::vec3 normal;
	glm::vec2 uv;
};

struct alignas(16) ModelProperties {
	glm::mat4 view_projection;
	glm::mat4 model;
	glm::vec4 camera_position;
	glm::vec4 light_direction_intensity;
	glm::vec4 light_color;
	glm::vec4 ambient_color_intensity;
	glm::vec4 albedo_tint;
	glm::vec4 specular_color_shininess;
	glm::vec4 emissive_parameters;
};

struct PushConstants {
	int32_t animated_sampling;
	float time;
	float strength;
	float padding;
};

struct Texture {
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
};

struct Material {
	const char* name;
	uint32_t albedo;
	uint32_t specular;
	uint32_t emissive;
	VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
};

struct Object {
	const char* name;
	VkBuffer vertex_buffer = VK_NULL_HANDLE;
	VmaAllocation vertex_allocation = VK_NULL_HANDLE;
	VkBuffer index_buffer = VK_NULL_HANDLE;
	VmaAllocation index_allocation = VK_NULL_HANDLE;
	uint32_t index_count = 0;
	glm::vec3 position{0.0f};
	glm::vec3 rotation{0.0f};
	glm::vec3 scale{1.0f};
	uint32_t material = 0;
	bool animated_sampling = false;
	float sampling_strength = 1.0f;
};

VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
VkPipeline pipeline = VK_NULL_HANDLE;
VkSampler texture_sampler = VK_NULL_HANDLE;
VkCommandPool upload_command_pool = VK_NULL_HANDLE;
VkBuffer properties_buffer = VK_NULL_HANDLE;
VmaAllocation properties_allocation = VK_NULL_HANDLE;
ModelProperties* mapped_properties = nullptr;
VkDeviceSize properties_stride = 0;
glm::vec3 camera_position{4.0f, 3.0f, 4.0f};
float camera_yaw = -135.0f;
float camera_pitch = -20.0f;
float field_of_view = 60.0f;
glm::vec3 light_direction{-0.5f, -1.0f, -0.3f};
glm::vec3 light_color{1.0f};
float light_intensity = 1.0f;
glm::vec3 ambient_color{0.18f};
float ambient_intensity = 0.3f;
glm::vec3 albedo_tint{1.0f};
glm::vec3 specular_color{1.0f};
float shininess = 64.0f;
float emissive_intensity = 1.0f;
double current_time = 0.0;
std::vector<Texture> textures;
std::vector<Material> materials;
std::vector<Object> objects;

void check_vk(VkResult result, const char* operation) {
	if (result != VK_SUCCESS)
		throw std::runtime_error(operation);
}

void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocationCreateFlags flags, VmaMemoryUsage memory_usage, VkBuffer& buffer, VmaAllocation& allocation, void** mapped = nullptr) {
	VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	buffer_info.size = size;
	buffer_info.usage = usage;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo allocation_info{};
	allocation_info.usage = memory_usage;
	allocation_info.flags = flags;
	VmaAllocationInfo result{};
	check_vk(vmaCreateBuffer(graphics::internal::context.allocator, &buffer_info, &allocation_info, &buffer, &allocation, mapped ? &result : nullptr), "Failed to create Lab 3 buffer");
	if (mapped)
		*mapped = result.pMappedData;
}

void immediate_submit(const std::function<void(VkCommandBuffer)>& record) {
	const VkDevice device = graphics::internal::context.device;
	VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	allocate.commandPool = upload_command_pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	VkCommandBuffer command = VK_NULL_HANDLE;
	check_vk(vkAllocateCommandBuffers(device, &allocate, &command), "Failed to allocate upload command buffer");

	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	check_vk(vkBeginCommandBuffer(command, &begin), "Failed to begin upload command buffer");
	record(command);
	check_vk(vkEndCommandBuffer(command), "Failed to end upload command buffer");

	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command;
	check_vk(vkQueueSubmit(graphics::internal::context.graphics_queue, 1, &submit, VK_NULL_HANDLE), "Failed to submit upload command buffer");
	check_vk(vkQueueWaitIdle(graphics::internal::context.graphics_queue), "Failed waiting for upload commands");
	vkFreeCommandBuffers(device, upload_command_pool, 1, &command);
}

Texture load_texture(const char* path) {
	int width = 0;
	int height = 0;
	int channels = 0;
	stbi_set_flip_vertically_on_load(1);
	stbi_uc* pixels = stbi_load(path, &width, &height, &channels, STBI_rgb_alpha);
	if (!pixels)
		throw std::runtime_error(std::string("Failed to load texture ") + path + ": " + stbi_failure_reason());

	const VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
	VkBuffer staging_buffer = VK_NULL_HANDLE;
	VmaAllocation staging_allocation = VK_NULL_HANDLE;
	void* staging_data = nullptr;
	create_buffer(image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, VMA_MEMORY_USAGE_AUTO, staging_buffer, staging_allocation, &staging_data);
	std::memcpy(staging_data, pixels, static_cast<size_t>(image_size));
	check_vk(vmaFlushAllocation(graphics::internal::context.allocator, staging_allocation, 0, image_size), "Failed to flush texture staging buffer");
	stbi_image_free(pixels);

	Texture texture;
	VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	image_info.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VmaAllocationCreateInfo image_allocation{};
	image_allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	check_vk(vmaCreateImage(graphics::internal::context.allocator, &image_info, &image_allocation, &texture.image, &texture.allocation, nullptr), "Failed to create texture image");

	immediate_submit([&](VkCommandBuffer command) {
		VkImageMemoryBarrier to_transfer{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_transfer.image = texture.image;
		to_transfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);

		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = image_info.extent;
		vkCmdCopyBufferToImage(command, staging_buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

		VkImageMemoryBarrier to_shader{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_shader.image = texture.image;
		to_shader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_shader);
	});
	vmaDestroyBuffer(graphics::internal::context.allocator, staging_buffer, staging_allocation);

	VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	view_info.image = texture.image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	check_vk(vkCreateImageView(graphics::internal::context.device, &view_info, nullptr, &texture.view), "Failed to create texture image view");
	return texture;
}

void create_sampler() {
	VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	info.magFilter = VK_FILTER_LINEAR;
	info.minFilter = VK_FILTER_LINEAR;
	info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	info.maxLod = 0.0f;
	check_vk(vkCreateSampler(graphics::internal::context.device, &info, nullptr, &texture_sampler), "Failed to create texture sampler");
}

void upload_geometry(Object& object, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
	object.index_count = static_cast<uint32_t>(indices.size());
	const VkDeviceSize vertex_bytes = vertices.size() * sizeof(Vertex);
	const VkDeviceSize index_bytes = indices.size() * sizeof(uint32_t);
	VkBuffer vertex_staging = VK_NULL_HANDLE;
	VmaAllocation vertex_staging_allocation = VK_NULL_HANDLE;
	VkBuffer index_staging = VK_NULL_HANDLE;
	VmaAllocation index_staging_allocation = VK_NULL_HANDLE;
	void* vertex_data = nullptr;
	void* index_data = nullptr;
	const VkBufferUsageFlags staging_usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	const VmaAllocationCreateFlags host_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	create_buffer(vertex_bytes, staging_usage, host_flags, VMA_MEMORY_USAGE_AUTO,  vertex_staging, vertex_staging_allocation, &vertex_data);
	create_buffer(index_bytes, staging_usage, host_flags, VMA_MEMORY_USAGE_AUTO,  index_staging, index_staging_allocation, &index_data);
	std::memcpy(vertex_data, vertices.data(), static_cast<size_t>(vertex_bytes));
	std::memcpy(index_data, indices.data(), static_cast<size_t>(index_bytes));
	check_vk(vmaFlushAllocation(graphics::internal::context.allocator, vertex_staging_allocation, 0, vertex_bytes), "Failed to flush vertex staging buffer");
	check_vk(vmaFlushAllocation(graphics::internal::context.allocator, index_staging_allocation, 0, index_bytes), "Failed to flush index staging buffer");

	create_buffer(vertex_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, object.vertex_buffer, object.vertex_allocation);
	create_buffer(index_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 0, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, object.index_buffer, object.index_allocation);
	immediate_submit([&](VkCommandBuffer command) {
		VkBufferCopy vertex_copy{0, 0, vertex_bytes};
		VkBufferCopy index_copy{0, 0, index_bytes};
		vkCmdCopyBuffer(command, vertex_staging, object.vertex_buffer, 1, &vertex_copy);
		vkCmdCopyBuffer(command, index_staging, object.index_buffer, 1, &index_copy);
	});
	vmaDestroyBuffer(graphics::internal::context.allocator, vertex_staging, vertex_staging_allocation);
	vmaDestroyBuffer(graphics::internal::context.allocator, index_staging, index_staging_allocation);
}

void make_cube(Object& object) {
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
	const glm::vec2 face_uv[] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	for (uint32_t face = 0; face < 6; ++face) {
		const uint32_t base = static_cast<uint32_t>(vertices.size());
		for (int corner = 0; corner < 4; ++corner)
			vertices.push_back({corners[faces[face][corner]], normals[face], face_uv[corner]});
		indices.insert(indices.end(), {
			base, base + 1, base + 2, base, base + 2, base + 3,
		});
	}
	upload_geometry(object, vertices, indices);
}

void make_sphere(Object& object, int stacks, int slices) {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	for (int y = 0; y <= stacks; ++y) {
		const float v = static_cast<float>(y) / static_cast<float>(stacks);
		const float phi = glm::pi<float>() * v;
		for (int x = 0; x <= slices; ++x) {
			const float u = static_cast<float>(x) / static_cast<float>(slices);
			const float theta = glm::two_pi<float>() * u;
			const glm::vec3 normal(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
			vertices.push_back({normal, normal, {u, v}});
		}
	}
	for (int y = 0; y < stacks; ++y) {
		for (int x = 0; x < slices; ++x) {
			const uint32_t a = static_cast<uint32_t>(y * (slices + 1) + x);
			const uint32_t b = a + static_cast<uint32_t>(slices + 1);
			indices.insert(indices.end(), {a, a + 1, b, a + 1, b + 1, b});
		}
	}
	upload_geometry(object, vertices, indices);
}

void create_descriptors() {
	VkPhysicalDeviceProperties device_properties{};
	vkGetPhysicalDeviceProperties(graphics::internal::context.physical_device, &device_properties);
	const VkDeviceSize uniform_alignment = std::max<VkDeviceSize>(1, device_properties.limits.minUniformBufferOffsetAlignment);
	properties_stride = (sizeof(ModelProperties) + uniform_alignment - 1) / uniform_alignment * uniform_alignment;
	const VkDeviceSize buffer_size = properties_stride * objects.size();
	void* mapped = nullptr;
	create_buffer(buffer_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, VMA_MEMORY_USAGE_AUTO, properties_buffer, properties_allocation, &mapped);
	mapped_properties = static_cast<ModelProperties*>(mapped);

	const VkDescriptorSetLayoutBinding bindings[] = {
		{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
	};
	VkDescriptorSetLayoutCreateInfo layout_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	layout_info.bindingCount = 4;
	layout_info.pBindings = bindings;
	check_vk(vkCreateDescriptorSetLayout(graphics::internal::context.device, &layout_info, nullptr, &descriptor_layout), "Failed to create Lab 3 descriptor set layout");

	VkDescriptorPoolSize pool_sizes[] = {
		{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, static_cast<uint32_t>(materials.size())},
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		 static_cast<uint32_t>(materials.size() * 3)},
	};
	VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pool_info.maxSets = static_cast<uint32_t>(materials.size());
	pool_info.poolSizeCount = 2;
	pool_info.pPoolSizes = pool_sizes;
	check_vk(vkCreateDescriptorPool(graphics::internal::context.device, &pool_info, nullptr, &descriptor_pool), "Failed to create Lab 3 descriptor pool");

	VkDescriptorSetLayout layouts[] = {descriptor_layout, descriptor_layout};
	std::vector<VkDescriptorSet> descriptor_sets(materials.size(), VK_NULL_HANDLE);
	VkDescriptorSetAllocateInfo allocate_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	allocate_info.descriptorPool = descriptor_pool;
	allocate_info.descriptorSetCount = static_cast<uint32_t>(materials.size());
	allocate_info.pSetLayouts = layouts;
	check_vk(vkAllocateDescriptorSets(graphics::internal::context.device, &allocate_info, descriptor_sets.data()), "Failed to allocate Lab 3 material descriptor sets");

	for (size_t i = 0; i < materials.size(); ++i) {
		materials[i].descriptor_set = descriptor_sets[i];
		VkDescriptorBufferInfo buffer_info{properties_buffer, 0, sizeof(ModelProperties)};
		VkDescriptorImageInfo image_infos[3]{};
		const uint32_t texture_indices[] = {
			materials[i].albedo, materials[i].specular, materials[i].emissive,
		};
		VkWriteDescriptorSet writes[4]{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = descriptor_sets[i];
		writes[0].dstBinding = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		writes[0].pBufferInfo = &buffer_info;
		for (uint32_t binding = 1; binding <= 3; ++binding) {
			const Texture& texture = textures[texture_indices[binding - 1]];
			image_infos[binding - 1] = { texture_sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, };
			writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[binding].dstSet = descriptor_sets[i];
			writes[binding].dstBinding = binding;
			writes[binding].descriptorCount = 1;
			writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[binding].pImageInfo = &image_infos[binding - 1];
		}
		vkUpdateDescriptorSets(graphics::internal::context.device, 4, writes, 0, nullptr);
	}
}

VkShaderModule create_shader_module(const std::vector<char>& code) {
	VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	info.codeSize = code.size();
	info.pCode = reinterpret_cast<const uint32_t*>(code.data());
	VkShaderModule module = VK_NULL_HANDLE;
	check_vk(vkCreateShaderModule(graphics::internal::context.device, &info, nullptr, &module), "Failed to create shader module");
	return module;
}

std::vector<char> read_shader(const char* path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file)
		throw std::runtime_error(std::string("Failed to open shader: ") + path);
	const auto end = file.tellg();
	if (end <= 0)
		throw std::runtime_error(std::string("Shader is empty: ") + path);
	std::vector<char> data(static_cast<size_t>(end));
	file.seekg(0);
	if (!file.read(data.data(), static_cast<std::streamsize>(data.size())))
		throw std::runtime_error(std::string("Failed to read shader: ") + path);
	return data;
}

void create_pipeline() {
	const VkShaderModule vertex_module = create_shader_module(read_shader("shaders/shader.vert.spv"));
	const VkShaderModule fragment_module = create_shader_module(read_shader("shaders/shader.frag.spv"));
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
		{2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
	};
	VkPipelineVertexInputStateCreateInfo vertex_input{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &vertex_binding;
	vertex_input.vertexAttributeDescriptionCount = 3;
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

	VkPushConstantRange push_range{};
	push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	push_range.offset = 0;
	push_range.size = sizeof(PushConstants);
	VkPipelineLayoutCreateInfo layout_info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &descriptor_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	check_vk(vkCreatePipelineLayout(graphics::internal::context.device, &layout_info, nullptr, &pipeline_layout), "Failed to create Lab 3 pipeline layout");

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
	check_vk(vkCreateGraphicsPipelines(graphics::internal::context.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline), "Failed to create Lab 3 graphics pipeline");
	vkDestroyShaderModule(graphics::internal::context.device, vertex_module, nullptr);
	vkDestroyShaderModule(graphics::internal::context.device, fragment_module, nullptr);
}

glm::vec3 camera_forward() {
	const float yaw = glm::radians(camera_yaw);
	const float pitch = glm::radians(camera_pitch);
	return glm::normalize(glm::vec3(std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch)));
}

void update_properties() {
	const VkExtent2D extent = graphics::internal::context.swapchain_extent;
	const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
	glm::mat4 projection = glm::perspective(glm::radians(field_of_view), aspect, 0.1f, 100.0f);
	projection[1][1] *= -1.0f;
	const glm::mat4 view = glm::lookAt(camera_position, camera_position + camera_forward(), glm::vec3(0, 1, 0));
	for (size_t i = 0; i < objects.size(); ++i) {
		Object& object = objects[i];
		glm::mat4 model = glm::translate(glm::mat4(1.0f), object.position);
		model = glm::rotate(model, glm::radians(object.rotation.x), {1, 0, 0});
		model = glm::rotate(model, glm::radians(object.rotation.y), {0, 1, 0});
		model = glm::rotate(model, glm::radians(object.rotation.z), {0, 0, 1});
		model = glm::scale(model, object.scale);
		auto* properties = reinterpret_cast<ModelProperties*>(reinterpret_cast<unsigned char*>(mapped_properties) + properties_stride * i);
		properties->view_projection = projection * view;
		properties->model = model;
		properties->camera_position = glm::vec4(camera_position, 1.0f);
		const glm::vec3 direction = glm::length(light_direction) > 0.0001f ? glm::normalize(light_direction) : glm::vec3(0, -1, 0);
		properties->light_direction_intensity = glm::vec4(direction, light_intensity);
		properties->light_color = glm::vec4(light_color, 1.0f);
		properties->ambient_color_intensity = glm::vec4(ambient_color, ambient_intensity);
		properties->albedo_tint = glm::vec4(albedo_tint, 1.0f);
		properties->specular_color_shininess = glm::vec4(specular_color, shininess);
		properties->emissive_parameters = glm::vec4(emissive_intensity, 0, 0, 0);
	}
	check_vk(vmaFlushAllocation(graphics::internal::context.allocator, properties_allocation, 0, properties_stride * objects.size()), "Failed to flush dynamic model properties");
}

} // namespace

bool initialize() {
	try {
		VkCommandPoolCreateInfo command_pool{
			VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		command_pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		command_pool.queueFamilyIndex = graphics::internal::context.graphics_queue_index;
		check_vk(vkCreateCommandPool(graphics::internal::context.device, &command_pool, nullptr, &upload_command_pool), "Failed to create upload command pool");

		textures.push_back(load_texture("textures/checker.ppm"));
		textures.push_back(load_texture("textures/stone.ppm"));
		textures.push_back(load_texture("textures/specular.ppm"));
		textures.push_back(load_texture("textures/emissive.ppm"));
		textures.push_back(load_texture("textures/black.ppm"));
		create_sampler();

		objects.push_back({"Cube"});
		objects.back().position = {-1.5f, 0.0f, 0.0f};
		objects.back().scale = glm::vec3(0.8f);
		objects.back().material = 0;
		objects.back().animated_sampling = true;
		make_cube(objects.back());
		objects.push_back({"Sphere"});
		objects.back().position = {1.5f, 0.0f, 0.0f};
		objects.back().scale = glm::vec3(0.9f);
		objects.back().material = 1;
		make_sphere(objects.back(), 24, 32);

		materials = {
			{"Checker + specular + emissive", 0, 2, 3},
			{"Stone (no specular/emissive)", 1, 4, 4},
		};
		create_descriptors();
		create_pipeline();
		update_properties();
		return true;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "Lab 3 initialization failed: %s\n", error.what());
		return false;
	}
}

void shutdown() {
	const VkDevice device = graphics::internal::context.device;
	const VmaAllocator allocator = graphics::internal::context.allocator;
	if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
	if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
	if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
	if (descriptor_layout) vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
	if (properties_buffer) vmaDestroyBuffer(allocator, properties_buffer, properties_allocation);
	for (auto& object : objects) {
		if (object.vertex_buffer)
			vmaDestroyBuffer(allocator, object.vertex_buffer, object.vertex_allocation);
		if (object.index_buffer)
			vmaDestroyBuffer(allocator, object.index_buffer, object.index_allocation);
	}
	for (auto& texture : textures) {
		if (texture.view) vkDestroyImageView(device, texture.view, nullptr);
		if (texture.image) vmaDestroyImage(allocator, texture.image, texture.allocation);
	}
	if (texture_sampler) vkDestroySampler(device, texture_sampler, nullptr);
	if (upload_command_pool) vkDestroyCommandPool(device, upload_command_pool, nullptr);
	objects.clear();
	materials.clear();
	textures.clear();
	pipeline = VK_NULL_HANDLE;
	pipeline_layout = VK_NULL_HANDLE;
	descriptor_pool = VK_NULL_HANDLE;
	descriptor_layout = VK_NULL_HANDLE;
	properties_buffer = VK_NULL_HANDLE;
	texture_sampler = VK_NULL_HANDLE;
	upload_command_pool = VK_NULL_HANDLE;
}

void update(double time) {
	current_time = time;
	ImGui::Begin("Lab 3 - Textures and materials");
	ImGui::SeparatorText("Camera");
	ImGui::SliderFloat3("Position", &camera_position.x, -10.0f, 10.0f);
	ImGui::SliderFloat("Yaw", &camera_yaw, -180.0f, 180.0f);
	ImGui::SliderFloat("Pitch", &camera_pitch, -89.0f, 89.0f);
	ImGui::SliderFloat("Field of view", &field_of_view, 20.0f, 100.0f);

	ImGui::SeparatorText("Lighting");
	ImGui::SliderFloat3("Light direction", &light_direction.x, -1.0f, 1.0f);
	ImGui::ColorEdit3("Light color", &light_color.x);
	ImGui::SliderFloat("Light intensity", &light_intensity, 0.0f, 5.0f);
	ImGui::ColorEdit3("Ambient color", &ambient_color.x);
	ImGui::SliderFloat("Ambient intensity", &ambient_intensity, 0.0f, 2.0f);

	ImGui::SeparatorText("Material");
	ImGui::ColorEdit3("Albedo tint", &albedo_tint.x);
	ImGui::ColorEdit3("Specular color", &specular_color.x);
	ImGui::SliderFloat("Shininess", &shininess, 1.0f, 256.0f);
	ImGui::SliderFloat("Emissive intensity", &emissive_intensity, 0.0f, 5.0f);
	for (size_t i = 0; i < objects.size(); ++i) {
		Object& object = objects[i];
		ImGui::PushID(static_cast<int>(i));
		ImGui::SeparatorText(object.name);
		if (ImGui::BeginCombo("Texture set", materials[object.material].name)) {
			for (size_t m = 0; m < materials.size(); ++m) {
				const bool selected = m == object.material;
				if (ImGui::Selectable(materials[m].name, selected))
					object.material = static_cast<uint32_t>(m);
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SliderFloat3("Position", &object.position.x, -5.0f, 5.0f);
		ImGui::SliderFloat3("Rotation", &object.rotation.x, -180.0f, 180.0f);
		ImGui::Checkbox("Animated texture sampling", &object.animated_sampling);
		if (object.animated_sampling)
			ImGui::SliderFloat("Sampling strength", &object.sampling_strength, 0.0f, 2.0f);
		ImGui::PopID();
	}
	ImGui::End();
	update_properties();
}

void render(const graphics::internal::FrameData& frame) {
	const VkExtent2D extent = graphics::internal::context.swapchain_extent;
	VkCommandBuffer command = frame.command_buffer;
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	check_vk(vkBeginCommandBuffer(command, &begin), "Failed to begin Lab 3 command buffer");
	VkClearValue clear_values[2]{};
	clear_values[0].color = {{ 0.035f, 0.045f, 0.07f, 1.0f }};
	clear_values[1].depthStencil = { 1.0f, 0 };
	VkRenderPassBeginInfo render_pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
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
	for (size_t i = 0; i < objects.size(); ++i) {
		const Object& object = objects[i];
		const VkDescriptorSet set = materials[object.material].descriptor_set;
		const uint32_t dynamic_offset = static_cast<uint32_t>(properties_stride * i);
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &set, 1, &dynamic_offset);
		const PushConstants push{ object.animated_sampling ? 1 : 0, static_cast<float>(current_time), object.sampling_strength, 0.0f, };
		vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		const VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(command, 0, 1, &object.vertex_buffer, &offset);
		vkCmdBindIndexBuffer(command, object.index_buffer, 0, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(command, object.index_count, 1, 0, 0, 0);
	}
	vkCmdEndRenderPass(command);
	check_vk(vkEndCommandBuffer(command), "Failed to record Lab 3 command buffer");
}

} // namespace application
