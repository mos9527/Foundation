#pragma once
#include "Math.hlsli"
const static float LTC_LUT_SIZE = 64.0;
const static float LTC_LUT_SCALE = (LTC_LUT_SIZE - 1.0) / LTC_LUT_SIZE;
const static float LTC_LUT_BIAS = 0.5 / LTC_LUT_SIZE;
// see https://github.com/selfshadow/ltc_code/
// RTR4 p390
float3 IntegrateEdgeVec(float3 v1, float3 v2)
{
    float x = dot(v1, v2);
    float y = abs(x);

    float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    float b = 3.4175940 + (4.1616724 + y) * y;
    float v = a / b;

    float theta_sintheta = (x > 0.0) ? v : 0.5 * rsqrt(max(1.0 - x * x, 1e-7)) - v;

    return cross(v1, v2) * theta_sintheta;
}
float IntegrateEdge(float3 v1, float3 v2)
{
    return IntegrateEdgeVec(v1, v2).z;
}
float D(float3 w, float3x3 Minv)
{
    float3 wo = mul(w, Minv);
    float lo = length(wo);
    float res = 1.0 / M_PI * max(0.0, wo.z / lo) * abs(determinant(Minv)) / (lo * lo * lo);
    return res;
}
float Fpo(float d, float l)
{
    return l / (d * (d * d + l * l)) + atan(l / d) / (d * d);
}

float Fwt(float d, float l)
{
    return l * l / (d * (d * d + l * l));
}
float sqr(float x)
{
    return x * x;
}
float I_diffuse_line(float3 p1, float3 p2)
{
    // tangent
    float3 wt = normalize(p2 - p1);

    // clamping
    if (p1.z <= 0.0 && p2.z <= 0.0)
        return 0.0;
    if (p1.z < 0.0)
        p1 = (+p1 * p2.z - p2 * p1.z) / (+p2.z - p1.z);
    if (p2.z < 0.0)
        p2 = (-p1 * p2.z + p2 * p1.z) / (-p2.z + p1.z);

    // parameterization
    float l1 = dot(p1, wt);
    float l2 = dot(p2, wt);

    // shading point orthonormal projection on the line
    float3 po = p1 - l1 * wt;

    // distance to line
    float d = length(po);

    // integral
    float I = (Fpo(d, l2) - Fpo(d, l1)) * po.z +
              (Fwt(d, l2) - Fwt(d, l1)) * wt.z;
    return I / M_PI;
}

float I_ltc_line(float3 p1, float3 p2, float3x3 Minv)
{
    // transform to diffuse configuration
    float3 p1o = mul(p1, Minv);   
    float3 p2o = mul(p2, Minv);
    float I_diffuse = I_diffuse_line(p1o, p2o);

    // width factor
    float3 ortho = normalize(cross(p1, p2));
    float w = 1.0 / length(mul(ortho, Minv));

    return w * I_diffuse;
}
float I_ltc_disks(float3 p1, float3 p2, float R, float3x3 Minv)
{
    float A = M_PI * R * R;
    float3 wt = normalize(p2 - p1);
    float3 wp1 = normalize(p1);
    float3 wp2 = normalize(p2);
    float Idisks = A * (
    D(wp1,Minv) * max(0.0, dot(+wt, wp1)) / dot(p1, p1) +
    D(wp2,Minv) * max(0.0, dot(-wt, wp2)) / dot(p2, p2));
    return Idisks;
}
// An extended version of the implementation from
// "How to solve a cubic equation, revisited"
// http://momentsingraphics.de/?p=105
float3 SolveCubic(float4 Coefficient)
{
    // Normalize the polynomial
    Coefficient.xyz /= Coefficient.w;
    // Divide middle coefficients by three
    Coefficient.yz /= 3.0;

    float A = Coefficient.w;
    float B = Coefficient.z;
    float C = Coefficient.y;
    float D = Coefficient.x;

    // Compute the Hessian and the discriminant
    float3 Delta = float3(
        -Coefficient.z * Coefficient.z + Coefficient.y,
        -Coefficient.y * Coefficient.z + Coefficient.x,
        dot(float2(Coefficient.z, -Coefficient.y), Coefficient.xy)
    );

    float Discriminant = dot(float2(4.0 * Delta.x, -Delta.y), Delta.zy);

    float3 RootsA, RootsD;

    float2 xlc, xsc;

    // Algorithm A
    {
        float A_a = 1.0;
        float C_a = Delta.x;
        float D_a = -2.0 * B * Delta.x + Delta.y;

        // Take the cubic root of a normalized complex number
        float Theta = atan2(sqrt(Discriminant), -D_a) / 3.0;

        float x_1a = 2.0 * sqrt(-C_a) * cos(Theta);
        float x_3a = 2.0 * sqrt(-C_a) * cos(Theta + (2.0 / 3.0) * M_PI);

        float xl;
        if ((x_1a + x_3a) > 2.0 * B)
            xl = x_1a;
        else
            xl = x_3a;

        xlc = float2(xl - B, A);
    }

    // Algorithm D
    {
        float A_d = D;
        float C_d = Delta.z;
        float D_d = -D * Delta.y + 2.0 * C * Delta.z;

        // Take the cubic root of a normalized complex number
        float Theta = atan2(D * sqrt(Discriminant), -D_d) / 3.0;

        float x_1d = 2.0 * sqrt(-C_d) * cos(Theta);
        float x_3d = 2.0 * sqrt(-C_d) * cos(Theta + (2.0 / 3.0) * M_PI);

        float xs;
        if (x_1d + x_3d < 2.0 * C)
            xs = x_1d;
        else
            xs = x_3d;

        xsc = float2(-D, xs + C);
    }

    float E = xlc.y * xsc.y;
    float F = -xlc.x * xsc.y - xlc.y * xsc.x;
    float G = xlc.x * xsc.x;

    float2 xmc = float2(C * F - B * G, -B * F + C * E);

    float3 Root = float3(xsc.x / xsc.y, xmc.x / xmc.y, xlc.x / xlc.y);

    if (Root.x < Root.y && Root.x < Root.z)
        Root.xyz = Root.yxz;
    else if (Root.z < Root.x && Root.z < Root.y)
        Root.xyz = Root.xzy;

    return Root;
}
float3 LTC_EvaluateLine(float3 N, float3 V, float3 P, float3x3 Minv, float3 P1, float3 P2, float R, uint endCaps)
{
    // construct orthonormal basis around N
    float3 T1, T2;
    T1 = normalize(V - N * dot(V, N));
    T2 = cross(N, T1);

    float3x3 B = transpose(float3x3(T1, T2, N));

    float3 p1 = mul(P1 - P, B);
    float3 p2 = mul(P2 - P, B);
    
    float Iline = R * I_ltc_line(p1, p2, Minv);
    float Idisks = endCaps ? I_ltc_disks(p1, p2, R, Minv) : 0.0;
    return splat3(min(1.0, Iline + Idisks));
}
float3 LTC_EvaluateQuad(float3 N, float3 V, float3 P, float3x3 Minv, float3 points[4], bool twoSided)
{
    // construct orthonormal basis around N
    float3 T1, T2;
    T1 = normalize(V - N * dot(V, N));
    T2 = cross(N, T1);

    // rotate area light in (T1, T2, N) basis
    Minv = mul(transpose(float3x3(T1, T2, N)), Minv);
    
    // polygon (allocate 4 vertices for clipping)
    float3 L[4];
    // transform polygon from LTC back to origin Do (cosine weighted)
    L[0] = mul((points[0] - P), Minv).xyz;
    L[1] = mul((points[1] - P), Minv).xyz;
    L[2] = mul((points[2] - P), Minv).xyz;
    L[3] = mul((points[3] - P), Minv).xyz;

    // use tabulated horizon-clipped sphere
    // check if the shading point is behind the light
    float3 dir = points[0] - P; // LTC space
    float3 lightNormal = cross(points[1] - points[0], points[3] - points[0]);
    bool behind = (dot(dir, lightNormal) < 0.0);

    // project onto sphere
    L[0] = normalize(L[0]);
    L[1] = normalize(L[1]);
    L[2] = normalize(L[2]);
    L[3] = normalize(L[3]);

    // integrate
    float3 vsum = splat3(0);
    vsum += IntegrateEdgeVec(L[0], L[1]);
    vsum += IntegrateEdgeVec(L[1], L[2]);
    vsum += IntegrateEdgeVec(L[2], L[3]);
    vsum += IntegrateEdgeVec(L[3], L[0]);
    
    // form factor of the polygon in direction vsum
    float len = length(vsum);

    float z = vsum.z / len;
    if (behind)
        z = -z;

    // Fetch the form factor for horizon clipping
    float2 uv = float2(z * 0.5f + 0.5f, len); // range [0, 1]
    uv = uv * LTC_LUT_SCALE + LTC_LUT_BIAS;
    float scale = LTC_SampleLut(uv, 1).w;
    
    float sum = len * scale;
    if (behind && !twoSided)
        sum = 0.0;

    return splat3(sum);
}
float3 LTC_EvaluateDisk(float3 N, float3 V, float3 P, float3x3 Minv, float3 points[4], bool twoSided)
{
    // construct orthonormal basis around N
    float3 T1, T2;
    T1 = normalize(V - N*dot(V, N));
    T2 = cross(N, T1);

    // rotate area light in (T1, T2, N) basis
    Minv = mul(transpose(float3x3(T1, T2, N)), Minv);

    // polygon
    float3 L_[4];
    L_[0] = mul(points[0] - P, Minv);
    L_[1] = mul(points[1] - P, Minv);
    L_[2] = mul(points[2] - P, Minv);
    L_[3] = mul(points[3] - P, Minv);
    
    float3 Lo_i = splat3(0);

    // init ellipse
    float3 C  = 0.5 * (L_[0] + L_[2]);
    float3 V1 = 0.5 * (L_[1] - L_[2]);
    float3 V2 = 0.5 * (L_[1] - L_[0]);

    if(!twoSided && dot(cross(V1, V2), C) < 0.0)
        return splat3(0.0);

    // compute eigenvectors of ellipse
    float a, b;
    float d11 = dot(V1, V1);
    float d22 = dot(V2, V2);
    float d12 = dot(V1, V2);
    if (abs(d12)/sqrt(d11*d22) > 0.0001)
    {
        float tr = d11 + d22;
        float det = -d12*d12 + d11*d22;

        // use sqrt matrix to solve for eigenvalues
        det = sqrt(det);
        float u = 0.5*sqrt(tr - 2.0*det);
        float v = 0.5*sqrt(tr + 2.0*det);
        float e_max = sqr(u + v);
        float e_min = sqr(u - v);

        float3 V1_, V2_;

        if (d11 > d22)
        {
            V1_ = d12*V1 + (e_max - d11)*V2;
            V2_ = d12*V1 + (e_min - d11)*V2;
        }
        else
        {
            V1_ = d12*V2 + (e_max - d22)*V1;
            V2_ = d12*V2 + (e_min - d22)*V1;
        }

        a = 1.0 / e_max;
        b = 1.0 / e_min;
        V1 = normalize(V1_);
        V2 = normalize(V2_);
    }
    else
    {
        a = 1.0 / dot(V1, V1);
        b = 1.0 / dot(V2, V2);
        V1 *= sqrt(a);
        V2 *= sqrt(b);
    }

    float3 V3 = cross(V1, V2);
    if (dot(C, V3) < 0.0)
        V3 *= -1.0;

    float L  = dot(V3, C);
    float x0 = dot(V1, C) / L;
    float y0 = dot(V2, C) / L;

    float E1 = rsqrt(a);
    float E2 = rsqrt(b);

    a *= L*L;
    b *= L*L;

    float c0 = a*b;
    float c1 = a*b*(1.0 + x0*x0 + y0*y0) - a - b;
    float c2 = 1.0 - a*(1.0 + x0*x0) - b*(1.0 + y0*y0);
    float c3 = 1.0;

    float3 roots = SolveCubic(float4(c0, c1, c2, c3));
    float e1 = roots.x;
    float e2 = roots.y;
    float e3 = roots.z;

    float3 avgDir = float3(a*x0/(a - e2), b*y0/(b - e2), 1.0);

    float3x3 rotate = float3x3(V1, V2, V3);

    avgDir = mul(avgDir, rotate);
    avgDir = normalize(avgDir);

    float L1 = sqrt(-e2/e3);
    float L2 = sqrt(-e2/e1);

    float formFactor = L1 * L2 * rsqrt((1.0 + L1 * L1) * (1.0 + L2 * L2));

    // use tabulated horizon-clipped sphere
    float2 uv = float2(avgDir.z*0.5 + 0.5, formFactor);
    uv = uv * LTC_LUT_SCALE + LTC_LUT_BIAS;
    float scale = LTC_SampleLut(uv, 1).w;

    float spec = formFactor*scale;

    return splat3(spec);
}
