# Engine Platformer Project

Win32 기반의 실시간 게임 엔진 프로토타입입니다. DirectX 12와 Vulkan을 모두 지원하는 RHI(Render Hardware Interface)를 중심으로, Unity 스타일 ImGui 에디터, 프로젝트 런처, Scene/Game 뷰 분리, 실시간 asset import/hot reload, PhysX 물리, ECS 벤치마크, 커스텀 메모리 할당자, Job System, RenderGraph 기반 렌더링 파이프라인을 단계적으로 구축하고 있습니다.

현재 목표는 “작게 동작하는 렌더링 엔진”에서 출발해, 에디터와 런타임 구조를 점진적으로 분리하고 실제 프로젝트를 만들고 저장하고 다시 열 수 있는 엔진 아키텍처로 발전시키는 것입니다.

## 주요 특징

### 멀티 그래픽 API와 렌더링 파이프라인

- DirectX 12 / Vulkan 백엔드 지원
- 런타임에서 그래픽 API 전환 가능
- Forward, Deferred, Forward+ 렌더 모드 UI 제공
- Forward / Forward+는 실시간 light 8개 고정 경로
- Deferred는 growable light buffer와 tiled light list를 사용해 8개 고정 제한 없이 확장 가능한 경로
- RenderGraph 기반 frame pass 구성과 pass별 CPU timing 수집
- HDR render target + ACES tone mapping + exposure/gamma correction
- Shadow map 시스템과 shadow debug view
- View frustum culling, draw/triangle/instance 통계, renderer health snapshot 제공

### Material / Texture / Lighting

- `PBR`, `Phong`, `Unlit` material shading model 지원
- BaseColor, Normal, Opacity, Emissive, Metallic, Roughness, MetallicRoughness, AO, Specular, Shininess texture slot 지원
- Assimp material 분석 기반 texture 자동 매핑과 fallback texture 제공
- imported texture의 sRGB/linear slot 구분
- primitive white diffuse fallback 및 material diffuse color 관리
- Normal Y Flip, Use Vertex Color, material debug view 지원
- Directional / Point / Spot light 렌더링
- Ambient color/intensity, exposure, key light 대표값을 Editor에서 조절 가능

### Unity 스타일 ImGui 에디터 셸

- DockSpace 기반 `Scene`, `Game`, `Hierarchy`, `Inspector`, `Project`, `Benchmark`, `Console` 패널
- Scene 카메라와 Game 카메라 분리
- Scene View에서 GameCamera frustum gizmo, collider gizmo 표시
- Hierarchy 선택, 우클릭 context menu, rename/duplicate/delete 단축키 지원
- Hierarchy drag/drop reorder 지원
- Inspector Transform 편집, Add Component, component on/off, component remove 지원
- File 메뉴, Save/Open Scene, Reveal Project, dirty state 표시
- Renderer Roadmap Health 섹션으로 렌더링 핵심 기능 상태를 Console에서 확인 가능

### Component 기반 Scene 구조

- `EntityId` 기반 flat hierarchy
- `Name`, `Transform`, `Bounds`, `Mesh`, `Camera`, `Light`, `Animator`, `RigidBody`, `Collider`, `PhysicsMaterial` 컴포넌트 지원
- `SceneComponentStore` 기반 generic component API
- Transform은 필수 컴포넌트로 유지하고, 나머지 optional component는 Inspector에서 추가/삭제/활성화 가능
- Rename, Duplicate, Delete, scene 저장/로드 시 component 데이터와 enabled 상태 보존

### 프로젝트 런처와 Scene 저장

- `EngineLauncher.exe`에서 새 프로젝트 생성, 기존 프로젝트 열기, 최근 프로젝트 실행 지원
- `EnginePlatformer.exe --project "<project.engineproject>"` 프로젝트 모드 지원
- 새 프로젝트 생성 시 경량 프로젝트 구조 생성
- `.scene` JSON 저장/불러오기
- 프로젝트 모드에서는 빈 Project Scene + 기본 Camera/Light로 시작
- 직접 실행 시 개발용 샘플 모드 유지

새 프로젝트 기본 구조는 다음과 같습니다.

```text
MyGame/
  MyGame.engineproject
  Assets/
  Scenes/
    Main.scene
  Settings/
  Library/
  Temp/
  .gitignore
```

### 실시간 Asset File System

- Project 패널에서 project `Assets` 트리 캐시 표시
- 파일/폴더 선택, 열기, Explorer reveal, refresh 지원
- FBX/OBJ/GLTF/GLB 모델 drag/drop import 지원
- Windows Explorer에서 엔진 창으로 모델 파일 drop 지원
- import worker thread와 completion queue 기반 비동기 모델 로딩
- 현재 scene에 로드된 모델/텍스처 대상 hot reload 지원
- import 실패 시 Assimp error, mesh/material/animation count, vertex/face/index count, 추정 실패 원인을 Console에 출력

### Animation / Animator

- FBX animation clip import
- `AnimatorComponent`로 clip 선택, 재생/정지, loop, speed, time scrub 관리
- Entity별 CPU skinning 경로
- Duplicate 시 Animator 상태 독립 복사
- Hot reload 후 clip index/time 안전 clamp

### PhysX 기반 Physics v1

- CPU PhysX backend 기반 rigid body simulation
- Static / Dynamic / Kinematic Rigidbody 지원
- Box / Sphere / Capsule / Plane Collider 지원
- Gravity, fixed timestep, Transform 동기화
- Inspector 물리 컴포넌트 편집
- Simulate Physics on/off와 edit-mode Transform snapshot restore
- primitive 생성 시 Unity식 Collider-only static 기본값 적용

### ECS Benchmark

- Non-ECS AoS 방식과 ECS archetype/chunk 기반 방식 비교
- Primitive / Spider 타입 지원
- 100, 1,000, 10,000, 100,000, 1,000,000, 10,000,000 오브젝트 스케일 테스트 UI
- CPU update, render collect, frame time, FPS 분리 표시
- 대량 오브젝트 테스트에서 materialized count와 GPU/procedural 의미를 구분

### Memory / Jobs

- `TrackedAllocator`, `LinearFrameAllocator`, `FixedBlockPoolAllocator` 기반 커스텀 메모리 모듈
- tag별 allocation/current/peak/leak 통계 출력
- Editor Console Memory 섹션과 shutdown leak report
- `JobSystem` 기반 worker pool, `JobHandle`, dependency wait, `ParallelFor`
- `FramePhaseScheduler`로 BeginFrame, Start, FixedUpdate, Update, LateUpdate, Animation, Physics, RenderPrepare, Commit, EndFrame 순서 보장
- worker thread는 Scene/GPU를 직접 수정하지 않고, 병렬 계산 후 main-thread commit 구조 사용

### 자동 Smoke Test

- CLI smoke mode로 렌더링 backend와 render mode를 빠르게 검증
- smoke 실행 시 테스트 primitive를 생성하고 Scene/Game 카메라를 프레이밍한 뒤 renderer health를 로그로 남김
- Vulkan / DX12, Forward / Deferred / Forward+ 조합 확인에 사용

```powershell
x64/Release/EnginePlatformer.exe --smoke-test=3 --smoke-api=vulkan --smoke-render-mode=deferred --smoke-log C:/tmp/engine-smoke-vulkan-deferred.log
x64/Release/EnginePlatformer.exe --smoke-test=3 --smoke-api=dx12 --smoke-render-mode=forward+ --smoke-log C:/tmp/engine-smoke-dx12-forwardplus.log
```

## 장점

- **RHI 학습과 실험에 적합**
  - DX12와 Vulkan을 같은 엔진 흐름 안에서 비교하며 구현할 수 있습니다.
  - API별 차이는 backend에 모으고, 상위 Engine/Renderer 흐름은 공통 구조를 유지합니다.

- **에디터와 런타임을 함께 검증**
  - 단순 샘플 렌더링이 아니라 Scene/Game/Inspector/Hierarchy를 통해 실제 엔진 편집 흐름을 실험합니다.
  - 프로젝트 단위 asset root, scene 저장, hierarchy 편집, component 편집, physics simulation을 한 흐름에서 검증할 수 있습니다.

- **데이터 지향 구조와 성능 실험 기반**
  - ECS Benchmark, RenderGraph timing, draw stats, memory stats, job scheduling을 통해 병목을 관찰할 수 있습니다.
  - 대량 오브젝트와 대량 light 시나리오를 Forward/Deferred 경로에서 비교할 수 있습니다.

- **확장 가능한 폴더 구조**
  - `Core`, `Rendering`, `Scene`, `Assets`, `Editor`, `Physics`, `Projects`, `Jobs`, `Memory`, `ECS`, `Samples` 등 도메인별로 코드가 분리되어 있습니다.

## 기술 스택

- C++ latest mode on MSVC
- Win32
- DirectX 12
- Vulkan
- ImGui docking
- Assimp
- PhysX
- glslang
- stb
- rapidjson
- vcpkg

## 프로젝트 구조

```text
Src/
  App/Win32                 Win32 진입점과 애플리케이션 창
  Core/Engine               엔진 생명주기, 렌더 루프, phase orchestration
  Rendering                 RHI, renderer, render state, mesh draw path
  Rendering/Backends        DX12 / Vulkan backend 구현
  Rendering/Graph           RenderGraph와 pass timing
  Rendering/Lighting        Light 수집, shadow system, shadow resources
  Rendering/Post            HDR post process와 tone mapping
  Rendering/Resources       GPU buffer/texture/material resource 관리
  Materials                 material description, texture slot, shader variant
  Resources                 runtime resource registry와 통계
  Scene                     엔티티, 컴포넌트, 피킹, scene persistence
  Assets                    Assimp loader, asset file system, import/hot reload
  Editor                    ImGui 기반 에디터 셸과 패널 UI
  Physics                   PhysX backend, PhysicsWorld, physics components
  Projects                  project descriptor와 생성/로드 서비스
  Jobs                      worker pool, job handle, phase scheduler
  Memory                    custom allocator, memory stats, STL adapter
  ECS                       benchmark용 ECS 구조
  Samples/Benchmark         ECS / Non-ECS 비교 벤치마크
  Samples/Spider            개발용 Spider 샘플 씬
  Math                      Camera, Transform, math helper
  Input                     입력 처리
  Utilities                 셰이더/공통 유틸리티
```

## 빌드 및 실행

### 요구 사항

- Windows
- Visual Studio / MSVC v145 호환 툴셋
- Windows 10 SDK
- vcpkg
- Vulkan runtime 또는 Vulkan SDK

### 의존성 설치

프로젝트 루트의 `vcpkg.json`에 필요한 패키지가 정의되어 있습니다.

```powershell
vcpkg install --triplet x64-windows
```

`Directory.Build.props`는 다음 순서로 vcpkg 설치 경로를 찾습니다.

1. 프로젝트 로컬 `vcpkg_installed/<triplet>`
2. `VCPKG_ROOT/installed/<triplet>`

Visual Studio vcpkg manifest 자동 빌드는 끄고, 프로젝트 설정에서 include/lib 경로를 직접 해석합니다. 런타임 DLL 복사에서도 `vulkan-1.dll`은 제외하며, Vulkan loader는 시스템 설치 runtime을 사용합니다.

### 빌드

Visual Studio에서 `EningePlatformer.slnx`를 열어 빌드하거나, MSBuild로 빌드할 수 있습니다.

```powershell
MSBuild EningePlatformer.slnx /p:Configuration=Debug /p:Platform=x64
MSBuild EningePlatformer.slnx /p:Configuration=Release /p:Platform=x64
```

빌드 산출물은 `x64/Debug` 또는 `x64/Release` 아래에 생성됩니다.

```text
x64/Debug/EnginePlatformer.exe
x64/Debug/EngineLauncher.exe
x64/Release/EnginePlatformer.exe
x64/Release/EngineLauncher.exe
```

### 실행

엔진을 직접 실행하면 개발용 기본 모드로 열립니다.

```powershell
x64/Debug/EnginePlatformer.exe
```

런처를 실행하면 새 프로젝트 생성, 기존 프로젝트 열기, 최근 프로젝트 실행을 사용할 수 있습니다.

```powershell
x64/Debug/EngineLauncher.exe
```

프로젝트 파일을 직접 지정해 엔진을 열 수도 있습니다.

```powershell
x64/Debug/EnginePlatformer.exe --project "D:/Projects/MyGame/MyGame.engineproject"
```

## 현재 샘플

- **Project Scene**
  - 프로젝트 모드의 기본 작업 씬입니다.
  - Camera/Light/primitive/model entity를 만들고, Inspector에서 component를 추가/비활성/삭제할 수 있습니다.
  - `.scene` 저장/로드, hierarchy reorder, asset drag/drop import, hot reload, physics simulation을 검증합니다.

- **Spider Sample**
  - 개발용 Assimp animated mesh 샘플입니다.
  - Animator, mesh/material, texture resolve, 복제 entity 렌더링을 확인하는 데 사용합니다.

- **ECS Benchmark**
  - `Non-ECS`와 `ECS` 모드를 전환하며 CPU update/render collect 비용을 비교합니다.
  - Primitive와 Spider 타입을 선택해 단순 오브젝트와 실제 asset 기반 부하를 함께 확인합니다.

## 최근 검증 상태

2026-06-17 기준으로 다음 항목을 확인했습니다.

- `Debug|x64` 빌드 성공, 경고 0개
- `Release|x64` 빌드 성공, 경고 0개
- Vulkan Forward / Deferred / Forward+ smoke test 성공
- DX12 Forward / Deferred / Forward+ smoke test 성공
- Deferred 경로에서 growable light buffer, tiled light culling, HDR/tone mapping, shadow/debug/stat path 동작 확인
- Vulkan shutdown 시 stale buffer lifetime 문제 수정

## 향후 방향성

- **에디터 고도화**
  - Scene/Game을 실제 offscreen render texture로 완전히 분리
  - transform gizmo, parent/child hierarchy, reparent drag/drop
  - Undo/Redo command stack
  - Play/Edit mode 분리
  - Prefab v1과 prefab override 추적

- **렌더링 확장**
  - IBL, reflection probe, irradiance/prefiltered environment map
  - shadow cascade, soft shadow, shadow bias UI
  - clustered/Forward+ light culling 고도화
  - GPU skinning, GPU-driven rendering, indirect draw
  - material graph, texture compression, mip/import setting
  - render target 기반 Scene/Game viewport와 picking buffer 분리

- **Physics 확장**
  - raycast/shape cast scene query
  - trigger event, collision callback
  - mesh collider, convex collider
  - character controller, joints

- **Animation 확장**
  - Animator Controller
  - blend tree, transition, animation event
  - root motion
  - animation retargeting

- **Asset / Project 확장**
  - asset dependency graph
  - import setting cache
  - prefab asset과 material asset 분리
  - scene diff/merge 친화 포맷 개선

- **엔진 안정화**
  - 자동화된 렌더링 regression test
  - CI 빌드와 smoke matrix
  - crash-safe save, backup scene
  - profiler panel과 frame capture tooling

## 현재 상태

이 프로젝트는 학습과 실험을 겸한 엔진 프로토타입입니다. API 안정성보다는 구조 개선, 렌더링 백엔드 실험, 에디터 워크플로우 검증에 초점을 두고 있습니다. 다만 이제는 프로젝트 생성, asset import, scene 저장, component editing, physics simulation, material/lighting debug, smoke test까지 이어지는 기본 엔진 루프를 갖춘 상태입니다.
