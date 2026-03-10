#version 450

// Vulkan static mesh vertex shader
// Assimp로 읽은 정점 버퍼의 위치/노멀/UV를 입력으로 받아 카메라 행렬을 적용합니다.
// 이번 단계에서는 diffuse texture를 GPU sampled image로 바꿔 샘플링하므로
// Vulkan 셰이더는 UV와 COLOR attribute를 fragment shader로 넘겨줍니다.

// Vulkan 경로는 카메라 행렬을 uniform buffer로 받아 정점 위치를 ViewProjection으로 변환합니다.
layout(set = 0, binding = 0) uniform CameraConstants
{
	mat4 ViewProjection;
	vec4 CameraPosition;
} cameraConstants;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inColor;
layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out vec4 outColor;

void main()
{
	// Vulkan의 기본 좌표계 위에 카메라 ViewProjection 행렬을 곱해 씬 공간에서 화면 공간으로 변환합니다.
	gl_Position = cameraConstants.ViewProjection * vec4(inPosition, 1.0);
	outTexCoord = inTexCoord;
	outColor = inColor;
}
