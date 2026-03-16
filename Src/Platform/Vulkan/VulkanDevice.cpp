#include "VulkanDevice.h"

#include "VulkanCommandList.h"
#include "VulkanBuffer.h"
#include "VulkanResource.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace
{
	void LogVulkanMessage(const char* level, const char* message)
	{
		char buffer[512] = {};
		snprintf(buffer, sizeof(buffer), "[Vulkan][%s] %s\n", level, message);
		OutputDebugStringA(buffer);
	}

	void LogVulkanStageSuccess(const char* stage)
	{
		LogVulkanMessage("OK", stage);
	}

	void LogVulkanWarning(const char* message)
	{
		LogVulkanMessage("WARN", message);
	}

	void LogVulkanError(const char* message)
	{
		LogVulkanMessage("ERROR", message);
	}

	bool ContainsLayer(const std::vector<VkLayerProperties>& layers, const char* layerName)
	{
		for (const VkLayerProperties& layer : layers)
		{
			if (strcmp(layer.layerName, layerName) == 0)
			{
				return true;
			}
		}

		return false;
	}

	bool ContainsExtension(const std::vector<VkExtensionProperties>& extensions, const char* extensionName)
	{
		for (const VkExtensionProperties& extension : extensions)
		{
			if (strcmp(extension.extensionName, extensionName) == 0)
			{
				return true;
			}
		}

		return false;
	}
}

VulkanDevice::VulkanDevice(void* windowHandle, int width, int height)
	: m_windowHandle(windowHandle), m_width(width), m_height(height)
{
}

VulkanDevice::~VulkanDevice()
{
	Shutdown();
}

bool VulkanDevice::Init()
{
	try
	{
		// Vulkan 스왑체인 이미지가 아직 준비되기 전에도 RHI 호출이 안전하도록 임시 백버퍼 래퍼를 만듭니다.
		m_nullBackBufferResource = std::make_unique<VulkanNullResource>();
		LogVulkanStageSuccess("Null back buffer resource prepared.");

		// Vulkan 초기화의 기본 순서는 Instance -> Surface -> PhysicalDevice -> Device입니다.
		CreateInstance();
		LogVulkanStageSuccess("Instance created.");
		SetupDebugMessenger();
		LogVulkanStageSuccess("Debug messenger setup finished.");
		CreateSurface();
		LogVulkanStageSuccess("Presentation surface created.");
		PickPhysicalDevice();
		LogVulkanStageSuccess("Physical device selected.");
		CreateLogicalDevice();
		LogVulkanStageSuccess("Logical device created.");
		CreateSyncObjects();
		LogVulkanStageSuccess("Synchronization objects created.");

		// 디바이스 생성까지 끝나면 실제 화면 출력용 swapchain을 준비합니다.
		CreateSwapchain();
		LogVulkanStageSuccess("Swapchain created.");
		CreateSwapchainImageViews();
		LogVulkanStageSuccess("Swapchain image views created.");
		CreateSwapchainResources();
		LogVulkanStageSuccess("Swapchain wrapper resources created.");
		CreateRenderPass();
		LogVulkanStageSuccess("Render pass created.");
		CreateDepthResources();
		LogVulkanStageSuccess("Depth resources created.");
		CreateFramebuffers();
		LogVulkanStageSuccess("Framebuffers created.");
		CreateCommandPool();
		LogVulkanStageSuccess("Command pool created.");
		CreateCommandBuffer();
		LogVulkanStageSuccess("Command buffer allocated.");
		AcquireNextImage();
		LogVulkanStageSuccess("Initial swapchain image acquired.");
		LogVulkanStageSuccess("Vulkan device initialization completed.");
		return true;
	}
	catch (const std::exception& ex)
	{
		// 실패 원인을 디버그 출력에 바로 남겨야 긴 시스템 로그 속에서도 실제 문제 줄만 빠르게 찾을 수 있습니다.
		LogVulkanError(ex.what());
		Shutdown();
		return false;
	}
	catch (...)
	{
		// 어느 단계에서 실패하더라도 지금까지 만든 Vulkan 객체를 역순으로 정리합니다.
		LogVulkanError("Unknown exception during Vulkan initialization.");
		Shutdown();
		return false;
	}
}

void VulkanDevice::Shutdown()
{
	// 먼저 디바이스 전체가 idle 상태가 될 때까지 기다려 안전하게 자원을 정리합니다.
	if (m_device != VK_NULL_HANDLE)
	{
		vkDeviceWaitIdle(m_device);
	}

	// framebuffer는 swapchain image view와 depth view를 참조하므로 가장 먼저 정리합니다.
	DestroyFramebuffers();
	DestroyDepthResources();

	if (m_renderPass != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
	{
		vkDestroyRenderPass(m_device, m_renderPass, nullptr);
		m_renderPass = VK_NULL_HANDLE;
	}

	if (m_commandPool != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
	{
		vkDestroyCommandPool(m_device, m_commandPool, nullptr);
		m_commandPool = VK_NULL_HANDLE;
		m_commandBuffer = VK_NULL_HANDLE;
	}

	// swapchain 관련 객체들은 논리 디바이스보다 먼저 파괴해야 합니다.
	DestroySwapchain();

	if (m_imageAvailableSemaphore != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
	{
		vkDestroySemaphore(m_device, m_imageAvailableSemaphore, nullptr);
		m_imageAvailableSemaphore = VK_NULL_HANDLE;
	}

	if (m_renderFinishedSemaphore != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
	{
		vkDestroySemaphore(m_device, m_renderFinishedSemaphore, nullptr);
		m_renderFinishedSemaphore = VK_NULL_HANDLE;
	}

	if (m_inFlightFence != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
	{
		vkDestroyFence(m_device, m_inFlightFence, nullptr);
		m_inFlightFence = VK_NULL_HANDLE;
	}

	if (m_device != VK_NULL_HANDLE)
	{
		vkDestroyDevice(m_device, nullptr);
		m_device = VK_NULL_HANDLE;
	}

	if (m_surface != VK_NULL_HANDLE && m_instance != VK_NULL_HANDLE)
	{
		vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
		m_surface = VK_NULL_HANDLE;
	}

	DestroyDebugMessenger();

	if (m_instance != VK_NULL_HANDLE)
	{
		vkDestroyInstance(m_instance, nullptr);
		m_instance = VK_NULL_HANDLE;
	}

	m_graphicsQueue = VK_NULL_HANDLE;
	m_presentQueue = VK_NULL_HANDLE;
	m_physicalDevice = VK_NULL_HANDLE;
	m_graphicsQueueFamilyIndex = InvalidQueueFamilyIndex();
	m_presentQueueFamilyIndex = InvalidQueueFamilyIndex();
	m_currentBackBufferIndex = 0;
	m_hasAcquiredSwapchainImage = false;
	m_hasLoggedSubmitSuccess = false;
	m_hasLoggedPresentSuccess = false;
	m_nullBackBufferResource.reset();
}

void VulkanDevice::WaitForGPU()
{
	// 정식 프레임 동기화 경로에서는 제출 fence를 기다려 GPU 완료를 확인합니다.
	if (m_inFlightFence != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
	{
		vkWaitForFences(m_device, 1, &m_inFlightFence, VK_TRUE, UINT64_MAX);
	}
	else if (m_device != VK_NULL_HANDLE)
	{
		vkDeviceWaitIdle(m_device);
	}
}

void VulkanDevice::MoveToNextFrame()
{
	// vkDeviceWaitIdle은 graphics 큐와 present 큐 모두가 완전히 idle 상태가 될 때까지 기다립니다.
	// 이렇게 해야 presentation engine이 스왑체인 이미지를 반환한 뒤에 다음 acquire가 진행되어
	// 이미지 부족으로 인한 영구 블록을 방지할 수 있습니다.
	if (m_device != VK_NULL_HANDLE)
	{
		vkDeviceWaitIdle(m_device);
	}
	AcquireNextImage();
}

void VulkanDevice::Present()
{
	// swapchain이 없다면 프레젠트할 수 없습니다.
	if (m_swapchain == VK_NULL_HANDLE)
	{
		return;
	}

	// 현재 acquire된 swapchain 이미지를 present queue에 넘깁니다.
	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &m_renderFinishedSemaphore;
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &m_swapchain;
	presentInfo.pImageIndices = &m_currentBackBufferIndex;

	const VkResult presentResult = vkQueuePresentKHR(m_presentQueue, &presentInfo);
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
	{
		// API 전환 직후 present가 swapchain 재생성 경로로 빠지는지 확인할 수 있도록 결과를 남깁니다.
		LogVulkanWarning(presentResult == VK_ERROR_OUT_OF_DATE_KHR ?
			"Present returned VK_ERROR_OUT_OF_DATE_KHR. Recreating swapchain." :
			"Present returned VK_SUBOPTIMAL_KHR. Recreating swapchain.");
		Resize(m_width, m_height);
		return;
	}

	if (presentResult != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to present Vulkan swapchain image.");
	}

	if (!m_hasLoggedPresentSuccess)
	{
		LogVulkanStageSuccess("First Vulkan present completed.");
		m_hasLoggedPresentSuccess = true;
	}
}

void VulkanDevice::Resize(int width, int height)
{
	// 최소화 상태에서는 0 크기가 들어올 수 있으므로 실제 크기가 생길 때까지 재생성을 미룹니다.
	if (width <= 0 || height <= 0)
	{
		return;
	}

	m_width = width;
	m_height = height;

	if (m_device == VK_NULL_HANDLE || m_surface == VK_NULL_HANDLE)
	{
		return;
	}

	// swapchain을 참조하는 리소스를 모두 안전하게 재생성하기 위해 먼저 GPU 완료를 기다립니다.
	vkDeviceWaitIdle(m_device);

	// 이전 swapchain 이미지가 acquire된 채로 resize에 들어오면 해당 semaphore는 더 이상 wait로 소비되지 않습니다.
	// binary semaphore는 signal 상태에서 재사용할 수 없으므로 새 semaphore로 교체해 다음 acquire를 준비합니다.
	if (m_hasAcquiredSwapchainImage)
	{
		ResetImageAvailableSemaphore();
		m_hasAcquiredSwapchainImage = false;
	}

	DestroyFramebuffers();
	DestroyDepthResources();

	if (m_renderPass != VK_NULL_HANDLE)
	{
		vkDestroyRenderPass(m_device, m_renderPass, nullptr);
		m_renderPass = VK_NULL_HANDLE;
	}

	DestroySwapchain();

	CreateSwapchain();
	LogVulkanStageSuccess("Swapchain recreated after resize.");
	CreateSwapchainImageViews();
	LogVulkanStageSuccess("Swapchain image views recreated after resize.");
	CreateSwapchainResources();
	LogVulkanStageSuccess("Swapchain wrapper resources recreated after resize.");
	CreateRenderPass();
	LogVulkanStageSuccess("Render pass recreated after resize.");
	CreateDepthResources();
	LogVulkanStageSuccess("Depth resources recreated after resize.");
	CreateFramebuffers();
	LogVulkanStageSuccess("Framebuffers recreated after resize.");
	AcquireNextImage();
	LogVulkanStageSuccess("Swapchain image acquired after resize.");
}

ICommandList* VulkanDevice::CreateCommandList()
{
	// 현재 Vulkan 커맨드 버퍼에 기록하는 VulkanCommandList를 생성합니다.
	return new VulkanCommandList(this);
}

IBuffer* VulkanDevice::CreateBuffer(const BufferDesc& desc)
{
	// RHI의 BufferDesc를 기반으로 실제 VulkanBuffer를 생성합니다.
	return new VulkanBuffer(this, desc);
}

void VulkanDevice::ExecuteCommandList(ICommandList* cmdList)
{
	// 현재 프레임의 커맨드 버퍼를 그래픽 큐에 제출합니다.
	auto native = reinterpret_cast<VkCommandBuffer>(cmdList->GetNativeResource());
	if (native == VK_NULL_HANDLE)
	{
		throw std::runtime_error("Invalid Vulkan command buffer.");
	}

	// vkDeviceWaitIdle은 graphics 큐뿐 아니라 present 큐까지 모두 기다립니다.
	// fence만 쓰면 present 큐가 아직 이미지를 쥔 채로 다음 submit에 들어갈 수 있어
	// 스왑체인 이미지가 부족할 때 영구 블록이 발생할 수 있습니다.
	vkDeviceWaitIdle(m_device);

	// Vulkan spec: vkQueueSubmit에 전달하는 fence는 반드시 unsignaled 상태여야 합니다.
	// vkDeviceWaitIdle은 fence 상태를 바꾸지 않으므로 직접 reset 해야 합니다.
	// 이걸 빠뜨리면 signaled fence로 submit하게 되어 undefined behavior가 발생합니다.
	vkResetFences(m_device, 1, &m_inFlightFence);

	const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &m_imageAvailableSemaphore;
	submitInfo.pWaitDstStageMask = waitStages;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &native;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &m_renderFinishedSemaphore;

	if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFence) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to submit Vulkan command buffer.");
	}

	if (!m_hasLoggedSubmitSuccess)
	{
		// 초기화나 API 전환 직후 첫 submit이 실제로 graphics queue까지 도달했는지 확인하기 위한 1회성 로그입니다.
		LogVulkanStageSuccess("First Vulkan queue submit completed.");
		m_hasLoggedSubmitSuccess = true;
	}

	// acquire semaphore는 위 submit의 wait 단계에서 소비되므로, 다음 프레임에서 다시 acquire에 사용할 수 있습니다.
	m_hasAcquiredSwapchainImage = false;
}

IGpuResource* VulkanDevice::GetBackBufferResource()
{
	if (m_currentBackBufferIndex < m_swapchainResources.size())
	{
		return m_swapchainResources[m_currentBackBufferIndex].get();
	}

	return m_nullBackBufferResource.get();
}

void* VulkanDevice::GetCurrentBackBufferRTV()
{
	// Vulkan은 DX12식 RTV 핸들을 직접 사용하지 않습니다.
	// 현재 RHI는 DX12 스타일을 가정하므로 렌더패스/프레임버퍼 구현 전까지 nullptr를 반환합니다.
	return nullptr;
}

void* VulkanDevice::GetDepthStencilView()
{
	// Vulkan은 DX12식 DSV 핸들을 직접 사용하지 않으므로 현재 단계에서는 nullptr를 반환합니다.
	return nullptr;
}

void VulkanDevice::CreateInstance()
{
	// VkApplicationInfo는 애플리케이션 메타데이터를 드라이버에 전달합니다.
	VkApplicationInfo appInfo = {};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "EnginePlatformer";
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.pEngineName = "EnginePlatformer";
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = VK_API_VERSION_1_2;

	// Win32 창에 표시하려면 surface 관련 인스턴스 확장이 필요합니다.
	std::vector<const char*> instanceExtensions =
	{
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME
	};

	// Validation layer 이름 목록입니다.
	std::vector<const char*> validationLayers;
	std::vector<VkLayerProperties> availableLayers;
	std::vector<VkExtensionProperties> availableExtensions;

#if defined(_DEBUG) || defined(DEBUG)
	uint32_t layerCount = 0;
	vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
	availableLayers.resize(layerCount);
	vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

	uint32_t extensionCount = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
	availableExtensions.resize(extensionCount);
	vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, availableExtensions.data());

	// Debug build에서는 validation layer를 켜 보되,
	// 시스템에 layer가 없으면 Vulkan 초기화 자체를 막지 않고 경고만 출력합니다.
	const bool hasValidationLayer = ContainsLayer(availableLayers, "VK_LAYER_KHRONOS_validation");
	const bool hasDebugUtilsExtension = ContainsExtension(availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	if (hasValidationLayer)
	{
		validationLayers.push_back("VK_LAYER_KHRONOS_validation");
		LogVulkanStageSuccess("Validation layer enabled.");
	}
	else
	{
		LogVulkanWarning("VK_LAYER_KHRONOS_validation not found. The runtime must discover the layer manifest through the Vulkan SDK, registry, or VK_LAYER_PATH.");
	}

	if (hasDebugUtilsExtension)
	{
		instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		LogVulkanStageSuccess("Debug utils extension enabled.");
	}
	else
	{
		LogVulkanWarning("VK_EXT_debug_utils not found in instance extension list.");
	}
#endif

	VkInstanceCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
	createInfo.ppEnabledExtensionNames = instanceExtensions.data();
	createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
	createInfo.ppEnabledLayerNames = validationLayers.empty() ? nullptr : validationLayers.data();

	if (vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to create Vulkan instance.");
	}
}

void VulkanDevice::SetupDebugMessenger()
{
#if defined(_DEBUG) || defined(DEBUG)
	if (m_instance == VK_NULL_HANDLE)
	{
		return;
	}

	// vkCreateDebugUtilsMessengerEXT는 확장 함수이므로 인스턴스에서 직접 로드해야 합니다.
	auto createFunc = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
		vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
	if (!createFunc)
	{
		LogVulkanWarning("vkCreateDebugUtilsMessengerEXT is not available.");
		return;
	}

	// 메시지 심각도와 종류를 설정합니다.
	// VERBOSE는 너무 많으므로 WARNING + ERROR만 받습니다.
	VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {};
	messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	messengerInfo.messageSeverity =
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	messengerInfo.messageType =
		VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	messengerInfo.pfnUserCallback = DebugCallback;

	if (createFunc(m_instance, &messengerInfo, nullptr, &m_debugMessenger) != VK_SUCCESS)
	{
		LogVulkanWarning("Failed to create debug messenger.");
	}
#endif
}

void VulkanDevice::DestroyDebugMessenger()
{
#if defined(_DEBUG) || defined(DEBUG)
	if (m_debugMessenger == VK_NULL_HANDLE || m_instance == VK_NULL_HANDLE)
	{
		return;
	}

	// 확장 함수이므로 인스턴스에서 직접 로드합니다.
	auto destroyFunc = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
		vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
	if (destroyFunc)
	{
		destroyFunc(m_instance, m_debugMessenger, nullptr);
	}
	m_debugMessenger = VK_NULL_HANDLE;
#endif
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::DebugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageType,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData)
{
	// Vulkan validation 메시지를 Visual Studio 출력 창(Output)에 실시간으로 표시합니다.
	const char* severity = "[INFO]";
	if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		severity = "[ERROR]";
	else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
		severity = "[WARNING]";

	char buffer[4096];
	snprintf(buffer, sizeof(buffer), "[Vulkan Validation] %s %s\n", severity, pCallbackData->pMessage);
	OutputDebugStringA(buffer);

	return VK_FALSE;
}

void VulkanDevice::CreateSurface()
{
	// Win32 HWND와 Vulkan을 연결하는 surface를 만듭니다.
	VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {};
	surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	surfaceCreateInfo.hinstance = GetModuleHandle(nullptr);
	surfaceCreateInfo.hwnd = static_cast<HWND>(m_windowHandle);

	if (vkCreateWin32SurfaceKHR(m_instance, &surfaceCreateInfo, nullptr, &m_surface) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to create Vulkan Win32 surface.");
	}
}

void VulkanDevice::PickPhysicalDevice()
{
	// 시스템에 있는 물리 디바이스 개수를 먼저 조회합니다.
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
	if (deviceCount == 0)
	{
		throw std::runtime_error("Failed to find a Vulkan-capable GPU.");
	}

	// 조건을 만족하는 첫 번째 GPU를 선택합니다.
	std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
	vkEnumeratePhysicalDevices(m_instance, &deviceCount, physicalDevices.data());

	for (VkPhysicalDevice physicalDevice : physicalDevices)
	{
		uint32_t graphicsQueueFamily = InvalidQueueFamilyIndex();
		uint32_t presentQueueFamily = InvalidQueueFamilyIndex();

		if (TryFindQueueFamilies(physicalDevice, graphicsQueueFamily, presentQueueFamily) &&
			SupportsRequiredDeviceExtensions(physicalDevice))
		{
			m_physicalDevice = physicalDevice;
			m_graphicsQueueFamilyIndex = graphicsQueueFamily;
			m_presentQueueFamilyIndex = presentQueueFamily;
			return;
		}
	}

	throw std::runtime_error("Failed to find a suitable Vulkan physical device.");
}

void VulkanDevice::CreateLogicalDevice()
{
	// 논리 디바이스는 실제로 명령을 보내는 VkDevice 객체입니다.
	const float queuePriority = 1.0f;
	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	std::vector<uint32_t> uniqueQueueFamilies;

	uniqueQueueFamilies.push_back(m_graphicsQueueFamilyIndex);
	if (m_presentQueueFamilyIndex != m_graphicsQueueFamilyIndex)
	{
		uniqueQueueFamilies.push_back(m_presentQueueFamilyIndex);
	}

	for (uint32_t queueFamilyIndex : uniqueQueueFamilies)
	{
		VkDeviceQueueCreateInfo queueCreateInfo = {};
		queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;
		queueCreateInfos.push_back(queueCreateInfo);
	}

	// swapchain을 만들기 위해 VK_KHR_swapchain 디바이스 확장을 활성화합니다.
	const std::vector<const char*> deviceExtensions =
	{
		VK_KHR_SWAPCHAIN_EXTENSION_NAME
	};

	VkPhysicalDeviceFeatures deviceFeatures = {};

	VkDeviceCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos = queueCreateInfos.data();
	createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
	createInfo.ppEnabledExtensionNames = deviceExtensions.data();
	createInfo.pEnabledFeatures = &deviceFeatures;

	if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to create Vulkan logical device.");
	}

	// 논리 디바이스 생성 후 실제 큐 핸들을 받아옵니다.
	vkGetDeviceQueue(m_device, m_graphicsQueueFamilyIndex, 0, &m_graphicsQueue);
	vkGetDeviceQueue(m_device, m_presentQueueFamilyIndex, 0, &m_presentQueue);
}

void VulkanDevice::CreateSyncObjects()
{
	// 이미지 acquire / 렌더 완료 / GPU 완료를 분리해 동기화하기 위한 semaphore와 fence를 만듭니다.
	VkSemaphoreCreateInfo semaphoreCreateInfo = {};
	semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	if (vkCreateSemaphore(m_device, &semaphoreCreateInfo, nullptr, &m_imageAvailableSemaphore) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to create Vulkan image-available semaphore.");
	}

	if (vkCreateSemaphore(m_device, &semaphoreCreateInfo, nullptr, &m_renderFinishedSemaphore) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to create Vulkan render-finished semaphore.");
	}

	VkFenceCreateInfo fenceCreateInfo = {};
	fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	if (vkCreateFence(m_device, &fenceCreateInfo, nullptr, &m_inFlightFence) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to create Vulkan in-flight fence.");
	}
}

void VulkanDevice::CreateSwapchain()
{
	// surface가 지원하는 포맷, present 모드, 이미지 개수 범위를 먼저 조회합니다.
	const SwapchainSupportDetails supportDetails = QuerySwapchainSupport(m_physicalDevice);
	if (supportDetails.formats.empty() || supportDetails.presentModes.empty())
	{
		throw std::runtime_error("Failed to find valid Vulkan swapchain support details.");
	}

	const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(supportDetails.formats);
	const VkPresentModeKHR presentMode = ChoosePresentMode(supportDetails.presentModes);
	const VkExtent2D extent = ChooseSwapExtent(supportDetails.capabilities);

	uint32_t imageCount = supportDetails.capabilities.minImageCount + 1;
	if (supportDetails.capabilities.maxImageCount > 0 && imageCount > supportDetails.capabilities.maxImageCount)
	{
		imageCount = supportDetails.capabilities.maxImageCount;
	}

	VkSwapchainCreateInfoKHR createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = m_surface;
	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = surfaceFormat.format;
	createInfo.imageColorSpace = surfaceFormat.colorSpace;
	createInfo.imageExtent = extent;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	const uint32_t queueFamilyIndices[] =
	{
		m_graphicsQueueFamilyIndex,
		m_presentQueueFamilyIndex
	};

	if (m_graphicsQueueFamilyIndex != m_presentQueueFamilyIndex)
	{
		createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		createInfo.queueFamilyIndexCount = 2;
		createInfo.pQueueFamilyIndices = queueFamilyIndices;
	}
	else
	{
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}

	createInfo.preTransform = supportDetails.capabilities.currentTransform;
	createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	createInfo.presentMode = presentMode;
	createInfo.clipped = VK_TRUE;
	createInfo.oldSwapchain = VK_NULL_HANDLE;

	if (vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to create Vulkan swapchain.");
	}

	// 실제로 생성된 swapchain 이미지 핸들을 받아옵니다.
	vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr);
	m_swapchainImages.resize(imageCount);
	vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data());
	m_swapchainImageLayouts.assign(imageCount, VK_IMAGE_LAYOUT_UNDEFINED);

	m_swapchainImageFormat = surfaceFormat.format;
	m_swapchainExtent = extent;
}

void VulkanDevice::CreateSwapchainImageViews()
{
	// swapchain 이미지는 image view를 통해 렌더 타겟처럼 접근합니다.
	m_swapchainImageViews.clear();
	m_swapchainImageViews.reserve(m_swapchainImages.size());

	for (VkImage image : m_swapchainImages)
	{
		VkImageViewCreateInfo imageViewCreateInfo = {};
		imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		imageViewCreateInfo.image = image;
		imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		imageViewCreateInfo.format = m_swapchainImageFormat;
		imageViewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		imageViewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		imageViewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		imageViewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
		imageViewCreateInfo.subresourceRange.levelCount = 1;
		imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
		imageViewCreateInfo.subresourceRange.layerCount = 1;

		VkImageView imageView = VK_NULL_HANDLE;
		if (vkCreateImageView(m_device, &imageViewCreateInfo, nullptr, &imageView) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to create Vulkan swapchain image view.");
		}

		m_swapchainImageViews.push_back(imageView);
	}
}

void VulkanDevice::CreateSwapchainResources()
{
	// RHI의 GetBackBufferResource와 연결하기 위해 swapchain image/view 쌍을 래퍼 객체로 묶습니다.
	m_swapchainResources.clear();
	m_swapchainResources.reserve(m_swapchainImages.size());

	for (size_t i = 0; i < m_swapchainImages.size(); ++i)
	{
		m_swapchainResources.push_back(std::make_unique<VulkanImageResource>(m_swapchainImages[i], m_swapchainImageViews[i]));
	}
}

void VulkanDevice::AcquireNextImage()
{
	// 렌더링할 다음 swapchain 이미지를 acquire합니다.
	if (m_swapchain == VK_NULL_HANDLE || m_imageAvailableSemaphore == VK_NULL_HANDLE)
	{
		return;
	}

	// 같은 binary semaphore로 acquire를 한 번 더 호출하면 validation error가 발생합니다.
	// 아직 submit wait로 소비되지 않은 acquire가 남아 있다면 현재 이미지를 계속 사용합니다.
	if (m_hasAcquiredSwapchainImage)
	{
		return;
	}

	const VkResult acquireResult = vkAcquireNextImageKHR(
		m_device,
		m_swapchain,
		UINT64_MAX,
		m_imageAvailableSemaphore,
		VK_NULL_HANDLE,
		&m_currentBackBufferIndex);

	if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR)
	{
		Resize(m_width, m_height);
		return;
	}

	if (acquireResult != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to acquire Vulkan swapchain image.");
	}

	m_hasAcquiredSwapchainImage = true;
}

void VulkanDevice::ResetImageAvailableSemaphore()
{
	if (m_device == VK_NULL_HANDLE)
	{
		return;
	}

	if (m_imageAvailableSemaphore != VK_NULL_HANDLE)
	{
		vkDestroySemaphore(m_device, m_imageAvailableSemaphore, nullptr);
		m_imageAvailableSemaphore = VK_NULL_HANDLE;
	}

	VkSemaphoreCreateInfo semaphoreCreateInfo = {};
	semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	if (vkCreateSemaphore(m_device, &semaphoreCreateInfo, nullptr, &m_imageAvailableSemaphore) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to recreate Vulkan image-available semaphore.");
	}

	LogVulkanStageSuccess("Image-available semaphore recreated.");
}

void VulkanDevice::CreateRenderPass()
{
	// RenderPass는 컬러 attachment와 깊이 attachment를 어떤 방식으로 사용하고 끝낼지 정의합니다.
	VkAttachmentDescription colorAttachment = {};
	colorAttachment.format = m_swapchainImageFormat;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentDescription depthAttachment = {};
	depthAttachment.format = m_depthFormat;
	depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colorAttachmentRef = {};
	colorAttachmentRef.attachment = 0;
	colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depthAttachmentRef = {};
	depthAttachmentRef.attachment = 1;
	depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorAttachmentRef;
	subpass.pDepthStencilAttachment = &depthAttachmentRef;

	const VkAttachmentDescription attachments[] = { colorAttachment, depthAttachment };

	VkRenderPassCreateInfo renderPassCreateInfo = {};
	renderPassCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassCreateInfo.attachmentCount = 2;
	renderPassCreateInfo.pAttachments = attachments;
	renderPassCreateInfo.subpassCount = 1;
	renderPassCreateInfo.pSubpasses = &subpass;

	if (vkCreateRenderPass(m_device, &renderPassCreateInfo, nullptr, &m_renderPass) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to create Vulkan render pass.");
	}
}

void VulkanDevice::CreateDepthResources()
{
	// Depth 버퍼는 스왑체인과 별도의 GPU 이미지/메모리로 생성합니다.
	VkImageCreateInfo imageCreateInfo = {};
	imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
	imageCreateInfo.extent.width = m_swapchainExtent.width;
	imageCreateInfo.extent.height = m_swapchainExtent.height;
	imageCreateInfo.extent.depth = 1;
	imageCreateInfo.mipLevels = 1;
	imageCreateInfo.arrayLayers = 1;
	imageCreateInfo.format = m_depthFormat;
	imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageCreateInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if (vkCreateImage(m_device, &imageCreateInfo, nullptr, &m_depthImage) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to create Vulkan depth image.");
	}

	VkMemoryRequirements memoryRequirements = {};
	vkGetImageMemoryRequirements(m_device, m_depthImage, &memoryRequirements);

	VkMemoryAllocateInfo allocateInfo = {};
	allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocateInfo.allocationSize = memoryRequirements.size;
	allocateInfo.memoryTypeIndex = FindMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	if (vkAllocateMemory(m_device, &allocateInfo, nullptr, &m_depthImageMemory) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to allocate Vulkan depth image memory.");
	}

	vkBindImageMemory(m_device, m_depthImage, m_depthImageMemory, 0);

	VkImageViewCreateInfo imageViewCreateInfo = {};
	imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	imageViewCreateInfo.image = m_depthImage;
	imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	imageViewCreateInfo.format = m_depthFormat;
	imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
	imageViewCreateInfo.subresourceRange.levelCount = 1;
	imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
	imageViewCreateInfo.subresourceRange.layerCount = 1;

	if (vkCreateImageView(m_device, &imageViewCreateInfo, nullptr, &m_depthImageView) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to create Vulkan depth image view.");
	}
}

void VulkanDevice::CreateFramebuffers()
{
	// 각 스왑체인 이미지마다 컬러 뷰 + 공용 depth 뷰를 묶은 framebuffer를 생성합니다.
	m_framebuffers.clear();
	m_framebuffers.reserve(m_swapchainImageViews.size());

	for (VkImageView colorImageView : m_swapchainImageViews)
	{
		const VkImageView attachments[] = { colorImageView, m_depthImageView };

		VkFramebufferCreateInfo framebufferCreateInfo = {};
		framebufferCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferCreateInfo.renderPass = m_renderPass;
		framebufferCreateInfo.attachmentCount = 2;
		framebufferCreateInfo.pAttachments = attachments;
		framebufferCreateInfo.width = m_swapchainExtent.width;
		framebufferCreateInfo.height = m_swapchainExtent.height;
		framebufferCreateInfo.layers = 1;

		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		if (vkCreateFramebuffer(m_device, &framebufferCreateInfo, nullptr, &framebuffer) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to create Vulkan framebuffer.");
		}

		m_framebuffers.push_back(framebuffer);
	}
}

void VulkanDevice::CreateCommandPool()
{
	// 커맨드 풀은 동일한 큐 패밀리용 커맨드 버퍼들을 할당하는 메모리 풀입니다.
	VkCommandPoolCreateInfo commandPoolCreateInfo = {};
	commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	commandPoolCreateInfo.queueFamilyIndex = m_graphicsQueueFamilyIndex;

	if (vkCreateCommandPool(m_device, &commandPoolCreateInfo, nullptr, &m_commandPool) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to create Vulkan command pool.");
	}
}

void VulkanDevice::CreateCommandBuffer()
{
	// 현재 예제는 하나의 명령 리스트만 사용하므로 단일 primary command buffer만 할당합니다.
	VkCommandBufferAllocateInfo allocateInfo = {};
	allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocateInfo.commandPool = m_commandPool;
	allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocateInfo.commandBufferCount = 1;

	if (vkAllocateCommandBuffers(m_device, &allocateInfo, &m_commandBuffer) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to allocate Vulkan command buffer.");
	}
}

void VulkanDevice::BeginFrameCommandRecording()
{
	// 현재 프레임 기록을 시작하기 전에 이전 내용을 reset하고 begin 합니다.
	vkResetCommandBuffer(m_commandBuffer, 0);

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to begin Vulkan command buffer.");
	}
}

void VulkanDevice::EndFrameCommandRecording()
{
	if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to end Vulkan command buffer.");
	}
}

void VulkanDevice::DestroyFramebuffers()
{
	for (VkFramebuffer framebuffer : m_framebuffers)
	{
		if (framebuffer != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
		{
			vkDestroyFramebuffer(m_device, framebuffer, nullptr);
		}
	}

	m_framebuffers.clear();
}

void VulkanDevice::DestroyDepthResources()
{
	if (m_depthImageView != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
	{
		vkDestroyImageView(m_device, m_depthImageView, nullptr);
		m_depthImageView = VK_NULL_HANDLE;
	}

	if (m_depthImage != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
	{
		vkDestroyImage(m_device, m_depthImage, nullptr);
		m_depthImage = VK_NULL_HANDLE;
	}

	if (m_depthImageMemory != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
	{
		vkFreeMemory(m_device, m_depthImageMemory, nullptr);
		m_depthImageMemory = VK_NULL_HANDLE;
	}
}

uint32_t VulkanDevice::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
	// Vulkan 메모리 할당은 GPU가 제공하는 메모리 타입 목록 중 조건에 맞는 타입을 직접 골라야 합니다.
	VkPhysicalDeviceMemoryProperties memoryProperties = {};
	vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);

	for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
	{
		if ((typeFilter & (1 << i)) && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
		{
			return i;
		}
	}

	throw std::runtime_error("Failed to find suitable Vulkan memory type.");
}

void VulkanDevice::DestroySwapchain()
{
	// swapchain 관련 객체는 생성의 역순으로 정리합니다.
	m_swapchainResources.clear();

	for (VkImageView imageView : m_swapchainImageViews)
	{
		if (imageView != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
		{
			vkDestroyImageView(m_device, imageView, nullptr);
		}
	}

	m_swapchainImageViews.clear();
	m_swapchainImages.clear();
	m_swapchainImageLayouts.clear();
	m_hasAcquiredSwapchainImage = false;

	if (m_swapchain != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
	{
		vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
		m_swapchain = VK_NULL_HANDLE;
	}
}

bool VulkanDevice::TryFindQueueFamilies(VkPhysicalDevice physicalDevice, uint32_t& graphicsQueueFamily, uint32_t& presentQueueFamily) const
{
	// GPU가 제공하는 큐 패밀리 목록을 조회합니다.
	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
	if (queueFamilyCount == 0)
	{
		return false;
	}

	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

	graphicsQueueFamily = InvalidQueueFamilyIndex();
	presentQueueFamily = InvalidQueueFamilyIndex();

	for (uint32_t i = 0; i < queueFamilyCount; ++i)
	{
		// 그래픽 명령을 실행할 수 있는 큐인지 검사합니다.
		if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			graphicsQueueFamily = i;
		}

		// 현재 surface에 프레젠트 가능한 큐인지 검사합니다.
		VkBool32 supportsPresent = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, m_surface, &supportsPresent);
		if (supportsPresent == VK_TRUE)
		{
			presentQueueFamily = i;
		}

		if (graphicsQueueFamily != InvalidQueueFamilyIndex() && presentQueueFamily != InvalidQueueFamilyIndex())
		{
			return true;
		}
	}

	return false;
}

bool VulkanDevice::SupportsRequiredDeviceExtensions(VkPhysicalDevice physicalDevice) const
{
	// swapchain 확장을 지원하는지 확인합니다.
	uint32_t extensionCount = 0;
	vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);

	std::vector<VkExtensionProperties> extensions(extensionCount);
	vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, extensions.data());

	for (const VkExtensionProperties& extension : extensions)
	{
		if (strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
		{
			return true;
		}
	}

	return false;
}

VulkanDevice::SwapchainSupportDetails VulkanDevice::QuerySwapchainSupport(VkPhysicalDevice physicalDevice) const
{
	SwapchainSupportDetails details;

	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, m_surface, &details.capabilities);

	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_surface, &formatCount, nullptr);
	if (formatCount > 0)
	{
		details.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_surface, &formatCount, details.formats.data());
	}

	uint32_t presentModeCount = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_surface, &presentModeCount, nullptr);
	if (presentModeCount > 0)
	{
		details.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_surface, &presentModeCount, details.presentModes.data());
	}

	return details;
}

VkSurfaceFormatKHR VulkanDevice::ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const
{
	// 가장 흔한 BGRA8_UNORM + SRGB 조합을 우선 선택합니다.
	for (const VkSurfaceFormatKHR& format : formats)
	{
		if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			return format;
		}
	}

	return formats[0];
}

VkPresentModeKHR VulkanDevice::ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const
{
	// Vulkan에서 수직동기화를 끄려면 IMMEDIATE 모드를 우선 사용합니다.
	// 플랫폼이 IMMEDIATE를 지원하지 않으면 MAILBOX, 마지막으로 FIFO 순서로 안전하게 폴백합니다.
	for (VkPresentModeKHR presentMode : presentModes)
	{
		if (presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR)
		{
			return presentMode;
		}
	}

	// IMMEDIATE 미지원 플랫폼에서는 MAILBOX를 우선 사용해 입력 지연을 줄입니다.
	for (VkPresentModeKHR presentMode : presentModes)
	{
		if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR)
		{
			return presentMode;
		}
	}

	// FIFO는 모든 플랫폼에서 보장되는 최종 폴백입니다.
	return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanDevice::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const
{
	// currentExtent가 고정값이면 플랫폼이 크기를 이미 정해준 것이므로 그대로 사용합니다.
	if (capabilities.currentExtent.width != (std::numeric_limits<uint32_t>::max)())
	{
		return capabilities.currentExtent;
	}

	// 그렇지 않다면 현재 윈도우 크기를 capability 범위 안으로 clamp합니다.
	VkExtent2D actualExtent =
	{
		static_cast<uint32_t>(m_width),
		static_cast<uint32_t>(m_height)
	};

	actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
	actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
	return actualExtent;
}
