# Engine Platformer Project

Win32 기반의 실시간 렌더링 엔진 프로토타입입니다. DirectX 12와 Vulkan을 모두 지원하는 RHI(Render Hardware Interface)를 중심으로, Assimp 모델 로딩, ImGui 기반 Unity 스타일 에디터 셸, Scene/Game 뷰 분리, ECS 벤치마크 샘플을 실험하고 있습니다.

현재 목표는 “작게 동작하는 렌더링 엔진”에서 출발해, 에디터와 런타임 구조를 점진적으로 분리하고 확장 가능한 엔진 아키텍처로 발전시키는 것입니다.

## 주요 특징

- **멀티 그래픽 API**
  - DirectX 12 / Vulkan 백엔드 지원
  - 런타임에서 그래픽 API 전환 가능
  - Forward, Deferred, Forward+ 렌더 모드 UI 제공

- **Unity 스타일 ImGui 에디터 셸**
  - DockSpace 기반 `Scene`, `Game`, `Hierarchy`, `Inspector`, `Project`, `Benchmark`, `Console` 패널
  - Scene 카메라와 Game 카메라 분리
  - Hierarchy 선택, Inspector Transform 편집, Project asset browse 지원

- **Scene / Asset / Rendering 분리**
  - 엔티티, Transform, Bounds, Mesh, Camera, Light 컴포넌트 기반 Scene 구조
  - Assimp 기반 Spider 샘플 모델 로딩
  - diffuse/base color 텍스처 resolve 및 GPU 업로드

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

- **성능 실험 기반**
  - ECS Benchmark를 통해 데이터 구조와 시스템 업데이트 비용을 비교할 수 있습니다.
  - 대량 오브젝트 렌더링에서는 CPU materialized count와 GPU/procedural draw 의미를 분리해 관찰할 수 있습니다.

- **확장 가능한 폴더 구조**
  - `Core`, `Rendering`, `Scene`, `Assets`, `Editor`, `ECS`, `Samples` 등 도메인별로 코드가 나뉘어 있습니다.

## 기술 스택

- C++ latest mode on MSVC
- Win32
- DirectX 12
- Vulkan
- ImGui docking
- Assimp
- glslang
- stb
- vcpkg

## 프로젝트 구조

```text
Src/
  App/Win32                 Win32 진입점과 애플리케이션 창
  Core/Engine               엔진 생명주기, 렌더 루프, API 전환
  Rendering                 RHI, DX12/Vulkan backend, 렌더 리소스
  Scene                     엔티티, 컴포넌트, 피킹, 씬 상태
  Assets                    Assimp 모델 로더와 StaticMesh 데이터
  Editor                    ImGui 기반 에디터 셸
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
MSBuild DX12Eninge.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
MSBuild DX12Eninge.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64
```

빌드 산출물은 `x64/Debug/EnginePlatformer.exe` 또는 `x64/Release/EnginePlatformer.exe`에 생성됩니다.

## 현재 샘플

- **Spider Sample**
  - Assimp로 Spider 모델을 로드하고, 여러 복제 오브젝트와 투명 머티리얼을 렌더링합니다.
  - Inspector에서 Transform과 mesh/material 정보를 확인할 수 있습니다.

- **ECS Benchmark**
  - `Non-ECS`와 `ECS` 모드를 전환하며 CPU update/render collect 비용을 비교합니다.
  - Primitive와 Spider 타입을 선택해 단순 오브젝트와 실제 asset 기반 부하를 함께 확인합니다.

## 향후 방향성

- **에디터 고도화**
  - Scene/Game을 실제 offscreen render texture로 분리
  - transform gizmo, asset drag/drop, prefab/scene 저장 기능 추가
  - Inspector rotation을 quaternion 직접 편집에서 Euler/gizmo 기반으로 개선

- **렌더링 확장**
  - PBR material pipeline
  - normal / metallic / roughness texture 실제 셰이딩 반영
  - shadow, light system, post-processing
  - DX12/Vulkan 리소스 관리 계층 정리

- **ECS 통합**
  - benchmark 전용 ECS를 실제 Scene runtime과 단계적으로 연결
  - archetype/chunk query 기반 시스템 확장
  - serialization, editor integration, job system 검토

- **엔진 안정화**
  - 렌더러/에디터/샘플 간 책임 분리 강화
  - 자동화된 smoke test와 렌더링 regression test 추가
  - CI 빌드 구성

## 현재 상태

이 프로젝트는 학습과 실험을 겸한 엔진 프로토타입입니다. API 안정성보다는 구조 개선, 렌더링 백엔드 실험, 에디터 워크플로우 검증에 초점을 두고 있습니다.
