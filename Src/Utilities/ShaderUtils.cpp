#include "ShaderUtils.h"
#include <glslang/Public/resource_limits_c.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <windows.h>

namespace ShaderUtils
{
	namespace
	{
		void OutputDebugLine(const std::string& message)
		{
			OutputDebugStringA(message.c_str());
			OutputDebugStringA("\n");
		}
	}

	std::string LoadShaderSource(std::string_view filepath)
	{
		std::ifstream file(std::filesystem::path(std::string(filepath)), std::ios::binary);
		if (!file)
		{
			OutputDebugLine(std::format("[ShaderUtils][ERROR] Failed to open: {}", filepath));
			return {};
		}

		return {
			std::istreambuf_iterator<char>(file),
			std::istreambuf_iterator<char>()
		};
	}

	std::vector<uint32_t> CompileGlslToSpirv(
		glslang_stage_t stage, 
		std::string_view source, 
		std::string_view entryPoint)
	{
		// glslang을 사용하여 GLSL 소스를 SPIR-V 바이너리로 컴파일합니다.
		glslang_input_t input = {
			.language = GLSLANG_SOURCE_GLSL,
			.stage = stage,
			.client = GLSLANG_CLIENT_VULKAN,
			.client_version = GLSLANG_TARGET_VULKAN_1_2,
			.target_language = GLSLANG_TARGET_SPV,
			.target_language_version = GLSLANG_TARGET_SPV_1_5,
			.code = source.data(),
			.default_version = 100,
			.default_profile = GLSLANG_NO_PROFILE,
			.force_default_version_and_profile = false,
			.forward_compatible = false,
			.messages = static_cast<glslang_messages_t>(
				GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT),
			.resource = glslang_default_resource()
		};

		std::unique_ptr<glslang_shader_t, decltype(&glslang_shader_delete)> shader(
			glslang_shader_create(&input),
			glslang_shader_delete);
		glslang_shader_set_entry_point(shader.get(), entryPoint.data());

		if (!glslang_shader_preprocess(shader.get(), &input) || !glslang_shader_parse(shader.get(), &input))
		{
			const char* log = glslang_shader_get_info_log(shader.get());
			OutputDebugLine(std::format("[ShaderUtils][ERROR] Shader compilation failed:\n{}", log ? log : "<no log>"));
			return {};
		}

		std::unique_ptr<glslang_program_t, decltype(&glslang_program_delete)> program(
			glslang_program_create(),
			glslang_program_delete);
		glslang_program_add_shader(program.get(), shader.get());
		
		if (!glslang_program_link(program.get(), input.messages))
		{
			const char* log = glslang_program_get_info_log(program.get());
			OutputDebugLine(std::format("[ShaderUtils][ERROR] Shader linking failed:\n{}", log ? log : "<no log>"));
			return {};
		}

		glslang_program_SPIRV_generate(program.get(), stage);
		const size_t wordCount = glslang_program_SPIRV_get_size(program.get());
		std::vector<uint32_t> spirv(wordCount);
		glslang_program_SPIRV_get(program.get(), spirv.data());

		const char* spvMessages = glslang_program_SPIRV_get_messages(program.get());
		if (spvMessages && spvMessages[0] != '\0')
		{
			OutputDebugStringA("[ShaderUtils][SPIR-V] ");
			OutputDebugStringA(spvMessages);
			OutputDebugStringA("\n");
		}
		
		return spirv;
	}
}
