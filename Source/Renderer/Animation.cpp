#include "Animation.hpp"
#include <algorithm>

namespace
{
constexpr int ComponentCount(FAnimPath path) { return path == FAnimPath::Rotation ? 4 : 3; }

struct KeySegment
{
    size_t i0{0};
    size_t i1{0};
    float u{0.0f};
    float dt{0.0f};
};

KeySegment FindSegment(Vector<float> const& times, float t)
{
    if (times.size() <= 1 || t <= times.front())
        return {0, 0, 0.0f, 0.0f};
    if (t >= times.back())
    {
        size_t last = times.size() - 1;
        return {last, last, 0.0f, 0.0f};
    }
    size_t i1 = static_cast<size_t>(std::upper_bound(times.begin(), times.end(), t) - times.begin());
    size_t i0 = i1 - 1;
    float dt = times[i1] - times[i0];
    return {i0, i1, dt > 0.0f ? (t - times[i0]) / dt : 0.0f, dt};
}

size_t ValueOffset(FAnimChannel const& c, size_t k)
{
    int comp = ComponentCount(c.path);
    return c.interp == FAnimInterp::CubicSpline ? k * 3 * comp + comp : k * comp;
}
size_t OutTangentOffset(FAnimChannel const& c, size_t k) { return k * 3 * ComponentCount(c.path) + 2 * ComponentCount(c.path); }
size_t InTangentOffset(FAnimChannel const& c, size_t k) { return k * 3 * ComponentCount(c.path); }

float3 ReadVec3(FAnimChannel const& c, size_t off) { return float3(c.values[off], c.values[off + 1], c.values[off + 2]); }
// glTF stores quats xyzw; glm built with GLM_FORCE_QUAT_CTOR_XYZW.
quat ReadQuat(FAnimChannel const& c, size_t off) { return quat(c.values[off], c.values[off + 1], c.values[off + 2], c.values[off + 3]); }

float3 SampleVec3(FAnimChannel const& c, KeySegment const& seg)
{
    if (seg.i0 == seg.i1 || c.interp == FAnimInterp::Step)
        return ReadVec3(c, ValueOffset(c, seg.i0));
    float3 p0 = ReadVec3(c, ValueOffset(c, seg.i0));
    float3 p1 = ReadVec3(c, ValueOffset(c, seg.i1));
    if (c.interp == FAnimInterp::Linear)
        return mix(p0, p1, seg.u);
    float3 m0 = ReadVec3(c, OutTangentOffset(c, seg.i0)) * seg.dt;
    float3 m1 = ReadVec3(c, InTangentOffset(c, seg.i1)) * seg.dt;
    float u = seg.u, u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1;
}

quat SampleQuat(FAnimChannel const& c, KeySegment const& seg)
{
    if (seg.i0 == seg.i1 || c.interp == FAnimInterp::Step)
        return normalize(ReadQuat(c, ValueOffset(c, seg.i0)));
    quat q0 = ReadQuat(c, ValueOffset(c, seg.i0));
    quat q1 = ReadQuat(c, ValueOffset(c, seg.i1));
    if (dot(q0, q1) < 0.0f)
        q1 = -q1;
    if (c.interp == FAnimInterp::Linear)
        return normalize(slerp(q0, q1, seg.u));
    quat m0 = ReadQuat(c, OutTangentOffset(c, seg.i0));
    quat m1 = ReadQuat(c, InTangentOffset(c, seg.i1));
    float u = seg.u, u2 = u * u, u3 = u2 * u;
    float a = 2 * u3 - 3 * u2 + 1, b = (u3 - 2 * u2 + u) * seg.dt;
    float cc = -2 * u3 + 3 * u2, d = (u3 - u2) * seg.dt;
    quat r(a * q0.x + b * m0.x + cc * q1.x + d * m1.x, a * q0.y + b * m0.y + cc * q1.y + d * m1.y,
           a * q0.z + b * m0.z + cc * q1.z + d * m1.z, a * q0.w + b * m0.w + cc * q1.w + d * m1.w);
    return normalize(r);
}
} // namespace

mat4 JointLocalMatrix(float3 const& t, quat const& r, float3 const& s)
{
    return translate(mat4(1.0f), t) * mat4_cast(r) * scale(mat4(1.0f), s);
}

void FPose::Resize(uint32_t jointCount)
{
    translations.resize(jointCount);
    rotations.resize(jointCount);
    scales.resize(jointCount);
    globals.resize(jointCount);
}

void ResetToRest(FSkeleton const& skel, FPose& pose)
{
    pose.Resize(skel.Count());
    for (uint32_t i = 0; i < skel.Count(); ++i)
    {
        pose.translations[i] = skel.joints[i].restTranslation;
        pose.rotations[i] = skel.joints[i].restRotation;
        pose.scales[i] = skel.joints[i].restScale;
    }
}

void SampleClip(FAnimationClip const& clip, float t, FPose& pose)
{
    for (FAnimChannel const& c : clip.channels)
    {
        if (c.times.empty() || c.joint >= pose.translations.size())
            continue;
        KeySegment seg = FindSegment(c.times, t);
        switch (c.path)
        {
        case FAnimPath::Translation: pose.translations[c.joint] = SampleVec3(c, seg); break;
        case FAnimPath::Scale: pose.scales[c.joint] = SampleVec3(c, seg); break;
        case FAnimPath::Rotation: pose.rotations[c.joint] = SampleQuat(c, seg); break;
        }
    }
}

void BlendClip(FAnimationClip const& clip, float t, float weight, FPose& pose)
{
    if (weight <= 0.0f)
        return;
    for (FAnimChannel const& c : clip.channels)
    {
        if (c.times.empty() || c.joint >= pose.translations.size())
            continue;
        KeySegment seg = FindSegment(c.times, t);
        switch (c.path)
        {
        case FAnimPath::Translation:
            pose.translations[c.joint] = mix(pose.translations[c.joint], SampleVec3(c, seg), weight);
            break;
        case FAnimPath::Scale:
            pose.scales[c.joint] = mix(pose.scales[c.joint], SampleVec3(c, seg), weight);
            break;
        case FAnimPath::Rotation:
        {
            quat sampled = SampleQuat(c, seg);
            quat const& cur = pose.rotations[c.joint];
            if (dot(cur, sampled) < 0.0f)
                sampled = -sampled;
            pose.rotations[c.joint] = normalize(slerp(cur, sampled, weight));
            break;
        }
        }
    }
}

void ComputeGlobals(FSkeleton const& skel, FPose& pose)
{
    uint32_t n = skel.Count();
    if (pose.globals.size() < n)
        pose.globals.resize(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        mat4 local = JointLocalMatrix(pose.translations[i], pose.rotations[i], pose.scales[i]);
        int32_t parent = skel.joints[i].parent;
        pose.globals[i] = parent >= 0 ? pose.globals[static_cast<uint32_t>(parent)] * local : local;
    }
}

void ComputeSkinningMatrices(FSkeleton const& skel, FPose const& pose, Span<mat4> outPalette)
{
    uint32_t n = skel.Count();
    CHECK_MSG(outPalette.size() >= n, "Skinning palette too small ({} < {})", outPalette.size(), n);
    for (uint32_t i = 0; i < n; ++i)
        outPalette[i] = pose.globals[i] * skel.joints[i].inverseBind;
}

void SkinVertices(Span<const FVertex> bind, Span<const FSkinBinding> binding, Span<const mat4> palette,
                  Span<FQVertex> out, FSerializedBounds* outBounds)
{
    CHECK_MSG(binding.size() >= bind.size() && out.size() >= bind.size(),
              "SkinVertices span size mismatch (bind {}, binding {}, out {})", bind.size(), binding.size(), out.size());
    uint32_t paletteCount = static_cast<uint32_t>(palette.size());
    FSerializedBounds bounds = FSerializedBounds::Empty();
    for (size_t v = 0; v < bind.size(); ++v)
    {
        FVertex const& src = bind[v];
        FSkinBinding const& b = binding[v];
        float sumW = b.weights[0] + b.weights[1] + b.weights[2] + b.weights[3];
        if (sumW <= 0.0f)
        {
            out[v] = FQVertex::Pack(src);
            bounds += src.position;
            continue;
        }
        float inv = 1.0f / sumW;
        mat4 m(0.0f);
        for (int k = 0; k < 4; ++k)
        {
            float w = b.weights[k] * inv;
            if (w == 0.0f)
                continue;
            uint32_t j = b.joints[k];
            if (j < paletteCount)
                m += w * palette[j];
        }
        mat3 basis(m);
        FVertex d = src;
        d.position = float3(m * float4(src.position, 1.0f));
        d.normal = normalize(basis * src.normal);
        d.tangent = normalize(basis * src.tangent);
        out[v] = FQVertex::Pack(d);
        bounds += d.position;
    }
    if (outBounds)
        *outBounds = bounds.IsValid() ? bounds : FSerializedBounds{};
}
