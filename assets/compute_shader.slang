#include "mesh.h"

[[vk::push_constant]] ShaderData shaderData;

[shader("compute")]
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadId)
{
    Mesh* meshes = shaderData.meshes;
    DrawData *drawData = shaderData.drawData;
    DrawCommand *drawCommands = shaderData.drawCommands;
    Globals *globals = shaderData.globals;

    uint drawCount = globals.drawCount;
    if (id.x >= drawCount) {
        return;
    }

    DrawData meshDraw = drawData[id.x];
  
    Mesh mesh = meshes[meshDraw.meshIndex];
    float3 center = mesh.center;
    float radius = mesh.radius;
    uint32_t vertexCount = mesh.vertexCount;
    uint32_t vertexOffset = mesh.vertexOffset;

    center = rotateQuat(center, meshDraw.orientation) * meshDraw.scale + meshDraw.position;
    center = mul(globals.view, float4(center, 1.0f)).xyz;
    radius = radius * meshDraw.scale;

    bool visible = true;
    visible = visible && center.z * globals.frustum[1] - abs(center.x) * globals.frustum[0] > -radius;
    visible = visible && center.z * globals.frustum[3] - abs(center.y) * globals.frustum[2] > -radius;
    visible = visible && center.z + radius > globals.near && center.z - radius < globals.far;

    if (visible) {
        uint dci = __atomic_add(shaderData.drawCommandCount[0], 1);

        // Calculate LOD based on distance relative to the object's radius (approximate screen size).
        // lodScale controls how aggressively LODs drop. Higher value = stays high-detail further away.
        float lodScale = 5.0f; // Tip: You can pass this via globals to tweak it at runtime!
        float dist = max(0.0f, distance(center, float3(0)) - radius);
        float lodTarget = log2(max(1.0f, dist / max(0.001f, radius * lodScale)));
        uint lodIndex = clamp(int(lodTarget), 0, int(mesh.lodCount) - 1);

        MeshLod lod = mesh.meshLods[lodIndex];

        drawCommands[dci].indexCount = lod.indexCount;
        drawCommands[dci].drawId = id.x;
        drawCommands[dci].instanceCount = 1;
        drawCommands[dci].firstIndex = lod.indexOffset;
        drawCommands[dci].vertexOffset = vertexOffset;
        drawCommands[dci].firstInstance = 0;
    }
}
