#version 450

// Vulkan Triangle Fragment Shader
// 버텍스 셰이더에서 전달된 UV로 diffuse texture를 샘플링하고 vertex color와 곱해 출력합니다.

layout(set = 0, binding = 0) uniform CameraConstants
{
	mat4 WorldViewProjection;
	mat4 ViewProjection;
	mat4 World;
	vec4 CameraPosition;
	vec4 BenchmarkParams;
	vec4 LightDirection;
	vec4 LightColorIntensity;
	vec4 AmbientSpecular;
} cameraConstants;

layout(set = 0, binding = 1) uniform sampler2D diffuseTexture;

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inWorldPosition;
layout(location = 3) in vec3 inNormalWorld;
layout(location = 0) out vec4 outColor;

vec4 applyPhongLighting(vec4 baseColor)
{
	if (cameraConstants.BenchmarkParams.x > 0.5 || cameraConstants.AmbientSpecular.w < 0.5)
	{
		return baseColor;
	}

	vec3 normal = inNormalWorld;
	if (dot(normal, normal) < 0.000001)
	{
		normal = vec3(0.0, 1.0, 0.0);
	}
	normal = normalize(normal);

	vec3 lightDirection = cameraConstants.LightDirection.xyz;
	if (dot(lightDirection, lightDirection) < 0.000001)
	{
		lightDirection = vec3(0.0, 1.0, 0.0);
	}
	lightDirection = normalize(lightDirection);

	vec3 viewDirection = cameraConstants.CameraPosition.xyz - inWorldPosition;
	if (dot(viewDirection, viewDirection) < 0.000001)
	{
		viewDirection = vec3(0.0, 0.0, 1.0);
	}
	viewDirection = normalize(viewDirection);

	const float ambient = clamp(cameraConstants.AmbientSpecular.x, 0.0, 1.0);
	const float diffuse = max(dot(normal, lightDirection), 0.0);
	const vec3 reflectedLight = reflect(-lightDirection, normal);
	const float specularPower = max(cameraConstants.AmbientSpecular.z, 1.0);
	const float specularStrength = max(cameraConstants.AmbientSpecular.y, 0.0);
	const float specular = pow(max(dot(viewDirection, reflectedLight), 0.0), specularPower) * specularStrength;
	const vec3 lightColor = max(cameraConstants.LightColorIntensity.rgb * cameraConstants.LightColorIntensity.w, vec3(0.0));
	const vec3 litColor = baseColor.rgb * (ambient + diffuse * lightColor) + specular * lightColor;
	return vec4(clamp(litColor, vec3(0.0), vec3(1.0)), baseColor.a);
}

void main()
{
	vec4 baseColor = inColor;
	if (inTexCoord.x >= 0.0)
	{
		const vec4 sampledColor = texture(diffuseTexture, inTexCoord);
		// Vulkan 경로도 diffuse texture의 투명 배경을 버려 거미 실루엣만 남깁니다.
		if (sampledColor.a < 0.1)
		{
			discard;
		}
		baseColor = sampledColor * inColor;
	}

	outColor = applyPhongLighting(baseColor);
}
