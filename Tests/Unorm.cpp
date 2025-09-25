#include <Core/Core.hpp>
#include <Math/Unorm.hpp>
#include <Math/Quantization.hpp>
using namespace Foundation;
using namespace Math;
int main()
{
    vec3 pz{0,0,1}, nz{0,0,-1};
    int p1 = quantizeSnormShifted(1, 15);
    int n1 = quantizeSnormShifted(-1, 15);
    CHECK(p1 != n1);
    vec2 po = packUnitOctahedral(pz);
    vec2 no = packUnitOctahedral(nz);
    CHECK(po != no);
    auto pack = [](vec3 v) -> vec2
    {
        vec2 octa = packUnitOctahedral(v);
        return { quantizeSnormShifted(octa.x, 15), quantizeSnormShifted(octa.y, 15) };
    };
    auto unpack = [](vec2 q) -> vec3
    {
        vec2 octa = { dequantizeSnormShifted(q.x, 15), dequantizeSnormShifted(q.y, 15) };
        return unpackUnitOctahedral(octa);
    };
    auto pz_q = pack(pz);
    auto pz_r = unpack(pz_q);
    auto nz_q = pack(nz);
    auto nz_r = unpack(nz_q);
    CHECK(length(pz - pz_r) < 1e-3);
    CHECK(length(nz - nz_r) < 1e-3);
    printf("pass!\n");
}