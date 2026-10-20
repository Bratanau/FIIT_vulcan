#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 worldNormal;

layout(set = 0, binding = 0) uniform Scene {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 lightDirection;
    vec4 lightColorIntensity;
    vec4 ambientColorIntensity;
    uvec4 counts;
} scene;

layout(set = 1, binding = 0) uniform Model {
    mat4 model;
    vec4 albedo;
    vec4 specularShininess;
} model;

void main() {
    vec4 position = model.model * vec4(inPosition, 1.0);
    worldPosition = position.xyz;
    worldNormal = mat3(transpose(inverse(model.model))) * inNormal;
    gl_Position = scene.viewProjection * position;
}
