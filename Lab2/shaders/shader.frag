#version 450

layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 0) out vec4 outColor;

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

struct PointLight {
    vec4 positionIntensity;
    vec4 color;
};
layout(std430, set = 0, binding = 1) readonly buffer PointLights {
    PointLight pointLights[];
};

struct SpotLight {
    vec4 positionIntensity;
    vec4 directionInnerCos;
    vec4 colorOuterCos;
};
layout(std430, set = 0, binding = 2) readonly buffer SpotLights {
    SpotLight spotLights[];
};

vec3 blinn_phong_spot(SpotLight light, vec3 N, vec3 worldPos, vec3 V, vec3 albedo, vec3 specColor, float shininess) {
    vec3 toLight = light.positionIntensity.xyz - worldPos;
    float dist = length(toLight);
    vec3 L = toLight / max(dist, 1e-4);
    vec3 H = normalize(L + V);

    float diff = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), max(shininess, 1.0));

    vec3 spotDir = normalize(light.directionInnerCos.xyz);
    float cosAngle = dot(-L, spotDir);
    float innerCos = light.directionInnerCos.w;
    float outerCos = light.colorOuterCos.w;
    float spotFactor = smoothstep(outerCos, innerCos, cosAngle);

    float attenuation = 1.0 / max(dist * dist, 1e-4);
    float intensity = light.positionIntensity.w;

    vec3 direct = (albedo * diff + specColor * spec) * light.colorOuterCos.rgb * intensity;
    return direct * attenuation * spotFactor;
}

vec3 blinn_phong_point(PointLight light, vec3 N, vec3 worldPos, vec3 V, vec3 albedo, vec3 specColor, float shininess) {
    vec3 toLight = light.positionIntensity.xyz - worldPos;
    float dist = length(toLight);
    vec3 L = toLight / max(dist, 1e-4);
    vec3 H = normalize(L + V);

    float diff = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), max(shininess, 1.0));

    float attenuation = 1.0 / max(dist * dist, 1e-4);
    float intensity = light.positionIntensity.w;

    vec3 direct = (albedo * diff + specColor * spec) * light.color.rgb * intensity;
    return direct * attenuation;
}

void main() {
    vec3 N = normalize(worldNormal);
    vec3 V = normalize(scene.cameraPosition.xyz - worldPosition);
    vec3 L = normalize(-scene.lightDirection.xyz);
    vec3 H = normalize(L + V);

    float diff = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), max(model.specularShininess.w, 1.0));

    vec3 ambient = model.albedo.rgb * scene.ambientColorIntensity.rgb * scene.ambientColorIntensity.w;
    vec3 direct = (model.albedo.rgb * diff + model.specularShininess.rgb * spec) * scene.lightColorIntensity.rgb * scene.lightColorIntensity.w;

    vec3 result = ambient + direct;

    for (uint i = 0u; i < scene.counts.x; ++i)
        result += blinn_phong_point(pointLights[i], N, worldPosition, V, model.albedo.rgb, model.specularShininess.rgb, model.specularShininess.w);

    for (uint i = 0u; i < scene.counts.y; ++i)
        result += blinn_phong_spot(spotLights[i], N, worldPosition, V, model.albedo.rgb, model.specularShininess.rgb, model.specularShininess.w);

    outColor = vec4(result, 1.0);
}
