#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

// Vulkan Win32 surface를 사용하기 위한 공통 플랫폼 헤더입니다.
#include <Windows.h>
#include <vulkan/vulkan.h>
