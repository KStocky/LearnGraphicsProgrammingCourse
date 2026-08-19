#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d12.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <lgp/framework/error.hpp>

namespace lgp::framework
{

struct ShaderDefine final
{
    std::wstring name;
    std::wstring value;
};

struct ShaderCompileOptions final
{
    std::filesystem::path sourcePath;
    std::wstring entryPoint;
    std::wstring targetProfile;
    std::vector<ShaderDefine> defines;
    std::vector<std::filesystem::path> includeDirectories;
    std::vector<std::wstring> additionalArguments;
    bool enableDebugInformation{false};
    bool optimize{true};
};

struct CompiledShader final
{
    std::vector<std::byte> bytecode;
    std::string diagnostics;
    std::filesystem::path sourcePath;
    std::wstring entryPoint;
    std::wstring targetProfile;

    [[nodiscard]] D3D12_SHADER_BYTECODE Bytecode() const noexcept
    {
        return {bytecode.empty() ? nullptr : static_cast<void const *>(bytecode.data()),
                static_cast<SIZE_T>(bytecode.size())};
    }
};

class ShaderCompiler final
{
  public:
    ShaderCompiler() = default;
    ShaderCompiler(ShaderCompiler &&other) noexcept;
    ShaderCompiler &operator=(ShaderCompiler &&other) noexcept;
    ShaderCompiler(ShaderCompiler const &) = delete;
    ShaderCompiler &operator=(ShaderCompiler const &) = delete;
    ~ShaderCompiler();

    [[nodiscard]] static Result<ShaderCompiler> Create();
    [[nodiscard]] Result<CompiledShader> Compile(ShaderCompileOptions const &options) const;

  private:
    class Impl;

    explicit ShaderCompiler(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_{};
};

} // namespace lgp::framework
