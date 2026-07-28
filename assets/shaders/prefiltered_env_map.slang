#include "includes/math.slang"
#include "includes/filtercube.slang"
#include "includes/pbr_helpers.slang"

static const uint NUM_SAMPLES = 32;

float3 PrefilterEnvMap(float3 R, float roughness)
{
    float3 N = R;
    float3 V = R;
    float3 color = float3(0.0);
    float totalWeight = 0.0;
    uint width, height, numMipLevels;
    textures[skyboxData.textureIndex].GetDimensions(0, width, height, numMipLevels);

    for (uint i = 0; i < NUM_SAMPLES; i++) {
        float2 Xi = Hammersley2d(i, NUM_SAMPLES);
        float3 H = ImportanceSample_GGX(Xi, roughness, N);
        float3 L = 2.0 * dot(V, H) * H - V;
        float dotNL = clamp(dot(N, L), 0.0, 1.0);
        if (dotNL > 0.0) {
            float dotNH = clamp(dot(N, H), 0.0, 1.0);
            float dotVH = clamp(dot(V, H), 0.0, 1.0);
            float pdf =  D_GGX(dotNH, roughness) * dotNH / (4.0 * dotVH) + 0.0001;
            float omegaS = 1.0 / (float(NUM_SAMPLES) * pdf);
            float omegaP = 4.0 * PI / (6.0 * width * width);
            //Biased (+1.0) mip level for better result
            float mipLevel = roughness == 0 ? 0.0 : max(0.5 * log2(omegaS / omegaP) + 1.0, 0.0);
            color += textures[skyboxData.textureIndex].SampleLevel(L, mipLevel).rgb * dotNL;
            totalWeight += dotNL;
        }
    }

    return (color / totalWeight);
}

[shader("fragment")]
float4 main(VSOutput input) : SV_Target
{
    float3 N = normalize(input.uvw);
    return float4(PrefilterEnvMap(N, skyboxData.roughness), 1.0);
}