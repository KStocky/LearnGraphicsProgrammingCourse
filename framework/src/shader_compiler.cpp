#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <objidl.h>
#include <oleauto.h>
#include <unknwn.h>

#include <dxcapi.h>
#include <wrl/client.h>

#include <lgp/framework/shader_compiler.hpp>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace lgp::framework
{

class ShaderCompiler::Impl final
{
  public:
    Microsoft::WRL::ComPtr<IDxcUtils> utils{};
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler{};
};

namespace
{

[[nodiscard]] Status ValidateCompileOptions(ShaderCompileOptions const &options)
{
    if (options.sourcePath.empty())
    {
        return std::unexpected(MakeError("ShaderCompiler::Compile", "A source path is required."));
    }

    if (options.entryPoint.empty())
    {
        return std::unexpected(MakeError("ShaderCompiler::Compile", "An HLSL entry point is required."));
    }

    if (options.targetProfile.empty())
    {
        return std::unexpected(MakeError("ShaderCompiler::Compile", "A shader target profile is required."));
    }

    return {};
}

[[nodiscard]] std::string GetUtf8Output(IDxcResult &result, DXC_OUT_KIND outputKind)
{
    Microsoft::WRL::ComPtr<IDxcBlobUtf8> output;
    HRESULT const getResult = result.GetOutput(outputKind, IID_PPV_ARGS(output.ReleaseAndGetAddressOf()), nullptr);
    if (FAILED(getResult) || output == nullptr || output->GetStringLength() == 0U)
    {
        return {};
    }

    return {output->GetStringPointer(), output->GetStringLength()};
}

[[nodiscard]] std::vector<LPCWSTR> BuildArguments(ShaderCompileOptions const &options)
{
    std::vector<LPCWSTR> arguments;
    arguments.reserve(16U + options.additionalArguments.size() + (options.includeDirectories.size() * 2U));

    arguments.push_back(DXC_ARG_PACK_MATRIX_ROW_MAJOR);
    arguments.push_back(DXC_ARG_WARNINGS_ARE_ERRORS);
    arguments.push_back(DXC_ARG_ENABLE_STRICTNESS);
    arguments.push_back(L"-HV");
    arguments.push_back(L"2021");

    if (options.enableDebugInformation)
    {
        arguments.push_back(DXC_ARG_DEBUG);
        arguments.push_back(L"-Qembed_debug");
        arguments.push_back(DXC_ARG_DEBUG_NAME_FOR_SOURCE);
    }

    arguments.push_back(options.optimize ? DXC_ARG_OPTIMIZATION_LEVEL3 : DXC_ARG_SKIP_OPTIMIZATIONS);

    return arguments;
}

} // namespace

ShaderCompiler::ShaderCompiler(ShaderCompiler &&other) noexcept = default;

ShaderCompiler &ShaderCompiler::operator=(ShaderCompiler &&other) noexcept
{
    if (this != &other)
    {
        impl_ = std::move(other.impl_);
    }

    return *this;
}

ShaderCompiler::~ShaderCompiler() = default;

ShaderCompiler::ShaderCompiler(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}

Result<ShaderCompiler> ShaderCompiler::Create()
{
    auto impl = std::make_unique<Impl>();

    HRESULT const utilsResult = ::DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(impl->utils.ReleaseAndGetAddressOf()));
    if (FAILED(utilsResult))
    {
        return std::unexpected(MakeHResultError("DxcCreateInstance", utilsResult, "Failed to create IDxcUtils."));
    }

    HRESULT const compilerResult =
        ::DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(impl->compiler.ReleaseAndGetAddressOf()));
    if (FAILED(compilerResult))
    {
        return std::unexpected(
            MakeHResultError("DxcCreateInstance", compilerResult, "Failed to create IDxcCompiler3."));
    }

    return ShaderCompiler{std::move(impl)};
}

Result<CompiledShader> ShaderCompiler::Compile(ShaderCompileOptions const &options) const
{
    if (impl_ == nullptr || impl_->utils == nullptr || impl_->compiler == nullptr)
    {
        return std::unexpected(MakeError("ShaderCompiler::Compile", "The shader compiler has not been initialized."));
    }

    auto const validation = ValidateCompileOptions(options);
    if (!validation)
    {
        return std::unexpected(std::move(validation.error()));
    }

    std::wstring const sourcePath = options.sourcePath.wstring();

    UINT32 codePage = 0U;
    Microsoft::WRL::ComPtr<IDxcBlobEncoding> sourceBlob;
    HRESULT const loadResult =
        impl_->utils->LoadFile(sourcePath.c_str(), &codePage, sourceBlob.ReleaseAndGetAddressOf());
    if (FAILED(loadResult))
    {
        return std::unexpected(
            MakeHResultError("IDxcUtils::LoadFile", loadResult, "Failed to load an HLSL source file."));
    }

    Microsoft::WRL::ComPtr<IDxcIncludeHandler> includeHandler;
    HRESULT const includeResult = impl_->utils->CreateDefaultIncludeHandler(includeHandler.ReleaseAndGetAddressOf());
    if (FAILED(includeResult))
    {
        return std::unexpected(MakeHResultError("IDxcUtils::CreateDefaultIncludeHandler", includeResult,
                                                "Failed to create the default DXC include handler."));
    }

    std::vector<std::wstring> includeDirectories;
    includeDirectories.reserve(options.includeDirectories.size() + 1U);
    if (options.sourcePath.has_parent_path())
    {
        includeDirectories.push_back(options.sourcePath.parent_path().wstring());
    }

    for (auto const &includeDirectory : options.includeDirectories)
    {
        if (!includeDirectory.empty())
        {
            includeDirectories.push_back(includeDirectory.wstring());
        }
    }

    std::vector<LPCWSTR> arguments = BuildArguments(options);
    std::vector<std::wstring> defineArguments;
    defineArguments.reserve(options.defines.size());

    for (auto const &define : options.defines)
    {
        std::wstring defineArgument = L"-D";
        defineArgument += define.name;
        if (!define.value.empty())
        {
            defineArgument += L"=";
            defineArgument += define.value;
        }
        defineArguments.push_back(std::move(defineArgument));
    }

    arguments.reserve(arguments.size() + options.additionalArguments.size() + (includeDirectories.size() * 2U) +
                      defineArguments.size());

    for (std::wstring const &includeDirectory : includeDirectories)
    {
        arguments.push_back(L"-I");
        arguments.push_back(includeDirectory.c_str());
    }

    for (std::wstring const &defineArgument : defineArguments)
    {
        arguments.push_back(defineArgument.c_str());
    }

    for (std::wstring const &argument : options.additionalArguments)
    {
        arguments.push_back(argument.c_str());
    }

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = codePage;

    Microsoft::WRL::ComPtr<IDxcResult> result;
    HRESULT const compileCall =
        impl_->compiler->Compile(&sourceBuffer, arguments.data(), static_cast<UINT32>(arguments.size()),
                                 includeHandler.Get(), IID_PPV_ARGS(result.ReleaseAndGetAddressOf()));

    if (FAILED(compileCall))
    {
        return std::unexpected(
            MakeHResultError("IDxcCompiler3::Compile", compileCall, "DXC failed to start compilation."));
    }

    HRESULT compileStatus = S_OK;
    HRESULT const statusResult = result->GetStatus(&compileStatus);
    if (FAILED(statusResult))
    {
        return std::unexpected(MakeHResultError("IDxcOperationResult::GetStatus", statusResult,
                                                "Failed to retrieve the DXC compilation status."));
    }

    std::string diagnostics = GetUtf8Output(*result.Get(), DXC_OUT_ERRORS);
    if (diagnostics.empty())
    {
        diagnostics = GetUtf8Output(*result.Get(), DXC_OUT_REMARKS);
    }

    if (FAILED(compileStatus))
    {
        return std::unexpected(Error{
            ErrorDomain::Dxc,
            "IDxcCompiler3::Compile",
            diagnostics.empty() ? DescribeHRESULT(compileStatus) : diagnostics,
            compileStatus,
            static_cast<DWORD>(compileStatus),
        });
    }

    Microsoft::WRL::ComPtr<IDxcBlob> shaderObject;
    HRESULT const objectResult =
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(shaderObject.ReleaseAndGetAddressOf()), nullptr);
    if (FAILED(objectResult))
    {
        return std::unexpected(
            MakeHResultError("IDxcResult::GetOutput", objectResult, "DXC did not produce a compiled shader object."));
    }

    CompiledShader shader;
    shader.bytecode.resize(static_cast<std::size_t>(shaderObject->GetBufferSize()));
    if (!shader.bytecode.empty())
    {
        std::memcpy(shader.bytecode.data(), shaderObject->GetBufferPointer(), shader.bytecode.size());
    }
    shader.diagnostics = std::move(diagnostics);
    shader.sourcePath = options.sourcePath;
    shader.entryPoint = options.entryPoint;
    shader.targetProfile = options.targetProfile;

    return shader;
}

} // namespace lgp::framework
