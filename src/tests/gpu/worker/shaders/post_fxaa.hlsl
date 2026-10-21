// SPDX-License-Identifier: GPL-3.0-or-later
// Post pass 14 (Phase 4a design §6.4): FXAA 3.11 (Timothy Lottes, NVIDIA),
// the quality path at preset 12 (5 search steps: 1.0, 1.5, 2.0, 4.0, 12.0),
// subpix 0.75, edge threshold 0.166, minimum 0.0833; luma from the
// display-encoded colour. ldrA -> ldrB. Written from the published algorithm.
#include "post.hlsli"

SB_BIND_T0 Texture2D<float4> source : register(t0);

float3 rgbAt(float2 p)
{
    return source.SampleLevel(linearClamp, p, 0.0).rgb;
}

float lumaAt(float2 p)
{
    return dot(rgbAt(p), float3(0.299, 0.587, 0.114));
}

float4 PSMain(FullscreenOut input) : SV_Target0
{
    const float subpix = 0.75, edgeThreshold = 0.166, edgeThresholdMin = 0.0833;
    const float steps[5] = {1.0, 1.5, 2.0, 4.0, 12.0};
    float2 rcp = sourceSize.xy;
    float2 pos = sourceUv(input.uv);
    float3 rgbM = rgbAt(pos);
    float lumaM = dot(rgbM, float3(0.299, 0.587, 0.114));
    float lumaS = lumaAt(pos + float2(0.0, rcp.y));
    float lumaE = lumaAt(pos + float2(rcp.x, 0.0));
    float lumaN = lumaAt(pos - float2(0.0, rcp.y));
    float lumaW = lumaAt(pos - float2(rcp.x, 0.0));
    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    if (range < max(edgeThresholdMin, rangeMax * edgeThreshold))
        return float4(rgbM, 1.0);

    float lumaNW = lumaAt(pos + float2(-rcp.x, -rcp.y));
    float lumaSE = lumaAt(pos + float2(rcp.x, rcp.y));
    float lumaNE = lumaAt(pos + float2(rcp.x, -rcp.y));
    float lumaSW = lumaAt(pos + float2(-rcp.x, rcp.y));
    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = -2.0 * lumaM + lumaNS, edgeVert1 = -2.0 * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = -2.0 * lumaE + lumaNESE, edgeVert2 = -2.0 * lumaN + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2), edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    float edgeHorz3 = -2.0 * lumaW + lumaNWSW, edgeVert3 = -2.0 * lumaS + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4, edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = rcp.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) {
        lumaN = lumaW;
        lumaS = lumaE;
    } else {
        lengthSign = rcp.y;
    }
    float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN)
        lengthSign = -lengthSign;
    float subpixC = saturate(abs(subpixB) * subpixRcpRange);

    float2 posB = pos;
    float2 offNP = horzSpan ? float2(rcp.x, 0.0) : float2(0.0, rcp.y);
    if (!horzSpan)
        posB.x += lengthSign * 0.5;
    else
        posB.y += lengthSign * 0.5;
    float2 posN = posB - offNP * steps[0];
    float2 posP = posB + offNP * steps[0];
    float subpixD = -2.0 * subpixC + 3.0;
    float subpixE = subpixC * subpixC;
    if (!pairN)
        lumaNN = lumaSS;
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    float lumaEndN = lumaAt(posN) - lumaNN * 0.5;
    float lumaEndP = lumaAt(posP) - lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN)
        posN -= offNP * steps[1];
    if (!doneP)
        posP += offNP * steps[1];
    [unroll] for (int s = 2; s < 5; ++s) {
        if (!doneN || !doneP) {
            if (!doneN)
                lumaEndN = lumaAt(posN) - lumaNN * 0.5;
            if (!doneP)
                lumaEndP = lumaAt(posP) - lumaNN * 0.5;
            doneN = abs(lumaEndN) >= gradientScaled;
            doneP = abs(lumaEndP) >= gradientScaled;
            if (!doneN)
                posN -= offNP * steps[s];
            if (!doneP)
                posP += offNP * steps[s];
        }
    }
    float dstN = horzSpan ? pos.x - posN.x : pos.y - posN.y;
    float dstP = horzSpan ? posP.x - pos.x : posP.y - pos.y;
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLengthRcp = 1.0 / (dstP + dstN);
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = dst * (-spanLengthRcp) + 0.5;
    float subpixH = subpixG * subpix;
    float pixelOffsetSubpix = max(goodSpan ? pixelOffset : 0.0, subpixH);
    if (!horzSpan)
        pos.x += pixelOffsetSubpix * lengthSign;
    else
        pos.y += pixelOffsetSubpix * lengthSign;
    return float4(rgbAt(pos), 1.0);
}
