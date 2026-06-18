#pragma once

#include "Assets/AssetDatabase.h"
#include "Assets/AssetFileSystem.h"
#include "Assets/AssetImportSettings.h"
#include "Editor/EditorCommandStack.h"
#include "Jobs/JobSystem.h"
#include "Materials/MaterialResource.h"
#include "Materials/ShaderVariant.h"
#include "Math/Camera.h"
#include "Memory/MemoryTypes.h"
#include "Rendering/RHI/GraphicsCommon.h"
#include "Rendering/Graph/RenderGraph.h"
#include "Rendering/Lighting/ShadowSystem.h"
#include "Rendering/Post/PostProcessSystem.h"
#include "Rendering/Sky/SkyboxSettings.h"
#include "Rendering/RenderMode.h"
#include "Rendering/Systems/StaticMeshRenderer.h"
#include "Resources/ResourceTypes.h"
#include "Samples/Benchmark/BenchmarkRunner.h"
#include "Scene/Scene.h"
#include "Scene/ScenePersistenceService.h"
#include "Scripting/ScriptRuntime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ImDrawList;
struct ImVec2;

namespace Editor
{
	struct ViewportPanelState
	{
		float Left = 0.0f;
		float Top = 0.0f;
		float Width = 0.0f;
		float Height = 0.0f;
		bool IsVisible = false;
		bool IsHovered = false;
		bool IsFocused = false;

		[[nodiscard]] bool CanRender() const noexcept
		{
			return IsVisible && Width >= 1.0f && Height >= 1.0f;
		}

		[[nodiscard]] float AspectRatio() const noexcept
		{
			return Height > 0.0f ? Width / Height : 1.0f;
		}
	};

	enum class AssetDropTarget : uint8_t
	{
		Scene,
		Game,
		External
	};

	enum class ProjectCreateAssetKind : uint8_t
	{
		Folder,
		Scene,
		Material,
		Skybox,
		Script,
		Prefab
	};

	enum class EntityDropPlacement : uint8_t
	{
		Before,
		After,
		AsChild
	};

	enum class HierarchyQuickFilter : uint8_t
	{
		All,
		Mesh,
		Camera,
		Light,
		Physics,
		Script,
		Hidden,
		Locked,
		Nested
	};

	enum class ProjectQuickFilter : uint8_t
	{
		All,
		Favorites,
		Folders,
		Models,
		Images,
		Scenes,
		Materials,
		Prefabs,
		Source,
		Text
	};

	enum class ContentDrawerSortMode : uint8_t
	{
		Path,
		Name,
		Type,
		SizeDescending,
		ModifiedDescending
	};

	enum class CommandPaletteScope : uint8_t
	{
		All,
		Commands,
		Entities,
		Assets
	};

	enum class TransformGizmoMode : uint8_t
	{
		Translate,
		Rotate,
		Scale
	};

	enum class TransformGizmoAxis : uint8_t
	{
		None,
		X,
		Y,
		Z
	};

	enum class TransformGizmoPlane : uint8_t
	{
		None,
		XY,
		XZ,
		YZ
	};

	enum class TransformGizmoSpace : uint8_t
	{
		World,
		Local
	};

	enum class TransformGizmoPivot : uint8_t
	{
		Pivot,
		Center
	};

	enum class SceneMeasureTarget : uint8_t
	{
		Ground,
		ViewPlane,
		SelectionBounds,
		MeshSurface
	};

	struct TransformEditRecord
	{
		EntityId Entity = InvalidEntityId;
		Math::Transform Before = Math::Transform::Identity();
		Math::Transform After = Math::Transform::Identity();
	};

	struct ComponentEditRecord
	{
		EntityId Entity = InvalidEntityId;
		SceneComponentKind Kind = SceneComponentKind::Mesh;
		ScenePersistence::LoadedSceneEntity Before;
		ScenePersistence::LoadedSceneEntity After;
	};

	struct MaterialTextureAssignment
	{
		Asset::MaterialTextureSlot Slot = Asset::MaterialTextureSlot::Count;
		std::filesystem::path Path;
	};

	struct MaterialTextureBatchAssignment
	{
		size_t MaterialIndex = static_cast<size_t>(-1);
		std::vector<MaterialTextureAssignment> Assignments;
	};

	struct MaterialEditRecord
	{
		EntityId Entity = InvalidEntityId;
		size_t MaterialIndex = static_cast<size_t>(-1);
		Asset::StaticMeshMaterial Before;
		Asset::StaticMeshMaterial After;
	};

	struct ExportProfileSettings
	{
		std::filesystem::path OutputDirectory;
		bool CopyAssets = true;
		bool CopyScenes = true;
		bool WriteManifest = true;
		bool RevealAfterExport = true;
	};

	struct SceneReferenceRuntimeStatus
	{
		bool Loaded = false;
		bool FileExists = false;
		bool Watching = false;
		bool PendingExternalReload = false;
		size_t LoadedEntityCount = 0;
		std::filesystem::path ResolvedScenePath;
		std::string StatusText;
	};

	struct NestedSceneChildStatus
	{
		bool IsNestedSceneChild = false;
		EntityId OwnerEntity = InvalidEntityId;
		std::filesystem::path SourceScenePath;
		size_t SiblingCount = 0;
	};

	enum class MeshRestoreMaterialFocusKind : uint8_t
	{
		None,
		ShadingModel,
		BaseColor,
		UseVertexColor,
		NormalYFlip,
		EmissiveColor,
		Opacity,
		Metallic,
		Roughness,
		SpecularColor,
		Shininess,
		TextureSlot
	};

	struct MeshRestoreMaterialDiffRow
	{
		size_t MaterialIndex = static_cast<size_t>(-1);
		Asset::MaterialTextureSlot TextureSlot = Asset::MaterialTextureSlot::Count;
		MeshRestoreMaterialFocusKind FocusKind = MeshRestoreMaterialFocusKind::None;
		std::string Field;
		std::string CurrentValue;
		std::string RestoreValue;
	};

	struct MeshRestoreRuntimeStatus
	{
		bool HasStatus = false;
		bool Pending = false;
		bool Failed = false;
		bool Cancelled = false;
		bool Conflicted = false;
		uint64_t Generation = 0;
		std::filesystem::path SourcePath;
		std::filesystem::path SourcePrefabPath;
		size_t ImportedVertexCount = 0;
		size_t ImportedIndexCount = 0;
		size_t ImportedMaterialCount = 0;
		std::vector<std::string> MaterialDiffLines;
		std::vector<MeshRestoreMaterialDiffRow> MaterialDiffRows;
		std::string Message;
	};

	struct EditorContext
	{
		GraphicsAPI CurrentApi;
		RenderMode CurrentRenderMode;
		Camera& SceneCamera;
		Camera& GameCamera;
		Scene& ActiveScene;
		Samples::Benchmark::SampleMode& SampleMode;
		Samples::Benchmark::BenchmarkRunner& BenchmarkRunner;
		bool& ShowDemoWindow;
		int ViewportWidth = 0;
		int ViewportHeight = 0;
		std::string ProjectName = "Development";
		std::filesystem::path ProjectRootPath;
		std::filesystem::path CurrentScenePath;
		std::shared_ptr<const Asset::AssetFileSnapshot> ProjectSnapshot;
		const std::vector<std::string>* AssetLogLines = nullptr;
		Memory::MemorySystemStats MemoryStats;
		Jobs::JobSystemStats JobStats;
		Scripting::ScriptRuntimeStats ScriptStats;
		Resources::ResourceManagerStats ResourceStats;
		Materials::MaterialResourceStats MaterialStats;
		Materials::ShaderVariantCacheStats ShaderVariantStats;
		Rendering::RenderGraphStats RenderGraphStats;
		const std::vector<Rendering::RenderGraphPass>* RenderGraphPasses = nullptr;
		Rendering::RenderFrameStats RenderFrameStats;
		Rendering::ShadowSettings ShadowSettings;
		Rendering::ShadowStats ShadowStats;
		Rendering::PostProcessStats PostProcessStats;
		uint32_t ForwardLightLimit = 0;
		uint32_t SceneLightCount = 0;
		uint32_t ForwardLightUsedCount = 0;
		uint32_t ForwardLightTruncatedCount = 0;
		bool UsesFallbackLight = false;
		uint32_t DeferredLightCount = 0;
		uint32_t DeferredLightBufferCapacity = 0;
		uint32_t DeferredTileCountX = 0;
		uint32_t DeferredTileCountY = 0;
		uint32_t DeferredTileViewportCount = 0;
		uint32_t DeferredTileCountTotal = 0;
		uint32_t DeferredTileLightReferenceCount = 0;
		uint32_t DeferredMaxTileLightCount = 0;
		uint32_t DeferredFullTileLightCount = 0;
		bool ProjectRefreshInProgress = false;
		bool IsSceneDirty = false;
		bool CanEditProjectScene = false;
		bool CanControlPlayMode = false;
		bool ActiveSceneIsRuntimeClone = false;
		bool PhysicsSimulationEnabled = false;
		bool AutosaveEnabled = true;
		bool AutosaveLastSucceeded = false;
		float AutosaveIntervalSeconds = 120.0f;
		float AutosaveElapsedSeconds = 0.0f;
		std::filesystem::path AutosavePath;
		std::string AutosaveStatusMessage;
		EditorPlayState PlayState = EditorPlayState::Edit;
		bool CanUndo = false;
		bool CanRedo = false;
		std::string UndoLabel;
		std::string RedoLabel;
		DirectX::XMFLOAT3 AmbientColor = { 0.62f, 0.68f, 0.78f };
		float AmbientIntensity = 0.35f;
		float Exposure = 1.0f;
		Rendering::SkyboxSettings Skybox;
		float KeyLightIntensity = 3.25f;
		MaterialDebugView DebugView = MaterialDebugView::Lit;
		bool ViewFrustumCullingEnabled = true;
		std::function<void(GraphicsAPI)> OnGraphicsApiChanged;
		std::function<void(RenderMode)> OnRenderModeChanged;
		std::function<void()> OnSaveScene;
		std::function<void()> OnSaveSceneAs;
		std::function<void()> OnOpenSceneDialog;
		std::function<void(const std::filesystem::path&)> OnOpenScene;
		std::function<void()> OnSaveSelectedPrefab;
		std::function<void()> OnExportProject;
		std::function<bool(const ExportProfileSettings&)> OnExportProjectProfile;
		std::function<void()> OnRevealProject;
		std::function<void()> OnExit;
		std::function<void()> OnUndo;
		std::function<void()> OnRedo;
		std::function<void(bool)> OnPlayModeChanged;
		std::function<void(bool)> OnPlayPausedChanged;
		std::function<void()> OnPlayStep;
		std::function<void()> OnResetPlayRuntimeScene;
		std::function<void(bool)> OnAutosaveEnabledChanged;
		std::function<void(float)> OnAutosaveIntervalChanged;
		std::function<void()> OnFrameSelected;
		std::function<void()> OnAlignGameCameraToScene;
		std::function<void()> OnAlignSceneCameraToGame;
		std::function<void(float, float, float, float)> OnScenePick;
		std::function<void(const std::filesystem::path&)> OnAssetOpen;
		std::function<void(const std::filesystem::path&)> OnAssetReveal;
		std::function<void(const std::filesystem::path&)> OnAssetImportSettingsRequested;
		std::function<void(const std::filesystem::path&)> OnAssetReimportRequested;
		std::function<void(const std::filesystem::path&, AssetDropTarget)> OnModelDrop;
		std::function<void()> OnProjectRefresh;
		std::function<bool(const std::filesystem::path&, ScenePersistence::LoadedSceneEntity&, std::string&)> OnLoadPrefabForInspection;
		std::function<bool(EntityId)> OnApplyEntityToPrefab;
		std::function<bool(const std::filesystem::path&, const ScenePersistence::LoadedSceneEntity&)> OnSavePrefabInspectionRoot;
		std::function<bool(EntityId, const ScenePersistence::LoadedSceneEntity&, const std::filesystem::path&)> OnRevertMeshToPrefabSource;
		std::function<MeshRestoreRuntimeStatus(EntityId)> OnGetMeshRestoreStatus;
		std::function<bool(EntityId)> OnCancelMeshRestore;
		std::function<bool(EntityId)> OnApplyConflictedMeshRestore;
		std::function<bool(EntityId)> OnReloadMeshRestoreFromPrefabSource;
		std::function<bool(EntityId)> OnLoadSceneReference;
		std::function<bool(EntityId)> OnUnloadSceneReference;
		std::function<SceneReferenceRuntimeStatus(EntityId)> OnGetSceneReferenceStatus;
		std::function<NestedSceneChildStatus(EntityId)> OnGetNestedSceneChildStatus;
		std::function<bool(EntityId)> OnMakeNestedSceneChildLocal;
		std::function<void(ProjectCreateAssetKind, const std::filesystem::path&)> OnCreateProjectAsset;
		std::function<void(ProjectCreateAssetKind, const std::filesystem::path&, std::string_view)> OnCreateNamedProjectAsset;
		std::function<void(EntityId, std::string_view)> OnRenameEntity;
		std::function<void(EntityId)> OnDuplicateEntity;
		std::function<void(std::vector<EntityId>)> OnDuplicateEntities;
		std::function<void(EntityId)> OnDeleteEntity;
		std::function<void(std::vector<EntityId>)> OnDeleteEntities;
		std::function<void(EntityId, bool)> OnEntitySceneVisibilityChanged;
		std::function<void(EntityId, bool)> OnEntityScenePickabilityChanged;
		std::function<void(EntityId)> OnCreateEmptyEntity;
		std::function<void(EntityId)> OnCreateCameraEntity;
		std::function<void(EntityId)> OnCreateLightEntity;
		std::function<void(Asset::PrimitiveMeshKind, EntityId)> OnCreatePrimitive;
		std::function<void(EntityId)> OnCreateEmptyParentForEntity;
		std::function<void(EntityId, EntityId, EntityDropPlacement)> OnMoveEntity;
		std::function<void(std::vector<EntityId>, EntityId, EntityDropPlacement)> OnMoveEntities;
		std::function<void(EntityId, SceneComponentKind)> OnComponentAdded;
		std::function<void(EntityId, SceneComponentKind)> OnComponentRemoved;
		std::function<void(EntityId, SceneComponentKind, bool)> OnComponentEnabledChanged;
		std::function<void(EntityId, SceneComponentKind)> OnComponentReset;
		std::function<void(EntityId, SceneComponentKind, const ScenePersistence::LoadedSceneEntity&)> OnComponentPaste;
		std::function<void(EntityId, const Math::Transform&, const Math::Transform&)> OnTransformEditCommitted;
		std::function<void(std::vector<TransformEditRecord>)> OnTransformBatchEditCommitted;
		std::function<void(std::vector<ComponentEditRecord>)> OnComponentBatchEditCommitted;
		std::function<void(EntityId, size_t, const Asset::StaticMeshMaterial&, const Asset::StaticMeshMaterial&)> OnMaterialEditCommitted;
		std::function<void(std::vector<MaterialEditRecord>)> OnMaterialBatchEditCommitted;
		std::function<void(EntityId, size_t, Asset::MaterialShadingModel)> OnMaterialShadingModelChanged;
		std::function<void(EntityId, size_t, Asset::MaterialTextureSlot, const std::filesystem::path&)> OnMaterialTextureAssigned;
		std::function<void(EntityId, size_t, const std::vector<MaterialTextureAssignment>&)> OnMaterialTexturesAssigned;
		std::function<void(EntityId, const std::vector<MaterialTextureBatchAssignment>&)> OnMaterialTextureBatchAssigned;
		std::function<void(EntityId, size_t, Asset::MaterialTextureSlot)> OnMaterialTextureCleared;
		std::function<void(EntityId, size_t, Asset::MaterialTextureSlot)> OnMaterialTextureBrowseRequested;
		std::function<void(EntityId, size_t)> OnMaterialEdited;
		std::function<void(const DirectX::XMFLOAT3&)> OnAmbientColorChanged;
		std::function<void(float)> OnAmbientIntensityChanged;
		std::function<void(float)> OnExposureChanged;
		std::function<void(const Rendering::SkyboxSettings&)> OnSkyboxSettingsChanged;
		std::function<void(float)> OnKeyLightIntensityChanged;
		std::function<void(MaterialDebugView)> OnMaterialDebugViewChanged;
		std::function<void(bool)> OnViewFrustumCullingChanged;
		std::function<void(const Rendering::ShadowSettings&)> OnShadowSettingsChanged;
		std::function<void()> OnSceneEdited;
		std::function<void(bool)> OnPhysicsSimulationChanged;
		std::function<void(EntityId)> OnPhysicsActorDirty;
	};

	class EditorLayer
	{
	public:
		void Draw(EditorContext& context);
		[[nodiscard]] const ViewportPanelState& GetSceneViewport() const noexcept { return m_SceneViewport; }
		[[nodiscard]] const ViewportPanelState& GetGameViewport() const noexcept { return m_GameViewport; }

	private:
		struct MeshMeasureTriangleCacheEntry
		{
			uint32_t I0 = 0;
			uint32_t I1 = 0;
			uint32_t I2 = 0;
			DirectX::XMFLOAT3 LocalMin = {};
			DirectX::XMFLOAT3 LocalMax = {};
		};

		struct MeshMeasureAccelerationCache
		{
			const Asset::StaticMeshAsset* Mesh = nullptr;
			size_t VertexCount = 0;
			size_t IndexCount = 0;
			uint64_t FrameIndex = 0;
			bool Animated = false;
			std::vector<MeshMeasureTriangleCacheEntry> Triangles;
		};

		struct SourceControlSummary
		{
			std::filesystem::path RootPath;
			std::string Branch = "<unknown>";
			std::string Upstream;
			std::string Message = "Source control status has not been refreshed.";
			std::vector<std::string> ChangedFiles;
			size_t ModifiedCount = 0;
			size_t AddedCount = 0;
			size_t DeletedCount = 0;
			size_t RenamedCount = 0;
			size_t UntrackedCount = 0;
			size_t ConflictedCount = 0;
			size_t StagedCount = 0;
			size_t UnstagedCount = 0;
			int AheadCount = 0;
			int BehindCount = 0;
			double LastRefreshTime = -1.0;
			bool IsGitRepository = false;
			bool IsClean = false;
			bool HasUpstream = false;
		};

		void DrawDockSpace();
		void DrawToolbar(EditorContext& context);
		void DrawHierarchy(EditorContext& context);
		void DrawSceneView(EditorContext& context);
		void DrawGameView(EditorContext& context);
		void DrawInspector(EditorContext& context);
		void DrawProject(EditorContext& context);
		void DrawBenchmark(EditorContext& context);
		void DrawProfiler(EditorContext& context);
		void DrawConsole(EditorContext& context);
		void DrawStatusBar(EditorContext& context);
		void DrawUnsavedSceneList(EditorContext& context);
		void DrawSourceControlStatus(EditorContext& context);
		void DrawExportProfile(EditorContext& context);
		void DrawProjectCreateAssetModal(EditorContext& context);
		void OpenProjectCreateAssetDialog(ProjectCreateAssetKind kind, const std::filesystem::path& targetDirectory);
		void DrawCommandPalette(EditorContext& context);
		void DrawContentDrawer(EditorContext& context);
		void DrawShortcutReference(EditorContext& context);
		void DrawPrefabOverrideDiff(EditorContext& context, EntityId entityId, const PrefabInstanceComponent& prefab);
		void DrawReflectionQuickEdit(
			EditorContext& context,
			EntityId entityId,
			const std::vector<SceneComponentKind>& componentOrder,
			const std::vector<EntityId>& inspectorSelection,
			std::string_view inspectorFilter,
			const ScenePersistence::LoadedSceneEntity* prefabOverrideSource);
		void DrawReflectionSchema(EditorContext& context, EntityId entityId, const std::vector<SceneComponentKind>& componentOrder, std::string_view inspectorFilter);
		void DrawSceneViewOverlay(EditorContext& context, const ImVec2& canvasPosition, const ImVec2& canvasSize);
		void DrawSceneViewCube(EditorContext& context, const ImVec2& canvasPosition, const ImVec2& canvasSize);
		void DrawSceneMeasurement(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize) const;
		[[nodiscard]] bool HandleSceneMeasureTool(EditorContext& context, const ImVec2& canvasPosition, const ImVec2& canvasSize, bool mouseBlockedByUi);
		[[nodiscard]] bool HandleSceneFocusOrbit(EditorContext& context, bool mouseBlockedByUi);
		[[nodiscard]] bool HandleSceneMarqueeSelection(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize, bool mouseBlockedByUi);
		void DrawSceneGrid(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize) const;
		[[nodiscard]] bool DrawSceneGizmos(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize);
		void DrawSelectionBoundsGizmo(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize) const;
		void DrawGameCameraFrustumGizmo(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize) const;
		void DrawColliderGizmos(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize) const;
		[[nodiscard]] bool DrawTransformGizmo(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize);
		[[nodiscard]] bool ProjectWorldToSceneCanvas(
			const Camera& sceneCamera,
			const DirectX::XMFLOAT3& worldPosition,
			const ImVec2& canvasPosition,
			const ImVec2& canvasSize,
			ImVec2& screenPosition) const;
		[[nodiscard]] bool BuildSceneMouseRay(
			const Camera& sceneCamera,
			const ImVec2& mousePosition,
			const ImVec2& canvasPosition,
			const ImVec2& canvasSize,
			DirectX::XMFLOAT3& rayOrigin,
			DirectX::XMFLOAT3& rayDirection) const;
		[[nodiscard]] bool ProjectSceneMouseToMeasureTarget(
			EditorContext& context,
			const ImVec2& mousePosition,
			const ImVec2& canvasPosition,
			const ImVec2& canvasSize,
			DirectX::XMFLOAT3& worldPosition);
		void BuildDefaultLayout(unsigned int dockspaceId, float viewportWidth, float viewportHeight);
		void StoreViewportState(ViewportPanelState& target, float screenLeft, float screenTop, float width, float height, bool hovered, bool focused) const;
		void DrawProjectEntryRecursive(const Asset::AssetFileEntry& entry, EditorContext& context);
		void DrawSelectedAssetDetails(const Asset::AssetFileSnapshot& snapshot, EditorContext& context);
		void DrawImageAssetPreview(const std::filesystem::path& imagePath);
		void DrawImportSettingsEditor(const Asset::AssetFileSnapshot& snapshot, EditorContext& context);
		void EnsureImportSettingsLoadedForAsset(const std::filesystem::path& assetPath);
		bool SaveSelectedImportSettings(EditorContext& context, bool reimportAfterSave);
		void DrawSelectedAssetDependencyView(const Asset::AssetFileSnapshot& snapshot, EditorContext& context);
		void EnsureProjectReferenceIndex(const Asset::AssetFileSnapshot& snapshot, const EditorContext& context);
		void RefreshSelectedAssetAnalysis(const Asset::AssetFileSnapshot& snapshot, const EditorContext& context);
		void EnsureProjectImagePreview(const std::filesystem::path& imagePath);
		[[nodiscard]] bool IsProjectFavorite(const std::filesystem::path& path) const;
		void ToggleProjectFavorite(const std::filesystem::path& path, const EditorContext& context);
		void AddRecentProjectAssetPath(const std::filesystem::path& assetPath, const EditorContext& context, bool saveState);
		void DrawRecentProjectAssets(const Asset::AssetFileSnapshot& snapshot, EditorContext& context, std::string_view projectFilter);
		void EnsureProjectStateLoaded(const EditorContext& context);
		void LoadProjectState(const std::filesystem::path& projectRootPath);
		void SaveProjectState(const EditorContext& context) const;
		void TrackCurrentSceneInRecentScenes(const EditorContext& context);
		void AddRecentScenePath(const std::filesystem::path& scenePath, const EditorContext& context, bool saveState);
		void DrawRecentScenesMenu(EditorContext& context);
		void EnsureExportProfileDefaults(const EditorContext& context);
		[[nodiscard]] ExportProfileSettings BuildExportProfileSettings(const EditorContext& context) const;
		void SaveCurrentEditorLayout(const EditorContext& context);
		void RestoreSavedEditorLayout();
		void ResetEditorLayoutToDefault();
		void DrawEditorLayoutTools(EditorContext& context);
		void HandleHierarchyShortcuts(EditorContext& context);
		void OpenCommandPalette();
		void OpenContentDrawer();
		void OpenShortcutReference();
		void ExecuteConsoleCommand(EditorContext& context, std::string_view command);
		void RefreshSourceControlStatus(const EditorContext& context);
		bool RunSourceControlCommand(const EditorContext& context, std::string_view gitArguments, std::string_view successMessage);
		void OpenRenamePopup(EntityId entityId, std::string_view currentName);
		void DrawRenamePopup(EditorContext& context);

		std::filesystem::path m_SelectedAssetPath;
		EntityId m_RenamingEntity = InvalidEntityId;
		EntityId m_TransformEditingEntity = InvalidEntityId;
		Math::Transform m_TransformEditBefore = Math::Transform::Identity();
		std::vector<TransformEditRecord> m_MultiTransformEditBefore;
		std::vector<ComponentEditRecord> m_MultiComponentEditBefore;
		EntityId m_MaterialEditingEntity = InvalidEntityId;
		size_t m_MaterialEditingIndex = static_cast<size_t>(-1);
		Asset::StaticMeshMaterial m_MaterialEditBefore;
		std::unordered_map<std::string, std::filesystem::path> m_TextureRemapCandidateOverrides;
		EntityId m_FocusedMaterialEntity = InvalidEntityId;
		size_t m_FocusedMaterialIndex = static_cast<size_t>(-1);
		Asset::MaterialTextureSlot m_FocusedMaterialTextureSlot = Asset::MaterialTextureSlot::Count;
		MeshRestoreMaterialFocusKind m_FocusedMaterialControl = MeshRestoreMaterialFocusKind::None;
		int m_FocusedMaterialHighlightFrames = 0;
		bool m_FocusedMaterialFocusPinned = false;
		std::array<char, 128> m_RenameBuffer = {};
		std::array<char, 96> m_HierarchyFilter = {};
		std::array<char, 96> m_InspectorFilter = {};
		std::array<char, 96> m_ProjectFilter = {};
		std::array<char, 96> m_ConsoleLogFilter = {};
		std::array<char, 96> m_TextureRemapFilter = {};
		std::array<char, 128> m_ReflectionAssetPickerFilter = {};
		std::array<char, 256> m_ConsoleCommandBuffer = {};
		std::array<char, 128> m_CommandPaletteFilter = {};
		std::array<char, 128> m_ContentDrawerFilter = {};
		std::array<char, 128> m_ProjectCreateNameBuffer = {};
		std::array<char, 256> m_SourceControlCommitMessageBuffer = {};
		std::array<char, 260> m_ExportOutputDirectoryBuffer = {};
		EntityId m_DefaultParentEntity = InvalidEntityId;
		EntityId m_LockedInspectorEntity = InvalidEntityId;
		SceneComponentKind m_ComponentClipboardKind = SceneComponentKind::Mesh;
		ScenePersistence::LoadedSceneEntity m_ComponentClipboardSnapshot;
		std::unordered_map<EntityId, std::vector<SceneComponentKind>> m_InspectorComponentOrders;
		std::vector<SceneComponentKind> m_InspectorPinnedComponents;
		std::vector<EntityId> m_HierarchySelection;
		EntityId m_LastHierarchyClickedEntity = InvalidEntityId;
		HierarchyQuickFilter m_HierarchyQuickFilter = HierarchyQuickFilter::All;
		ProjectQuickFilter m_ProjectQuickFilter = ProjectQuickFilter::All;
		ContentDrawerSortMode m_ContentDrawerSortMode = ContentDrawerSortMode::Path;
		bool m_ContentDrawerSortDescending = false;
		bool m_ContentDrawerDetailsVisible = true;
		bool m_ProjectFolderScopeEnabled = false;
		std::vector<std::filesystem::path> m_ProjectFavoritePaths;
		std::vector<std::filesystem::path> m_ProjectRecentAssetPaths;
		std::vector<std::filesystem::path> m_RecentScenePaths;
		std::vector<std::string> m_ProjectSavedSearches;
		std::vector<std::string> m_CommandPalettePinnedCommands;
		std::vector<std::string> m_CommandPaletteRecentCommands;
		std::vector<std::string> m_ConsoleCommandHistory;
		std::string m_CommandPaletteLastFilter;
		CommandPaletteScope m_CommandPaletteScope = CommandPaletteScope::All;
		SourceControlSummary m_SourceControlSummary;
		std::string m_SourceControlOperationStatus;
		bool m_SourceControlPushSetUpstream = false;
		std::filesystem::path m_ExportProfileProjectRootPath;
		std::filesystem::path m_LastExportOutputDirectory;
		std::string m_LastExportStatusMessage;
		size_t m_LastExportWrittenFileCount = 0;
		std::filesystem::path m_ProjectStateRootPath;
		std::filesystem::path m_LastTrackedCurrentScenePath;
		std::string m_ProjectEditorLayoutIni;
		std::string m_EditorLayoutStatusMessage;
		std::filesystem::path m_ProjectAnalysisSelectionPath;
		std::filesystem::path m_ProjectAnalysisRootPath;
		std::vector<std::filesystem::path> m_ProjectDependencyPaths;
		std::vector<std::filesystem::path> m_ProjectReferencePaths;
		std::vector<std::string> m_ProjectModelInspectionLines;
		size_t m_ProjectAnalysisScannedFiles = 0;
		size_t m_ProjectAnalysisSkippedFiles = 0;
		Asset::AssetReferenceIndex m_ProjectReferenceIndex;
		bool m_ForceRebuildProjectReferenceIndex = false;
		std::filesystem::path m_ProjectPreviewImagePath;
		std::filesystem::path m_ProjectImportSettingsAssetPath;
		Asset::AssetImportSettings m_ProjectImportSettings;
		std::string m_ProjectImportSettingsStatus;
		std::string m_ProjectImportSettingsError;
		std::string m_ProjectCreateStatus;
		std::vector<uint32_t> m_ProjectPreviewPixels;
		int m_ProjectPreviewWidth = 0;
		int m_ProjectPreviewHeight = 0;
		int m_ProjectPreviewSourceWidth = 0;
		int m_ProjectPreviewSourceHeight = 0;
		int m_ProjectPreviewSourceChannels = 0;
		std::string m_ProjectPreviewError;
		bool m_ProjectTwoColumnLayout = true;
		bool m_ShouldOpenCommandPalette = false;
		bool m_ShouldFocusCommandPalette = false;
		bool m_ShouldOpenContentDrawer = false;
		bool m_ShouldFocusContentDrawer = false;
		bool m_ShouldOpenShortcutReference = false;
		bool m_ShouldOpenProjectCreateAssetModal = false;
		bool m_ShouldOpenRenamePopup = false;
		bool m_ShouldFocusRenameInput = false;
		size_t m_CommandPaletteSelectedIndex = 0;
		bool m_InspectorLocked = false;
		bool m_HasComponentClipboard = false;
		bool m_ExportCopyAssets = true;
		bool m_ExportCopyScenes = true;
		bool m_ExportWriteManifest = true;
		bool m_ExportRevealAfterBuild = true;
		bool m_MultiTransformEditing = false;
		bool m_MultiComponentEditing = false;
		SceneComponentKind m_MultiComponentEditingKind = SceneComponentKind::Mesh;
		std::array<float, 180> m_ProfilerFrameMsHistory = {};
		std::array<float, 180> m_ProfilerRenderCpuMsHistory = {};
		std::array<float, 180> m_ProfilerDrawCallHistory = {};
		std::array<float, 180> m_ProfilerTriangleKHistory = {};
		size_t m_ProfilerSampleCount = 0;
		uint64_t m_ProfilerLastFrameIndex = 0;
		bool m_ProfilerPaused = false;
		ViewportPanelState m_SceneViewport;
		ViewportPanelState m_GameViewport;
		bool m_DefaultLayoutBuilt = false;
		bool m_ProjectEditorLayoutRestored = false;
		bool m_ProjectImportSettingsLoaded = false;
		bool m_ProjectImportSettingsDirty = false;
		ProjectCreateAssetKind m_PendingProjectCreateKind = ProjectCreateAssetKind::Folder;
		std::filesystem::path m_PendingProjectCreateDirectory;
		bool m_ShowSceneGizmos = true;
		bool m_ShowSceneGrid = true;
		bool m_ShowSelectionOutline = true;
		bool m_FocusOrbitEnabled = true;
		bool m_MeasureToolEnabled = false;
		bool m_MeasureToolHasStart = false;
		bool m_MeasureToolDragging = false;
		bool m_ViewCubeDragging = false;
		bool m_SceneMarqueeTracking = false;
		bool m_SceneMarqueeSelecting = false;
		bool m_SceneMarqueeAdditive = false;
		SceneMeasureTarget m_MeasureTarget = SceneMeasureTarget::Ground;
		bool m_TransformSnappingEnabled = false;
		float m_TranslateSnap = 1.0f;
		float m_RotateSnapDegrees = 15.0f;
		float m_ScaleSnap = 0.25f;
		float m_GameCameraGizmoDepth = 25.0f;
		TransformGizmoMode m_TransformGizmoMode = TransformGizmoMode::Translate;
		TransformGizmoSpace m_TransformGizmoSpace = TransformGizmoSpace::World;
		TransformGizmoPivot m_TransformGizmoPivot = TransformGizmoPivot::Pivot;
		TransformGizmoAxis m_TransformGizmoActiveAxis = TransformGizmoAxis::None;
		TransformGizmoPlane m_TransformGizmoActivePlane = TransformGizmoPlane::None;
		bool m_TransformGizmoUniformScaleActive = false;
		EntityId m_TransformGizmoActiveEntity = InvalidEntityId;
		Math::Transform m_TransformGizmoStartTransform = Math::Transform::Identity();
		Math::Transform m_TransformGizmoStartWorldTransform = Math::Transform::Identity();
		DirectX::XMFLOAT2 m_TransformGizmoStartMouse = { 0.0f, 0.0f };
		DirectX::XMFLOAT2 m_TransformGizmoStartAxisScreen = { 1.0f, 0.0f };
		DirectX::XMFLOAT2 m_TransformGizmoStartPlaneSecondScreen = { 0.0f, 1.0f };
		float m_TransformGizmoStartScale = 1.0f;
		DirectX::XMFLOAT3 m_MeasureStart = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT3 m_MeasureEnd = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT2 m_SceneMarqueeStart = { 0.0f, 0.0f };
		DirectX::XMFLOAT2 m_SceneMarqueeEnd = { 0.0f, 0.0f };
		MeshMeasureAccelerationCache m_MeshMeasureAccelerationCache;
		size_t m_LastMeshMeasureTrianglesTested = 0;
		size_t m_LastMeshMeasureTriangleCount = 0;
		size_t m_LastMeshMeasureCacheTriangleCount = 0;
		double m_LastMeshMeasureRaycastMs = 0.0;
		double m_LastMeshMeasureCacheBuildMs = 0.0;
		bool m_LastMeshMeasureUsedBudget = false;
		bool m_LastMeshMeasureUsedAcceleration = false;
		bool m_LastMeshMeasureUsedDynamicAcceleration = false;
		bool m_LastMeshMeasureCacheRebuilt = false;
		bool m_LastMeshMeasureBoundsRejected = false;
		bool m_LastMeshMeasureHit = false;
	};
}
