#version 450

layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 texCoord;
layout(location = 0) out vec4 outColor;

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
layout(set = 0, binding = 1) uniform sampler2D albedoTexture;
layout(set = 0, binding = 2) uniform sampler2D specularTexture;
layout(set = 0, binding = 3) uniform sampler2D emissiveTexture;

layout(push_constant) uniform SamplingOptions {
    int animatedSampling;
    float time;
    float strength;
    float padding;
} sampling;

void main() {
    vec2 uv = texCoord;
    vec4 albedoSample;
    if (sampling.animatedSampling != 0) {
        vec2 shift = vec2(sin(sampling.time + uv.y * 8.0),
                          cos(sampling.time + uv.x * 8.0)) * 0.035 * sampling.strength;
        vec4 center = texture(albedoTexture, uv + shift);
        vec4 offsetA = texture(albedoTexture, uv * 1.7 + vec2(0.13, -sampling.time * 0.05));
        vec4 offsetB = texture(albedoTexture, uv * 0.63 - vec2(sampling.time * 0.07, 0.19));
        albedoSample = mix(center, (center + offsetA + offsetB) / 3.0, 0.35);
        albedoSample.rgb *= 0.8 + 0.2 * sin(sampling.time + uv.x * 9.0);
    } else {
        albedoSample = texture(albedoTexture, uv);
    }

    vec3 albedo = albedoSample.rgb * properties.albedoTint.rgb;
    vec3 normal = normalize(worldNormal);
    vec3 light = normalize(-properties.lightDirectionIntensity.xyz);
    vec3 view = normalize(properties.cameraPosition.xyz - worldPosition);
    vec3 halfway = normalize(light + view);
    float diffuse = max(dot(normal, light), 0.0);
    float specularMask = texture(specularTexture, uv).r;
    float specular = pow(max(dot(normal, halfway), 0.0),
                         max(properties.specularColorShininess.w, 1.0))
                   * specularMask;

    vec3 ambient = albedo * properties.ambientColorIntensity.rgb
                 * properties.ambientColorIntensity.w;
    vec3 direct = (albedo * diffuse
                 + properties.specularColorShininess.rgb * specular)
                * properties.lightColor.rgb * properties.lightDirectionIntensity.w;
    vec3 emissive = texture(emissiveTexture, uv).rgb * properties.emissiveParameters.x;
    outColor = vec4(ambient + direct + emissive, albedoSample.a);
}
