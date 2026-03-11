#version 450

// Vulkan Triangle Fragment Shader
// 버텍스 셰이더에서 전달된 UV로 diffuse texture를 샘플링하고 vertex color와 곱해 출력합니다.

layout(set = 0, binding = 0) uniform CameraConstants
{
	mat4 viewProjection;
	vec4 cameraPosition;
	vec4 debugOptions;
} cameraConstants;

layout(set = 0, binding = 1) uniform sampler2D diffuseTexture;

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main()
{
	const vec4 sampledColor = texture(diffuseTexture, inTexCoord);
	if (sampledColor.a < 0.5)
	{
		discard;
	}

	if (cameraConstants.debugOptions.x > 0.5)
	{
		// Vulkan UV 디버그 경로도 alpha cutout을 먼저 유지해 카드형 메시가 사각형으로 가득 차 보이지 않게 합니다.
		// 또한 fract UV를 출력하고, 0..1 범위를 벗어난 UV는 파란색 채널로 강조해 반복/초과 UV를 쉽게 구분합니다.
		const vec2 wrappedUv = fract(inTexCoord);
		const bool isOutOfRange = inTexCoord.x < 0.0 || inTexCoord.x > 1.0 || inTexCoord.y < 0.0 || inTexCoord.y > 1.0;
		outColor = vec4(wrappedUv, isOutOfRange ? 1.0 : 0.0, 1.0);
		return;
	}

	outColor = sampledColor * inColor;
}
