
struct MeshLod {
    uint indexOffset;
    uint indexCount;
    float error;
};

struct Mesh {
    float3 center;
    float radius;
    uint vertexOffset;
    uint vertexCount;
    uint textureIndex;
    uint lodCount;
    MeshLod meshLods[8];
};

struct DrawData 
{
    float4 orientation;
    float3 position;
    float scale;
    uint meshIndex;
};

struct Globals {
    float P00, P11, near, far;
    float4 frustum;
    float4x4 projection;
    float4x4 view;
    float4 lightPos;
    uint selected;
    uint drawCount;
};

struct DrawCommand {
    uint drawId;
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    uint vertexOffset;
    uint firstInstance;
};

struct ShaderData {
    Globals *globals;
    Mesh *meshes;
    DrawCommand *drawCommands;
    uint *drawCommandCount;
    DrawData *drawData;
};

float3 rotateQuat(float3 v, float4 q)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}
