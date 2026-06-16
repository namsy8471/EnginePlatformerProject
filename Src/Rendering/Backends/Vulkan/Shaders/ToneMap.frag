#version 450

layout(set = 0, binding = 0) uniform PostProcessConstants
{
	vec4 CameraPosition;
	vec4 AmbientColorIntensity;
	vec4 ExposureDebug;
} postProcessConstants;

layout(set = 0, binding = 1) uniform sampler2D hdrColor;

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

vec3 tonemapAces(vec3 color)
{
	color = max(color, vec3(0.0));
	return clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), vec3(0.0), vec3(1.0));
}

void main()
{
	vec4 hdr = texture(hdrColor, inTexCoord);
	hdr.rgb = tonemapAces(hdr.rgb * max(postProcessConstants.ExposureDebug.x, 0.0));
	hdr.rgb = pow(hdr.rgb, vec3(1.0 / 2.2));
	outColor = hdr;
}
