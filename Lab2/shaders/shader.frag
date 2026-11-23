#version 450

layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform Scene {
    mat4 viewProjection;
    mat4 model;
    vec4 cameraPosition;
    vec4 lightDirection;
    vec4 lightColorIntensity;
    vec4 ambientColorIntensity;
    vec4 albedo;
    vec4 specularShininess;
} scene;

void main() {
    vec3 normal = normalize(worldNormal);
    vec3 light = normalize(-scene.lightDirection.xyz);
    vec3 view = normalize(scene.cameraPosition.xyz - worldPosition);
    vec3 halfway = normalize(light + view);

    float diffuse = max(dot(normal, light), 0.0);
    float specular = pow(max(dot(normal, halfway), 0.0), max(scene.specularShininess.w, 1.0));

    vec3 ambient = scene.albedo.rgb * scene.ambientColorIntensity.rgb * scene.ambientColorIntensity.w;
    vec3 direct = (scene.albedo.rgb * diffuse + scene.specularShininess.rgb * specular) * scene.lightColorIntensity.rgb * scene.lightColorIntensity.w;
    outColor = vec4(ambient + direct, 1.0);
}
