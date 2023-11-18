#include "D3D12Shader.hpp"
#include "../../AssetRegistry/IO.hpp"
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
        const wchar_t* targetProfile)
    {
        LOG(INFO) << "Compiling shader " << wstring_to_utf8(targetProfile) << ", " << wstring_to_utf8(sourcePath) << " @ " << wstring_to_utf8(entrypoint);

        ComPtr<IDxcLibrary> library;
        CHECK_HR(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library)));
        
        ComPtr<IDxcCompiler3> compiler;
        CHECK_HR(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));

        ComPtr<IDxcUtils> utils;
        CHECK_HR(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
        
        ComPtr<IDxcIncludeHandler> includeHandler;
        utils->CreateDefaultIncludeHandler(includeHandler.GetAddressOf());

        std::vector<const wchar_t*> arguments; 
        arguments.push_back(DXC_ARG_WARNINGS_ARE_ERRORS);
        arguments.push_back(L"-I");
        auto parent_path = get_absolute_path(sourcePath).parent_path();
        arguments.push_back(parent_path.c_str());
        arguments.push_back(L"-E"); // entry point
        arguments.push_back(entrypoint);
        arguments.push_back(L"-T"); // target profile
        arguments.push_back(targetProfile);
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
        sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
        CHECK(sourceBuffer.Ptr && "Shader did not load!");
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
            }           
#ifdef _DEBUG
            __debugbreak();
#endif
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
    Shader::Shader(const wchar_t* sourcePath, const wchar_t* entrypoint, const wchar_t* targetProfile) {
        auto blob = CompileShaderDXC(sourcePath, entrypoint, targetProfile);
        if (blob) {
            m_Data.resize(blob->GetBufferSize());
            memcpy(m_Data.data(), blob->GetBufferPointer(), m_Data.size());
        }
        SetName(sourcePath);
    }
}