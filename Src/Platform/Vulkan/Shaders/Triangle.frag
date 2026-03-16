#version 450

// Vulkan Triangle Fragment Shader
// 버텍스 셰이더에서 전달된 UV로 diffuse texture를 샘플링하고 vertex color와 곱해 출력합니다.

layout(set = 0, binding = 1) uniform sampler2D diffuseTexture;

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main()
{
	const vec4 sampledColor = texture(diffuseTexture, inTexCoord);
	// Vulkan 경로도 diffuse texture의 투명 배경을 버려 거미 실루엣만 남깁니다.
	if (sampledColor.a < 0.1)
	{
		discard;
	}
	outColor = sampledColor * inColor;
}
