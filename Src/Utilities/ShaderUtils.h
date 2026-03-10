#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <glslang/Include/glslang_c_interface.h>

namespace ShaderUtils
{
	// 파일에서 셰이더 소스 코드를 읽어옵니다.
	[[nodiscard]] std::string LoadShaderSource(std::string_view filepath);

	// Vulkan GLSL 셰이더를 SPIR-V로 컴파일합니다.
	[[nodiscard]] std::vector<uint32_t> CompileGlslToSpirv(
		glslang_stage_t stage, 
		std::string_view source, 
		std::string_view entryPoint = "main");
}
