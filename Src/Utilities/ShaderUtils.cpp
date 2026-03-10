#include "ShaderUtils.h"
#include <glslang/Public/resource_limits_c.h>
#include <cstdio>
#include <windows.h>

namespace ShaderUtils
{
	std::string LoadShaderSource(std::string_view filepath)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, filepath.data(), "rb") != 0 || !file)
		{
			char errorBuffer[512] = {};
			snprintf(errorBuffer, sizeof(errorBuffer), "[ShaderUtils][ERROR] Failed to open: %s\n", filepath.data());
			OutputDebugStringA(errorBuffer);
			return {};
		}

		fseek(file, 0, SEEK_END);
		const long fileSize = ftell(file);
		fseek(file, 0, SEEK_SET);

		std::string content(fileSize, '\0');
		fread(content.data(), 1, fileSize, file);
		fclose(file);

		return content;
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

		glslang_shader_t* shader = glslang_shader_create(&input);
		glslang_shader_set_entry_point(shader, entryPoint.data());

		if (!glslang_shader_preprocess(shader, &input) || !glslang_shader_parse(shader, &input))
		{
			const char* log = glslang_shader_get_info_log(shader);
			char errorBuffer[2048] = {};
			snprintf(errorBuffer, sizeof(errorBuffer), "[ShaderUtils][ERROR] Shader compilation failed:\n%s\n", log);
			OutputDebugStringA(errorBuffer);
			glslang_shader_delete(shader);
			return {};
		}

		glslang_program_t* program = glslang_program_create();
		glslang_program_add_shader(program, shader);
		
		if (!glslang_program_link(program, input.messages))
		{
			const char* log = glslang_program_get_info_log(program);
			char errorBuffer[2048] = {};
			snprintf(errorBuffer, sizeof(errorBuffer), "[ShaderUtils][ERROR] Shader linking failed:\n%s\n", log);
			OutputDebugStringA(errorBuffer);
			glslang_program_delete(program);
			glslang_shader_delete(shader);
			return {};
		}

		glslang_program_SPIRV_generate(program, stage);
		const size_t wordCount = glslang_program_SPIRV_get_size(program);
		std::vector<uint32_t> spirv(wordCount);
		glslang_program_SPIRV_get(program, spirv.data());

		const char* spvMessages = glslang_program_SPIRV_get_messages(program);
		if (spvMessages && spvMessages[0] != '\0')
		{
			OutputDebugStringA("[ShaderUtils][SPIR-V] ");
			OutputDebugStringA(spvMessages);
			OutputDebugStringA("\n");
		}

		glslang_program_delete(program);
		glslang_shader_delete(shader);
		
		return spirv;
	}
}
