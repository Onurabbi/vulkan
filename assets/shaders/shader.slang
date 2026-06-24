#include "mesh.glsl"
#include "math.glsl"

Sampler2D textures[];

[[vk::push_constant]] ShaderData shaderData;

struct VSOutput {
    float4 position : SV_Position;
    float3 normal;
    float4 tangent;
    float2 uv;
    float3 wPos;
    uint drawID;
};

[shader("vertex")]
VSOutput main(uint instanceIndex : SV_VulkanInstanceID, uint drawID : SV_DrawIndex, uint vi : SV_VertexID)
{
    VSOutput output;

    DrawCommand *drawCommands = shaderData.drawCommands;
    DrawData *drawData = shaderData.drawData;
    Globals *globals = shaderData.globals;
    DrawCommand cmd = drawCommands[drawID];
    DrawData meshDraw = drawData[cmd.drawId];
    Mesh mesh = shaderData.meshes[meshDraw.meshIndex];
    Vertex *vertices = shaderData.vertices;
    uint vertexIndex = vi + cmd.vertexOffset;
    float3 pos      = float3(vertices[vertexIndex].vx, vertices[vertexIndex].vy, vertices[vertexIndex].vz);
    float3 normal   = float3(int(vertices[vertexIndex].nx), int(vertices[vertexIndex].ny), int(vertices[vertexIndex].nz)) / 127.0 - 1.0;
    float2 uv       = float2(vertices[vertexIndex].u, vertices[vertexIndex].v);
    float4 tangent  = float4(int(vertices[vertexIndex].tx), int(vertices[vertexIndex].ty), int(vertices[vertexIndex].tz), int(vertices[vertexIndex].tw)) / 127.0 - 1.0; 
    float3 wpos     = rotateQuat(pos, meshDraw.orientation) * meshDraw.scale + meshDraw.position;
    output.position = mul(globals.projection, mul(globals.view, float4(wpos, 1.0f)));
    output.normal   = normal;
    output.uv       = uv;
    output.wPos     = wpos;
    output.drawID   = cmd.drawId;
    return output;
}

[shader("fragment")]
float4 main(VSOutput input) {
    DrawData drawData = shaderData.drawData[input.drawID];
    Material material = shaderData.materials[drawData.materialIndex];

    float deband = gradientNoise(input.position.xy) * 2 - 1;
    float4 albedo = material.diffuseFactor;
    albedo *= textures[NonUniformResourceIndex(material.albedo)].Sample(input.uv);
    return float4(albedo.rgb, 1.0);
}
