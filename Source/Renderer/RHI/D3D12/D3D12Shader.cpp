#include "D3D12Shader.hpp"
namespace RHI {
    static Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
        const wchar_t* filename,
        const D3D_SHADER_MACRO* defines,
        const char* entrypoint,
        const char* target)
    {
        // from DirectX-Graphics-Samples
        UINT compileFlags = 0;
#if defined(_DEBUG) || defined(DBG)
        compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        Microsoft::WRL::ComPtr<ID3DBlob> byteCode = nullptr;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        CHECK_HR(D3DCompileFromFile(filename, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entrypoint, target, compileFlags, 0, &byteCode, &errors));
        return byteCode;
    }
    ShaderBlob::ShaderBlob(const wchar_t* sourcePath, const char* entrypoint, const char* target, D3D_SHADER_MACRO* macro) {
        auto blob = CompileShader(sourcePath, macro, entrypoint, target);
        m_Data.resize(blob->GetBufferSize());
        memcpy(m_Data.data(), blob->GetBufferPointer(), m_Data.size());
    }
    ShaderBlob::ShaderBlob(void* csoData, size_t size) {
        m_Data.resize(size);
        memcpy(m_Data.data(), csoData, size);
    }
}