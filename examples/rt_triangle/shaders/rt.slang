// Minimal ray-tracing pipeline: orthographic rays against one triangle.
// Hits shade barycentrics, misses shade dark blue - enough to verify raygen /
// miss / closest-hit dispatch through the SBT end-to-end.
import vsh;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure uScene;
[[vk::binding(1, 0)]] RWTexture2D<float4> uOutput;

struct Payload
{
    float3 color;
};

[shader("raygen")]
void raygenMain()
{
    const uint2  pixel = DispatchRaysIndex().xy;
    const uint2  dim   = DispatchRaysDimensions().xy;
    const float2 uv    = (float2(pixel) + 0.5) / float2(dim);
    const float2 ndc   = uv * 2.0 - 1.0;

    RayDesc ray;
    ray.Origin    = float3(ndc.x, -ndc.y, -2.0); // orthographic, looking down +Z
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin      = 0.01;
    ray.TMax      = 10.0;

    Payload payload;
    payload.color = float3(0.0, 0.0, 0.0);
    TraceRay(uScene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    uOutput[pixel] = float4(payload.color, 1.0);
}

[shader("miss")]
void missMain(inout Payload payload)
{
    payload.color = float3(0.05, 0.05, 0.15);
}

[shader("closesthit")]
void closestHitMain(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    payload.color =
        float3(attribs.barycentrics, 1.0 - attribs.barycentrics.x - attribs.barycentrics.y);
}
