#include "mesh.glsl"
#include "math.glsl"

struct VSInput {
    float3 Pos;
    float3 Normal;
	float2 UV;
};

Sampler2D textures[];

[[vk::push_constant]] ShaderData shaderData;

struct VSOutput {
    float4 Pos : SV_POSITION;
    float3 Normal;
    float2 UV;
    float3 wPos;
    uint drawID;
};

[shader("vertex")]
VSOutput main(VSInput input, uint instanceIndex : SV_VulkanInstanceID, uint drawID : SV_DrawIndex)
{
    VSOutput output;

    DrawCommand *drawCommands = shaderData.drawCommands;
    DrawData *drawData = shaderData.drawData;
    Globals *globals = shaderData.globals;
    DrawCommand cmd = drawCommands[drawID];
    DrawData meshDraw = drawData[cmd.drawId];
    Mesh mesh = shaderData.meshes[meshDraw.meshIndex];

    float3 wpos    = rotateQuat(input.Pos, meshDraw.orientation) * meshDraw.scale + meshDraw.position;
    output.Pos     = mul(globals.projection, mul(globals.view, float4(wpos, 1.0f)));
    output.Normal  = input.Normal;
    output.UV      = input.UV;
    output.wPos    = wpos;
    output.drawID  = drawID;
    return output;
}

[shader("fragment")]
float4 main(VSOutput input, float4 fragCoord: SV_Position) {
    DrawData drawData = shaderData.drawData[NonUniformResourceIndex(input.drawID)];
    Material material = shaderData.materials[NonUniformResourceIndex(drawData.materialIndex)];

    float deband = gradientNoise(fragCoord.xy) * 2 - 1;
    float4 albedo = material.diffuseFactor;
#if 1
    if (material.albedo >= 0 || material.albedo < 389) {
        albedo *= textures[NonUniformResourceIndex(material.albedo)].Sample(input.UV);
    }
#else
    albedo *= textures[NonUniformResourceIndex(material.albedo)].Sample(input.UV);

#endif
    return float4(albedo.rgb, 1.0);
}
