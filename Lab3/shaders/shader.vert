#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec2 texCoord;

layout(set = 0, binding = 0) uniform ModelProperties {
    mat4 viewProjection;
    mat4 model;
    vec4 cameraPosition;
    vec4 lightDirectionIntensity;
    vec4 lightColor;
    vec4 ambientColorIntensity;
    vec4 albedoTint;
    vec4 specularColorShininess;
    vec4 emissiveParameters;
} properties;

void main() {
    vec4 position = properties.model * vec4(inPosition, 1.0);
    worldPosition = position.xyz;
    worldNormal = mat3(transpose(inverse(properties.model))) * inNormal;
    texCoord = inTexCoord;
    gl_Position = properties.viewProjection * position;
}
