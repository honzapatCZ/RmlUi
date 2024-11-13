#include "./Flax/GUICommon.hlsl"

struct VS_INPUT
{
    float2 pos : POSITION;
    float2 uv : TEXCOORD;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

META_CB_BEGIN(0, Data)
float4 sourceRect; // (left, top, right, bottom) in normalized UV space
float4 targetRect; // (left, top, right, bottom) in normalized UV space
META_CB_END

Texture2D sourceTexture : register(t0);

META_VS(true, FEATURE_LEVEL_ES2)
META_VS_IN_ELEMENT(POSITION, 0, R32G32_FLOAT,       0, ALIGN, PER_VERTEX, 0, true)
META_VS_IN_ELEMENT(TEXCOORD, 0, R32G32_FLOAT,       0, ALIGN, PER_VERTEX, 0, true)
PS_INPUT VS(VS_INPUT input)
{
    PS_INPUT output;
       

    // Map the vertex position from [-1, 1] space to [0, 1] UV space
    float2 uvPos = input.uv;

    // Map UV position to the target rectangle in UV space
    output.pos = float4(input.pos, 0, 1);
    /*output.pos = float4(
        lerp(targetRect.x, targetRect.z, uvPos.x) * 2.0f - 1.0f,
        lerp(targetRect.w, targetRect.y, (input.pos.y + 1.0f) * 0.5f) * 2.0f - 1.0f,
        0, 1.0f
    );*/

    // Map UV to source rectangle in UV space
    //output.uv = uvPos;
    //output.uv.x = lerp(sourceRect.x, sourceRect.z, uvPos.x);
    //output.uv.y = lerp(sourceRect.w, sourceRect.y, uvPos.y);
    float2 targetMappedUV;
    targetMappedUV.x = (uvPos.x - targetRect.x) / (targetRect.z - targetRect.x);
    targetMappedUV.y = (uvPos.y - targetRect.y) / (targetRect.w - targetRect.y);

    // Map the UV coordinates within the source rectangle in UV space
    output.uv.x = lerp(sourceRect.x, sourceRect.z, targetMappedUV.x);
    output.uv.y = lerp(sourceRect.y, sourceRect.w, targetMappedUV.y);

    return output;
}

META_PS(true, FEATURE_LEVEL_ES2)
float4 PS_Main(PS_INPUT input) : SV_Target0
{
    return sourceTexture.Sample(SamplerLinearClamp, input.uv);
}
