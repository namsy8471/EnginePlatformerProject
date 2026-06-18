#version 450

layout(push_constant) uniform SkyboxConstants
{
	vec4 CameraRightTanX;
	vec4 CameraUpTanY;
	vec4 CameraForwardEnabled;
	vec4 ZenithColorIntensity;
	vec4 HorizonColorBlend;
	vec4 GroundColorHorizon;
	vec4 SunDirectionSize;
	vec4 SunColorIntensity;
} skybox;

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

vec3 tonemapAces(vec3 color)
{
	color = max(color, vec3(0.0));
	return clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), vec3(0.0), vec3(1.0));
}

vec3 buildSkyDirection(vec2 texCoord)
{
	const vec2 ndc = vec2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
	return normalize(
		skybox.CameraForwardEnabled.xyz +
		skybox.CameraRightTanX.xyz * ndc.x * skybox.CameraRightTanX.w +
		skybox.CameraUpTanY.xyz * ndc.y * skybox.CameraUpTanY.w);
}

vec3 evaluateSky(vec2 texCoord)
{
	if (skybox.CameraForwardEnabled.w < 0.5)
	{
		return vec3(0.025, 0.027, 0.032);
	}

	const vec3 direction = buildSkyDirection(texCoord);
	const float horizonHeight = clamp(skybox.GroundColorHorizon.w, -0.95, 0.95);
	const float blend = max(skybox.HorizonColorBlend.w, 0.05);
	const float height = direction.y;
	const float topFactor = clamp((height - horizonHeight) / max(1.0 - horizonHeight, 0.001), 0.0, 1.0);
	const float bottomFactor = clamp((horizonHeight - height) / max(1.0 + horizonHeight, 0.001), 0.0, 1.0);
	vec3 color = mix(skybox.HorizonColorBlend.rgb, skybox.ZenithColorIntensity.rgb, pow(topFactor, blend));
	color = mix(color, skybox.GroundColorHorizon.rgb, pow(bottomFactor, blend));
	color *= skybox.ZenithColorIntensity.w;

	const vec3 sunDirection = normalize(skybox.SunDirectionSize.xyz);
	const float sunDot = clamp(dot(direction, sunDirection), 0.0, 1.0);
	const float sunSize = clamp(skybox.SunDirectionSize.w, 0.001, 0.35);
	const float sunCore = smoothstep(cos(sunSize * 1.45), cos(sunSize), sunDot);
	const float sunGlow = pow(sunDot, max(2.0, 0.18 / sunSize));
	color += skybox.SunColorIntensity.rgb * skybox.SunColorIntensity.w * (sunCore + sunGlow * 0.18);
	return color;
}

void main()
{
	vec3 color = evaluateSky(inTexCoord);
	color = tonemapAces(color);
	color = pow(color, vec3(1.0 / 2.2));
	outColor = vec4(color, 1.0);
}
