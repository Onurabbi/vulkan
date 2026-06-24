struct Vertex {
    half vx, vy, vz, vw;
    uint8_t nx, ny, nz, nw;
    uint8_t tx, ty, tz, tw;
    half u, v;
};

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
    uint lodCount;
    MeshLod meshLods[8];
};

struct DrawData 
{
    float4 orientation;
    float3 position;
    float scale;
    uint meshIndex;
    uint materialIndex;
    uint postPass;
};

struct Globals {
    float P00, P11, near, far;
    float4 frustum;
    float4x4 projection;
    float4x4 view;
    float4 lightPos;
    uint selected;
    uint drawCount;
    float lodTarget;
};

struct DrawCommand {
    uint drawId;
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    uint vertexOffset;
    uint firstInstance;
};

struct Material {
    uint albedo;
    uint normal;
    uint specular;
    uint emissive;
    float4 diffuseFactor;
    float4 specularFactor;
    float3 emissiveFactor;
};

struct ShaderData {
    Globals *globals;
    Mesh *meshes;
    DrawCommand *drawCommands;
    uint *drawCommandCount;
    DrawData *drawData;
    Material *materials;
    Vertex *vertices;
};
