#include "RenderContext.h"
#include "Material.h"
#include "Mesh.h"
#include "Engine.h"
#include "EngineLimits.h"
#include "PostRenderPass.h"
#include "TextureCompressor.h"
#include "WorldRenderPass.h"
#include "CoreLib/LibIO.h"
#include "CoreLib/Graphics/TextureFile.h"
#include <assert.h>

namespace GameEngine
{
	using namespace CoreLib;
	using namespace CoreLib::IO;
	using namespace VectorMath;

	String GetSpireOutput(SpireDiagnosticSink * sink)
	{
		int bufferSize = spGetDiagnosticOutput(sink, nullptr, 0);
		List<char> buffer;
		buffer.SetSize(bufferSize);
		spGetDiagnosticOutput(sink, buffer.Buffer(), buffer.Count());
		return String(buffer.Buffer());
	}

	PipelineClass * Drawable::GetPipeline(int passId, PipelineContext & pipelineManager)
	{
		if (!Engine::Instance()->GetGraphicsSettings().UsePipelineCache || pipelineCache[passId] == nullptr)
		{
			auto rs = pipelineManager.GetPipeline(&vertFormat);
			pipelineCache[passId] = rs;
		}
		return pipelineCache[passId];
	}

	bool Drawable::IsTransparent()
	{
		return material->IsTransparent;
	}

	void Drawable::UpdateMaterialUniform()
	{
		if (material->ParameterDirty)
		{
			material->ParameterDirty = false;
			for (auto & p : pipelineCache)
				p = nullptr;

			auto update = [=](ModuleInstance & moduleInstance)
			{
				if (moduleInstance.BufferLength)
				{
					unsigned char * ptr0 = (unsigned char*)moduleInstance.UniformMemory->BufferPtr() + moduleInstance.BufferOffset;
					auto ptr = ptr0;
					auto end = ptr + moduleInstance.BufferLength;
					material->FillInstanceUniformBuffer(&moduleInstance, [](const String&) {},
						[&](auto & val)
					{
						if (ptr + sizeof(val) > end)
							throw InvalidOperationException("insufficient buffer.");
						*((decltype(&val))ptr) = val;
						ptr += sizeof(val);
					},
						[&](int alignment)
					{
						if (auto m = (int)(((unsigned long long)(void*)ptr) % alignment))
						{
							ptr += (alignment - m);
						}
					}
					);
					scene->instanceUniformMemory.Sync(ptr0, moduleInstance.BufferLength);
				}
			};
			update(material->MaterialGeometryModule);
			update(material->MaterialPatternModule);
		}
	}

	void Drawable::UpdateTransformUniform(const VectorMath::Matrix4 & localTransform)
	{
		if (type != DrawableType::Static)
			throw InvalidOperationException("cannot update non-static drawable with static transform data.");
		if (!transformModule->UniformMemory)
			throw InvalidOperationException("invalid buffer.");
		transformModule->SetUniformData((void*)&localTransform, sizeof(Matrix4));
	}

	void Drawable::UpdateTransformUniform(const VectorMath::Matrix4 & localTransform, const Pose & pose, RetargetFile * retarget)
	{
		if (type != DrawableType::Skeletal)
			throw InvalidOperationException("cannot update static drawable with skeletal transform data.");
		if (!transformModule->UniformMemory)
			throw InvalidOperationException("invalid buffer.");

		const int poseMatrixSize = skeleton->Bones.Count() * sizeof(Matrix4);

		// ensure allocated transform buffer is sufficient
		_ASSERT(transformModule->BufferLength >= poseMatrixSize);

		List<Matrix4> matrices;
		pose.GetMatrices(skeleton, matrices, true, retarget);
		for (int i = 0; i < matrices.Count(); i++)
		{
			Matrix4::Multiply(matrices[i], localTransform, matrices[i]);
		}
		transformModule->SetUniformData((void*)matrices.Buffer(), sizeof(Matrix4) * matrices.Count());
	}
    RefPtr<DrawableMesh> SceneResource::CreateDrawableMesh(Mesh * mesh)
    {
        RefPtr<DrawableMesh> result = new DrawableMesh(rendererResource);
        result->vertexBufferOffset = (int)((char*)rendererResource->vertexBufferMemory.Alloc(mesh->GetVertexCount() * mesh->GetVertexSize()) - (char*)rendererResource->vertexBufferMemory.BufferPtr());
        result->indexBufferOffset = (int)((char*)rendererResource->indexBufferMemory.Alloc(mesh->Indices.Count() * sizeof(mesh->Indices[0])) - (char*)rendererResource->indexBufferMemory.BufferPtr());
        result->vertexFormat = rendererResource->pipelineManager.LoadVertexFormat(mesh->GetVertexFormat());
        result->vertexCount = mesh->GetVertexCount();
        rendererResource->indexBufferMemory.SetDataAsync(result->indexBufferOffset, mesh->Indices.Buffer(), mesh->Indices.Count() * sizeof(mesh->Indices[0]));
        rendererResource->vertexBufferMemory.SetDataAsync(result->vertexBufferOffset, mesh->GetVertexBuffer(), mesh->GetVertexCount() * result->vertexFormat.Size());
        result->indexCount = mesh->Indices.Count();
        return result;
    }
	RefPtr<DrawableMesh> SceneResource::LoadDrawableMesh(Mesh * mesh)
	{
		RefPtr<DrawableMesh> result;
		if (meshes.TryGetValue(mesh->GetUID(), result))
		    return result;

        result = CreateDrawableMesh(mesh);
		meshes[mesh->GetUID()] = result;
		return result;
	}
	Texture2D * SceneResource::LoadTexture2D(const String & name, CoreLib::Graphics::TextureFile & data)
	{
		RefPtr<Texture2D> value;
		if (textures.TryGetValue(name, value))
			return value.Ptr();
		StorageFormat format;
		DataType dataType = DataType::Byte4;
		char * textureData = (char*)data.GetData(0).Buffer();
		CoreLib::List<char> translatedData;
		switch (data.GetFormat())
		{
		case CoreLib::Graphics::TextureStorageFormat::R8:
			format = StorageFormat::R_8;
			dataType = DataType::Byte;
			break;
		case CoreLib::Graphics::TextureStorageFormat::RG8:
			format = StorageFormat::RG_8;
			dataType = DataType::Byte2;
			break;
		case CoreLib::Graphics::TextureStorageFormat::RGB8:
			format = StorageFormat::RGBA_8;
			dataType = DataType::Byte4;
			translatedData = Graphics::TranslateThreeChannelTextureFormat(textureData, data.GetWidth()*data.GetHeight(), 1);
			textureData = translatedData.Buffer();
			break;
		case CoreLib::Graphics::TextureStorageFormat::RGBA8:
			format = StorageFormat::RGBA_8;
			dataType = DataType::Byte4;
			break;
		case CoreLib::Graphics::TextureStorageFormat::R_F32:
			format = StorageFormat::R_F32;
			dataType = DataType::Float;
			break;
		case CoreLib::Graphics::TextureStorageFormat::RG_F32:
			format = StorageFormat::RG_F32;
			dataType = DataType::Float2;
			break;
		case CoreLib::Graphics::TextureStorageFormat::RGB_F32:
			format = StorageFormat::RGBA_F32;
			dataType = DataType::Float4;
			translatedData = Graphics::TranslateThreeChannelTextureFormat(textureData, data.GetWidth() * data.GetHeight(), 4);
			textureData = translatedData.Buffer();
			break;
		case CoreLib::Graphics::TextureStorageFormat::RGBA_F32:
			format = StorageFormat::RGBA_F32;
			dataType = DataType::Float4;
			break;
		case CoreLib::Graphics::TextureStorageFormat::BC1:
			format = StorageFormat::BC1;
			break;
		case CoreLib::Graphics::TextureStorageFormat::BC3:
			format = StorageFormat::BC3;
			break;
		case CoreLib::Graphics::TextureStorageFormat::BC5:
			format = StorageFormat::BC5;
			break;
		default:
			throw NotImplementedException("unsupported texture format.");
		}

		auto hw = rendererResource->hardwareRenderer.Ptr();

		GameEngine::Texture2D* rs;
		if (format == StorageFormat::BC1 || format == StorageFormat::BC5 || format == StorageFormat::BC3)
		{
			Array<void*, 32> mipData;
			for(int level = 0; level < data.GetMipLevels(); level++)
				mipData.Add(data.GetData(level).Buffer());
			rs = hw->CreateTexture2D(TextureUsage::Sampled, data.GetWidth(), data.GetHeight(), data.GetMipLevels(), format, dataType, mipData.GetArrayView());
		}
		else
			rs = hw->CreateTexture2D(data.GetWidth(), data.GetHeight(), format, dataType, textureData);
		textures[name] = rs;
		return rs;
	}
	Texture2D * SceneResource::LoadTexture(const String & filename)
	{
		RefPtr<Texture2D> value;
		if (textures.TryGetValue(filename, value))
			return value.Ptr();

		auto actualFilename = Engine::Instance()->FindFile(Path::ReplaceExt(filename, "texture"), ResourceType::Texture);
		if (!actualFilename.Length())
			actualFilename = Engine::Instance()->FindFile(filename, ResourceType::Texture);
		if (actualFilename.Length())
		{
			if (actualFilename.ToLower().EndsWith(".texture"))
			{
				CoreLib::Graphics::TextureFile file(actualFilename);
				return LoadTexture2D(filename, file);
			}
			else
			{
				CoreLib::Imaging::Bitmap bmp(actualFilename);
				List<unsigned int> pixelsInversed;
				int * sourcePixels = (int*)bmp.GetPixels();
				pixelsInversed.SetSize(bmp.GetWidth() * bmp.GetHeight());
				for (int i = 0; i < bmp.GetHeight(); i++)
				{
					for (int j = 0; j < bmp.GetWidth(); j++)
						pixelsInversed[i*bmp.GetWidth() + j] = sourcePixels[(bmp.GetHeight() - 1 - i)*bmp.GetWidth() + j];
				}
				CoreLib::Graphics::TextureFile texFile;
				TextureCompressor::CompressRGBA_BC1(texFile, MakeArrayView((unsigned char*)pixelsInversed.Buffer(), pixelsInversed.Count() * 4), bmp.GetWidth(), bmp.GetHeight());
				texFile.SaveToFile(Path::ReplaceExt(actualFilename, "texture"));
				return LoadTexture2D(filename, texFile);
			}
		}
		else
		{
			Print("cannot load texture '%S'\n", filename.ToWString());
			CoreLib::Graphics::TextureFile errTex;
			unsigned char errorTexContent[] =
			{
				255,0,255,255,   0,0,0,255,
				0,0,0,255,       255,0,255,255
			};
			errTex.SetData(CoreLib::Graphics::TextureStorageFormat::RGBA8, 2, 2, 0, ArrayView<unsigned char>(errorTexContent, 16));
			return LoadTexture2D("ERROR_TEXTURE", errTex);
		}
	}

	void SceneResource::CreateMaterialModuleInstance(ModuleInstance & result, Material* material, const char * moduleName, bool isPatternModule)
	{
		bool isValid = true;
		auto module = spEnvFindModule(spireEnv, moduleName);
		if (!module)
		{
			Print("Invalid material(%S): shader '%S' does not define '%s'.", material->Name.ToWString(), material->ShaderFile.ToWString(), moduleName);
		}
		else
		{
			int paramCount = spModuleGetParameterCount(module);
			EnumerableDictionary<String, int> bindingLocs;
			EnumerableDictionary<String, DynamicVariable*> vars;
			int bufferSize = spModuleGetParameterBufferSize(module);
			int loc = bufferSize == 0 ? 0 : 1;
			for (int i = 0; i < paramCount; i++)
			{
				SpireComponentInfo param;
				spModuleGetParameter(module, i, &param);
				if (param.BindableResourceType != SPIRE_NON_BINDABLE)
				{
					bindingLocs[param.Name] = loc++;
				}
				else
				{
					if (auto val = material->Variables.TryGetValue(param.Name))
						vars[param.Name] = val;
					else
					{
						Print("Invalid material(%S): shader parameter '%S' is not provided in material file.\n", material->Name.ToWString(), String(param.Name).ToWString());
						isValid = false;
					}
				}
			}
			if (isValid)
			{
				CreateModuleInstance(result, module, &instanceUniformMemory);
				if (result)
				{
					// update all versions of descriptor set with material texture binding
					for (int i = 0; i < DynamicBufferLengthMultiplier; i++)
					{
						auto descSet = result.GetDescriptorSet(i);
						descSet->BeginUpdate();
						for (auto binding : bindingLocs)
						{
							DynamicVariable val;
							if (material->Variables.TryGetValue(binding.Key, val))
							{
								auto tex = LoadTexture(val.StringValue);
								if (tex)
									descSet->Update(binding.Value, tex, TextureAspect::Color);
							}
							else
							{
								Print("Invalid material(%S): shader parameter '%S' is not provided in material file.\n", material->Name.ToWString(), binding.Key.ToWString());
								descSet->Update(binding.Value, LoadTexture("error.texture"), TextureAspect::Color);
							}
						}
						descSet->EndUpdate();
					}
				}
				if (isPatternModule)
					for (auto& v : vars)
						material->PatternVariables.Add(v.Value);
				else
					for (auto& v : vars)
						material->GeometryVariables.Add(v.Value);
			}
		}
	}

	void SceneResource::RegisterMaterial(Material * material)
	{
		if (!material->MaterialPatternModule)
		{
			auto shaderFile = Engine::Instance()->FindFile(material->ShaderFile, ResourceType::Shader);
			auto patternShaderName = Path::GetFileNameWithoutEXT(shaderFile) + "Pattern";
			auto geomShaderName = Path::GetFileNameWithoutEXT(shaderFile) + "Geometry";
			auto patternModule = spEnvFindModule(spireEnv, patternShaderName.Buffer());
			
			if (!patternModule && shaderFile.Length())
			{
				SpireDiagnosticSink * spireSink = spCreateDiagnosticSink(spireContext);
				spEnvLoadModuleLibrary(spireEnv, shaderFile.Buffer(), spireSink);
				if (spDiagnosticSinkHasAnyErrors(spireSink))
					Print("Invalid material(%S): cannot compile shader '%S'. Output message:\n%S", material->Name.ToWString(), shaderFile.ToWString(), GetSpireOutput(spireSink).ToWString());
				spDestroyDiagnosticSink(spireSink);
			}
			CreateMaterialModuleInstance(material->MaterialPatternModule, material, patternShaderName.Buffer(), true);
			auto geometryModuleName = Path::GetFileNameWithoutEXT(shaderFile) + "Geometry";
			if (spEnvFindModule(spireEnv, geometryModuleName.Buffer()))
				CreateMaterialModuleInstance(material->MaterialGeometryModule, material, geometryModuleName.Buffer(), false);
		}
		// use default material if failed to load
		if (!material->MaterialPatternModule)
		{
			CreateMaterialModuleInstance(material->MaterialPatternModule, material, "DefaultPattern", true);
			if (!material->MaterialPatternModule)
				throw InvalidOperationException("failed to load default material.");
		}
		// use default geometry if failed to load
		if (!material->MaterialGeometryModule)
		{
			CreateMaterialModuleInstance(material->MaterialGeometryModule, material, "DefaultGeometry", false);
			if (!material->MaterialGeometryModule)
				throw InvalidOperationException("failed to load default material.");
		}
		material->IsDoubleSided = spModuleHasAttrib(material->MaterialPatternModule.GetModule(), "DoubleSided") != 0;
		material->IsTransparent = spModuleHasAttrib(material->MaterialPatternModule.GetModule(), "Transparent") != 0;

	}
	
	SceneResource::SceneResource(RendererSharedResource * resource)
		: rendererResource(resource)
	{
		spireContext = resource->spireContext;
		sharedSpireEnv = resource->sharedSpireEnvironment;
		auto hwRenderer = resource->hardwareRenderer.Ptr();
		hardwareRenderer = hwRenderer;
		instanceUniformMemory.Init(hwRenderer, BufferUsage::UniformBuffer, false, 24, hwRenderer->UniformBufferAlignment());
		transformMemory.Init(hwRenderer, BufferUsage::UniformBuffer, false, 25, hwRenderer->UniformBufferAlignment());
		spireEnv = nullptr;
		Clear();
	}
	
	void SceneResource::Clear()
	{
		Destroy();
		meshes = CoreLib::EnumerableDictionary<CoreLib::String, RefPtr<DrawableMesh>>();
		textures = EnumerableDictionary<String, RefPtr<Texture2D>>();
		if (spireEnv)
			spReleaseEnvironment(spireEnv);
		spireEnv = spCreateEnvironment(spireContext, sharedSpireEnv);
		rendererResource->pipelineManager.SetSpireEnvironment(spireEnv);
	}

	// Converts StorageFormat to DataType
	DataType GetStorageDataType(StorageFormat format)
	{
		DataType dataType;
		switch (format)
		{
		case StorageFormat::RGBA_8:
			dataType = DataType::Byte4;
			break;
		case StorageFormat::Depth24Stencil8:
			dataType = DataType::Float;
			break;
		case StorageFormat::Depth32:
			dataType = DataType::UInt;
			break;
		case StorageFormat::RGBA_F16:
			dataType = DataType::Float4;
			break;
		case StorageFormat::RGB10_A2:
			dataType = DataType::Float4;
			break;
		case StorageFormat::R11F_G11F_B10F:
			dataType = DataType::Float3;
			break;
		default:
			throw HardwareRendererException("Unsupported storage format as render target.");
		}
		return dataType;
	}

	void ShadowMapResource::Init(HardwareRenderer * hwRenderer)
	{
		auto & graphicsSettings = Engine::Instance()->GetGraphicsSettings();

		shadowMapArray = hwRenderer->CreateTexture2DArray(TextureUsage::SampledDepthAttachment, graphicsSettings.ShadowMapResolution, graphicsSettings.ShadowMapResolution,
			graphicsSettings.ShadowMapArraySize, 1, StorageFormat::Depth32);
		shadowMapArrayFreeBits.SetMax(graphicsSettings.ShadowMapArraySize);
		shadowMapArrayFreeBits.Clear();
		shadowMapArraySize = graphicsSettings.ShadowMapArraySize;

		shadowMapRenderTargetLayout = hwRenderer->CreateRenderTargetLayout(MakeArrayView(AttachmentLayout(TextureUsage::SampledDepthAttachment, StorageFormat::Depth32)));
		shadowMapRenderOutputs.SetSize(shadowMapArraySize);

		shadowView = new ViewResource(hwRenderer);

		for (int i = 0; i < shadowMapArraySize; i++)
		{
			RenderAttachments attachment;
			attachment.SetAttachment(0, shadowMapArray.Ptr(), i);
			shadowMapRenderOutputs[i] = shadowView->CreateRenderOutput(shadowMapRenderTargetLayout.Ptr(),
				new RenderTarget(StorageFormat::Depth32, shadowMapArray, i, graphicsSettings.ShadowMapResolution, graphicsSettings.ShadowMapResolution));
		}
	}

	void ShadowMapResource::Destroy()
	{
		shadowMapRenderTargetLayout = nullptr;
		for (auto & x : shadowMapRenderOutputs)
			x = nullptr;
		shadowMapRenderOutputs.Clear();
		shadowMapArray = nullptr;
		shadowView = nullptr;
	}

	void ShadowMapResource::Reset()
	{
		shadowMapArrayFreeBits.Clear();
	}

	int ShadowMapResource::AllocShadowMaps(int count)
	{
		for (int i = 0; i <= shadowMapArraySize - count; i++)
		{
			if (!shadowMapArrayFreeBits.Contains(i))
			{
				bool occupied = false;
				for (int j = i + 1; j < i + count; j++)
				{
					if (shadowMapArrayFreeBits.Contains(j))
					{
						occupied = true;
						break;
					}
				}
				if (occupied)
					i += count - 1;
				else
				{
					for (int j = i; j < i + count; j++)
					{
						shadowMapArrayFreeBits.Add(j);
					}
					return i;
				}
			}
		}
		return -1;
	}
	void ShadowMapResource::FreeShadowMaps(int id, int count)
	{
		assert(id + count <= shadowMapArraySize);
		for (int i = id; i < id + count; i++)
		{
			assert(shadowMapArrayFreeBits.Contains(i));
			shadowMapArrayFreeBits.Remove(i);
		}
	}

	SpireShader * RendererSharedResource::LoadSpireShader(const char * key, const char * source)
	{
		SpireShader * rs = nullptr;
		if (entryPointShaders.TryGetValue(key, rs))
			return rs;
		rs = spEnvCreateShaderFromSource(sharedSpireEnvironment, source, spireSink);
		entryPointShaders[key] = rs;
		return rs;
	}

	void RendererResource::CreateModuleInstance(ModuleInstance & rs, SpireModule * shaderModule, DeviceMemory * uniformMemory, int uniformBufferSize)
	{
		rs.Init(spireContext, shaderModule);

		rs.BindingName = spGetModuleName(shaderModule);
		rs.BufferLength = Math::Max(spModuleGetParameterBufferSize(shaderModule), uniformBufferSize);
		if (rs.BufferLength > 0)
		{
			rs.BufferLength = Math::RoundUpToAlignment(rs.BufferLength, hardwareRenderer->UniformBufferAlignment());;
			auto ptr = (unsigned char *)uniformMemory->Alloc(rs.BufferLength * DynamicBufferLengthMultiplier);
			rs.UniformMemory = uniformMemory;
			rs.BufferOffset = (int)(ptr - (unsigned char*)uniformMemory->BufferPtr());
			assert(rs.BufferOffset%hardwareRenderer->UniformBufferAlignment() == 0);
		}
		else
			rs.UniformMemory = nullptr;
		
		RefPtr<DescriptorSetLayout> layout;
		if (!descLayouts.TryGetValue(spGetModuleUID(shaderModule), layout))
		{
			int paramCount = spModuleGetParameterCount(shaderModule);
			List<DescriptorLayout> descs;
			if (rs.UniformMemory)
				descs.Add(DescriptorLayout(sfGraphics, 0, BindingType::UniformBuffer));
			for (int i = 0; i < paramCount; i++)
			{
				SpireComponentInfo info;
				spModuleGetParameter(shaderModule, i, &info);
				if (info.BindableResourceType != SPIRE_NON_BINDABLE)
				{
					DescriptorLayout descLayout;
					descLayout.Location = descs.Count();
					switch (info.BindableResourceType)
					{
					case SPIRE_TEXTURE:
						descLayout.Type = BindingType::Texture;
						break;
					case SPIRE_UNIFORM_BUFFER:
						descLayout.Type = BindingType::UniformBuffer;
						break;
					case SPIRE_SAMPLER:
						descLayout.Type = BindingType::Sampler;
						break;
					case SPIRE_STORAGE_BUFFER:
						descLayout.Type = BindingType::StorageBuffer;
						break;
					}
					descs.Add(descLayout);
				}
				if (info.Specialize)
				{
					rs.SpecializeParamOffsets.Add(info.Offset);
				}
			}
			layout = hardwareRenderer->CreateDescriptorSetLayout(descs.GetArrayView());
			descLayouts[spGetModuleUID(shaderModule)] = layout;
		}
		rs.SetDescriptorSetLayout(hardwareRenderer.Ptr(), layout.Ptr());
		if (rs.UniformMemory)
		{
			for (int i = 0; i < DynamicBufferLengthMultiplier; i++)
			{
				auto descSet = rs.GetDescriptorSet(i);
				descSet->BeginUpdate();
				descSet->Update(0, rs.UniformMemory->GetBuffer(), rs.BufferOffset + rs.BufferLength * i, rs.BufferLength);
				descSet->EndUpdate();
			}
		}
	}

	void RendererResource::Destroy()
	{
		descLayouts = CoreLib::EnumerableDictionary<unsigned int, CoreLib::RefPtr<GameEngine::DescriptorSetLayout>>();
	}

	void RendererSharedResource::LoadShaderLibrary()
	{
		spAddSearchPath(spireContext, Engine::Instance()->GetDirectory(true, ResourceType::Shader).Buffer());
		spAddSearchPath(spireContext, Engine::Instance()->GetDirectory(false, ResourceType::Shader).Buffer());

		auto pipelineDefFile = Engine::Instance()->FindFile("GlPipeline.shader", ResourceType::Shader);
		if (!pipelineDefFile.Length())
			throw InvalidOperationException("'Pipeline.shader' not found. Engine directory is not setup correctly.");
		auto engineShaderDir = CoreLib::IO::Path::GetDirectoryName(pipelineDefFile);
		auto utilDefFile = Engine::Instance()->FindFile("Utils.shader", ResourceType::Shader);
		if (!utilDefFile.Length())
			throw InvalidOperationException("'Utils.shader' not found. Engine directory is not setup correctly.");
		spEnvLoadModuleLibrary(sharedSpireEnvironment, pipelineDefFile.Buffer(), spireSink);
		spEnvLoadModuleLibrary(sharedSpireEnvironment, utilDefFile.Buffer(), spireSink);

		auto defShaderFile = Engine::Instance()->FindFile("Default.shader", ResourceType::Shader);
		if (!defShaderFile.Length())
			throw InvalidOperationException("'Default.shader' not found. Engine directory is not setup correctly.");
		spEnvLoadModuleLibrary(sharedSpireEnvironment, defShaderFile.Buffer(), spireSink);
		if (spDiagnosticSinkHasAnyErrors(spireSink))
		{
			Diagnostics::Debug::WriteLine(GetSpireOutput(spireSink));
			throw HardwareRendererException("shader compilation error.");
		}
	}

	void RendererSharedResource::Init(HardwareRenderer * phwRenderer)
	{
		hardwareRenderer = phwRenderer;
		// Vertex buffer for VS bypass
		const float fsTri[] =
		{
			-1.0f, -1.0f, 0.0f, 0.0f,
			1.0f, -1.0f, 1.0f, 0.0f,
			1.0f, 1.0f, 1.0f, 1.0f,
			-1.0f, 1.0f, 0.0f, 1.0f
		};
		fullScreenQuadVertBuffer = hardwareRenderer->CreateBuffer(BufferUsage::ArrayBuffer, sizeof(fsTri));
		fullScreenQuadVertBuffer->SetData((void*)&fsTri[0], sizeof(fsTri));

		// Create common texture samplers
		nearestSampler = hardwareRenderer->CreateTextureSampler();
		nearestSampler->SetFilter(TextureFilter::Nearest);

		linearSampler = hardwareRenderer->CreateTextureSampler();
		linearSampler->SetFilter(TextureFilter::Linear);

		linearClampedSampler = hardwareRenderer->CreateTextureSampler();
		linearClampedSampler->SetFilter(TextureFilter::Linear);
		linearClampedSampler->SetWrapMode(WrapMode::Clamp);

		envMapSampler = hardwareRenderer->CreateTextureSampler();
		envMapSampler->SetFilter(TextureFilter::Trilinear);

		textureSampler = hardwareRenderer->CreateTextureSampler();
		textureSampler->SetFilter(TextureFilter::Anisotropic16x);

		shadowSampler = hardwareRenderer->CreateTextureSampler();
		shadowSampler->SetFilter(TextureFilter::Linear);
		shadowSampler->SetDepthCompare(CompareFunc::LessEqual);

		shadowMapResources.Init(hardwareRenderer.Ptr());

		spireContext = spCreateCompilationContext("");
		sharedSpireEnvironment = spGetCurrentEnvironment(spireContext);
		LoadShaderLibrary();
		
		pipelineManager.Init(spireContext, sharedSpireEnvironment, hardwareRenderer.Ptr(), &renderStats);

		indexBufferMemory.Init(hardwareRenderer.Ptr(), BufferUsage::IndexBuffer, false, 26, 256);
		vertexBufferMemory.Init(hardwareRenderer.Ptr(), BufferUsage::ArrayBuffer, false, 28, 256);

		spireSink = spCreateDiagnosticSink(spireContext);
		envMapArray = hardwareRenderer->CreateTextureCubeArray(TextureUsage::SampledColorAttachment, EnvMapSize, Math::Log2Floor(EnvMapSize) + 1, MaxEnvMapCount, StorageFormat::RGBA_F16);
	
		// create default color lookup texture
		defaultColorLookupTexture = hardwareRenderer->CreateTexture3D(TextureUsage::Sampled, 16, 16, 16, 1, StorageFormat::RGBA_8);
		struct color { unsigned char r, g, b, a; };
		List<color> buffer;
		buffer.SetSize(16 * 16 * 16);
		for (int i = 0; i<16; i++)
			for (int j = 0; j<16; j++)
				for (int k = 0; k < 16; k++)
				{
					color c;
					c.a = 255;
					c.r = (unsigned char)(k * 255 / 15);
					c.g = (unsigned char)(j * 255 / 15);
					c.b = (unsigned char)(i * 255 / 15);
					buffer[i * 16 * 16 + j * 16 + k] = c;
				}
		defaultColorLookupTexture->SetData(0, 0, 0, 0, 16, 16, 16, DataType::Byte4, buffer.Buffer());
	}
	void RendererSharedResource::Destroy()
	{
		RendererResource::Destroy();
		spDestroyDiagnosticSink(spireSink);
		shadowMapResources.Destroy();
		textureSampler = nullptr;
		nearestSampler = nullptr;
		linearSampler = nullptr;
		envMapSampler = nullptr;
		fullScreenQuadVertBuffer = nullptr;
		envMapArray = nullptr;
		//ModuleInstance::ClosePool();
		spReleaseEnvironment(sharedSpireEnvironment);
		spDestroyCompilationContext(spireContext);

	}
	
	RenderTarget::RenderTarget(GameEngine::StorageFormat format, CoreLib::RefPtr<Texture2DArray> texArray, int layer, int w, int h)
	{
		TextureArray = texArray;
		Format = format;
		UseFixedResolution = true;
		Layer = layer;
		ResolutionScale = 0.0f;
		FixedWidth = Width = w;
		FixedHeight = Height = h;
	}
	Buffer * DrawableMesh::GetVertexBuffer()
	{
		return renderRes->vertexBufferMemory.GetBuffer();
	}
	Buffer * DrawableMesh::GetIndexBuffer()
	{
		return renderRes->indexBufferMemory.GetBuffer();
	}
	DrawableMesh::~DrawableMesh()
	{
		if (vertexCount)
			renderRes->vertexBufferMemory.Free((char*)renderRes->vertexBufferMemory.BufferPtr() + vertexBufferOffset, vertexCount * vertexFormat.Size());
		if (indexCount)
			renderRes->indexBufferMemory.Free((char*)renderRes->indexBufferMemory.BufferPtr() + indexBufferOffset, indexCount * sizeof(int));
	}
	
	inline void BindDescSet(DescriptorSet** curStates, CommandBuffer* cmdBuf, int id, DescriptorSet * descSet)
	{
		if (curStates[id] != descSet)
		{
			curStates[id] = descSet;
			cmdBuf->BindDescriptorSet(id, descSet);
		}
	}

	void WorldPassRenderTask::SetFixedOrderDrawContent(PipelineContext & pipelineManager, CoreLib::ArrayView<Drawable*> drawables)
	{
		// Note: Intel's vulkan driver seem to have a limit on the size of a secondary command buffer
		// to play safe, we create multiple secondary command buffers, each holds 128 draw calls.
		commandBuffers.Clear();
		apiCommandBuffers.Clear();
		renderOutput->GetSize(viewport.Width, viewport.Height);
		auto cmd0 = pass->AllocCommandBuffer();
		commandBuffers.Add(cmd0);
		auto cmdBuf = cmd0->BeginRecording(renderOutput->GetFrameBuffer());
		apiCommandBuffers.Add(cmdBuf);
		cmdBuf->SetViewport(viewport.X, viewport.Y, viewport.Width, viewport.Height);
		if (clearOutput)
			cmdBuf->ClearAttachments(renderOutput->GetFrameBuffer());
		DescriptorSetBindingArray bindings;
		pipelineManager.GetBindings(bindings);
		for (int i = 0; i < bindings.Count(); i++)
			cmdBuf->BindDescriptorSet(i, bindings[i]);
		PipelineClass * lastPipeline = nullptr;
		numDrawCalls = 0;
		numMaterials = 0;
		numShaders = 0;
		Array<DescriptorSet*, 32> boundSets;
		boundSets.SetSize(boundSets.GetCapacity());
		for (auto & descSet : boundSets)
			descSet = (DescriptorSet*)-1;
		DrawableMesh * lastMesh = nullptr;
		if (drawables.Count())
		{
			Material* lastMaterial = drawables[0]->GetMaterial();
			pipelineManager.SetCullMode(lastMaterial->IsDoubleSided ? CullMode::Disabled : CullMode::CullBackFace);

			cmdBuf->BindIndexBuffer(drawables[0]->GetMesh()->GetIndexBuffer(), 0);
			pipelineManager.PushModuleInstance(&lastMaterial->MaterialGeometryModule);
			pipelineManager.PushModuleInstance(&lastMaterial->MaterialPatternModule);
			numMaterials++;
			BindDescSet(boundSets.Buffer(), cmdBuf, bindings.Count(), lastMaterial->MaterialGeometryModule.GetCurrentDescriptorSet());
			BindDescSet(boundSets.Buffer(), cmdBuf, bindings.Count() + 1, lastMaterial->MaterialPatternModule.GetCurrentDescriptorSet());
			for (auto obj : drawables)
			{
				numDrawCalls++;
				if ((numDrawCalls & 127) == 0)
				{
					cmdBuf->EndRecording();
					auto cmd = pass->AllocCommandBuffer();
					commandBuffers.Add(cmd);
					cmdBuf = cmd->BeginRecording(renderOutput->GetFrameBuffer());
					cmdBuf->SetViewport(viewport.X, viewport.Y, viewport.Width, viewport.Height);
					apiCommandBuffers.Add(cmdBuf);
					for (int i = 0; i < boundSets.Count(); i++)
						boundSets[i] = (DescriptorSet*)-1;
					for (int i = 0; i < bindings.Count(); i++)
						cmdBuf->BindDescriptorSet(i, bindings[i]);
					cmdBuf->BindIndexBuffer(obj->GetMesh()->GetIndexBuffer(), 0);
					BindDescSet(boundSets.Buffer(), cmdBuf, bindings.Count(), lastMaterial->MaterialGeometryModule.GetCurrentDescriptorSet());
					BindDescSet(boundSets.Buffer(), cmdBuf, bindings.Count() + 1, lastMaterial->MaterialPatternModule.GetCurrentDescriptorSet());
					lastPipeline = nullptr;
					lastMesh = nullptr;
				}
				auto newMaterial = obj->GetMaterial();
				if (newMaterial != lastMaterial)
				{
					numMaterials++;
					pipelineManager.PopModuleInstance();
					pipelineManager.PopModuleInstance();
					pipelineManager.PushModuleInstance(&newMaterial->MaterialGeometryModule);
					pipelineManager.PushModuleInstance(&newMaterial->MaterialPatternModule);
					pipelineManager.SetCullMode(newMaterial->IsDoubleSided ? CullMode::Disabled : CullMode::CullBackFace);
				}
				pipelineManager.PushModuleInstanceNoShaderChange(obj->GetTransformModule());
				if (auto pipelineInst = obj->GetPipeline(renderPassId, pipelineManager))
				{
					if (pipelineInst != lastPipeline)
					{
						cmdBuf->BindPipeline(pipelineInst->pipeline.Ptr());
						lastPipeline = pipelineInst;
						numShaders++;
					}
					auto mesh = obj->GetMesh();
					if (newMaterial != lastMaterial)
					{
						BindDescSet(boundSets.Buffer(), cmdBuf, bindings.Count(), newMaterial->MaterialGeometryModule.GetCurrentDescriptorSet());
						BindDescSet(boundSets.Buffer(), cmdBuf, bindings.Count() + 1, newMaterial->MaterialPatternModule.GetCurrentDescriptorSet());
					}
					BindDescSet(boundSets.Buffer(), cmdBuf, bindings.Count() + 2, obj->GetTransformModule()->GetCurrentDescriptorSet());
					if (mesh != lastMesh)
					{
						cmdBuf->BindVertexBuffer(mesh->GetVertexBuffer(), mesh->vertexBufferOffset);
						lastMesh = mesh;
					}
					BindDescSet(boundSets.Buffer(), cmdBuf, bindings.Count() + 3, nullptr);

					auto range = obj->GetElementRange();
					cmdBuf->DrawIndexed(mesh->indexBufferOffset / sizeof(int) + range.StartIndex, range.Count);
				}
				else
					throw "error";
				lastMaterial = newMaterial;
				pipelineManager.PopModuleInstance();
			}
			pipelineManager.PopModuleInstance();
			pipelineManager.PopModuleInstance();
		}
		cmdBuf->EndRecording();
	}
	void WorldPassRenderTask::SetDrawContent(PipelineContext & pipelineManager, CoreLib::List<Drawable*>& reorderBuffer, CoreLib::ArrayView<Drawable*> drawables)
	{
		reorderBuffer.Clear();
		Material* lastMaterial = nullptr;

		if (drawables.Count())
		{
			lastMaterial = drawables[0]->GetMaterial();
			pipelineManager.PushModuleInstance(&lastMaterial->MaterialGeometryModule);
			pipelineManager.PushModuleInstance(&lastMaterial->MaterialPatternModule);
		}

		for (auto obj : drawables)
		{
			obj->ReorderKey = 0;
			auto newMaterial = obj->GetMaterial();
			if (newMaterial != lastMaterial)
			{
				pipelineManager.PopModuleInstance();
				pipelineManager.PopModuleInstance();
				pipelineManager.PushModuleInstance(&newMaterial->MaterialGeometryModule);
				pipelineManager.PushModuleInstance(&newMaterial->MaterialPatternModule);
				lastMaterial = newMaterial;
			}
			pipelineManager.PushModuleInstanceNoShaderChange(obj->GetTransformModule());
			obj->ReorderKey = (obj->GetPipeline(renderPassId, pipelineManager)->Id << 18) + newMaterial->Id;
			pipelineManager.PopModuleInstance();

			reorderBuffer.Add(obj);
		}
		if (drawables.Count())
		{
			pipelineManager.PopModuleInstance();
			pipelineManager.PopModuleInstance();
		}
		reorderBuffer.Sort([](Drawable* d1, Drawable* d2) {return d1->ReorderKey < d2->ReorderKey; });
		SetFixedOrderDrawContent(pipelineManager, reorderBuffer.GetArrayView());

	}
	void PostPassRenderTask::Execute(HardwareRenderer * /*hwRenderer*/, RenderStat & /*stats*/)
	{
		postPass->Execute(sharedModules);
	}
	void WorldPassRenderTask::Execute(HardwareRenderer * hwRenderer, RenderStat & stats)
	{
		stats.NumPasses++;
		stats.NumDrawCalls += numDrawCalls;
		stats.NumMaterials += numMaterials;
		stats.NumShaders += numShaders;
		
		hwRenderer->ExecuteRenderPass(renderOutput->GetFrameBuffer(), MakeArrayView<CommandBuffer*>(apiCommandBuffers.Buffer(), apiCommandBuffers.Count()), nullptr);
	}
	GeneralRenderTask::GeneralRenderTask(AsyncCommandBuffer * cmdBuffer)
	{
		commandBuffer = cmdBuffer;
	}
	void GeneralRenderTask::Execute(HardwareRenderer * hwRenderer, RenderStat & /*stats*/)
	{
		hwRenderer->ExecuteNonRenderCommandBuffers(MakeArrayView(commandBuffer->GetBuffer()));
	}
}


