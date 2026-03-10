#pragma once

#include "VulkanCommon.h"
#include "RHI/IGraphicsDevice.h"

#include <limits>
#include <memory>
#include <vector>

class VulkanImageResource;

class VulkanDevice : public IGraphicsDevice
{
public:
	VulkanDevice(void* windowHandle, int width, int height);
	virtual ~VulkanDevice();

	bool Init() override;
	void Shutdown() override;

	void WaitForGPU() override;
	void MoveToNextFrame() override;
	void Present() override;
	void Resize(int width, int height) override;

	ICommandList* CreateCommandList() override;
	IBuffer* CreateBuffer(const BufferDesc& desc) override;

	void ExecuteCommandList(ICommandList* cmdList) override;
	IGpuResource* GetBackBufferResource() override;

	void* GetCurrentBackBufferRTV() override;
	void* GetDepthStencilView() override;

	// Vulkan 전용 렌더링 확장에서 사용할 최소 getter들입니다.
	VkDevice GetVkDevice() const { return m_device; }
	VkPhysicalDevice GetVkPhysicalDevice() const { return m_physicalDevice; }
	VkRenderPass GetVkRenderPass() const { return m_renderPass; }
	VkExtent2D GetVkSwapchainExtent() const { return m_swapchainExtent; }
	// Vulkan 텍스처 업로드 시 일회성 복사 커맨드를 제출하려면 graphics queue와 command pool 접근이 필요합니다.
	VkQueue GetVkGraphicsQueue() const { return m_graphicsQueue; }
	VkCommandPool GetVkCommandPool() const { return m_commandPool; }
	// Vulkan 텍스처 이미지 메모리 할당 시 기존 디바이스 메모리 선택 로직을 재사용합니다.
	uint32_t FindMemoryTypeForTexture(uint32_t typeFilter, VkMemoryPropertyFlags properties) const { return FindMemoryType(typeFilter, properties); }

private:
	friend class VulkanCommandList;
	friend class VulkanBuffer;

private:
	struct SwapchainSupportDetails
	{
		// surface capability는 이미지 개수, 크기 범위, transform 같은 제약을 제공합니다.
		VkSurfaceCapabilitiesKHR capabilities = {};

		// surface format은 스왑체인 이미지 포맷과 색 공간 후보 목록입니다.
		std::vector<VkSurfaceFormatKHR> formats;

		// present mode는 화면 교체 정책(FIFO, MAILBOX 등) 후보 목록입니다.
		std::vector<VkPresentModeKHR> presentModes;
	};

	static constexpr uint32_t InvalidQueueFamilyIndex()
	{
		return (std::numeric_limits<uint32_t>::max)();
	}

	void CreateInstance();
	void SetupDebugMessenger();
	void DestroyDebugMessenger();
	void CreateSurface();
	void PickPhysicalDevice();
	void CreateLogicalDevice();
	void CreateSyncObjects();

	// Vulkan validation layer 콜백: 에러/경고를 Visual Studio 출력 창에 실시간으로 보여줍니다.
	static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
		VkDebugUtilsMessageTypeFlagsEXT messageType,
		const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
		void* pUserData);
	void CreateSwapchain();
	void CreateSwapchainImageViews();
	void CreateSwapchainResources();
	void CreateRenderPass();
	void CreateDepthResources();
	void CreateFramebuffers();
	void CreateCommandPool();
	void CreateCommandBuffer();
	void AcquireNextImage();
	void ResetImageAvailableSemaphore();
	void BeginFrameCommandRecording();
	void EndFrameCommandRecording();
	void DestroySwapchain();
	void DestroyFramebuffers();
	void DestroyDepthResources();
	uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

	bool TryFindQueueFamilies(VkPhysicalDevice physicalDevice, uint32_t& graphicsQueueFamily, uint32_t& presentQueueFamily) const;
	bool SupportsRequiredDeviceExtensions(VkPhysicalDevice physicalDevice) const;
	SwapchainSupportDetails QuerySwapchainSupport(VkPhysicalDevice physicalDevice) const;
	VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
	VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const;
	VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;

private:
	// 윈도우 핸들은 Win32 surface 생성에 사용됩니다.
	void* m_windowHandle = nullptr;
	int m_width = 0;
	int m_height = 0;

	// 스왑체인이 아직 준비되지 않았을 때 반환할 임시 백버퍼 래퍼입니다.
	std::unique_ptr<IGpuResource> m_nullBackBufferResource;

	// Vulkan 시작점 객체입니다. 확장 로딩과 물리 디바이스 열거가 여기서 시작됩니다.
	VkInstance m_instance = VK_NULL_HANDLE;

	// Debug build에서만 사용하는 validation layer 메시지 콜백 핸들입니다.
	VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;

	// Win32 창과 Vulkan 프레젠트를 연결하는 표면 객체입니다.
	VkSurfaceKHR m_surface = VK_NULL_HANDLE;

	// 선택된 실제 GPU와 그 위에 생성된 논리 디바이스입니다.
	VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
	VkDevice m_device = VK_NULL_HANDLE;

	// 명령 제출용 그래픽 큐와 화면 출력용 프레젠트 큐입니다.
	VkQueue m_graphicsQueue = VK_NULL_HANDLE;
	VkQueue m_presentQueue = VK_NULL_HANDLE;

	// 프레임 동기화를 위한 semaphore / fence입니다.
	VkSemaphore m_imageAvailableSemaphore = VK_NULL_HANDLE;
	VkSemaphore m_renderFinishedSemaphore = VK_NULL_HANDLE;
	VkFence m_inFlightFence = VK_NULL_HANDLE;

	// 화면 출력용 스왑체인과 그에 속한 이미지/이미지뷰들입니다.
	VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
	VkFormat m_swapchainImageFormat = VK_FORMAT_UNDEFINED;
	VkExtent2D m_swapchainExtent = {};
	std::vector<VkImage> m_swapchainImages;
	std::vector<VkImageView> m_swapchainImageViews;
	std::vector<std::unique_ptr<VulkanImageResource>> m_swapchainResources;
	std::vector<VkImageLayout> m_swapchainImageLayouts;
	uint32_t m_currentBackBufferIndex = 0;
	bool m_hasAcquiredSwapchainImage = false;
	bool m_hasLoggedSubmitSuccess = false;
	bool m_hasLoggedPresentSuccess = false;

	// 렌더패스는 컬러/깊이 attachment를 어떤 순서로 사용하고 최종 상태를 어떻게 둘지 정의합니다.
	VkRenderPass m_renderPass = VK_NULL_HANDLE;

	// 프레임버퍼는 특정 스왑체인 이미지 뷰와 depth 뷰를 렌더패스에 연결한 실제 렌더 대상입니다.
	std::vector<VkFramebuffer> m_framebuffers;

	// 깊이 버퍼는 컬러 스왑체인 이미지와 별도로 생성/소유하는 리소스입니다.
	VkFormat m_depthFormat = VK_FORMAT_D32_SFLOAT;
	VkImage m_depthImage = VK_NULL_HANDLE;
	VkDeviceMemory m_depthImageMemory = VK_NULL_HANDLE;
	VkImageView m_depthImageView = VK_NULL_HANDLE;

	// 커맨드 풀과 단일 커맨드 버퍼를 사용해 현재 프레임의 렌더 명령을 기록합니다.
	VkCommandPool m_commandPool = VK_NULL_HANDLE;
	VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;

	// 각 큐가 속한 큐 패밀리 인덱스입니다.
	uint32_t m_graphicsQueueFamilyIndex = InvalidQueueFamilyIndex();
	uint32_t m_presentQueueFamilyIndex = InvalidQueueFamilyIndex();
};
