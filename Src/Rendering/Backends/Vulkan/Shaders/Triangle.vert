#version 450

// Vulkan static mesh vertex shader
// Assimp로 읽은 정점 버퍼의 위치/노멀/UV를 입력으로 받아 카메라 행렬을 적용합니다.
// 이번 단계에서는 diffuse texture를 GPU sampled image로 바꿔 샘플링하므로
// Vulkan 셰이더는 UV와 COLOR attribute를 fragment shader로 넘겨줍니다.

// Vulkan 경로는 카메라 행렬을 uniform buffer로 받아 정점 위치를 ViewProjection으로 변환합니다.
layout(set = 0, binding = 0) uniform CameraConstants
{
	mat4 WorldViewProjection;
	mat4 ViewProjection;
	mat4 World;
	vec4 CameraPosition;
	vec4 BenchmarkParams; // x: instance count, y: local scale, z: fovY, w: aspect
	vec4 LightDirection; // xyz: normalized vector from surface to directional light, w: enabled
	vec4 LightColorIntensity; // rgb: light color, w: intensity
	vec4 AmbientSpecular; // x: ambient, y: specular strength, z: shininess, w: lighting enabled
} cameraConstants;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inColor;
layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec3 outWorldPosition;
layout(location = 3) out vec3 outNormalWorld;

float hash01(uint value)
{
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return float(value & 0x00ffffffu) / 16777215.0;
}

vec3 makeBenchmarkCenter(uint instanceId)
{
	const uint objectCount = max(uint(cameraConstants.BenchmarkParams.x), 1u);
	const float aspect = max(cameraConstants.BenchmarkParams.w, 0.1);
	const uint columnCount = max(uint(ceil(sqrt(float(objectCount) * aspect))), 1u);
	const uint rowCount = max((objectCount + columnCount - 1u) / columnCount, 1u);
	const uint row = instanceId / columnCount;
	const uint column = instanceId - row * columnCount;

	const float jitterX = mix(0.12, 0.88, hash01(instanceId * 1664525u + 1013904223u));
	const float jitterY = mix(0.12, 0.88, hash01(instanceId * 22695477u + 1u));
	const float normalizedX = ((float(column) + jitterX) / float(columnCount)) * 2.0 - 1.0;
	const float normalizedY = ((float(row) + jitterY) / float(rowCount)) * 2.0 - 1.0;

	const float nearDistance = 18.0;
	const float farDistance = 120.0;
	const float depthLerp = (float(instanceId % rowCount) + hash01(instanceId * 747796405u + 2891336453u)) / float(rowCount);
	const float depth = mix(nearDistance, farDistance, depthLerp);
	const float tanHalfFovY = tan(cameraConstants.BenchmarkParams.z * 0.5);
	const float viewX = normalizedX * tanHalfFovY * aspect * depth * 0.88;
	const float viewY = normalizedY * tanHalfFovY * depth * 0.82;
	return vec3(viewX, viewY, depth);
}

vec4 makeBenchmarkTint(uint instanceId)
{
	return vec4(
		mix(0.25, 1.0, hash01(instanceId * 9781u + 17u)),
		mix(0.25, 1.0, hash01(instanceId * 6271u + 93u)),
		mix(0.25, 1.0, hash01(instanceId * 3253u + 191u)),
		1.0);
}

void main()
{
	if (cameraConstants.BenchmarkParams.x > 0.5)
	{
		const vec3 center = makeBenchmarkCenter(gl_InstanceIndex);
		const vec3 localPosition = inPosition * cameraConstants.BenchmarkParams.y;
		gl_Position = cameraConstants.WorldViewProjection * vec4(center + localPosition, 1.0);
		outTexCoord = inTexCoord;
		outColor = inColor * makeBenchmarkTint(gl_InstanceIndex);
		outWorldPosition = center + localPosition;
		outNormalWorld = normalize(inNormal);
		return;
	}

	// Vulkan 경로는 엔진 오브젝트의 월드 변환까지 포함된 WorldViewProjection 행렬을 사용해
	// 로컬 메시 정점을 바로 화면 공간으로 변환합니다.
	const vec4 worldPosition = cameraConstants.World * vec4(inPosition, 1.0);
	gl_Position = cameraConstants.WorldViewProjection * vec4(inPosition, 1.0);
	outTexCoord = inTexCoord;
	outColor = inColor;
	outWorldPosition = worldPosition.xyz;
	outNormalWorld = normalize((cameraConstants.World * vec4(inNormal, 0.0)).xyz);
}
