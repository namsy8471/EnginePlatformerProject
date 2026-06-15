# Engine Platformer Project

Win32 기반의 실시간 렌더링 엔진 프로토타입입니다. DirectX 12와 Vulkan을 모두 지원하는 RHI(Render Hardware Interface)를 중심으로, Assimp 모델 로딩, ImGui 기반 Unity 스타일 에디터 셸, Scene/Game 뷰 분리, 프로젝트 런처, 실시간 asset import/hot reload, PhysX 기반 물리, ECS 벤치마크 샘플을 실험하고 있습니다.

현재 목표는 “작게 동작하는 렌더링 엔진”에서 출발해, 에디터와 런타임 구조를 점진적으로 분리하고 확장 가능한 엔진 아키텍처로 발전시키는 것입니다.

## 주요 특징

- **멀티 그래픽 API**
  - DirectX 12 / Vulkan 백엔드 지원
  - 런타임에서 그래픽 API 전환 가능
  - Forward, Deferred, Forward+ 렌더 모드 UI 제공

- **Unity 스타일 ImGui 에디터 셸**
  - DockSpace 기반 `Scene`, `Game`, `Hierarchy`, `Inspector`, `Project`, `Benchmark`, `Console` 패널
  - Scene 카메라와 Game 카메라 분리
  - Hierarchy 선택, 우클릭 context menu, 단축키 rename/duplicate/delete 지원
  - Inspector Transform 편집, Add Component, component on/off, component remove 지원
  - Scene View에서 GameCamera frustum과 collider gizmo 표시

- **Scene / Asset / Rendering / Physics 분리**
  - 엔티티, Transform, Bounds, Mesh, Camera, Light 컴포넌트 기반 Scene 구조
  - Animator, Rigidbody, Collider, PhysicsMaterial 컴포넌트 지원
  - Assimp 기반 FBX/OBJ/GLTF/GLB 모델 import
  - diffuse/base color 텍스처 resolve 및 GPU 업로드
  - primitive 생성, white diffuse fallback, Phong illumination 지원

- **프로젝트 런처와 Scene 저장**
  - `EngineLauncher.exe`에서 새 프로젝트 생성, 기존 프로젝트 열기, 최근 프로젝트 실행 지원
  - `EnginePlatformer.exe --project "<project.engineproject>"` 프로젝트 모드 지원
  - `.scene` JSON 저장/불러오기, dirty state, save/open/reveal project 메뉴 지원
  - 프로젝트 모드에서는 빈 Project Scene + 기본 Camera/Light로 시작

- **실시간 Asset File System**
  - Project 패널에서 project `Assets` 트리 캐시 표시
  - 모델 파일 drag/drop import 및 Windows Explorer drop 지원
  - import worker thread와 completion queue 기반 비동기 모델 로딩
  - 현재 scene에 로드된 모델/텍스처 대상 hot reload 지원

- **PhysX 기반 Physics v1**
  - static/dynamic/kinematic Rigidbody, Box/Sphere/Capsule/Plane Collider 지원
  - fixed timestep simulation, gravity, Transform 동기화
  - Inspector 물리 컴포넌트 편집과 simulate on/off snapshot restore 지원
  - primitive 생성 시 Unity식 Collider-only static 기본값 적용

- **ECS 벤치마크**
  - Non-ECS AoS 방식과 ECS 방식 비교
  - Primitive / Spider 타입 지원
  - 100, 1,000, 10,000, 100,000, 1,000,000, 10,000,000 오브젝트 스케일 테스트 UI
  - CPU update, render collect, frame time, FPS 분리 표시

- **현대 C++ 코드베이스**
  - MSVC `stdcpplatest` 설정 사용
  - RHI 생성/소유권은 `std::unique_ptr` 기반
  - `std::span`, `std::format`, `std::source_location`, `std::ranges`, `std::to_underlying` 등 현대 C++ 기능 일부 적용

## 장점

- **RHI 학습과 실험에 적합**
  - DX12와 Vulkan을 같은 엔진 흐름 안에서 비교하며 구현할 수 있습니다.
  - API별 차이는 backend에 모으고, 상위 Engine/Renderer 흐름은 공통 구조를 유지합니다.

- **에디터와 런타임을 함께 검증**
  - 단순 샘플 렌더링이 아니라 Scene/Game/Inspector/Hierarchy를 통해 실제 엔진 편집 흐름을 실험합니다.
  - Scene 카메라와 Game 카메라를 분리해 에디터형 워크플로우의 기본 구조를 갖췄습니다.
  - 프로젝트 단위 asset root, scene 저장, hierarchy 편집, component 편집을 한 흐름에서 검증할 수 있습니다.

- **성능 실험 기반**
  - ECS Benchmark를 통해 데이터 구조와 시스템 업데이트 비용을 비교할 수 있습니다.
  - 대량 오브젝트 렌더링에서는 CPU materialized count와 GPU/procedural draw 의미를 분리해 관찰할 수 있습니다.

- **확장 가능한 폴더 구조**
  - `Core`, `Rendering`, `Scene`, `Assets`, `Editor`, `Physics`, `Projects`, `ECS`, `Samples` 등 도메인별로 코드가 나뉘어 있습니다.

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
  Core/Engine               엔진 생명주기, 렌더 루프, API 전환
  Rendering                 RHI, DX12/Vulkan backend, 렌더 리소스
  Scene                     엔티티, 컴포넌트, 피킹, 씬 상태
  Assets                    Assimp 모델 로더, asset file system, import/hot reload
  Editor                    ImGui 기반 에디터 셸
  Physics                   PhysX backend, PhysicsWorld, physics components
  Projects                  프로젝트 descriptor와 생성/로드 서비스
  ECS                       ECS v1/v2 실험 구조
  Samples/Spider            Spider 샘플 씬
  Samples/Benchmark         ECS / Non-ECS 비교 벤치마크
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

### 빌드

Visual Studio에서 `EningePlatformer.slnx`를 열어 빌드하거나, MSBuild로 빌드할 수 있습니다.

```powershell
MSBuild EningePlatformer.slnx /p:Configuration=Debug /p:Platform=x64
MSBuild EningePlatformer.slnx /p:Configuration=Release /p:Platform=x64
```

빌드 산출물은 `x64/Debug/EnginePlatformer.exe`, `x64/Debug/EngineLauncher.exe` 또는 `x64/Release` 아래에 생성됩니다.

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
  - Assimp로 Spider 모델을 로드하고, 여러 복제 오브젝트와 투명 머티리얼을 렌더링합니다.
  - Inspector에서 Transform과 mesh/material 정보를 확인할 수 있습니다.

- **ECS Benchmark**
  - `Non-ECS`와 `ECS` 모드를 전환하며 CPU update/render collect 비용을 비교합니다.
  - Primitive와 Spider 타입을 선택해 단순 오브젝트와 실제 asset 기반 부하를 함께 확인합니다.

## 향후 방향성

- **에디터 고도화**
  - Scene/Game을 실제 offscreen render texture로 분리
  - transform gizmo, prefab, parent/child hierarchy 기능 추가
  - Inspector rotation을 quaternion 직접 편집에서 Euler/gizmo 기반으로 개선
  - Undo/Redo command stack과 Play/Edit mode 분리

- **렌더링 확장**
  - PBR material pipeline
  - normal / metallic / roughness texture 실제 셰이딩 반영
  - shadow, light system, post-processing
  - DX12/Vulkan 리소스 관리 계층 정리

- **ECS 통합**
  - benchmark 전용 ECS를 실제 Scene runtime과 단계적으로 연결
  - archetype/chunk query 기반 시스템 확장
  - serialization, editor integration, job system 검토

- **Asset / Animation 확장**
  - Animator Controller, blend tree, animation event
  - mesh collider, prefab asset, asset dependency graph

- **엔진 안정화**
  - 렌더러/에디터/샘플 간 책임 분리 강화
  - 자동화된 smoke test와 렌더링 regression test 추가
  - CI 빌드 구성

## 현재 상태

이 프로젝트는 학습과 실험을 겸한 엔진 프로토타입입니다. API 안정성보다는 구조 개선, 렌더링 백엔드 실험, 에디터 워크플로우 검증에 초점을 두고 있습니다.
