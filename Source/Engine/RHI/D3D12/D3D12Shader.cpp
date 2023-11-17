#include "D3D12Shader.hpp"
namespace RHI {
    // https://asawicki.info/news_1719_two_shader_compilers_of_direct3d_12
    [[deprecated]] static Microsoft::WRL::ComPtr<ID3DBlob> CompileShaderFXC(
        const wchar_t* sourcePath,
        const char* entrypoint,
        const char* targetProfile)
    {
        // from DirectX-Graphics-Samples
        UINT compileFlags = 0;
#ifdef _DEBUG
        compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        Microsoft::WRL::ComPtr<ID3DBlob> byteCode = nullptr;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        CHECK_HR(D3DCompileFromFile(sourcePath, NULL, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entrypoint, targetProfile, compileFlags, 0, &byteCode, &errors));
        return byteCode;
    }
    static Microsoft::WRL::ComPtr<IDxcBlob> CompileShaderDXC(
        const wchar_t* sourcePath,
        const wchar_t* entrypoint,
        const wchar_t* targetProfile)
    {
        ComPtr<IDxcLibrary> library;
        CHECK_HR(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library)));
        
        ComPtr<IDxcCompiler> compiler;
        CHECK_HR(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));

        uint32_t codePage = CP_UTF8;
        ComPtr<IDxcBlobEncoding> sourceBlob;
        CHECK_HR(library->CreateBlobFromFile(sourcePath, &codePage, &sourceBlob));        
        LOG(INFO) << "Compiling shader " << wstring_to_utf8(targetProfile) << ", " << wstring_to_utf8(sourcePath) << " @ " << wstring_to_utf8(entrypoint);
        ComPtr<IDxcOperationResult> result;
        std::vector<const wchar_t*> arguments;
#ifdef _DEBUG
        arguments.push_back(DXC_ARG_DEBUG);
        arguments.push_back(DXC_ARG_WARNINGS_ARE_ERRORS);
        arguments.push_back(L"-Qembed_debug");
#endif
        HRESULT hr = compiler->Compile(
            sourceBlob.Get(), // pSource
            sourcePath, // pSourceName
            entrypoint, // pEntryPoint
            targetProfile, // pTargetProfile
            arguments.data(), (UINT)arguments.size(), // pArguments, argCount
            NULL, 0, // pDefines, defineCount
            NULL, // pIncludeHandler
            &result
        ); // ppResult

        if (FAILED(hr))
        {
            if (result)
            {
                ComPtr<IDxcBlobEncoding> errorsBlob;
                hr = result->GetErrorBuffer(&errorsBlob);
                if (SUCCEEDED(hr) && errorsBlob)
                {
                    LOG(ERROR) << "Shader Compilation Error!";
                    LOG(ERROR) << errorsBlob->GetBufferPointer();
                }
            }
#ifdef _DEBUG
            __debugbreak();
#endif
        }
        else {
            LOG(INFO) << "Shader compiled!";
            ComPtr<IDxcBlob> code;
            result->GetResult(&code);
            return code;
        }
        return {};
    }

    Blob::Blob(void* csoData, size_t size) {
        m_Data.resize(size);
        memcpy(m_Data.data(), csoData, size);
    }
    Shader::Shader(const wchar_t* sourcePath, const wchar_t* entrypoint, const wchar_t* targetProfile) {
        auto blob = CompileShaderDXC(sourcePath, entrypoint, targetProfile);
        if (blob) {
            m_Data.resize(blob->GetBufferSize());
            memcpy(m_Data.data(), blob->GetBufferPointer(), m_Data.size());
        }
        SetName(sourcePath);
    }
}