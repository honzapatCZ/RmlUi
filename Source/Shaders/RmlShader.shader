#include "./Flax/GUICommon.hlsl"
#define MAX_NUM_STOPS 16

#define LINEAR 0
#define RADIAL 1
#define CONIC 2
#define REPEATING_LINEAR 3
#define REPEATING_RADIAL 4
#define REPEATING_CONIC 5

struct BasicVertex
{
    float2 Position : POSITION0;
    float2 TexCoord : TEXCOORD0;
    float4 Color : COLOR0;
    float2 ClipOrigin : TEXCOORD1;
    float4 ClipExtents : TEXCOORD2;
};

META_CB_BEGIN(0, Data)
int _func;
int _num_stops;
float2 _p;
float2 _v;


float2 Offset;
float4x4 ViewProjection;
float4x4 Model;
META_CB_END

StructuredBuffer<float4> _stop_colors : register(t0);
StructuredBuffer<float> _stop_positions : register(t1);

META_VS(true, FEATURE_LEVEL_ES2)
META_VS_IN_ELEMENT(POSITION, 0, R32G32_FLOAT,       0, ALIGN, PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(TEXCOORD, 0, R16G16_FLOAT,       0, ALIGN, PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(COLOR,    0, R32G32B32A32_FLOAT, 0, ALIGN, PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(TEXCOORD, 1, R32G32_FLOAT,       0, 0,     PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(TEXCOORD, 2, R32G32B32A32_FLOAT, 0, ALIGN, PER_VERTEX, 0, true)
VS2PS VS(BasicVertex input)
{
    VS2PS output;

    output.Position = mul(mul(float4(input.Position + Offset, 0, 1), Model), ViewProjection);
    output.Color = input.Color;
    output.TexCoord = input.TexCoord;
    output.ClipOriginAndPos = float4(input.ClipOrigin.xy, input.Position);
    output.ClipExtents = input.ClipExtents;
    output.CustomData = input.ClipOrigin.xy;

    return output;
}

META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_Gradient(VS2PS input) : SV_Target0
{
    PerformClipping(input);
    float t = 0.0;

    if (_func == LINEAR || _func == REPEATING_LINEAR)
    {
        float dist_square = dot(_v, _v);
        float2 V = input.TexCoord - _p;
        t = dot(_v, V) / dist_square;
    }
    else if (_func == RADIAL || _func == REPEATING_RADIAL)
    {
        float2 V = input.TexCoord - _p;
        t = length(_v * V);
    }
    else if (_func == CONIC || _func == REPEATING_CONIC)
    {
        float2x2 R = float2x2(_v.x, -_v.y, _v.y, _v.x);
        float2 V = mul(R, (input.TexCoord - _p));
        t = 0.5 + atan2(-V.x, V.y) / (2.0 * PI);
    }

    if (_func == REPEATING_LINEAR || _func == REPEATING_RADIAL || _func == REPEATING_CONIC)
    {
        float t0 = _stop_positions[0];
        float t1 = _stop_positions[_num_stops - 1];
        t = t0 + fmod(t - t0, t1 - t0);
    }

    float4 color = _stop_colors[0];
    for (int i = 1; i < _num_stops; i++)
        color = lerp(color, _stop_colors[i], smoothstep(_stop_positions[i - 1], _stop_positions[i], t));

    return color;
}
