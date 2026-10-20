#include "application.hpp"

#include <cmath>
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
#include <vk_mem_alloc.h>

namespace application {

namespace {

struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
};

struct UBO {
    glm::mat4 mvp;
    glm::vec4 color;
};

struct Object {
    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    VmaAllocation vertex_alloc = VK_NULL_HANDLE;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VmaAllocation index_alloc = VK_NULL_HANDLE;
    uint32_t index_count = 0;

    VkBuffer uniform_buffer = VK_NULL_HANDLE;
    VmaAllocation uniform_alloc = VK_NULL_HANDLE;
    void* uniform_mapped = nullptr;

    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

    glm::vec3 position {0.0f};
    glm::vec3 rotation {0.0f};
    glm::vec3 scale {1.0f};
    glm::vec3 color {1.0f};

    bool  animated = true;
    float orbit_radius = 2.0f;
    float orbit_height = 0.0f;
    float orbit_speed = 1.0f;
    float spin_speed = 45.0f;
};

VkDescriptorSetLayout g_descriptor_set_layout = VK_NULL_HANDLE;
VkDescriptorPool g_descriptor_pool = VK_NULL_HANDLE;
VkPipelineLayout g_pipeline_layout = VK_NULL_HANDLE;
VkPipeline g_pipeline = VK_NULL_HANDLE;

std::vector<Object> g_objects;

bool  g_use_perspective = true;
float g_fov = 60.0f;
float g_ortho_height = 4.0f;
float g_near_plane = 0.1f;
float g_far_plane = 100.0f;

bool   g_playing = true;
float  g_animation_speed = 1.0f;
double g_animation_time = 0.0;

glm::vec3 g_camera_position {4.0f, 3.0f, 4.0f};
glm::vec3 g_camera_target {0.0f, 0.0f, 0.0f};

std::vector<char> read_file(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("Failed to open file: " + filename);
    size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), size);
    return buffer;
}

VkShaderModule create_shader_module(const std::vector<char>& code) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(graphics::internal::context.device, &info, nullptr, &mod);
    return mod;
}

void create_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* out_buf, VmaAllocation* out_alloc, void** out_mapped) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo info{};
    vmaCreateBuffer(graphics::internal::context.allocator, &bi, &ai, out_buf, out_alloc, &info);
    if (out_mapped) *out_mapped = info.pMappedData;
}

void make_cube(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
    glm::vec3 P[8] = {
        {-1,-1,-1}, {1,-1,-1}, {1, 1,-1}, {-1, 1,-1},
        {-1,-1, 1}, {1,-1, 1}, {1, 1, 1}, {-1, 1, 1},
    };
    int F[6][4] = {
        {0,3,2,1},
        {4,5,6,7}, 
        {0,1,5,4}, 
        {3,7,6,2}, 
        {0,4,7,3}, 
        {1,2,6,5},
    };
    for (int f = 0; f < 6; ++f) {
        uint32_t base = static_cast<uint32_t>(vertices.size());
        for (int v = 0; v < 4; ++v) {
            glm::vec3 p = P[F[f][v]];
            Vertex vert;
            vert.position = p;
            vert.color = (p + glm::vec3(1.0f)) * 0.5f;  // задание 5
            vertices.push_back(vert);
        }
        indices.insert(indices.end(), {
            base, base + 1, base + 2,
            base, base + 2, base + 3
        });
    }
}

void make_sphere(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                 float radius, int stacks, int slices) {
    for (int i = 0; i <= stacks; ++i) {
        float phi = glm::pi<float>() * (float)i / (float)stacks;
        for (int j = 0; j <= slices; ++j) {
            float theta = glm::two_pi<float>() * (float)j / (float)slices;
            glm::vec3 p = radius * glm::vec3(
                sinf(phi) * cosf(theta),
                cosf(phi),
                sinf(phi) * sinf(theta));
            Vertex vert;
            vert.position = p;
            vert.color = (p / radius + glm::vec3(1.0f)) * 0.5f;
            vertices.push_back(vert);
        }
    }
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            uint32_t a = (uint32_t)(i * (slices + 1) + j);
            uint32_t b = a + slices + 1;
            indices.insert(indices.end(), {
                a, b, a + 1,
                a + 1, b, b + 1
            });
        }
    }
}

void create_descriptor_set_layout() {
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorCount = 1;
    b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings = &b;
    vkCreateDescriptorSetLayout(graphics::internal::context.device, &info, nullptr, &g_descriptor_set_layout);
}

void create_pipeline() {
    auto vert_code = read_file("shaders/shader.vert.spv");
    auto frag_code = read_file("shaders/shader.frag.spv");
    VkShaderModule vert_mod = create_shader_module(vert_code);
    VkShaderModule frag_mod = create_shader_module(frag_code);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vbind{};
    vbind.binding = 0;
    vbind.stride = sizeof(Vertex);
    vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vattr[2]{};
    vattr[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position) };
    vattr[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color) };

    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &vbind;
    vin.vertexAttributeDescriptionCount = 2;
    vin.pVertexAttributeDescriptions = vattr;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType  = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cb_att{};
    cb_att.colorWriteMask = 0xF;
    cb_att.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cb_att;

    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamic_states;

    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_descriptor_set_layout;
    vkCreatePipelineLayout(graphics::internal::context.device, &pl, nullptr, &g_pipeline_layout);

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vin;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dyn;
    pi.layout = g_pipeline_layout;
    pi.renderPass = graphics::internal::context.render_pass;
    pi.subpass = 0;

    vkCreateGraphicsPipelines(graphics::internal::context.device, VK_NULL_HANDLE, 1, &pi, nullptr, &g_pipeline);

    vkDestroyShaderModule(graphics::internal::context.device, vert_mod, nullptr);
    vkDestroyShaderModule(graphics::internal::context.device, frag_mod, nullptr);
}

Object build_object(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, VkDescriptorSet dset) {
    Object obj;
    obj.index_count = static_cast<uint32_t>(indices.size());
    obj.descriptor_set = dset;

    void* vertex_mapped = nullptr;
    create_host_buffer(vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &obj.vertex_buffer, &obj.vertex_alloc, &vertex_mapped);
    memcpy(vertex_mapped, vertices.data(), vertices.size() * sizeof(Vertex));

    void* index_mapped = nullptr;
    create_host_buffer(indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &obj.index_buffer, &obj.index_alloc, &index_mapped);
    memcpy(index_mapped, indices.data(), indices.size() * sizeof(uint32_t));

    create_host_buffer(sizeof(UBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &obj.uniform_buffer, &obj.uniform_alloc, &obj.uniform_mapped);

    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = obj.uniform_buffer;
    buffer_info.offset = 0;
    buffer_info.range = sizeof(UBO);

    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = dset;          
    w.dstBinding = 0;
    w.dstArrayElement = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(graphics::internal::context.device, 1, &w, 0, nullptr);

    return obj;
}

} 

bool initialize() {
    create_descriptor_set_layout();

    const uint32_t object_count = 2;

    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps.descriptorCount = object_count;

    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = object_count;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    vkCreateDescriptorPool(graphics::internal::context.device, &dpi, nullptr, &g_descriptor_pool);

    std::vector<VkDescriptorSetLayout> layouts(object_count, g_descriptor_set_layout);
    std::vector<VkDescriptorSet> dsets(object_count, VK_NULL_HANDLE);

    VkDescriptorSetAllocateInfo dsi{};
    dsi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsi.descriptorPool = g_descriptor_pool;
    dsi.descriptorSetCount = object_count;
    dsi.pSetLayouts = layouts.data();
    vkAllocateDescriptorSets(graphics::internal::context.device, &dsi, dsets.data());

    create_pipeline();
    {
        std::vector<Vertex> verts;
        std::vector<uint32_t> indices;
        make_cube(verts, indices);
        Object cube = build_object(verts, indices, dsets[0]);
        cube.color = { 1.0f, 0.7f, 0.4f };
        cube.scale = { 0.7f, 0.7f, 0.7f };
        cube.orbit_radius = 2.0f;
        cube.spin_speed = 45.0f;
        g_objects.push_back(cube);
    }
    {
        std::vector<Vertex> verts;
        std::vector<uint32_t> indices;
        make_sphere(verts, indices, 1.0f, 16, 24);
        Object sphere = build_object(verts, indices, dsets[1]);
        sphere.color = { 0.4f, 0.8f, 1.0f };
        sphere.scale = { 0.5f, 0.5f, 0.5f };
        sphere.orbit_radius = 4.0f;
        sphere.orbit_speed = 0.5f;
        sphere.spin_speed = 20.0f;
        g_objects.push_back(sphere);
    }

    return true;
}

void shutdown() {
    VkDevice dev = graphics::internal::context.device;
    VmaAllocator alloc = graphics::internal::context.allocator;

    for (auto& obj : g_objects) {
        vmaDestroyBuffer(alloc, obj.vertex_buffer, obj.vertex_alloc);
        vmaDestroyBuffer(alloc, obj.index_buffer, obj.index_alloc);
        vmaDestroyBuffer(alloc, obj.uniform_buffer, obj.uniform_alloc);
    }
    g_objects.clear();

    if (g_pipeline) vkDestroyPipeline(dev, g_pipeline, nullptr);
    if (g_pipeline_layout) vkDestroyPipelineLayout(dev, g_pipeline_layout, nullptr);
    if (g_descriptor_pool) vkDestroyDescriptorPool(dev, g_descriptor_pool, nullptr);
    if (g_descriptor_set_layout) vkDestroyDescriptorSetLayout(dev, g_descriptor_set_layout, nullptr);
}

namespace {
glm::mat4 compute_projection(float aspect) {
    if (g_use_perspective) {
        glm::mat4 proj = glm::perspective(glm::radians(g_fov), aspect, g_near_plane, g_far_plane);
        proj[1][1] *= -1.0f; 
        return proj;
    } else {
        float h = g_ortho_height;
        float w = h * aspect;
        glm::mat4 proj = glm::ortho(-w, w, -h, h, g_near_plane, g_far_plane);
        proj[1][1] *= -1.0f;
        return proj;
    }
}
} // namespace

void update(double time) {
    static double last_time = time;
    double dt = time - last_time;
    last_time = time;

    if (g_playing) g_animation_time += dt * g_animation_speed; 

    ImGui::Begin("Lab1");

    ImGui::SeparatorText("Projection");                         
    if (ImGui::RadioButton("Perspective",  g_use_perspective)) g_use_perspective = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("Orthographic", !g_use_perspective)) g_use_perspective = false;
    if (g_use_perspective) ImGui::SliderFloat("FOV",          &g_fov,          20.0f, 120.0f);
    else                   ImGui::SliderFloat("Ortho Height", &g_ortho_height,  0.5f,  10.0f);
    ImGui::SliderFloat("Near", &g_near_plane, 0.01f, 5.0f);
    ImGui::SliderFloat("Far", &g_far_plane, 5.0f, 200.0f);

    ImGui::SeparatorText("Animation");                         
    ImGui::Checkbox("Playing", &g_playing);
    ImGui::SliderFloat("Speed", &g_animation_speed, 0.0f, 5.0f);

    ImGui::SeparatorText("Objects");                          
    for (size_t i = 0; i < g_objects.size(); ++i) {
        Object& obj = g_objects[i];
        ImGui::PushID((int)i);
        ImGui::Text("Object %zu", i);
        ImGui::SliderFloat3("Position", &obj.position.x, -5.0f, 5.0f);
        ImGui::SliderFloat3("Rotation", &obj.rotation.x, -180.0f, 180.0f);
        ImGui::SliderFloat3("Scale", &obj.scale.x, 0.1f,  5.0f);
        ImGui::ColorEdit3("Color multiplier", &obj.color.x);

        ImGui::Checkbox("Animated", &obj.animated);
        if (obj.animated) {
            ImGui::SliderFloat("Orbit radius", &obj.orbit_radius, 0.0f, 6.0f);
            ImGui::SliderFloat("Orbit height", &obj.orbit_height, -3.0f, 3.0f);
            ImGui::SliderFloat("Orbit speed", &obj.orbit_speed, -3.0f, 3.0f);
            ImGui::SliderFloat("Spin speed", &obj.spin_speed, -180.0f, 180.0f);
        }
        ImGui::PopID();
        ImGui::Separator();
    }
    ImGui::End();

    float aspect = (float)graphics::internal::context.swapchain_extent.width / (float)graphics::internal::context.swapchain_extent.height;
    glm::mat4 proj = compute_projection(aspect);
    glm::mat4 view = glm::lookAt(g_camera_position, g_camera_target, glm::vec3(0.0f, 1.0f, 0.0f));

    for (auto& obj : g_objects) {
        glm::vec3 anim_pos = obj.position;
        glm::vec3 anim_rot = obj.rotation;

        if (obj.animated) {
            float t     = (float)g_animation_time;
            float angle = t * obj.orbit_speed;
            anim_pos += glm::vec3(cosf(angle) * obj.orbit_radius, obj.orbit_height, sinf(angle) * obj.orbit_radius);
            anim_rot.y += t * obj.spin_speed;
        }

        glm::mat4 model = glm::translate(glm::mat4(1.0f), anim_pos);
        model = glm::rotate(model, glm::radians(anim_rot.x), glm::vec3(1, 0, 0));
        model = glm::rotate(model, glm::radians(anim_rot.y), glm::vec3(0, 1, 0));
        model = glm::rotate(model, glm::radians(anim_rot.z), glm::vec3(0, 0, 1));
        model = glm::scale(model, obj.scale);

        UBO ubo;
        ubo.mvp = proj * view * model;
        ubo.color = glm::vec4(obj.color, 1.0f);
        memcpy(obj.uniform_mapped, &ubo, sizeof(UBO));
    }
}

void render(const graphics::internal::FrameData& fd) {
    VkCommandBuffer cmd = fd.command_buffer;
    VkExtent2D ext = graphics::internal::context.swapchain_extent;

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkClearValue clear_values[2];
    clear_values[0].color = {{0.05f, 0.05f, 0.08f, 1.0f}};
    clear_values[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = graphics::internal::context.render_pass;
    rp.framebuffer = fd.framebuffer;
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = ext;
    rp.clearValueCount = 2;
    rp.pClearValues = clear_values;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.width = (float)ext.width;
    vp.height = (float)ext.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = ext;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);

    for (const auto& obj : g_objects) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &obj.vertex_buffer, &offset);
        vkCmdBindIndexBuffer(cmd, obj.index_buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline_layout, 0, 1, &obj.descriptor_set, 0, nullptr);
        vkCmdDrawIndexed(cmd, obj.index_count, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

} // namespace application