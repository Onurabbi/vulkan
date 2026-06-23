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
VSOutput main(uint instanceIndex : SV_VulkanInstanceID, uint drawID : SV_DrawIndex, uint vertexIndex : SV_VertexID)
{
    VSOutput output;

    DrawCommand *drawCommands = shaderData.drawCommands;
    DrawData *drawData = shaderData.drawData;
    Globals *globals = shaderData.globals;
    DrawCommand cmd = drawCommands[drawID];
    DrawData meshDraw = drawData[cmd.drawId];
    Mesh mesh = shaderData.meshes[meshDraw.meshIndex];
    Vertex vertex = shaderData.vertices[vertexIndex];
    float3 pos      = float3(vertex.vx, vertex.vy, vertex.vz);
    float3 normal   = float3(int(vertex.nx), int(vertex.ny), int(vertex.nz)) / 127.0 - 1.0;
    float2 uv       = float2(vertex.u, vertex.v);
    float4 tangent  = float4(int(vertex.tx), int(vertex.ty), int(vertex.tz), int(vertex.tw)); 
    float3 wpos     = rotateQuat(pos, meshDraw.orientation) * meshDraw.scale + meshDraw.position;
    output.position = mul(globals.projection, mul(globals.view, float4(wpos, 1.0f)));
    output.normal   = normal;
    output.uv       = uv;
    output.wPos     = wpos;
    output.drawID   = cmd.drawId;
    return output;
}

[shader("fragment")]
float4 main(VSOutput input, float4 fragCoord: SV_Position) {
    DrawData drawData = shaderData.drawData[NonUniformResourceIndex(input.drawID)];
    Material material = shaderData.materials[NonUniformResourceIndex(drawData.materialIndex)];

    float deband = gradientNoise(fragCoord.xy) * 2 - 1;
    float4 albedo = material.diffuseFactor;
    albedo *= textures[NonUniformResourceIndex(material.albedo)].Sample(input.uv);
    return float4(albedo.rgb, 1.0);
}
