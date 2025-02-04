/*
 *  Copyright 2019-2021 Diligent Graphics LLC
 *  Copyright 2015-2019 Egor Yusov
 *  
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *  
 *      http://www.apache.org/licenses/LICENSE-2.0
 *  
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence), 
 *  contract, or otherwise, unless required by applicable law (such as deliberate 
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental, 
 *  or consequential damages of any character arising as a result of this License or 
 *  out of the use or inability to use the software (including but not limited to damages 
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and 
 *  all other commercial damages or losses), even if such Contributor has been advised 
 *  of the possibility of such damages.
 */

#include <memory>
#include <mutex>
#include <atomic>

// Platforms that support DXCompiler.
#if PLATFORM_WIN32
#    include "DXCompilerBaseWin32.hpp"
#elif PLATFORM_UNIVERSAL_WINDOWS
#    include "DXCompilerBaseUWP.hpp"
#elif PLATFORM_LINUX
#    include "DXCompilerBaseLiunx.hpp"
#else
#    error DXC is not supported on this platform
#endif

#include "DataBlobImpl.hpp"
#include "RefCntAutoPtr.hpp"
#include "ShaderToolsCommon.hpp"

#if D3D12_SUPPORTED
#    include <d3d12shader.h>

#    ifndef NTDDI_WIN10_VB // First defined in Win SDK 10.0.19041.0
#        define NO_D3D_SIT_ACCELSTRUCT_FEEDBACK_TEX 1

#        define D3D_SIT_RTACCELERATIONSTRUCTURE static_cast<D3D_SHADER_INPUT_TYPE>(D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER + 1)
#        define D3D_SIT_UAV_FEEDBACKTEXTURE     static_cast<D3D_SHADER_INPUT_TYPE>(D3D_SIT_RTACCELERATIONSTRUCTURE + 1)
#    endif
#endif

#include "HLSLUtils.hpp"

#include "dxc/DxilContainer/DxilContainer.h"

namespace Diligent
{

namespace
{
constexpr Uint32 VK_API_VERSION_1_1 = (1u << 22) | (1u << 12);
constexpr Uint32 VK_API_VERSION_1_2 = (1u << 22) | (2u << 12);


class DXCompilerImpl final : public DXCompilerBase
{
public:
    DXCompilerImpl(DXCompilerTarget Target, Uint32 APIVersion, const char* pLibName) :
        m_LibName{pLibName ? pLibName : (Target == DXCompilerTarget::Direct3D12 ? "dxcompiler" : "spv_dxcompiler")},
        m_Target{Target},
        m_APIVersion{APIVersion}
    {}

    ShaderVersion GetMaxShaderModel() override final
    {
        Load();
        // mutex is not needed here
        return m_MaxShaderModel;
    }

    bool IsLoaded() override final
    {
        return GetCreateInstaceProc() != nullptr;
    }

    DxcCreateInstanceProc GetCreateInstaceProc()
    {
        return Load();
    }

    virtual void GetVersion(Uint32& MajorVersion, Uint32& MinorVersion) const override final
    {
        MajorVersion = m_MajorVer;
        MinorVersion = m_MinorVer;
    }

    bool Compile(const CompileAttribs& Attribs) override final;

    virtual void Compile(const ShaderCreateInfo& ShaderCI,
                         ShaderVersion           ShaderModel,
                         const char*             ExtraDefinitions,
                         IDxcBlob**              ppByteCodeBlob,
                         std::vector<uint32_t>*  pByteCode,
                         IDataBlob**             ppCompilerOutput) noexcept(false) override final;

    virtual void GetD3D12ShaderReflection(IDxcBlob*                pShaderBytecode,
                                          ID3D12ShaderReflection** ppShaderReflection) override final;

    virtual bool RemapResourceBindings(const TResourceBindingMap& ResourceMap,
                                       IDxcBlob*                  pSrcBytecode,
                                       IDxcBlob**                 ppDstByteCode) override final;

private:
    DxcCreateInstanceProc Load()
    {
        std::unique_lock<std::mutex> lock{m_Guard};

        if (m_IsInitialized)
            return m_pCreateInstance;

        m_IsInitialized   = true;
        m_pCreateInstance = DXCompilerBase::Load(m_Target, m_LibName);

        if (m_pCreateInstance)
        {
            CComPtr<IDxcValidator> validator;
            if (SUCCEEDED(m_pCreateInstance(CLSID_DxcValidator, IID_PPV_ARGS(&validator))))
            {
                CComPtr<IDxcVersionInfo> info;
                if (SUCCEEDED(validator->QueryInterface(IID_PPV_ARGS(&info))))
                {
                    info->GetVersion(&m_MajorVer, &m_MinorVer);

                    LOG_INFO_MESSAGE("Loaded DX Shader Compiler, version ", m_MajorVer, ".", m_MinorVer);

                    auto ver = (m_MajorVer << 16) | (m_MinorVer & 0xFFFF);

                    // map known DXC version to maximum SM
                    switch (ver)
                    {
                        case 0x10005: m_MaxShaderModel = {6, 5}; break; // SM 6.5 and SM 6.6 preview
                        case 0x10004: m_MaxShaderModel = {6, 4}; break; // SM 6.4 and SM 6.5 preview
                        case 0x10003:
                        case 0x10002: m_MaxShaderModel = {6, 1}; break; // SM 6.1 and SM 6.2 preview
                        default: m_MaxShaderModel = (ver > 0x10005 ? ShaderVersion{6, 6} : ShaderVersion{6, 0}); break;
                    }
                }
            }
        }

        return m_pCreateInstance;
    }

    bool ValidateAndSign(DxcCreateInstanceProc CreateInstance, IDxcLibrary* library, CComPtr<IDxcBlob>& compiled, IDxcBlob** ppBlobOut) const;

    enum RES_TYPE : Uint32
    {
        RES_TYPE_CBV     = 0,
        RES_TYPE_SRV     = 1,
        RES_TYPE_SAMPLER = 2,
        RES_TYPE_UAV     = 3,
        RES_TYPE_COUNT,
        RES_TYPE_INVALID = ~0u
    };

    struct ResourceExtendedInfo
    {
        Uint32   SrcBindPoint = ~0u;
        Uint32   SrcSpace     = ~0u;
        Uint32   RecordId     = ~0u;
        RES_TYPE Type         = RES_TYPE_INVALID;
    };
    using TExtendedResourceMap = std::unordered_map<TResourceBindingMap::value_type const*, ResourceExtendedInfo>;

    static bool PatchDXIL(const TResourceBindingMap& ResourceMap, TExtendedResourceMap& ExtResMap, SHADER_TYPE ShaderType, String& DXIL);
    static void PatchResourceDeclaration(const TResourceBindingMap& ResourceMap, TExtendedResourceMap& ExtResMap, String& DXIL);
    static void PatchResourceDeclarationRT(const TResourceBindingMap& ResourceMap, TExtendedResourceMap& ExtResMap, String& DXIL);
    static void PatchResourceHandle(const TResourceBindingMap& ResourceMap, TExtendedResourceMap& ExtResMap, String& DXIL);

private:
    DxcCreateInstanceProc  m_pCreateInstance = nullptr;
    bool                   m_IsInitialized   = false;
    ShaderVersion          m_MaxShaderModel;
    std::mutex             m_Guard;
    const String           m_LibName;
    const DXCompilerTarget m_Target;
    const Uint32           m_APIVersion;
    // Compiler version
    UINT32 m_MajorVer = 0;
    UINT32 m_MinorVer = 0;
};


class DxcIncludeHandlerImpl final : public IDxcIncludeHandler
{
public:
    explicit DxcIncludeHandlerImpl(IShaderSourceInputStreamFactory* pStreamFactory, CComPtr<IDxcLibrary> pLibrary) :
        m_pLibrary{pLibrary},
        m_pStreamFactory{pStreamFactory}
    {
    }

    HRESULT STDMETHODCALLTYPE LoadSource(_In_ LPCWSTR pFilename, _COM_Outptr_result_maybenull_ IDxcBlob** ppIncludeSource) override
    {
        if (pFilename == nullptr)
            return E_FAIL;

        String fileName;
        fileName.resize(wcslen(pFilename));
        for (size_t i = 0; i < fileName.size(); ++i)
        {
            fileName[i] = static_cast<char>(pFilename[i]);
        }

        if (fileName.empty())
        {
            LOG_ERROR("Failed to convert shader include file name ", fileName, ". File name must be ANSI string");
            return E_FAIL;
        }

        // validate file name
        if (fileName.size() > 2 && fileName[0] == '.' && (fileName[1] == '\\' || fileName[1] == '/'))
            fileName.erase(0, 2);

        RefCntAutoPtr<IFileStream> pSourceStream;
        m_pStreamFactory->CreateInputStream(fileName.c_str(), &pSourceStream);
        if (pSourceStream == nullptr)
        {
            LOG_ERROR("Failed to open shader include file ", fileName, ". Check that the file exists");
            return E_FAIL;
        }

        RefCntAutoPtr<IDataBlob> pFileData{MakeNewRCObj<DataBlobImpl>()(0)};
        pSourceStream->ReadBlob(pFileData);

        CComPtr<IDxcBlobEncoding> sourceBlob;

        HRESULT hr = m_pLibrary->CreateBlobWithEncodingFromPinned(pFileData->GetDataPtr(), static_cast<UINT32>(pFileData->GetSize()), CP_UTF8, &sourceBlob);
        if (FAILED(hr))
        {
            LOG_ERROR("Failed to allocate space for shader include file ", fileName, ".");
            return E_FAIL;
        }

        m_FileDataCache.emplace_back(std::move(pFileData));

        sourceBlob->QueryInterface(IID_PPV_ARGS(ppIncludeSource));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
    {
        return E_FAIL;
    }

    ULONG STDMETHODCALLTYPE AddRef(void) override
    {
        return m_RefCount.fetch_add(1) + 1;
    }

    ULONG STDMETHODCALLTYPE Release(void) override
    {
        VERIFY(m_RefCount > 0, "Inconsistent call to Release()");
        return m_RefCount.fetch_add(-1) - 1;
    }

private:
    CComPtr<IDxcLibrary>                   m_pLibrary;
    IShaderSourceInputStreamFactory* const m_pStreamFactory;
    std::atomic_long                       m_RefCount{0};
    std::vector<RefCntAutoPtr<IDataBlob>>  m_FileDataCache;
};

} // namespace


std::unique_ptr<IDXCompiler> CreateDXCompiler(DXCompilerTarget Target, Uint32 APIVersion, const char* pLibraryName)
{
    return std::unique_ptr<IDXCompiler>{new DXCompilerImpl{Target, APIVersion, pLibraryName}};
}

bool DXCompilerImpl::Compile(const CompileAttribs& Attribs)
{
    auto CreateInstance = GetCreateInstaceProc();

    if (CreateInstance == nullptr)
    {
        LOG_ERROR("Failed to load DXCompiler");
        return false;
    }

    DEV_CHECK_ERR(Attribs.Source != nullptr && Attribs.SourceLength > 0, "'Source' must not be null and 'SourceLength' must be greater than 0");
    DEV_CHECK_ERR(Attribs.EntryPoint != nullptr, "'EntryPoint' must not be null");
    DEV_CHECK_ERR(Attribs.Profile != nullptr, "'Profile' must not be null");
    DEV_CHECK_ERR((Attribs.pDefines != nullptr) == (Attribs.DefinesCount > 0), "'DefinesCount' must be 0 if 'pDefines' is null");
    DEV_CHECK_ERR((Attribs.pArgs != nullptr) == (Attribs.ArgsCount > 0), "'ArgsCount' must be 0 if 'pArgs' is null");
    DEV_CHECK_ERR(Attribs.ppBlobOut != nullptr, "'ppBlobOut' must not be null");
    DEV_CHECK_ERR(Attribs.ppCompilerOutput != nullptr, "'ppCompilerOutput' must not be null");

    HRESULT hr;

    // NOTE: The call to DxcCreateInstance is thread-safe, but objects created by DxcCreateInstance aren't thread-safe.
    // Compiler objects should be created and then used on the same thread.
    // https://github.com/microsoft/DirectXShaderCompiler/wiki/Using-dxc.exe-and-dxcompiler.dll#dxcompiler-dll-interface

    CComPtr<IDxcLibrary> library;
    hr = CreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library));
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to create DXC Library");
        return false;
    }

    CComPtr<IDxcCompiler> compiler;
    hr = CreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to create DXC Compiler");
        return false;
    }

    CComPtr<IDxcBlobEncoding> sourceBlob;
    hr = library->CreateBlobWithEncodingFromPinned(Attribs.Source, UINT32{Attribs.SourceLength}, CP_UTF8, &sourceBlob);
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to create DXC Blob encoding");
        return false;
    }

    DxcIncludeHandlerImpl IncludeHandler{Attribs.pShaderSourceStreamFactory, library};

    CComPtr<IDxcOperationResult> result;
    hr = compiler->Compile(
        sourceBlob,
        L"",
        Attribs.EntryPoint,
        Attribs.Profile,
        Attribs.pArgs, UINT32{Attribs.ArgsCount},
        Attribs.pDefines, UINT32{Attribs.DefinesCount},
        Attribs.pShaderSourceStreamFactory ? &IncludeHandler : nullptr,
        &result);

    if (SUCCEEDED(hr))
    {
        HRESULT status;
        if (SUCCEEDED(result->GetStatus(&status)))
            hr = status;
    }

    if (result)
    {
        CComPtr<IDxcBlobEncoding> errorsBlob;
        CComPtr<IDxcBlobEncoding> errorsBlobUtf8;
        if (SUCCEEDED(result->GetErrorBuffer(&errorsBlob)) && SUCCEEDED(library->GetBlobAsUtf8(errorsBlob, &errorsBlobUtf8)))
        {
            errorsBlobUtf8->QueryInterface(IID_PPV_ARGS(Attribs.ppCompilerOutput));
        }
    }

    if (FAILED(hr))
    {
        return false;
    }

    CComPtr<IDxcBlob> compiled;
    hr = result->GetResult(&compiled);
    if (FAILED(hr))
        return false;

    // validate and sign
    if (m_Target == DXCompilerTarget::Direct3D12)
    {
        return ValidateAndSign(CreateInstance, library, compiled, Attribs.ppBlobOut);
    }

    *Attribs.ppBlobOut = compiled.Detach();
    return true;
}

bool DXCompilerImpl::ValidateAndSign(DxcCreateInstanceProc CreateInstance, IDxcLibrary* library, CComPtr<IDxcBlob>& compiled, IDxcBlob** ppBlobOut) const
{
    HRESULT                hr;
    CComPtr<IDxcValidator> validator;
    hr = CreateInstance(CLSID_DxcValidator, IID_PPV_ARGS(&validator));
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to create DXC Validator");
        return false;
    }

    CComPtr<IDxcOperationResult> validationResult;
    hr = validator->Validate(compiled, DxcValidatorFlags_InPlaceEdit, &validationResult);

    if (validationResult == nullptr || FAILED(hr))
    {
        LOG_ERROR("Failed to validate shader bytecode");
        return false;
    }

    HRESULT status = E_FAIL;
    validationResult->GetStatus(&status);

    if (SUCCEEDED(status))
    {
        CComPtr<IDxcBlob> validated;
        hr = validationResult->GetResult(&validated);
        if (FAILED(hr))
            return false;

        *ppBlobOut = validated ? validated.Detach() : compiled.Detach();
        return true;
    }
    else
    {
        CComPtr<IDxcBlobEncoding> validationOutput;
        CComPtr<IDxcBlobEncoding> validationOutputUtf8;
        validationResult->GetErrorBuffer(&validationOutput);
        library->GetBlobAsUtf8(validationOutput, &validationOutputUtf8);

        size_t      ValidationMsgLen = validationOutputUtf8 ? validationOutputUtf8->GetBufferSize() : 0;
        const char* ValidationMsg    = ValidationMsgLen > 0 ? static_cast<const char*>(validationOutputUtf8->GetBufferPointer()) : "";

        LOG_ERROR("Shader validation failed: ", ValidationMsg);
        return false;
    }
}

#if D3D12_SUPPORTED
class ShaderReflectionViaLibraryReflection final : public ID3D12ShaderReflection
{
public:
    ShaderReflectionViaLibraryReflection(CComPtr<ID3D12LibraryReflection> pLib, ID3D12FunctionReflection* pFunc) :
        m_pLib{std::move(pLib)},
        m_pFunc{pFunc}
    {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override
    {
        return E_FAIL;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return m_RefCount.fetch_add(1) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        VERIFY(m_RefCount > 0, "Inconsistent call to ReleaseStrongRef()");
        auto RefCount = m_RefCount.fetch_add(-1) - 1;
        if (RefCount == 0)
        {
            delete this;
        }
        return RefCount;
    }

    HRESULT STDMETHODCALLTYPE GetDesc(D3D12_SHADER_DESC* pDesc) override
    {
        D3D12_FUNCTION_DESC FnDesc{};
        HRESULT             hr = m_pFunc->GetDesc(&FnDesc);
        if (FAILED(hr))
            return hr;

        pDesc->Version                     = FnDesc.Version;
        pDesc->Creator                     = FnDesc.Creator;
        pDesc->Flags                       = FnDesc.Flags;
        pDesc->ConstantBuffers             = FnDesc.ConstantBuffers;
        pDesc->BoundResources              = FnDesc.BoundResources;
        pDesc->InputParameters             = 0;
        pDesc->OutputParameters            = 0;
        pDesc->InstructionCount            = FnDesc.InstructionCount;
        pDesc->TempRegisterCount           = FnDesc.TempRegisterCount;
        pDesc->TempArrayCount              = FnDesc.TempArrayCount;
        pDesc->DefCount                    = FnDesc.DefCount;
        pDesc->DclCount                    = FnDesc.DclCount;
        pDesc->TextureNormalInstructions   = FnDesc.TextureNormalInstructions;
        pDesc->TextureLoadInstructions     = FnDesc.TextureLoadInstructions;
        pDesc->TextureCompInstructions     = FnDesc.TextureCompInstructions;
        pDesc->TextureBiasInstructions     = FnDesc.TextureBiasInstructions;
        pDesc->TextureGradientInstructions = FnDesc.TextureGradientInstructions;
        pDesc->FloatInstructionCount       = FnDesc.FloatInstructionCount;
        pDesc->IntInstructionCount         = FnDesc.IntInstructionCount;
        pDesc->UintInstructionCount        = FnDesc.UintInstructionCount;
        pDesc->StaticFlowControlCount      = FnDesc.StaticFlowControlCount;
        pDesc->DynamicFlowControlCount     = FnDesc.DynamicFlowControlCount;
        pDesc->MacroInstructionCount       = FnDesc.MacroInstructionCount;
        pDesc->ArrayInstructionCount       = FnDesc.ArrayInstructionCount;
        pDesc->CutInstructionCount         = 0;
        pDesc->EmitInstructionCount        = 0;
        pDesc->GSOutputTopology            = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        pDesc->GSMaxOutputVertexCount      = 0;
        pDesc->InputPrimitive              = D3D_PRIMITIVE_UNDEFINED;
        pDesc->PatchConstantParameters     = 0;
        pDesc->cGSInstanceCount            = 0;
        pDesc->cControlPoints              = 0;
        pDesc->HSOutputPrimitive           = D3D_TESSELLATOR_OUTPUT_UNDEFINED;
        pDesc->HSPartitioning              = D3D_TESSELLATOR_PARTITIONING_UNDEFINED;
        pDesc->TessellatorDomain           = D3D_TESSELLATOR_DOMAIN_UNDEFINED;
        pDesc->cBarrierInstructions        = 0;
        pDesc->cInterlockedInstructions    = 0;
        pDesc->cTextureStoreInstructions   = 0;

        return S_OK;
    }

    ID3D12ShaderReflectionConstantBuffer* STDMETHODCALLTYPE GetConstantBufferByIndex(UINT Index) override
    {
        return m_pFunc->GetConstantBufferByIndex(Index);
    }

    ID3D12ShaderReflectionConstantBuffer* STDMETHODCALLTYPE GetConstantBufferByName(LPCSTR Name) override
    {
        return m_pFunc->GetConstantBufferByName(Name);
    }

    HRESULT STDMETHODCALLTYPE GetResourceBindingDesc(UINT ResourceIndex, D3D12_SHADER_INPUT_BIND_DESC* pDesc) override
    {
        return m_pFunc->GetResourceBindingDesc(ResourceIndex, pDesc);
    }

    HRESULT STDMETHODCALLTYPE GetInputParameterDesc(UINT ParameterIndex, D3D12_SIGNATURE_PARAMETER_DESC* pDesc) override
    {
        UNEXPECTED("not supported");
        return E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE GetOutputParameterDesc(UINT ParameterIndex, D3D12_SIGNATURE_PARAMETER_DESC* pDesc) override
    {
        UNEXPECTED("not supported");
        return E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE GetPatchConstantParameterDesc(UINT ParameterIndex, D3D12_SIGNATURE_PARAMETER_DESC* pDesc) override
    {
        UNEXPECTED("not supported");
        return E_FAIL;
    }

    ID3D12ShaderReflectionVariable* STDMETHODCALLTYPE GetVariableByName(LPCSTR Name) override
    {
        return m_pFunc->GetVariableByName(Name);
    }

    HRESULT STDMETHODCALLTYPE GetResourceBindingDescByName(LPCSTR Name, D3D12_SHADER_INPUT_BIND_DESC* pDesc) override
    {
        return m_pFunc->GetResourceBindingDescByName(Name, pDesc);
    }

    UINT STDMETHODCALLTYPE GetMovInstructionCount() override
    {
        UNEXPECTED("not supported");
        return 0;
    }

    UINT STDMETHODCALLTYPE GetMovcInstructionCount() override
    {
        UNEXPECTED("not supported");
        return 0;
    }

    UINT STDMETHODCALLTYPE GetConversionInstructionCount() override
    {
        UNEXPECTED("not supported");
        return 0;
    }

    UINT STDMETHODCALLTYPE GetBitwiseInstructionCount() override
    {
        UNEXPECTED("not supported");
        return 0;
    }

    D3D_PRIMITIVE STDMETHODCALLTYPE GetGSInputPrimitive() override
    {
        UNEXPECTED("not supported");
        return D3D_PRIMITIVE_UNDEFINED;
    }

    BOOL STDMETHODCALLTYPE IsSampleFrequencyShader() override
    {
        UNEXPECTED("not supported");
        return FALSE;
    }

    UINT STDMETHODCALLTYPE GetNumInterfaceSlots() override
    {
        UNEXPECTED("not supported");
        return 0;
    }

    HRESULT STDMETHODCALLTYPE GetMinFeatureLevel(D3D_FEATURE_LEVEL* pLevel) override
    {
        UNEXPECTED("not supported");
        return E_FAIL;
    }

    UINT STDMETHODCALLTYPE GetThreadGroupSize(UINT* pSizeX, UINT* pSizeY, UINT* pSizeZ) override
    {
        UNEXPECTED("not supported");
        *pSizeX = *pSizeY = *pSizeZ = 0;
        return 0;
    }

    UINT64 STDMETHODCALLTYPE GetRequiresFlags() override
    {
        UNEXPECTED("not supported");
        return 0;
    }

private:
    CComPtr<ID3D12LibraryReflection> m_pLib;
    ID3D12FunctionReflection*        m_pFunc = nullptr;
    std::atomic_long                 m_RefCount{0};
};
#endif // D3D12_SUPPORTED


void DXCompilerImpl::GetD3D12ShaderReflection(IDxcBlob*                pShaderBytecode,
                                              ID3D12ShaderReflection** ppShaderReflection)
{
#if D3D12_SUPPORTED
    try
    {
        auto CreateInstance = GetCreateInstaceProc();
        if (CreateInstance == nullptr)
            return;

        CComPtr<IDxcContainerReflection> pReflection;

        auto hr = CreateInstance(CLSID_DxcContainerReflection, IID_PPV_ARGS(&pReflection));
        if (FAILED(hr))
            LOG_ERROR_AND_THROW("Failed to create shader reflection instance");

        hr = pReflection->Load(pShaderBytecode);
        if (FAILED(hr))
            LOG_ERROR_AND_THROW("Failed to load shader reflection from bytecode");

        UINT32 shaderIdx = 0;

        hr = pReflection->FindFirstPartKind(DXC_PART_DXIL, &shaderIdx);
        if (SUCCEEDED(hr))
        {
            hr = pReflection->GetPartReflection(shaderIdx, IID_PPV_ARGS(ppShaderReflection));
            if (SUCCEEDED(hr))
                return;

            // Try to get the reflection via library reflection
            CComPtr<ID3D12LibraryReflection> pLib;

            hr = pReflection->GetPartReflection(shaderIdx, IID_PPV_ARGS(&pLib));
            if (SUCCEEDED(hr))
            {
#    ifdef DILIGENT_DEVELOPMENT
                {
                    D3D12_LIBRARY_DESC Desc = {};
                    pLib->GetDesc(&Desc);
                    DEV_CHECK_ERR(Desc.FunctionCount == 1, "Single-function library is expected");
                }
#    endif

                ID3D12FunctionReflection* pFunc = pLib->GetFunctionByIndex(0);
                if (pFunc != nullptr)
                {
                    *ppShaderReflection = new ShaderReflectionViaLibraryReflection{std::move(pLib), pFunc};
                    (*ppShaderReflection)->AddRef();
                    return;
                }
            }
        }

        LOG_ERROR_AND_THROW("Failed to get the shader reflection");
    }
    catch (...)
    {
    }
#endif // D3D12_SUPPORTED
}


void DXCompilerImpl::Compile(const ShaderCreateInfo& ShaderCI,
                             ShaderVersion           ShaderModel,
                             const char*             ExtraDefinitions,
                             IDxcBlob**              ppByteCodeBlob,
                             std::vector<uint32_t>*  pByteCode,
                             IDataBlob**             ppCompilerOutput) noexcept(false)
{
    if (!IsLoaded())
    {
        UNEXPECTED("DX compiler is not loaded");
        return;
    }

    ShaderVersion MaxSM = GetMaxShaderModel();

    // validate shader version
    if (ShaderModel == ShaderVersion{})
    {
        ShaderModel = MaxSM;
    }
    else if (ShaderModel.Major < 6)
    {
        LOG_INFO_MESSAGE("DXC only supports shader model 6.0+. Upgrading the specified shader model ",
                         Uint32{ShaderModel.Major}, '_', Uint32{ShaderModel.Minor}, " to 6_0");
        ShaderModel = ShaderVersion{6, 0};
    }
    else if (ShaderModel > MaxSM)
    {
        LOG_WARNING_MESSAGE("The maximum supported shader model by DXC is ", Uint32{MaxSM.Major}, '_', Uint32{MaxSM.Minor},
                            ". The specified shader model ", Uint32{ShaderModel.Major}, '_', Uint32{ShaderModel.Minor}, " will be downgraded.");
        ShaderModel = MaxSM;
    }

    const auto         Profile = GetHLSLProfileString(ShaderCI.Desc.ShaderType, ShaderModel);
    const std::wstring wstrProfile{Profile.begin(), Profile.end()};
    const std::wstring wstrEntryPoint{ShaderCI.EntryPoint, ShaderCI.EntryPoint + strlen(ShaderCI.EntryPoint)};

    std::vector<const wchar_t*> DxilArgs;
    if (m_Target == DXCompilerTarget::Direct3D12)
    {
        DxilArgs.push_back(L"-Zpc"); // Matrices in column-major order

        //DxilArgs.push_back(L"-WX");  // Warnings as errors
#ifdef DILIGENT_DEBUG
        DxilArgs.push_back(L"-Zi"); // Debug info
        DxilArgs.push_back(L"-Od"); // Disable optimization
        if (m_MajorVer > 1 || (m_MajorVer == 1 && m_MinorVer >= 5))
        {
            // Silence the following warning:
            // no output provided for debug - embedding PDB in shader container.  Use -Qembed_debug to silence this warning.
            DxilArgs.push_back(L"-Qembed_debug");
        }
#else
        if (m_MajorVer > 1 || (m_MajorVer == 1 && m_MinorVer >= 5))
            DxilArgs.push_back(L"-O3"); // Optimization level 3
        else
            DxilArgs.push_back(L"-Od"); // TODO: something goes wrong if optimization is enabled
#endif
    }
    else if (m_Target == DXCompilerTarget::Vulkan)
    {
        DxilArgs.assign(
            {
                L"-spirv",
                L"-fspv-reflect",
                //L"-WX", // Warnings as errors
                L"-O3", // Optimization level 3
                L"-Zpc" // Matrices in column-major order
            });

        if (m_APIVersion >= VK_API_VERSION_1_2 && ShaderModel >= ShaderVersion{6, 3})
        {
            // Ray tracing requires SM 6.3 and Vulkan 1.2
            // Inline ray tracing requires SM 6.5 and Vulkan 1.2
            DxilArgs.push_back(L"-fspv-target-env=vulkan1.2");
        }
        else if (m_APIVersion >= VK_API_VERSION_1_1)
        {
            // Wave operations require SM 6.0 and Vulkan 1.1
            DxilArgs.push_back(L"-fspv-target-env=vulkan1.1");
        }
    }
    else
    {
        UNEXPECTED("Unknown compiler target");
    }


    CComPtr<IDxcBlob> pDXIL;
    CComPtr<IDxcBlob> pDxcLog;

    IDXCompiler::CompileAttribs CA;

    const auto Source = BuildHLSLSourceString(ShaderCI, ExtraDefinitions);

    DxcDefine Defines[] = {{L"DXCOMPILER", L""}};

    CA.Source                     = Source.c_str();
    CA.SourceLength               = static_cast<Uint32>(Source.length());
    CA.EntryPoint                 = wstrEntryPoint.c_str();
    CA.Profile                    = wstrProfile.c_str();
    CA.pDefines                   = Defines;
    CA.DefinesCount               = _countof(Defines);
    CA.pArgs                      = DxilArgs.data();
    CA.ArgsCount                  = static_cast<Uint32>(DxilArgs.size());
    CA.pShaderSourceStreamFactory = ShaderCI.pShaderSourceStreamFactory;
    CA.ppBlobOut                  = &pDXIL;
    CA.ppCompilerOutput           = &pDxcLog;

    auto result = Compile(CA);
    HandleHLSLCompilerResult(result, pDxcLog.p, Source, ShaderCI.Desc.Name, ppCompilerOutput);

    if (result && pDXIL && pDXIL->GetBufferSize() > 0)
    {
        if (pByteCode != nullptr)
            pByteCode->assign(static_cast<uint32_t*>(pDXIL->GetBufferPointer()),
                              static_cast<uint32_t*>(pDXIL->GetBufferPointer()) + pDXIL->GetBufferSize() / sizeof(uint32_t));

        if (ppByteCodeBlob != nullptr)
            *ppByteCodeBlob = pDXIL.Detach();
    }
}

bool DXCompilerImpl::RemapResourceBindings(const TResourceBindingMap& ResourceMap,
                                           IDxcBlob*                  pSrcBytecode,
                                           IDxcBlob**                 ppDstByteCode)
{
#if D3D12_SUPPORTED
    auto CreateInstance = GetCreateInstaceProc();
    if (CreateInstance == nullptr)
    {
        LOG_ERROR("Failed to load DXCompiler");
        return false;
    }

    HRESULT              hr;
    CComPtr<IDxcLibrary> library;
    hr = CreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library));
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to create DXC Library");
        return false;
    }

    CComPtr<IDxcAssembler> assembler;
    hr = CreateInstance(CLSID_DxcAssembler, IID_PPV_ARGS(&assembler));
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to create DXC assembler");
        return false;
    }

    CComPtr<IDxcCompiler> compiler;
    hr = CreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to create DXC Compiler");
        return false;
    }

    CComPtr<IDxcBlobEncoding> disasm;
    hr = compiler->Disassemble(pSrcBytecode, &disasm);
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to disassemble bytecode");
        return false;
    }

    CComPtr<ID3D12ShaderReflection> pShaderReflection;
    try
    {
        GetD3D12ShaderReflection(pSrcBytecode, &pShaderReflection);
    }
    catch (...)
    {
        LOG_ERROR("Failed to get shader reflection");
        return false;
    }

    SHADER_TYPE ShaderType = SHADER_TYPE_UNKNOWN;
    {
        D3D12_SHADER_DESC ShDesc = {};
        pShaderReflection->GetDesc(&ShDesc);

        const Uint32 ShType = D3D12_SHVER_GET_TYPE(ShDesc.Version);
        switch (ShType)
        {
            // clang-format off
            case D3D12_SHVER_PIXEL_SHADER:    ShaderType = SHADER_TYPE_PIXEL;            break;
            case D3D12_SHVER_VERTEX_SHADER:   ShaderType = SHADER_TYPE_VERTEX;           break;
            case D3D12_SHVER_GEOMETRY_SHADER: ShaderType = SHADER_TYPE_GEOMETRY;         break;
            case D3D12_SHVER_HULL_SHADER:     ShaderType = SHADER_TYPE_HULL;             break;
            case D3D12_SHVER_DOMAIN_SHADER:   ShaderType = SHADER_TYPE_DOMAIN;           break;
            case D3D12_SHVER_COMPUTE_SHADER:  ShaderType = SHADER_TYPE_COMPUTE;          break;
            case 7:                           ShaderType = SHADER_TYPE_RAY_GEN;          break;
            case 8:                           ShaderType = SHADER_TYPE_RAY_INTERSECTION; break;
            case 9:                           ShaderType = SHADER_TYPE_RAY_ANY_HIT;      break;
            case 10:                          ShaderType = SHADER_TYPE_RAY_CLOSEST_HIT;  break;
            case 11:                          ShaderType = SHADER_TYPE_RAY_MISS;         break;
            case 12:                          ShaderType = SHADER_TYPE_CALLABLE;         break;
            case 13:                          ShaderType = SHADER_TYPE_MESH;             break;
            case 14:                          ShaderType = SHADER_TYPE_AMPLIFICATION;    break;
            // clang-format on
            default:
                UNEXPECTED("Unknown shader type");
        }
    }

    TExtendedResourceMap ExtResourceMap;

    for (auto& NameAndBinding : ResourceMap)
    {
        D3D12_SHADER_INPUT_BIND_DESC ResDesc = {};
        if (pShaderReflection->GetResourceBindingDescByName(NameAndBinding.first.GetStr(), &ResDesc) == S_OK)
        {
            auto& Ext        = ExtResourceMap[&NameAndBinding];
            Ext.SrcBindPoint = ResDesc.BindPoint;
            Ext.SrcSpace     = ResDesc.Space;

#    ifdef NO_D3D_SIT_ACCELSTRUCT_FEEDBACK_TEX
            switch (int{ResDesc.Type}) // Prevent "not a valid value for switch of enum '_D3D_SHADER_INPUT_TYPE'" warning
#    else
            switch (ResDesc.Type)
#    endif
            {
                case D3D_SIT_CBUFFER:
                    Ext.Type = RES_TYPE_CBV;
                    break;
                case D3D_SIT_SAMPLER:
                    Ext.Type = RES_TYPE_SAMPLER;
                    break;
                case D3D_SIT_TBUFFER:
                case D3D_SIT_TEXTURE:
                case D3D_SIT_STRUCTURED:
                case D3D_SIT_BYTEADDRESS:
                case D3D_SIT_RTACCELERATIONSTRUCTURE:
                    Ext.Type = RES_TYPE_SRV;
                    break;
                case D3D_SIT_UAV_RWTYPED:
                case D3D_SIT_UAV_RWSTRUCTURED:
                case D3D_SIT_UAV_RWBYTEADDRESS:
                case D3D_SIT_UAV_APPEND_STRUCTURED:
                case D3D_SIT_UAV_CONSUME_STRUCTURED:
                case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
                case D3D_SIT_UAV_FEEDBACKTEXTURE:
                    Ext.Type = RES_TYPE_UAV;
                    break;
                default:
                    LOG_ERROR("Unknown shader resource type");
                    return false;
            }

#    ifdef DILIGENT_DEVELOPMENT
            {
                static_assert(SHADER_RESOURCE_TYPE_LAST == 8, "Please update the switch below to handle the new shader resource type");
                RES_TYPE ExpectedResType = RES_TYPE_COUNT;
                switch (NameAndBinding.second.ResType)
                {
                    // clang-format off
                    case SHADER_RESOURCE_TYPE_CONSTANT_BUFFER:  ExpectedResType = RES_TYPE_CBV;     break;
                    case SHADER_RESOURCE_TYPE_TEXTURE_SRV:      ExpectedResType = RES_TYPE_SRV;     break;
                    case SHADER_RESOURCE_TYPE_BUFFER_SRV:       ExpectedResType = RES_TYPE_SRV;     break;
                    case SHADER_RESOURCE_TYPE_TEXTURE_UAV:      ExpectedResType = RES_TYPE_UAV;     break;
                    case SHADER_RESOURCE_TYPE_BUFFER_UAV:       ExpectedResType = RES_TYPE_UAV;     break;
                    case SHADER_RESOURCE_TYPE_SAMPLER:          ExpectedResType = RES_TYPE_SAMPLER; break;
                    case SHADER_RESOURCE_TYPE_INPUT_ATTACHMENT: ExpectedResType = RES_TYPE_SRV;     break;
                    case SHADER_RESOURCE_TYPE_ACCEL_STRUCT:     ExpectedResType = RES_TYPE_SRV;     break;
                    // clang-format on
                    default: UNEXPECTED("Unsupported shader resource type.");
                }
                DEV_CHECK_ERR(Ext.Type == ExpectedResType,
                              "There is a mismatch between the type of resource '", NameAndBinding.first.GetStr(),
                              "' expected by the client and the actual resource type.");
            }
#    endif

            // For some reason
            //      Texture2D g_Textures[]
            // produces BindCount == 0, but
            //      ConstantBuffer<CBData> g_ConstantBuffers[]
            // produces BindCount == UINT_MAX
            VERIFY_EXPR((Ext.Type != RES_TYPE_CBV && ResDesc.BindCount == 0) ||
                        (Ext.Type == RES_TYPE_CBV && ResDesc.BindCount == UINT_MAX) ||
                        NameAndBinding.second.ArraySize >= ResDesc.BindCount);
        }
    }

    String dxilAsm;
    dxilAsm.assign(static_cast<const char*>(disasm->GetBufferPointer()), disasm->GetBufferSize());

    if (!PatchDXIL(ResourceMap, ExtResourceMap, ShaderType, dxilAsm))
    {
        LOG_ERROR("Failed to patch resource bindings");
        return false;
    }

    CComPtr<IDxcBlobEncoding> patchedDisasm;
    hr = library->CreateBlobWithEncodingFromPinned(dxilAsm.data(), static_cast<UINT32>(dxilAsm.size()), 0, &patchedDisasm);
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to create disassemble blob");
        return false;
    }

    CComPtr<IDxcOperationResult> dxilResult;
    hr = assembler->AssembleToContainer(patchedDisasm, &dxilResult);
    if (FAILED(hr) || dxilResult == nullptr)
    {
        LOG_ERROR("Failed to create DXIL container");
        return false;
    }

    HRESULT status = E_FAIL;
    dxilResult->GetStatus(&status);

    if (FAILED(status))
    {
        CComPtr<IDxcBlobEncoding> errorsBlob;
        CComPtr<IDxcBlobEncoding> errorsBlobUtf8;
        if (SUCCEEDED(dxilResult->GetErrorBuffer(&errorsBlob)) && SUCCEEDED(library->GetBlobAsUtf8(errorsBlob, &errorsBlobUtf8)))
        {
            String errorLog;
            errorLog.assign(static_cast<const char*>(errorsBlobUtf8->GetBufferPointer()), errorsBlobUtf8->GetBufferSize());
            LOG_ERROR_MESSAGE("Compilation message: ", errorLog);
        }
        else
            LOG_ERROR("Failed to compile patched asm");

        return false;
    }

    CComPtr<IDxcBlob> compiled;
    hr = dxilResult->GetResult(static_cast<IDxcBlob**>(&compiled));
    if (FAILED(hr))
        return false;

    return ValidateAndSign(CreateInstance, library, compiled, ppDstByteCode);
#else

    return false;
#endif // D3D12_SUPPORTED
}

bool DXCompilerImpl::PatchDXIL(const TResourceBindingMap& ResourceMap, TExtendedResourceMap& ExtResMap, SHADER_TYPE ShaderType, String& DXIL)
{
    try
    {
        if ((ShaderType & SHADER_TYPE_ALL_RAY_TRACING) != 0)
        {
            PatchResourceDeclarationRT(ResourceMap, ExtResMap, DXIL);
        }
        else
        {
            PatchResourceDeclaration(ResourceMap, ExtResMap, DXIL);
            PatchResourceHandle(ResourceMap, ExtResMap, DXIL);
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

namespace
{
void ReplaceRecord(String& DXIL, size_t& pos, const String& NewValue, const char* Name, const char* RecordName, const Uint32 ExpectedPrevValue)
{
#define CHECK_PATCHING_ERROR(Cond, ...)                                                         \
    if (!(Cond))                                                                                \
    {                                                                                           \
        LOG_ERROR_AND_THROW("Unable to patch DXIL for resource '", Name, "': ", ##__VA_ARGS__); \
    }

    static const String i32           = "i32 ";
    static const String NumberSymbols = "+-0123456789";

    // , i32 -1
    // ^
    CHECK_PATCHING_ERROR(pos + 1 < DXIL.length() && DXIL[pos] == ',' && DXIL[pos + 1] == ' ', RecordName, " record is not found")

    pos += 2;
    // , i32 -1
    //   ^

    CHECK_PATCHING_ERROR(std::strncmp(&DXIL[pos], i32.c_str(), i32.length()) == 0, "unexpected ", RecordName, " record type")
    pos += i32.length();
    // , i32 -1
    //       ^

    auto RecordEndPos = DXIL.find_first_not_of(NumberSymbols, pos);
    CHECK_PATCHING_ERROR(pos != String::npos, "unable to find the end of the ", RecordName, " record data")
    // , i32 -1
    //         ^
    //    RecordEndPos

    Uint32 PrevValue = static_cast<Uint32>(std::stoi(DXIL.substr(pos, RecordEndPos - pos)));
    CHECK_PATCHING_ERROR(PrevValue == ExpectedPrevValue, "previous value does not match the expected");

    DXIL.replace(pos, RecordEndPos - pos, NewValue);
    // , i32 1
    //         ^
    //    RecordEndPos

    pos += NewValue.length();
    // , i32 1
    //        ^
    //       pos

#undef CHECK_PATCHING_ERROR
}

bool IsWordSymbol(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c == '_');
}

bool IsNumberSymbol(char c)
{
    return (c >= '0' && c <= '9');
}

} // namespace

void DXCompilerImpl::PatchResourceDeclarationRT(const TResourceBindingMap& ResourceMap, TExtendedResourceMap& ExtResMap, String& DXIL)
{
#define CHECK_PATCHING_ERROR(Cond, ...)                                                         \
    if (!(Cond))                                                                                \
    {                                                                                           \
        LOG_ERROR_AND_THROW("Unable to patch DXIL for resource '", Name, "': ", ##__VA_ARGS__); \
    }

    static const String i32              = "i32 ";
    static const String NumberSymbols    = "+-0123456789";
    static const String ResourceRecStart = "= !{";

    // This resource patching method is valid for ray tracing shaders and non-optimized shaders with metadata.
    for (auto& ResPair : ResourceMap)
    {
        // Patch metadata resource record

        // https://github.com/microsoft/DirectXShaderCompiler/blob/master/docs/DXIL.rst#metadata-resource-records
        // Idx | Type            | Description
        // ----|-----------------|------------------------------------------------------------------------------------------
        //  0  | i32             | Unique resource record ID, used to identify the resource record in createHandle operation.
        //  1  | Pointer         | Pointer to a global constant symbol with the original shape of resource and element type
        //  2  | Metadata string | Name of resource variable.
        //  3  | i32             | Bind space ID of the root signature range that corresponds to this resource.
        //  4  | i32             | Bind lower bound of the root signature range that corresponds to this resource.
        //  5  | i32             | Range size of the root signature range that corresponds to this resource.

        // Example:
        //
        // !158 = !{i32 0, %"class.RWTexture2D<vector<float, 4> >"* @"\01?g_ColorBuffer@@3V?$RWTexture2D@V?$vector@M$03@@@@A", !"g_ColorBuffer", i32 -1, i32 -1, i32 1, i32 2, i1 false, i1 false, i1 false, !159}

        const auto* Name      = ResPair.first.GetStr();
        const auto  Space     = ResPair.second.Space;
        const auto  BindPoint = ResPair.second.BindPoint;
        const auto  DxilName  = String{"!\""} + Name + "\"";
        auto&       Ext       = ExtResMap[&ResPair];

        size_t pos = DXIL.find(DxilName);
        if (pos == String::npos)
            continue;

        // !"g_ColorBuffer", i32 -1, i32 -1,
        // ^
        const size_t EndOfResTypeRecord = pos;

        // Parse resource class.
        pos = DXIL.rfind(ResourceRecStart, EndOfResTypeRecord);
        CHECK_PATCHING_ERROR(pos != String::npos, "");
        pos += ResourceRecStart.length();

        // !5 = !{i32 0,
        //        ^
        CHECK_PATCHING_ERROR(std::strncmp(DXIL.c_str() + pos, i32.c_str(), i32.length()) == 0, "");

        // !5 = !{i32 0,
        //            ^
        pos += i32.length();

        const size_t RecordIdStartPos = pos;

        pos = DXIL.find_first_not_of(NumberSymbols, pos);
        CHECK_PATCHING_ERROR(pos != String::npos, "");

        const Uint32 RecordId = static_cast<Uint32>(std::atoi(DXIL.c_str() + RecordIdStartPos));

        VERIFY_EXPR(Ext.RecordId == ~0u || Ext.RecordId == RecordId);
        Ext.RecordId = RecordId;

        // !"g_ColorBuffer", i32 -1, i32 -1,
        //                 ^
        pos = EndOfResTypeRecord + DxilName.length();
        ReplaceRecord(DXIL, pos, std::to_string(Space), Name, "space", Ext.SrcSpace);

        // !"g_ColorBuffer", i32 0, i32 -1,
        //                        ^
        ReplaceRecord(DXIL, pos, std::to_string(BindPoint), Name, "binding", Ext.SrcBindPoint);

        // !"g_ColorBuffer", i32 0, i32 1,
        //                               ^
    }
#undef CHECK_PATCHING_ERROR
}

void DXCompilerImpl::PatchResourceDeclaration(const TResourceBindingMap& ResourceMap, TExtendedResourceMap& ExtResMap, String& DXIL)
{
    // This resource patching method is valid for optimized shaders without metadata.

    static const String i32                   = "i32 ";
    static const String NumberSymbols         = "+-0123456789";
    static const String ResourceRecStart      = "= !{";
    static const String ResNameDecl           = ", !\"";
    static const String SamplerPart           = "SamplerState";
    static const String TexturePart           = "Texture";
    static const String RWTexturePart         = "RWTexture";
    static const String AccelStructPart       = "RaytracingAccelerationStructure";
    static const String StructBufferPart      = "StructuredBuffer<";
    static const String RWStructBufferPart    = "RWStructuredBuffer<";
    static const String ByteAddrBufPart       = "ByteAddressBuffer";
    static const String RWByteAddrBufPart     = "RWByteAddressBuffer";
    static const String TexBufferPart         = "Buffer<";
    static const String RWFmtBufferPart       = "RWBuffer<";
    static const String DxAlignmentLegacyPart = "dx.alignment.legacy.";
    static const String StructPart            = "struct.";
    static const String ClassPart             = "class.";

    enum
    {
        ALIGNMENT_LEGACY_PART = 1 << 0,
        STRUCT_PART           = 1 << 1,
        CLASS_PART            = 1 << 2,
        STRING_PART           = 1 << 3,
    };

    const auto IsTextureSuffix = [](const char* Str) //
    {
        return std::strncmp(Str, "1D<", 3) == 0 ||
            std::strncmp(Str, "1DArray<", 8) == 0 ||
            std::strncmp(Str, "2D<", 3) == 0 ||
            std::strncmp(Str, "2DArray<", 8) == 0 ||
            std::strncmp(Str, "3D<", 3) == 0 ||
            std::strncmp(Str, "2DMS<", 5) == 0 ||
            std::strncmp(Str, "2DMSArray<", 10) == 0 ||
            std::strncmp(Str, "Cube<", 5) == 0 ||
            std::strncmp(Str, "CubeArray<", 10) == 0;
    };

    const auto ReadRecord = [&DXIL](size_t& pos, Uint32& CurValue) //
    {
        // , i32 -1
        // ^
        if (pos + 1 >= DXIL.length() || DXIL[pos] != ',' || DXIL[pos + 1] != ' ')
            return false;

        pos += 2;
        // , i32 -1
        //   ^

        if (std::strncmp(&DXIL[pos], i32.c_str(), i32.length()) != 0)
            return false;
        pos += i32.length();
        // , i32 -1
        //       ^

        auto RecordEndPos = DXIL.find_first_not_of(NumberSymbols, pos);
        if (pos == String::npos)
            return false;
        // , i32 -1
        //         ^
        //    RecordEndPos

        CurValue = static_cast<Uint32>(std::stoi(DXIL.substr(pos, RecordEndPos - pos)));
        pos      = RecordEndPos;
        return true;
    };

    const auto ReadResName = [&DXIL](size_t& pos, String& name) //
    {
        VERIFY_EXPR(pos > 0 && DXIL[pos - 1] == '"');
        const size_t startPos = pos;
        for (; pos < DXIL.size(); ++pos)
        {
            const char c = DXIL[pos];
            if (IsWordSymbol(c))
                continue;

            if (c == '"')
            {
                name = DXIL.substr(startPos, pos - startPos);
                return true;
            }
            break;
        }
        return false;
    };

#define CHECK_PATCHING_ERROR(Cond, ...)                               \
    if (!(Cond))                                                      \
    {                                                                 \
        LOG_ERROR_AND_THROW("Unable to patch DXIL: ", ##__VA_ARGS__); \
    }
    for (size_t pos = 0; pos < DXIL.size();)
    {
        // Example:
        //
        // !5 = !{i32 0, %"class.Texture2D<vector<float, 4> >"* undef, !"", i32 -1, i32 -1, i32 1, i32 2, i32 0, !6}

        pos = DXIL.find(ResNameDecl, pos);
        if (pos == String::npos)
            break;

        // undef, !"", i32 -1,
        //      ^
        const size_t EndOfResTypeRecord = pos;

        // undef, !"", i32 -1,...  or  undef, !"g_Tex2D", i32 -1,...
        //         ^                            ^
        pos += ResNameDecl.length();
        const size_t BeginOfResName = pos;

        String ResName;
        if (!ReadResName(pos, ResName))
        {
            // This is not a resource declaration record, continue searching.
            continue;
        }

        // undef, !"", i32 -1,
        //           ^
        const size_t BindingRecordStart = pos + 1;
        VERIFY_EXPR(DXIL[BindingRecordStart] == ',');

        // Parse resource class.
        pos = DXIL.rfind(ResourceRecStart, EndOfResTypeRecord);
        CHECK_PATCHING_ERROR(pos != String::npos, "failed to find resource record start block");
        pos += ResourceRecStart.length();

        // !5 = !{i32 0,
        //        ^
        if (std::strncmp(DXIL.c_str() + pos, i32.c_str(), i32.length()) != 0)
        {
            // This is not a resource declaration record, continue searching.
            pos = BindingRecordStart;
            continue;
        }
        // !5 = !{i32 0,
        //            ^
        pos += i32.length();

        const size_t RecordIdStartPos = pos;

        pos = DXIL.find_first_not_of(NumberSymbols, pos);
        CHECK_PATCHING_ERROR(pos != String::npos, "failed to parse Record ID record data");
        // !{i32 0, %"class.Texture2D<...
        //        ^
        const Uint32 RecordId = static_cast<Uint32>(std::atoi(DXIL.c_str() + RecordIdStartPos));

        CHECK_PATCHING_ERROR(pos + 1 < DXIL.length() && DXIL[pos] == ',' && DXIL[pos + 1] == ' ', "failed to find the end of the Record ID record data");
        pos += 2;
        // !{i32 0, %"class.Texture2D<...  or  !{i32 0, [4 x %"class.Texture2D<...
        //          ^                                   ^

        // skip array declaration
        if (DXIL[pos] == '[')
        {
            ++pos;
            for (; pos < EndOfResTypeRecord; ++pos)
            {
                const char c = DXIL[pos];
                if (!(IsNumberSymbol(c) || (c == ' ') || (c == 'x')))
                    break;
            }
        }

        if (DXIL[pos] != '%')
        {
            // This is not a resource declaration record, continue searching.
            pos = BindingRecordStart;
            continue;
        }

        // !{i32 0, %"class.Texture2D<...  or  !{i32 0, [4 x %"class.Texture2D<...
        //           ^                                        ^
        ++pos;

        Uint32 NameParts = 0;
        if (DXIL[pos] == '"')
        {
            ++pos;
            NameParts |= STRING_PART;
        }

        if (std::strncmp(&DXIL[pos], DxAlignmentLegacyPart.c_str(), DxAlignmentLegacyPart.length()) == 0)
        {
            pos += DxAlignmentLegacyPart.length();
            NameParts |= ALIGNMENT_LEGACY_PART;
        }
        if (std::strncmp(&DXIL[pos], StructPart.c_str(), StructPart.length()) == 0)
        {
            pos += StructPart.length();
            NameParts |= STRUCT_PART;
        }
        if (std::strncmp(&DXIL[pos], ClassPart.c_str(), ClassPart.length()) == 0)
        {
            pos += ClassPart.length();
            NameParts |= CLASS_PART;
        }

        // !{i32 0, %"class.Texture2D<...
        //                  ^

        RES_TYPE ResType = RES_TYPE_INVALID;
        if (std::strncmp(&DXIL[pos], SamplerPart.c_str(), SamplerPart.length()) == 0)
            ResType = RES_TYPE_SAMPLER;
        else if (std::strncmp(&DXIL[pos], TexturePart.c_str(), TexturePart.length()) == 0 && IsTextureSuffix(&DXIL[pos + TexturePart.length()]))
            ResType = RES_TYPE_SRV;
        else if (std::strncmp(&DXIL[pos], StructBufferPart.c_str(), StructBufferPart.length()) == 0)
            ResType = RES_TYPE_SRV;
        else if (std::strncmp(&DXIL[pos], ByteAddrBufPart.c_str(), ByteAddrBufPart.length()) == 0)
            ResType = RES_TYPE_SRV;
        else if (std::strncmp(&DXIL[pos], TexBufferPart.c_str(), TexBufferPart.length()) == 0)
            ResType = RES_TYPE_SRV;
        else if (std::strncmp(&DXIL[pos], AccelStructPart.c_str(), AccelStructPart.length()) == 0)
            ResType = RES_TYPE_SRV;
        else if (std::strncmp(&DXIL[pos], RWTexturePart.c_str(), RWTexturePart.length()) == 0 && IsTextureSuffix(&DXIL[pos + RWTexturePart.length()]))
            ResType = RES_TYPE_UAV;
        else if (std::strncmp(&DXIL[pos], RWStructBufferPart.c_str(), RWStructBufferPart.length()) == 0)
            ResType = RES_TYPE_UAV;
        else if (std::strncmp(&DXIL[pos], RWByteAddrBufPart.c_str(), RWByteAddrBufPart.length()) == 0)
            ResType = RES_TYPE_UAV;
        else if (std::strncmp(&DXIL[pos], RWFmtBufferPart.c_str(), RWFmtBufferPart.length()) == 0)
            ResType = RES_TYPE_UAV;
        else if ((NameParts & ~ALIGNMENT_LEGACY_PART) == 0)
        {
            // !{i32 0, %Constants* undef,  or  !{i32 0, %dx.alignment.legacy.Constants* undef,
            //           ^                                                    ^

            // Try to find constant buffer.
            for (auto& ResInfo : ExtResMap)
            {
                const RES_TYPE Type = ResInfo.second.Type;

                if (Type != RES_TYPE_CBV)
                    continue;

                const char*  Name    = ResInfo.first->first.GetStr();
                const size_t NameLen = strlen(Name);
                if (std::strncmp(&DXIL[pos], Name, NameLen) == 0)
                {
                    const char c = DXIL[pos + NameLen];

                    if (IsWordSymbol(c))
                        continue; // name is partially equal, continue searching

                    VERIFY_EXPR((c == '*' && ResInfo.first->second.ArraySize == 1) || (c == ']' && ResInfo.first->second.ArraySize > 1));

                    ResType = RES_TYPE_CBV;
                    break;
                }
            }
        }

        if (ResType == RES_TYPE_INVALID)
        {
            // This is not a resource declaration record, continue searching.
            pos = BindingRecordStart;
            continue;
        }

        // Read binding & space.
        pos              = BindingRecordStart;
        Uint32 BindPoint = ~0u;
        Uint32 Space     = ~0u;

        // !"", i32 -1, i32 -1,
        //    ^
        if (!ReadRecord(pos, Space))
        {
            // This is not a resource declaration record, continue searching.
            continue;
        }
        // !"", i32 -1, i32 -1,
        //            ^
        if (!ReadRecord(pos, BindPoint))
        {
            // This is not a resource declaration record, continue searching.
            continue;
        }
        // Search in resource map.
        TResourceBindingMap::value_type const* pResPair = nullptr;
        ResourceExtendedInfo*                  pExt     = nullptr;
        for (auto& ResInfo : ExtResMap)
        {
            if (ResInfo.second.SrcBindPoint == BindPoint &&
                ResInfo.second.SrcSpace == Space &&
                ResInfo.second.Type == ResType)
            {
                pResPair = ResInfo.first;
                pExt     = &ResInfo.second;
                break;
            }
        }
        CHECK_PATCHING_ERROR(pResPair != nullptr && pExt != nullptr, "failed to find resource in ResourceMap");

        VERIFY_EXPR(ResName.empty() || ResName == pResPair->first.GetStr());
        VERIFY_EXPR(pExt->RecordId == ~0u || pExt->RecordId == RecordId);
        pExt->RecordId = RecordId;

        // Remap bindings.
        pos = BindingRecordStart;

        // !"", i32 -1, i32 -1,
        //    ^
        ReplaceRecord(DXIL, pos, std::to_string(pResPair->second.Space), pResPair->first.GetStr(), "space", pExt->SrcSpace);

        // !"", i32 0, i32 -1,
        //           ^
        ReplaceRecord(DXIL, pos, std::to_string(pResPair->second.BindPoint), pResPair->first.GetStr(), "register", pExt->SrcBindPoint);

        // !"", i32 0, i32 1,
        //                  ^

        // Add resource name
        if (ResName.empty())
        {
            DXIL.insert(BeginOfResName, pResPair->first.GetStr());
        }
    }
#undef CHECK_PATCHING_ERROR
}

void DXCompilerImpl::PatchResourceHandle(const TResourceBindingMap& ResourceMap, TExtendedResourceMap& ExtResMap, String& DXIL)
{
    // Patch createHandle command
    static const String   CallHandlePattern = " = call %dx.types.Handle @dx.op.createHandle(";
    static const String   i32               = "i32 ";
    static const String   i8                = "i8 ";
    static const RES_TYPE ResClassToType[]  = {RES_TYPE_SRV, RES_TYPE_UAV, RES_TYPE_CBV, RES_TYPE_SAMPLER};

    const auto NextArg = [&DXIL](size_t& pos) //
    {
        for (; pos < DXIL.size(); ++pos)
        {
            const char c = DXIL[pos];
            if (c == ',')
                return true; // OK
            if (c == ')' || c == '\n')
                return false; // end of createHandle()
        }
        // end of bytecode
        return false;
    };

    const auto ReplaceBindPoint = [&](Uint32 ResClass, Uint32 RangeId, size_t IndexStartPos, size_t IndexEndPos) //
    {
        const String SrcIndexStr = DXIL.substr(IndexStartPos, IndexEndPos - IndexStartPos);
        VERIFY_EXPR(IsNumberSymbol(SrcIndexStr.front()));

        const Uint32 SrcIndex = static_cast<Uint32>(std::stoi(SrcIndexStr));
        const auto   ResType  = ResClassToType[ResClass];

        BindInfo const*       pBind = nullptr;
        ResourceExtendedInfo* pExt  = nullptr;
        for (auto& ResInfo : ExtResMap)
        {
            if (ResInfo.second.RecordId == RangeId &&
                ResInfo.second.Type == ResType &&
                SrcIndex >= ResInfo.second.SrcBindPoint &&
                SrcIndex < ResInfo.second.SrcBindPoint + ResInfo.first->second.ArraySize)
            {
                pBind = &ResInfo.first->second;
                pExt  = &ResInfo.second;
                break;
            }
        }
        if (pBind == nullptr || pExt == nullptr)
            LOG_ERROR_AND_THROW("Failed to find resource in ResourceMap");

        VERIFY_EXPR(SrcIndex >= pExt->SrcBindPoint);
        VERIFY_EXPR(pExt->SrcBindPoint != ~0u);

        const Uint32 IndexOffset = SrcIndex - pExt->SrcBindPoint;
        VERIFY_EXPR((pBind->BindPoint + IndexOffset) >= pBind->BindPoint);

        const String NewIndexStr = std::to_string(pBind->BindPoint + IndexOffset);
        DXIL.replace(DXIL.begin() + IndexStartPos, DXIL.begin() + IndexEndPos, NewIndexStr);
    };

#define CHECK_PATCHING_ERROR(Cond, ...)                                              \
    if (!(Cond))                                                                     \
    {                                                                                \
        LOG_ERROR_AND_THROW("Unable to patch DXIL createHandle(): ", ##__VA_ARGS__); \
    }

    for (size_t pos = 0; pos < DXIL.size();)
    {
        // %dx.types.Handle @dx.op.createHandle(
        //        i32,                  ; opcode
        //        i8,                   ; resource class: SRV=0, UAV=1, CBV=2, Sampler=3
        //        i32,                  ; resource range ID (constant)
        //        i32,                  ; index into the range
        //        i1)                   ; non-uniform resource index: false or true

        // Example:
        //
        // = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)

        size_t callHandlePos = DXIL.find(CallHandlePattern, pos);
        if (callHandlePos == String::npos)
            break;

        pos = callHandlePos + CallHandlePattern.length();
        // @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)
        //                     ^

        // Skip opcode.

        CHECK_PATCHING_ERROR(std::strncmp(&DXIL[pos], i32.c_str(), i32.length()) == 0, "Opcode record is not found");
        pos += i32.length();
        // @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)
        //                         ^

        CHECK_PATCHING_ERROR(NextArg(pos), "failed to find end of the Opcode record data");
        // @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)
        //                           ^

        // Read resource class.

        CHECK_PATCHING_ERROR(pos + 1 < DXIL.length() && DXIL[pos] == ',' && DXIL[pos + 1] == ' ', "Resource Class record is not found");
        pos += 2;
        // @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)
        //                             ^

        CHECK_PATCHING_ERROR(std::strncmp(&DXIL[pos], i8.c_str(), i8.length()) == 0, "Resource Class record data is not found");
        pos += i8.length();
        // @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)
        //                                ^

        const size_t ResClassStartPos = pos;

        CHECK_PATCHING_ERROR(NextArg(pos), "failed to find end of the Resource class record data");
        // @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)
        //                                 ^
        const Uint32 ResClass = static_cast<Uint32>(std::atoi(DXIL.c_str() + ResClassStartPos));

        // Read resource range ID.

        CHECK_PATCHING_ERROR(pos + 1 < DXIL.length() && DXIL[pos] == ',' && DXIL[pos + 1] == ' ', "Range ID record is not found");
        pos += 2;
        // @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)
        //                                   ^

        CHECK_PATCHING_ERROR(std::strncmp(&DXIL[pos], i32.c_str(), i32.length()) == 0, "Range ID record data is not found");
        pos += i32.length();
        // @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)
        //                                       ^

        const size_t RangeIdStartPos = pos;

        CHECK_PATCHING_ERROR(NextArg(pos), "failed to find end of the Range ID record data");
        // @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)
        //                                        ^
        const Uint32 RangeId = static_cast<Uint32>(std::atoi(DXIL.c_str() + RangeIdStartPos));

        // Read index in range.

        CHECK_PATCHING_ERROR(pos + 1 < DXIL.length() && DXIL[pos] == ',' && DXIL[pos + 1] == ' ', "Index record is not found");
        pos += 2;
        // @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)
        //                                          ^

        CHECK_PATCHING_ERROR(std::strncmp(&DXIL[pos], i32.c_str(), i32.length()) == 0, "Index record data is not found");
        pos += i32.length();
        // @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)
        //                                              ^

        const size_t IndexStartPos = pos;

        CHECK_PATCHING_ERROR(NextArg(pos), "failed to find the end of the Index record data");
        // @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)
        //                                               ^

        // Replace index.
        const size_t IndexEndPos = pos;
        const String SrcIndexStr = DXIL.substr(IndexStartPos, pos - IndexStartPos);
        CHECK_PATCHING_ERROR(!SrcIndexStr.empty(), "Bind point index must not be empty");

        if (SrcIndexStr.front() == '%')
        {
            // dynamic bind point
            const String IndexDecl = SrcIndexStr + " = add i32 ";

            size_t IndexDeclPos = DXIL.rfind(IndexDecl, IndexEndPos);
            CHECK_PATCHING_ERROR(IndexDeclPos != String::npos, "failed to find dynamic index declaration");

            // Example:
            //   %22 = add i32 %17, 7
            //                 ^
            pos = IndexDeclPos + IndexDecl.length();

            // check first arg
            if (DXIL[pos] == '%')
            {
                // first arg is variable, move to second arg
                CHECK_PATCHING_ERROR(NextArg(pos), "");
                //   %22 = add i32 %17, 7  or  %24 = add i32 %j.0, 1
                //                    ^                          ^
                VERIFY_EXPR(pos + 1 < DXIL.length() && DXIL[pos] == ',' && DXIL[pos + 1] == ' ');
                pos += 2; // skip ', '

                // second arg must be a constant
                CHECK_PATCHING_ERROR(IsNumberSymbol(DXIL[pos]), "second argument expected to be an integer constant");

                const size_t ArgStart = pos;
                for (; pos < DXIL.size(); ++pos)
                {
                    const char c = DXIL[pos];
                    if (!IsNumberSymbol(c))
                        break;
                }
                CHECK_PATCHING_ERROR(DXIL[pos] == ',' || DXIL[pos] == '\n', "failed to parse second argument");

                //   %22 = add i32 %17, 7
                //                       ^

                const size_t ArgEnd = pos;
                ReplaceBindPoint(ResClass, RangeId, ArgStart, ArgEnd);
            }
            else
            {
                // first arg is a constant
                VERIFY_EXPR(IsNumberSymbol(DXIL[pos]));

                const size_t ArgStart = pos;
                for (; pos < DXIL.size(); ++pos)
                {
                    const char c = DXIL[pos];
                    if (!IsNumberSymbol(c))
                        break;
                }
                CHECK_PATCHING_ERROR(DXIL[pos] == ',' || DXIL[pos] == '\n', "failed to parse second argument");
                //   %22 = add i32 7, %17
                //                  ^

                const size_t ArgEnd = pos;
                ReplaceBindPoint(ResClass, RangeId, ArgStart, ArgEnd);
            }

#ifdef DILIGENT_DEVELOPMENT
            Uint32 IndexVarUsageCount = 0;
            for (pos = 0; pos < DXIL.size();)
            {
                pos = DXIL.find(SrcIndexStr, pos + 1);
                if (pos == String::npos)
                    break;

                pos += SrcIndexStr.size();
                if (DXIL[pos] == ' ' || DXIL[pos] == ',')
                    ++IndexVarUsageCount;
            }
            DEV_CHECK_ERR(IndexVarUsageCount == 2, "Temp variable '", SrcIndexStr, "' with resource bind point used more than 2 times, patching for this variable may lead to UB");
#endif
        }
        else
        {
            // constant bind point
            ReplaceBindPoint(ResClass, RangeId, IndexStartPos, IndexEndPos);
        }
        pos = IndexEndPos;
    }
#undef CHECK_PATCHING_ERROR
}

bool IsDXILBytecode(const void* pBytecode, size_t Size)
{
    const auto* data_begin = reinterpret_cast<const uint8_t*>(pBytecode);
    const auto* data_end   = data_begin + Size;
    const auto* ptr        = data_begin;

    if (ptr + sizeof(hlsl::DxilContainerHeader) > data_end)
    {
        // No space for the container header
        return false;
    }

    // A DXIL container is composed of a header, a sequence of part lengths, and a sequence of parts.
    // https://github.com/microsoft/DirectXShaderCompiler/blob/master/docs/DXIL.rst#dxil-container-format
    const auto& ContainerHeader = *reinterpret_cast<const hlsl::DxilContainerHeader*>(ptr);
    if (ContainerHeader.HeaderFourCC != hlsl::DFCC_Container)
    {
        // Incorrect FourCC
        return false;
    }

    if (ContainerHeader.Version.Major != hlsl::DxilContainerVersionMajor)
    {
        LOG_WARNING_MESSAGE("Unable to parse DXIL container: the container major version is ", Uint32{ContainerHeader.Version.Major},
                            " while ", Uint32{hlsl::DxilContainerVersionMajor}, " is expected");
        return false;
    }

    // The header is followed by uint32_t PartOffset[PartCount];
    // The offset is to a DxilPartHeader.
    ptr += sizeof(hlsl::DxilContainerHeader);
    if (ptr + sizeof(uint32_t) * ContainerHeader.PartCount > data_end)
    {
        // No space for offsets
        return false;
    }

    const auto* PartOffsets = reinterpret_cast<const uint32_t*>(ptr);
    for (uint32_t part = 0; part < ContainerHeader.PartCount; ++part)
    {
        const auto Offset = PartOffsets[part];
        if (data_begin + Offset + sizeof(hlsl::DxilPartHeader) > data_end)
        {
            // No space for the part header
            return false;
        }

        const auto& PartHeader = *reinterpret_cast<const hlsl::DxilPartHeader*>(data_begin + Offset);
        if (PartHeader.PartFourCC == hlsl::DFCC_DXIL)
        {
            // We found DXIL part
            return true;
        }
    }

    return false;
}

} // namespace Diligent
