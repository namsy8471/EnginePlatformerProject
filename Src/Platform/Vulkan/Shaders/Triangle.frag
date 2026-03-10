#version 450

// Vulkan Triangle Fragment Shader
// 버텍스 셰이더에서 전달된 UV로 diffuse texture를 샘플링하고 fallback color를 곱해 출력합니다.

layout(set = 0, binding = 1) uniform sampler2D diffuseTexture;

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main()
{
	const vec4 sampledColor = texture(diffuseTexture, inTexCoord);
	const float sampledLuminance = dot(sampledColor.rgb, vec3(0.2126, 0.7152, 0.0722));
	outColor = sampledLuminance < 0.001 ? inColor : sampledColor;
}
