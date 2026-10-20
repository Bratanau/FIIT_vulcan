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

constexpr uint32_t MAX_POINT_LIGHTS = 64;
constexpr uint32_t MAX_SPOT_LIGHTS = 16;

struct alignas(16) PointLight {
    glm::vec4 position_intensity; 
    glm::vec4 color;             
};

struct alignas(16) SpotLight {
    glm::vec4 position_intensity;   
    glm::vec4 direction_inner_cos;  
    glm::vec4 color_outer_cos;     
};

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
};

struct alignas(16) SceneUniform {
    glm::mat4 view_projection;
    glm::vec4 camera_position;
    glm::vec4 light_direction;
    glm::vec4 light_color_intensity;
    glm::vec4 ambient_color_intensity;
    glm::uvec4 counts; 
};

struct alignas(16) ModelUniform {
    glm::mat4 model;
    glm::vec4 albedo;
    glm::vec4 specular_shininess;
};

VkDescriptorSetLayout scene_layout = VK_NULL_HANDLE;
VkDescriptorSetLayout model_layout = VK_NULL_HANDLE;
VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
VkDescriptorSet scene_set = VK_NULL_HANDLE;
VkDescriptorSet model_set = VK_NULL_HANDLE;
VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
VkPipeline pipeline = VK_NULL_HANDLE;

VkBuffer vertex_buffer = VK_NULL_HANDLE;
VmaAllocation vertex_allocation = VK_NULL_HANDLE;
VkBuffer index_buffer = VK_NULL_HANDLE;
VmaAllocation index_allocation = VK_NULL_HANDLE;
uint32_t index_count = 0;

VkBuffer scene_ubo_buffer = VK_NULL_HANDLE;
VmaAllocation scene_ubo_alloc = VK_NULL_HANDLE;
SceneUniform* mapped_scene_ubo = nullptr;

VkBuffer model_ubo_buffer = VK_NULL_HANDLE;
VmaAllocation model_ubo_alloc = VK_NULL_HANDLE;
ModelUniform* mapped_model_ubo = nullptr;

VkBuffer point_ssbo_buffer = VK_NULL_HANDLE;
VmaAllocation point_ssbo_alloc = VK_NULL_HANDLE;
PointLight* mapped_point_ssbo = nullptr;

VkBuffer spot_ssbo_buffer = VK_NULL_HANDLE;
VmaAllocation spot_ssbo_alloc = VK_NULL_HANDLE;
SpotLight* mapped_spot_ssbo = nullptr;

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

std::vector<PointLight> g_point_lights;
std::vector<SpotLight> g_spot_lights;

void check_vk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(operation);
}

std::vector<char> read_binary(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error(std::string("Failed to open shader: ") + path);
    const auto end = file.tellg();
    if (end <= 0) throw std::runtime_error(std::string("Shader is empty: ") + path);
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
    check_vk(vkCreateShaderModule(graphics::internal::context.device, &info, nullptr, &module), "Failed to create shader module");
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
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
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
    check_vk(vmaCreateBuffer(graphics::internal::context.allocator, &buffer_info, &allocation_info, &buffer, &allocation, mapped ? &result : nullptr), "Failed to create buffer");
    if (mapped) *mapped = result.pMappedData;
}

void create_geometry() {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    create_cube(vertices, indices);
    index_count = static_cast<uint32_t>(indices.size());

    void* mapped = nullptr;
    create_buffer(vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer, vertex_allocation, &mapped);
    std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(Vertex));
    check_vk(vmaFlushAllocation(graphics::internal::context.allocator, vertex_allocation, 0, VK_WHOLE_SIZE), "Flush vertex");

    create_buffer(indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer, index_allocation, &mapped);
    std::memcpy(mapped, indices.data(), indices.size() * sizeof(uint32_t));
    check_vk(vmaFlushAllocation(graphics::internal::context.allocator, index_allocation, 0, VK_WHOLE_SIZE), "Flush index");
}

void create_descriptors() {
    VkDescriptorSetLayoutBinding scene_bindings[3]{};
    scene_bindings[0].binding = 0;
    scene_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    scene_bindings[0].descriptorCount = 1;
    scene_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    scene_bindings[1].binding = 1;
    scene_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    scene_bindings[1].descriptorCount = 1;
    scene_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    scene_bindings[2].binding = 2;
    scene_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    scene_bindings[2].descriptorCount = 1;
    scene_bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo scene_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    scene_layout_info.bindingCount = 3;
    scene_layout_info.pBindings = scene_bindings;
    check_vk(vkCreateDescriptorSetLayout(graphics::internal::context.device, &scene_layout_info, nullptr, &scene_layout), "Failed scene layout");

    VkDescriptorSetLayoutBinding model_binding{};
    model_binding.binding = 0;
    model_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    model_binding.descriptorCount = 1;
    model_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo model_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    model_layout_info.bindingCount = 1;
    model_layout_info.pBindings = &model_binding;
    check_vk(vkCreateDescriptorSetLayout(graphics::internal::context.device, &model_layout_info, nullptr, &model_layout), "Failed model layout");

    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }
    };
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = sizes;
    check_vk(vkCreateDescriptorPool(graphics::internal::context.device, &pool_info, nullptr, &descriptor_pool), "Failed pool");

    VkDescriptorSetAllocateInfo alloc_scene{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc_scene.descriptorPool = descriptor_pool;
    alloc_scene.descriptorSetCount = 1;
    alloc_scene.pSetLayouts = &scene_layout;
    check_vk(vkAllocateDescriptorSets(graphics::internal::context.device, &alloc_scene, &scene_set), "Failed alloc scene set");

    VkDescriptorSetAllocateInfo alloc_model{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc_model.descriptorPool = descriptor_pool;
    alloc_model.descriptorSetCount = 1;
    alloc_model.pSetLayouts = &model_layout;
    check_vk(vkAllocateDescriptorSets(graphics::internal::context.device, &alloc_model, &model_set), "Failed alloc model set");

    create_buffer(sizeof(SceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, scene_ubo_buffer, scene_ubo_alloc, reinterpret_cast<void**>(&mapped_scene_ubo));
    create_buffer(sizeof(ModelUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, model_ubo_buffer, model_ubo_alloc, reinterpret_cast<void**>(&mapped_model_ubo));
    create_buffer(MAX_POINT_LIGHTS * sizeof(PointLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, point_ssbo_buffer, point_ssbo_alloc, reinterpret_cast<void**>(&mapped_point_ssbo));
    create_buffer(MAX_SPOT_LIGHTS * sizeof(SpotLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, spot_ssbo_buffer, spot_ssbo_alloc, reinterpret_cast<void**>(&mapped_spot_ssbo));

    VkDescriptorBufferInfo scene_ubo_info{scene_ubo_buffer, 0, sizeof(SceneUniform)};
    VkDescriptorBufferInfo model_ubo_info{model_ubo_buffer, 0, sizeof(ModelUniform)};
    VkDescriptorBufferInfo point_ssbo_info{point_ssbo_buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo spot_ssbo_info{spot_ssbo_buffer, 0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet writes[4]{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = scene_set; writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &scene_ubo_info;

    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet = scene_set; writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &point_ssbo_info;

    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[2].dstSet = scene_set; writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &spot_ssbo_info;

    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[3].dstSet = model_set; writes[3].dstBinding = 0;
    writes[3].descriptorCount = 1; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[3].pBufferInfo = &model_ubo_info;

    vkUpdateDescriptorSets(graphics::internal::context.device, 4, writes, 0, nullptr);
}

void create_pipeline() {
    const auto vertex_code = read_binary("shaders/shader.vert.spv");
    const auto fragment_code = read_binary("shaders/shader.frag.spv");
    const VkShaderModule vertex_module = create_shader_module(vertex_code);
    const VkShaderModule fragment_module = create_shader_module(fragment_code);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
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
    blend_attachment.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkDescriptorSetLayout layouts[] = { scene_layout, model_layout };
    VkPipelineLayoutCreateInfo layout_info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layout_info.setLayoutCount = 2;
    layout_info.pSetLayouts = layouts;
    check_vk(vkCreatePipelineLayout(graphics::internal::context.device, &layout_info, nullptr, &pipeline_layout), "Failed pipeline layout");

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
    
    check_vk(vkCreateGraphicsPipelines(graphics::internal::context.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline), "Failed graphics pipeline");
    vkDestroyShaderModule(graphics::internal::context.device, vertex_module, nullptr);
    vkDestroyShaderModule(graphics::internal::context.device, fragment_module, nullptr);
}

glm::vec3 camera_forward() {
    const float yaw = glm::radians(camera_yaw);
    const float pitch = glm::radians(camera_pitch);
    return glm::normalize(glm::vec3(std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch)));
}

void update_uniforms() {
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

    mapped_scene_ubo->view_projection = projection * view;
    mapped_scene_ubo->camera_position = glm::vec4(camera_position, 1.0f);
    const glm::vec3 dir = glm::length(light_direction) > 0.0001f ? glm::normalize(light_direction) : glm::vec3(0, -1, 0);
    mapped_scene_ubo->light_direction = glm::vec4(dir, 0.0f);
    mapped_scene_ubo->light_color_intensity = glm::vec4(light_color, light_intensity);
    mapped_scene_ubo->ambient_color_intensity = glm::vec4(ambient_color, ambient_intensity);
    mapped_scene_ubo->counts = glm::uvec4(g_point_lights.size(), g_spot_lights.size(), 0, 0);

    mapped_model_ubo->model = model;
    mapped_model_ubo->albedo = glm::vec4(albedo, 1.0f);
    mapped_model_ubo->specular_shininess = glm::vec4(specular_color, shininess);

    check_vk(vmaFlushAllocation(graphics::internal::context.allocator, scene_ubo_alloc, 0, VK_WHOLE_SIZE), "Flush scene");
    check_vk(vmaFlushAllocation(graphics::internal::context.allocator, model_ubo_alloc, 0, VK_WHOLE_SIZE), "Flush model");
}

void update_lights() {
    for (size_t i = 0; i < g_point_lights.size(); ++i)
        mapped_point_ssbo[i] = g_point_lights[i];
    for (size_t i = 0; i < g_spot_lights.size(); ++i)
        mapped_spot_ssbo[i] = g_spot_lights[i];

    check_vk(vmaFlushAllocation(graphics::internal::context.allocator, point_ssbo_alloc, 0, VK_WHOLE_SIZE), "Flush points");
    check_vk(vmaFlushAllocation(graphics::internal::context.allocator, spot_ssbo_alloc, 0, VK_WHOLE_SIZE), "Flush spots");
}

} // namespace

bool initialize() {
    try {
        create_geometry();
        create_descriptors();
        create_pipeline();
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
    if (scene_layout) vkDestroyDescriptorSetLayout(device, scene_layout, nullptr);
    if (model_layout) vkDestroyDescriptorSetLayout(device, model_layout, nullptr);
    
    if (vertex_buffer) vmaDestroyBuffer(allocator, vertex_buffer, vertex_allocation);
    if (index_buffer) vmaDestroyBuffer(allocator, index_buffer, index_allocation);
    if (scene_ubo_buffer) vmaDestroyBuffer(allocator, scene_ubo_buffer, scene_ubo_alloc);
    if (model_ubo_buffer) vmaDestroyBuffer(allocator, model_ubo_buffer, model_ubo_alloc);
    if (point_ssbo_buffer) vmaDestroyBuffer(allocator, point_ssbo_buffer, point_ssbo_alloc);
    if (spot_ssbo_buffer) vmaDestroyBuffer(allocator, spot_ssbo_buffer, spot_ssbo_alloc);
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
    ImGui::ColorEdit3("Specular", &specular_color.x);
    ImGui::SliderFloat("Shininess", &shininess, 1.0f, 256.0f);
    ImGui::ColorEdit3("Ambient", &ambient_color.x);
    ImGui::SliderFloat("Ambient Int", &ambient_intensity, 0.0f, 2.0f);

    ImGui::SeparatorText("Point Lights");
    if (ImGui::Button("Add Point Light") && g_point_lights.size() < MAX_POINT_LIGHTS) {
        g_point_lights.push_back({ glm::vec4(0, 2, 0, 10), glm::vec4(1,1,1,1) });
    }
    for (int i = 0; i < (int)g_point_lights.size(); ++i) {
        ImGui::PushID(i);
        if (ImGui::Button("X")) { g_point_lights.erase(g_point_lights.begin() + i); ImGui::PopID(); break; }
        ImGui::SameLine();
        ImGui::SliderFloat3("Pos", &g_point_lights[i].position_intensity.x, -10, 10);
        ImGui::SliderFloat("Int", &g_point_lights[i].position_intensity.w, 0, 50);
        ImGui::ColorEdit3("Col", &g_point_lights[i].color.x);
        ImGui::PopID();
    }

    ImGui::SeparatorText("Spot Lights");
    if (ImGui::Button("Add Spot Light") && g_spot_lights.size() < MAX_SPOT_LIGHTS) {
        SpotLight sl;
        sl.position_intensity = glm::vec4(0, 4, 0, 20);
        sl.direction_inner_cos = glm::vec4(0, -1, 0, glm::cos(glm::radians(15.0f)));
        sl.color_outer_cos = glm::vec4(1,1,1, glm::cos(glm::radians(25.0f)));
        g_spot_lights.push_back(sl);
    }
    for (int i = 0; i < (int)g_spot_lights.size(); ++i) {
        ImGui::PushID(100 + i);
        if (ImGui::Button("X")) { g_spot_lights.erase(g_spot_lights.begin() + i); ImGui::PopID(); break; }
        ImGui::SameLine();
        ImGui::SliderFloat3("Pos", &g_spot_lights[i].position_intensity.x, -10, 10);
        ImGui::SliderFloat("Int", &g_spot_lights[i].position_intensity.w, 0, 50);
        ImGui::SliderFloat3("Dir", &g_spot_lights[i].direction_inner_cos.x, -1, 1);
        
        float innerDeg = glm::degrees(glm::acos(glm::clamp(g_spot_lights[i].direction_inner_cos.w, -1.0f, 1.0f)));
        float outerDeg = glm::degrees(glm::acos(glm::clamp(g_spot_lights[i].color_outer_cos.w, -1.0f, 1.0f)));
        
        ImGui::SliderFloat("Inner", &innerDeg, 1, 89);
        ImGui::SliderFloat("Outer", &outerDeg, 1, 89);
        
        if (innerDeg > outerDeg - 1.0f) innerDeg = outerDeg - 1.0f;

        g_spot_lights[i].direction_inner_cos.w = glm::cos(glm::radians(innerDeg));
        g_spot_lights[i].color_outer_cos.w = glm::cos(glm::radians(outerDeg));
        
        ImGui::ColorEdit3("Col", &g_spot_lights[i].color_outer_cos.x);
        ImGui::PopID();
    }

    ImGui::SeparatorText("Model");
    ImGui::SliderFloat3("Model Pos", &model_position.x, -5.0f, 5.0f);
    ImGui::SliderFloat3("Model Rot", &model_rotation.x, -180.0f, 180.0f);
    ImGui::SliderFloat3("Model Scl", &model_scale.x, 0.1f, 3.0f);
    
    ImGui::End();

    update_lights();
    update_uniforms();
}

void render(const graphics::internal::FrameData& frame) {
    const VkExtent2D extent = graphics::internal::context.swapchain_extent;
    VkCommandBuffer command = frame.command_buffer;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command, &begin), "Failed to begin cmd buffer");

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
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command, 0, 1, &viewport);
    
    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(command, 0, 1, &scissor);

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    
    VkDescriptorSet sets[] = { scene_set, model_set };
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 2, sets, 0, nullptr);
    
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &vertex_buffer, &offset);
    vkCmdBindIndexBuffer(command, index_buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(command, index_count, 1, 0, 0, 0);
    
    vkCmdEndRenderPass(command);
    check_vk(vkEndCommandBuffer(command), "Failed to end cmd buffer");
}

} // namespace application