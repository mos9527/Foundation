#include "D3D12Shader.hpp"
#include "../../../Common/IO.hpp"

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
    // https://simoncoenen.com/blog/programming/graphics/DxcCompiling
    static Microsoft::WRL::ComPtr<IDxcBlob> CompileShaderDXC(
        const wchar_t* sourcePath,
        const wchar_t* entrypoint,
        const wchar_t* targetProfile,
        std::vector<const wchar_t*>&& defines,
        std::vector<const wchar_t*>&& extraIncludes,
        std::string& errorBuffer
    )
    {
        LOG(INFO) << "Compiling shader " << wstring_to_utf8(targetProfile) << ", " << wstring_to_utf8(sourcePath) << " @ " << wstring_to_utf8(entrypoint);
        for (auto define : defines)
            LOG(INFO) << "  -D " << wstring_to_utf8(define);
        ComPtr<IDxcLibrary> library;
        CHECK_HR(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library)));
        
        ComPtr<IDxcCompiler3> compiler;
        CHECK_HR(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));

        ComPtr<IDxcUtils> utils;
        CHECK_HR(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
        
        ComPtr<IDxcIncludeHandler> includeHandler;
        utils->CreateDefaultIncludeHandler(includeHandler.GetAddressOf());

        std::vector<const wchar_t*> arguments; 
        // arguments.push_back(DXC_ARG_WARNINGS_ARE_ERRORS);
        for (auto extraInclude : extraIncludes) {
            arguments.push_back(L"-I");
            arguments.push_back(extraInclude);
        }
        arguments.push_back(L"-E"); // entry point
        arguments.push_back(entrypoint);
        arguments.push_back(L"-T"); // target profile
        arguments.push_back(targetProfile);
        for (auto& define : defines) {
            arguments.push_back(L"-D");
            arguments.push_back(define);
        }
#ifdef _DEBUG
        arguments.push_back(DXC_ARG_DEBUG); // -Zi
        arguments.push_back(DXC_ARG_DEBUG_NAME_FOR_SOURCE); // -Zss
        arguments.push_back(DXC_ARG_SKIP_OPTIMIZATIONS); // no optimizations
        arguments.push_back(L"-Qembed_debug");
        arguments.push_back(L"-Fd"); // output pdb path
        auto pdb_path = path_t(sourcePath).replace_extension(L".pdb");
        arguments.push_back(pdb_path.c_str());        
#else
        arguments.push_back(L"-O3"); // max optimizations
#endif
        
        uint32_t codePage = CP_UTF8;
        ComPtr<IDxcBlobEncoding> sourceBlob;
        utils->LoadFile(sourcePath, &codePage, &sourceBlob);        
        DxcBuffer sourceBuffer;
        if (!sourceBlob) {
            errorBuffer = std::format("Failed to load shader. Path={}", wstring_to_utf8(sourcePath));
            LOG(ERROR) << errorBuffer;
            return {};
        }
        sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
        sourceBuffer.Size = sourceBlob->GetBufferSize();
        sourceBuffer.Encoding = DXC_CP_UTF8;

        ComPtr<IDxcResult> pCompileResult;
        CHECK_HR(compiler->Compile(
            &sourceBuffer, // pSource
            arguments.data(), (UINT)arguments.size(), // pArguments, argCount
            includeHandler.Get(), // pIncludeHandler
            IID_PPV_ARGS(&pCompileResult)
        )); // ppResult
        HRESULT hr; CHECK_HR(pCompileResult->GetStatus(&hr));
        if (FAILED(hr))
        {
            ComPtr<IDxcBlobEncoding> errorsBlob;
            hr = pCompileResult->GetErrorBuffer(&errorsBlob);
            if (SUCCEEDED(hr) && errorsBlob)
            {
                LOG(ERROR) << "Shader Compilation Error!";
                LOG(ERROR) << (const char*)errorsBlob->GetBufferPointer();
                errorBuffer = (const char*)errorsBlob->GetBufferPointer();
                return {};
            }
        }
        else {
            ComPtr<IDxcBlob> code;
            pCompileResult->GetResult(&code); // shader bytecode
            if (pCompileResult->HasOutput(DXC_OUT_PDB)) { // pdb
                ComPtr<IDxcBlob> pdb;
                ComPtr<IDxcBlobUtf16> name;
                pCompileResult->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(&pdb), &name);
                path_t pdbPath = name->GetStringPointer();
                LOG(INFO) << "Outputing debug information to: " << wstring_to_utf8(pdbPath.c_str());
                write_data(pdbPath.c_str(), reinterpret_cast<uint8_t*>(pdb->GetBufferPointer()), pdb->GetBufferSize());
            }
            LOG(INFO) << "Shader compiled!";
#ifdef _DEBUG
            
#endif
            return code;            
        }
        return {};
    }

    Blob::Blob(void* csoData, size_t size) {
        m_Data.resize(size);
        memcpy(m_Data.data(), csoData, size);
    }
    Shader::Shader(const wchar_t* sourcePath, const wchar_t* entrypoint, const wchar_t* targetProfile, std::vector<const wchar_t*>&& defines, std::vector<const wchar_t*>&& extraIncludes) {
        auto blob = CompileShaderDXC(sourcePath, entrypoint, targetProfile, std::move(defines), std::move(extraIncludes), m_ErrorBuffer);
        if (blob) {
            m_Data.resize(blob->GetBufferSize());
            memcpy(m_Data.data(), blob->GetBufferPointer(), m_Data.size());
            m_Loaded = true;
        }
        SetName(sourcePath);
    }
}