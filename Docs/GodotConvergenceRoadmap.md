# Godot Convergence Roadmap

이 문서는 현재 엔진을 Godot 수준의 제작 워크플로우로 수렴시키기 위한 7개 축의 실행 계획과 현재 v1 기반 상태를 정리한다.

## 1. Prefab / Nested Scene / Resource

- 목표: entity 구성을 재사용 가능한 prefab/resource로 저장하고, scene 안에서 다른 scene을 additive/nested 형태로 참조한다.
- v1 실행 상태:
  - `PrefabInstanceComponent`와 `SceneReferenceComponent`를 Scene component로 추가했다.
  - `PrefabService`는 선택 entity를 entity 1개짜리 scene JSON 형태의 `.prefab`으로 저장/로드할 수 있다.
  - `.scene` 저장/로드는 prefab instance와 scene reference component를 보존한다.
  - File 메뉴의 `Save Selected As Prefab`은 `Assets/Prefabs/<Entity>.prefab`을 생성하고 선택 entity에 prefab reference를 붙인다.
  - Project 패널에서 `.prefab`을 열면 현재 SceneCamera 앞에 prefab instance를 생성한다.
  - `SceneReferenceComponent` Inspector에 `Open Scene`, `Reveal`, `Load As Children`, `Unload Loaded` 액션을 추가했다.
  - Inspector에서 resolved scene path, loaded 여부, loaded entity 수, auto reload watch 상태를 확인할 수 있다.
  - `Load As Children`은 참조 `.scene`의 root entity들을 SceneReference entity 아래 child로 붙이고, 참조 scene 내부 hierarchy를 유지한다.
  - `Reload`는 이미 로드된 child 묶음을 언로드한 뒤 같은 참조 scene에서 다시 생성한다.
  - `Unload Loaded`는 현재 세션에서 해당 SceneReference가 로드한 entity 묶음을 제거한다.
  - Scene load/restore 시 `Auto Load`가 켜진 enabled SceneReference를 자동으로 `Load As Children` 처리한다.
  - 자동/수동으로 확장된 nested scene child들은 scene save/autosave/play snapshot에서 제외해 다음 로드 때 중복 저장되지 않게 했다.
  - 로드된 referenced scene 파일의 `last_write_time`을 1초 간격으로 감지해 변경 시 dirty 상태를 오염시키지 않고 자동 reload한다.
  - 현재 scene이 dirty이면 외부 변경 reload를 pending 상태로 보류하고, 저장 후 또는 수동 Reload로 반영하게 했다.
  - Hierarchy는 runtime-expanded child에 `[Nested]` 배지를 표시하고, Inspector는 owner SceneReference/source scene/save-excluded 상태를 보여준다.
  - Hierarchy quick filter로 nested child만 즉시 좁혀 볼 수 있어 SceneReference 확장 결과를 추적하기 쉽다.
  - Hierarchy `Expand All` / `Collapse All`과 branch 단위 expand/collapse는 nested scene과 prefab instance처럼 깊어진 tree를 빠르게 탐색하기 위한 기본 Outliner 조작으로 제공된다.
  - Nested child 우클릭 메뉴와 Inspector에서 owner 선택, source scene 열기, source reload/unload를 바로 실행할 수 있다.
  - Nested child는 Undo/Redo 가능한 `Make Local`로 SceneReference 추적에서 분리해 현재 씬에 저장되는 일반 Entity/subtree로 전환할 수 있다.
  - Command Palette에서도 선택 Entity의 referenced scene open/load/reload/unload 명령을 실행할 수 있다.
  - Prefab Instance 선택 시 Inspector 상단에 reflected override 개수와 영향 component badge를 표시하는 override marker v1을 추가했다.
  - Entity 이름 override는 Inspector 상단의 `Revert Name` / `Apply Name` 버튼으로 prefab source와 바로 동기화할 수 있다.
   - Transform과 optional component 섹션 헤더에도 `[Override]` 배지를 표시해 어느 섹션이 prefab source와 다른지 바로 찾을 수 있다.
   - Transform override는 헤더 옆 `Revert` / `Apply` 버튼으로 prefab source 값으로 되돌리거나 현재 Transform을 prefab source에 반영할 수 있다.
   - Optional component 섹션의 `...` 메뉴에서 해당 component override를 prefab 값으로 `Revert`하거나 현재 component 값을 prefab source에 `Apply`할 수 있다.
   - Reflection Quick Edit는 prefab source와 다른 reflected property row에 `[Override]` 배지와 current/source tooltip을 표시한다.
   - Mesh 섹션은 prefab source와 다른 asset path/material count를 protected override 진단으로 표시해 위험한 자동 교체를 피한다.
   - Mesh asset path/material count 변경은 `Apply Mesh To Prefab...` 확인 모달을 통해 current mesh metadata와 material override 배열만 prefab source에 명시 반영할 수 있다.
   - `Revert Mesh From Prefab...`은 확인 모달 후 prefab Mesh를 현재 Entity에 되돌리며, 모델 파일은 async import queue를 사용하고 현재 이름/Transform을 유지한다.
   - Mesh replacement revert는 undo/redo command로 기록되며, 모델 asset은 captured Mesh snapshot restore job을 다시 큐잉하는 방식으로 복원한다.
   - Inspector multi-object optional component editing은 Camera, Physics Material, Prefab Instance, Scene Reference, Script, Sprite 2D, UI Element, Audio Source, Navigation Agent, Network Identity까지 확장했고, Camera는 FOV/Near/Far만 공유하며 GameCamera 역할은 Entity별로 유지한다.
   - Prefab Instance, Scene Reference, Script, Sprite 2D, UI Element, Audio Source, Network Identity multi-edit은 Prefab Path/Source Name/Scene Path/Script Path/Texture/Text/Clip/Network Id/Prefab Key 값을 Entity별로 보존해 대량 속성 편집 중 개별 reference/text/id가 의도치 않게 덮이지 않도록 했다.
   - async Mesh restore에는 Entity별 restore generation을 붙여 undo/redo 중 늦게 도착한 이전 import 결과가 현재 Mesh를 덮어쓰지 못하게 했다.
   - Inspector Mesh 섹션은 async Mesh restore의 Pending/Failed/Cancelled 상태, source path, generation을 표시하고 pending restore를 취소할 수 있다.
   - async Mesh restore 완료 시 queue 시점의 current Mesh 시그니처와 prefab source timestamp를 비교해, 현재 Entity Mesh 변경이나 source prefab 변경이 있으면 `Conflict` 상태로 결과 적용을 막는다.
   - conflict 상태에서는 `Apply Anyway`, `Reload Prefab Source`, `Keep Current` 액션으로 저장된 import 결과 적용, prefab 재로딩, 현재 상태 유지 중 하나를 선택할 수 있다.
   - conflict preview는 current Mesh와 stored restore의 source/count를 보여주고, overwrite 위험이 있는 `Apply Anyway`와 `Reload Prefab Source`는 확인 모달을 거친다.
   - conflict preview는 material name/shading/texture slot 변경을 `Material Slot Diff`로 접어 볼 수 있게 표시한다.
   - `Material Diff`는 Field / Current / Restore 3열 table로 표시하며 texture slot뿐 아니라 base/specular/emissive color, metallic/roughness/shininess/opacity, vertex color, normal Y flip factor 차이도 함께 보여준다.
   - `Material Diff` row를 클릭하면 해당 `Material[n]` row가 Materials 섹션에서 자동으로 열리고 잠깐 하이라이트된다.
   - texture slot diff row를 클릭하면 해당 `Material[n]`의 texture slot row까지 직접 스크롤되고 하이라이트된다.
   - material factor diff row를 클릭하면 Shading Model, Base Color, Vertex Color, Normal Y Flip, Emissive, Opacity, Metallic/Roughness, Specular/Shininess control까지 직접 스크롤되고 하이라이트된다.
   - `Material Diff`에 `Pin Focus`와 `Clear Focus`를 추가해 선택한 diff row의 Inspector focus 상태를 필요할 때 계속 유지할 수 있다.
   - async Mesh restore 상태에 source model/source prefab path validation을 추가해 파일 존재 여부를 `OK/Missing/Error`로 표시하고, 존재하는 파일은 `Open`/`Reveal`, 누락된 파일은 `Refresh Project`로 이어지게 했다.
   - Material texture slot은 저장된 texture path가 누락되면 `Missing texture dependency`와 Project image 후보 기반 `Suggested Remaps`를 표시하고, `Assign`으로 undo 가능한 texture assignment에 연결한다.
   - Materials 섹션에는 `Remap Filter`와 `Auto Remap Missing`을 추가해 후보를 텍스트로 좁히고, 현재 material의 missing texture slot들을 하나의 undo command로 일괄 remap할 수 있다.
   - Materials 섹션 상단에는 Mesh 전체 `Auto Remap All Missing`과 `Remap Preview`를 추가해 여러 material의 missing texture slot을 하나의 undo command로 일괄 복구할 수 있다.
   - Remap 후보는 `High`, `Medium`, `Low`, `Ambiguous` 신뢰도와 score를 표시하며, Mesh 전체 remap에 `Low`/`Ambiguous` 후보가 섞이면 확인 모달에서 위험 후보와 차순위 후보를 검토한 뒤 적용하게 했다.
   - `Remap Preview` row에는 candidate combo를 추가해 자동 후보 대신 다른 후보를 수동 선택할 수 있고, `Auto`/`Clear Candidate Choices`로 세션 내 선택을 되돌릴 수 있다.
   - Remap scoring은 Assimp importer의 nearby texture matching에 맞춰 same filename/stem, `.png/.tga` preferred variant, material/model token, `textures` folder, source model 주변 경로를 반영한다.
   - Materials 섹션은 material row와 texture slot 단위 override marker를 표시하고, source material이 있는 row는 `Revert Material` / `Apply Material To Prefab`을 지원한다.
  - Materials 섹션은 multi-select 상태에서 `Apply Scalars To Selected`를 제공해 현재 material row의 Shading Model, color/scalar factor, Use Vertex Color, Normal Y Flip 값을 같은 material index를 가진 선택 Entity들에 하나의 undo command로 적용한다.
  - material batch apply는 texture binding/path/embedded texture와 material name을 Entity별로 보존해 대량 재질 보정 중 개별 texture slot이 덮이지 않게 했다.
  - Project 패널은 선택 asset 기준 breadcrumb와 `Up` 버튼을 제공해 Godot FileSystem dock처럼 상위 폴더로 빠르게 돌아갈 수 있게 했다.
  - Project 패널의 `Scope Folder`는 선택 폴더를 임시 root로 렌더링해 Godot FileSystem dock의 현재 폴더 집중 탐색 흐름을 흉내낸다.
  - Project details와 폴더 context menu의 `Select Folder`, `Scope Here`, `Clear Scope`로 asset에서 containing folder 중심 탐색으로 바로 넘어갈 수 있다.
  - Project details의 `Search Name`, `Filter Type`, `Filter Extension`으로 선택 asset 주변의 같은 종류 파일을 빠르게 찾을 수 있다.
  - Project/Content Drawer active filter bar는 현재 `Scope`, `Type`, `Text` 필터와 drawer 검색어를 눈에 보이게 표시하고 개별 해제 버튼을 제공한다.
  - Project details의 `Directory Summary`는 선택 폴더의 direct/recursive asset 통계와 타입별 개수를 보여주고, 타입별 `Show`로 현재 폴더 안 해당 asset 종류만 즉시 드릴다운한다.
  - Project details의 Dependencies/References row는 클릭 선택, 더블클릭 open, context action을 지원해 resource dependency를 따라가며 검사할 수 있게 했다.
  - Project 패널과 Content Drawer는 Favorites/Folders/Models/Images/Scenes/Materials/Prefabs/Source/Text quick filter를 공유해 Godot FileSystem dock처럼 asset type별 탐색을 빠르게 한다.
  - Project 패널과 Content Drawer는 saved search 목록도 공유해 자주 쓰는 asset 검색어를 어느 쪽에서든 저장/재사용할 수 있다.
  - Project 패널은 Recent Assets 섹션을 제공해 최근 선택/열기/로드한 asset을 FileSystem dock처럼 빠르게 다시 찾을 수 있게 했다.
  - Content Drawer는 `Up/Down`, `PageUp/PageDown`, `Home/End`, `Enter`, `Ctrl+Enter` 결과 조작을 지원해 keyboard-first asset search 흐름을 제공한다.
  - Content Drawer는 Path/Name/Type/Size/Modified 정렬과 오름/내림 방향 전환을 지원해 큰 프로젝트에서도 quick asset drawer를 탐색용으로 쓸 수 있게 했다.
  - Content Drawer는 선택 asset details/preview pane을 토글할 수 있어 FileSystem dock의 inspector-like 확인 흐름을 빠른 drawer 안에서도 이어갈 수 있다.
  - Content Drawer 안에서도 favorite/unfavorite를 바로 토글할 수 있어 FileSystem dock과 quick drawer의 즐겨찾기 흐름이 끊기지 않는다.
  - Project/Content Browser는 asset absolute path, project-relative path, file name copy action을 제공해 외부 DCC/toolchain과 경로를 주고받기 쉽게 했다.
- 다음 단계:
   - importer/editor texture matching은 `Assets/TextureMatching.h`의 slot keyword, non-base-color filter, token ignore 규칙을 공유한다.
   - path candidate ordering과 embedded texture fallback까지 포함한 완전 공용 resolver 분리를 강화한다.
   - nested scene instance override 정책과 reload conflict UX를 확정한다.

## 2. Undo / Redo + Transform Gizmo + Play/Edit Mode

- 목표: 에디터 조작을 command 단위로 기록하고, Play mode는 edit scene snapshot과 runtime scene을 분리한다.
- v1 실행 상태:
  - `EditorCommandStack`을 추가해 Execute/Undo/Redo/RedoStack 기반을 만들었다.
  - `EditorPlayState`로 Edit/EnteringPlay/Play/Paused/ExitingPlay 상태를 정의했다.
  - Edit 메뉴와 `Ctrl+Z` / `Ctrl+Y`가 command stack을 호출한다.
  - Rename은 실제 undo/redo command로 기록된다.
  - Inspector Transform 편집은 drag commit 단위로 undo/redo command를 만든다.
  - Hierarchy 빈 공간의 primitive create와 Inspector Add Component는 undo command로 기록된다.
  - Duplicate/Delete는 `LoadedSceneEntity` snapshot을 사용해 undo/redo로 복원된다.
  - Component enable/remove는 command stack을 통해 undo/redo로 복원된다.
  - Material shading model, texture assign/clear, scalar/color 편집은 material snapshot 기반 undo/redo로 복원된다.
  - multi-select material scalar apply는 여러 Entity material snapshot을 하나의 undo/redo command로 복원한다.
  - Scene View에 선택 Entity용 global Translate/Rotate/Scale transform gizmo v1을 추가했다.
  - Scene View marquee selection을 추가해 viewport에서 여러 Entity를 박스 선택하고, Ctrl/Shift additive selection으로 Hierarchy multi-select와 연결한다.
  - View Cube face/edge/corner hotspot을 추가해 popup 없이 미니 3D cube 본체에서 SceneCamera 시점을 직접 전환한다.
  - Translate gizmo에 XY/XZ/YZ plane handle을 추가해 Scene View에서 평면 이동을 직접 조작할 수 있다.
  - Scale gizmo 중심에 uniform scale handle을 추가해 선택 Entity를 비율 유지 상태로 확대/축소할 수 있다.
  - Scale gizmo hover/active value badge를 추가해 axis/uniform scale 조작 중 현재 scale 값을 Scene View에서 확인할 수 있다.
  - Rotate gizmo에 X/Y/Z 회전 링을 추가해 회전 평면을 직접 보고 링 기반 hover/click으로 회전 조작을 시작할 수 있다.
  - View Cube hover label과 hand cursor affordance를 추가해 axis/face/edge/corner 클릭 대상을 Scene View 안에서 바로 확인할 수 있다.
  - Mesh surface measure는 static triangle AABB cache와 animated/skinned frame-local dynamic cache를 사용해 exact triangle 후보를 줄인다.
  - Profiler `Viewport Tools`에서 Mesh surface measure의 raycast/cache build 비용과 static/dynamic cache 상태를 확인할 수 있다.
  - Gizmo 드래그는 화면 overlay에서 즉시 TransformComponent를 갱신하고, 드래그 완료 시 기존 Transform undo command에 기록된다.
  - Toolbar `Play` / `Stop`은 `Temp/PlayModeSnapshot.scene` 파일 fallback과 in-memory edit scene snapshot을 만든 뒤, Play 진입 시 별도 `m_PlayScene` runtime clone을 만들고 Stop 시 edit scene을 보존해 Play 중 변경을 Edit scene으로 흘리지 않는다.
  - Frame phase의 script Start/Update/LateUpdate/EndFrame, animation, physics와 주요 render read path, picking은 `GetRuntimeScene()` accessor를 통해 실행 대상 Scene을 받으며 Play 중에는 `m_PlayScene`을 사용한다.
  - Play 중 `CanEditProjectScene`은 false로 내려 저장/생성/삭제 같은 edit-scene 명령을 잠그고, `CanControlPlayMode`로 Stop 조작은 유지한다.
  - Hierarchy, Inspector, Status Bar는 `ActiveSceneIsRuntimeClone` 상태를 기반으로 `Play Runtime Clone` / runtime-only 배지를 표시해 현재 보고 있는 Scene이 edit scene이 아니라 runtime clone임을 드러낸다.
  - Inspector runtime-only 카드에는 `Reset Runtime Clone` 빠른 액션을 넣어 Play 실험 상태를 toolbar로 돌아가지 않고 초기화할 수 있다.
  - `BlockEditSceneMutationDuringPlay` guard를 추가해 rename/duplicate/delete/create/component/material/transform/undo/redo command가 Play 중 edit scene에 기록되지 않게 했다.
  - Play 중 Transform, Material scalar/shading, component property edit commit은 edit stack 대신 `m_RuntimeCommandStack`에 기록해 Ctrl+Z/Ctrl+Y로 runtime clone 안에서만 되돌릴 수 있고, Stop/Reset 시 폐기된다.
  - Play 진입 전 dirty scene은 기존 Unsaved Scene dialog를 재사용해 Save / Don't Save / Cancel 선택을 받고, Cancel이면 Play 진입을 중단한다.
  - Toolbar, Command Palette, Console 명령과 `F5` Play/Stop, `F6` Pause/Resume, `F10` Step 단축키를 추가해 simulation phase를 멈춘 상태에서 editor/render 응답성을 유지하고 한 프레임씩 runtime clone을 진행할 수 있다.
  - Play Toolbar/Command Palette/Console `resetplay`는 현재 runtime clone을 잠긴 edit scene snapshot에서 다시 복제해 Stop 없이 실험 상태를 초기화한다.
- 다음 단계:
  - Mesh surface measure cache를 full BVH hierarchy로 확장할지 profiling 결과를 기준으로 검토한다.
  - Play 중 runtime-only Inspector 조작을 별도 runtime command로 정리해 실험용 변경과 edit-scene 변경의 UX 경계를 더 다듬는다.

## 3. Property Reflection / Serialization

- 목표: component property 정보를 에디터, serialization, scripting, prefab override가 공유한다.
- v1 실행 상태:
  - `Reflection::SceneComponentReflection`에 component descriptor와 property descriptor를 추가했다.
  - 새 optional component까지 metadata에 등록했다.
  - Inspector에 `Reflection Schema` drawer를 추가해 선택 Entity의 component descriptor, property 이름/타입, add/remove/disable metadata coverage를 확인할 수 있다.
  - Inspector에 `Reflection Quick Edit` drawer를 추가해 descriptor 기반으로 `Animator`, `Camera`, `Light`, `Rigidbody`, `Collider`, `Physics Material`, `Prefab Instance`, `Scene Reference`, `Script`, `Sprite2D`, `UI Element`, `Audio Source`, `Navigation Agent`, `Network Identity`의 안전한 bool/enum/숫자/vector/color/string/path 필드를 직접 편집할 수 있다.
  - Inspector optional component는 `Pin To Top` / `Unpin From Top`으로 자주 쓰는 component type을 상단에 고정할 수 있고, pin 상태는 프로젝트 editor state에 저장된다.
  - `Reflection Quick Edit`는 `Mesh`를 읽기 전용 진단 대상으로 포함해 asset source, primitive kind, vertex/index/submesh/material/animation count와 material row 목록을 표시한다.
  - Mesh material row의 `Focus` 액션으로 full `Materials` 섹션의 해당 material을 열고 하이라이트할 수 있다.
  - `Reflection Quick Edit`의 `Material Scalar Quick Edit`는 Shading Model, Base Color, Vertex Color, Normal Y Flip, Emissive, Opacity, Metallic/Roughness, Specular/Shininess 값을 기존 material undo/redo 경로로 편집한다.
  - Reflection 기반 material scalar edit는 texture slot을 직접 변경하지 않고 full `Materials` 섹션의 texture UI로 분리해 path/embedded texture binding을 안전하게 보존한다.
  - Reflection 기반 material scalar edit도 multi-select 상태에서 `Apply Scalars To Selected`를 제공하며, 같은 material index를 가진 선택 Entity들에 하나의 material batch undo command로 적용한다.
  - `Material Texture Slot Actions`는 Reflection Quick Edit 안에서 기존 `Browse`, `Clear`, Project drag/drop texture assignment 경로를 재사용해 texture slot을 안전하게 조작한다.
  - Reflection Quick Edit의 texture slot action도 prefab source와 다른 slot에 `[Override]` marker와 current/source tooltip을 표시한다.
  - Quick Edit에서 물리 component를 바꾸면 PhysX actor dirty callback을 호출해 simulation 중 변경도 반영된다.
  - path 필드는 직접 타이핑 대신 Project asset drag/drop, `Pick` asset popup, `Clear` 버튼으로 할당한다.
  - Inspector에 `Prefab Overrides` 진단 패널을 추가해 Prefab Instance의 source prefab과 현재 Entity의 reflected component/property 차이를 표로 확인할 수 있다.
  - `Prefab Overrides`에서 이름과 지원 component override를 prefab source 기준으로 되돌리는 `Revert Name`, `Revert <Component>`, `Revert All Supported` 액션을 추가했다.
   - Override 표의 각 row에 `Revert` / `Apply` 액션을 추가해 개별 reflected property를 prefab 값으로 되돌리거나 현재 값으로 prefab에 반영할 수 있다.
   - Override 표의 `<component>` row도 `Apply`를 지원해 현재 Entity에 추가/삭제된 optional component를 source prefab에 반영할 수 있다.
   - `Apply Current to Prefab` 액션을 추가해 현재 Entity 상태를 source prefab asset에 저장할 수 있다.
   - Apply 시 `PrefabInstance` link 자체는 prefab asset에 쓰지 않아 자기 자신을 참조하는 prefab을 피한다.
   - Mesh와 PrefabInstance link 자체는 import/link 부작용이 커서 v1 revert/apply 보호 대상으로 둔다.
   - Reflection Quick Edit의 property row에 prefab source와 다른 값은 `[Override]` marker로 표시하고 tooltip에서 current/source 값을 비교할 수 있다.
   - Mesh/material prefab override UX는 Mesh asset/count 보호 진단, material row 요약, texture slot marker, material 단위 Revert/Apply로 확장했다.
   - `Prefab Overrides` 패널은 material scalar와 texture slot 차이를 `Material Overrides` 표로 요약하고, `Focus` 액션으로 full `Materials` 섹션의 해당 material/slot을 열고 하이라이트한다.
   - Mesh metadata와 material override 배열은 확인 모달을 거쳐 prefab source에 적용할 수 있고, prefab source에서 현재 Entity로 되돌리는 Mesh replacement는 확인 모달, async import queue, undo/redo snapshot command, stale restore discard, current/source conflict detection, Inspector restore status/cancel/resolve UI, conflict preview/confirm modal/material side-by-side diff table/slot and factor focus link/focus pin/source path validation/texture remap/search/material+mesh bulk/confidence/manual candidate UX로 처리한다.
- 다음 단계:
   - Mesh/material까지 reflection quick edit 적용 범위를 넓힌다.
   - importer/editor texture matching은 `Assets/TextureMatching.h`의 slot keyword, non-base-color filter, token ignore 규칙을 공유한다.
   - path candidate ordering과 embedded texture fallback까지 포함한 완전 공용 resolver 분리를 강화한다.
   - ScenePersistence와 Prefab override diff가 reflection descriptor를 사용하게 만든다.

## 4. ScriptComponent

- 목표: entity에 script를 붙이고 Start/Update/LateUpdate/EndFrame 같은 phase를 엔진 scheduler와 연결한다.
- v1 실행 상태:
  - `ScriptComponent`를 추가하고 Inspector Add Component, enable/remove, scene 저장/로드에 연결했다.
  - Script path, class name, language, run-in-editor 플래그를 보존한다.
  - `Scripting::NativeScriptRuntime`을 추가하고 `GameScript`, `SpinScript` native class를 등록했다.
  - Project Scene의 phase scheduler에 `Start -> Update -> LateUpdate -> EndFrame` script lifecycle을 연결했다.
  - `Update/LateUpdate/EndFrame`은 job으로 스케줄하고 실제 Scene 변경은 `SceneCommandBuffer`를 통해 Commit phase에서 적용한다.
  - Inspector에서 Native script class를 선택할 수 있고, Console의 `Scripting` 섹션에서 active/started/scheduled job 수를 확인할 수 있다.
- 다음 단계:
  - Native script property binding과 user C++ module discovery를 추가한다.
  - Lua/C# 같은 외부 런타임을 선택하고 hot reload/reload failure reporting을 붙인다.

## 5. Asset Import Settings

- 목표: 모델/텍스처 import 옵션을 asset별로 저장하고 reimport/hot reload에 반영한다.
- v1 실행 상태:
  - `AssetImportSettingsService`를 추가했다.
  - asset 옆 `.import.json` 파일에 model scale, animation/material import, tangent, collider, normal flip, texture sRGB/mip/normal map 설정을 저장/로드할 수 있다.
  - Project 패널의 `Import Settings` 버튼으로 선택 asset의 `.import.json`을 생성/갱신할 수 있다.
  - 모델 import worker가 `.import.json`을 읽어 Assimp tangent 생성, animation import on/off, material import on/off, static import transform, normal Y flip, generated collider 정책을 실제 import 결과에 반영한다.
  - texture load path는 image `.import.json`의 `srgb`와 `normalMap` 설정을 읽어 slot upload 색공간을 결정한다.
  - 로드된 model은 `<model>.import.json`까지 hot reload watch 대상에 포함해 settings 변경 시 reimport될 수 있다.
  - Project 패널의 `Reimport` 버튼은 현재 Scene에 로드된 model을 즉시 reload queue에 넣는다.
  - Project 패널 details pane에 editable Import Settings UI를 추가해 model scale/rotation, material/animation/tangent/collider/normal 옵션과 texture sRGB/mip/normal/clamp 옵션을 직접 편집하고 저장할 수 있다.
  - model asset은 `Save & Reimport`로 `.import.json` 저장 후 현재 Scene에 로드된 instance reload를 바로 요청할 수 있다.
  - Project toolbar와 directory context menu의 Create는 이름 입력 모달을 거쳐 Folder/Scene/Material/Script/Prefab을 만들 수 있고, Command Palette Create는 기본 이름 빠른 생성으로 유지한다.
- 다음 단계:
  - animated mesh용 root scale/rotation import transform을 CPU skinning bind pose와 함께 안전하게 적용한다.

## 6. Runtime Player / Export

- 목표: editor가 아닌 runtime player 패키지를 만들어 프로젝트를 실행/배포한다.
- v1 실행 상태:
  - `ProjectBuildService`를 추가했다.
  - Assets/Scenes 복사와 `runtime-package.json` manifest 생성을 지원한다.
  - File 메뉴의 `Export Project Package`가 `<Project>/Builds/Windows`에 runtime package를 생성한다.
  - `EnginePlatformer.exe --runtime-package <manifest>` 명령줄을 추가했다.
  - Runtime package manifest를 읽어 package root의 `Assets`/`Scenes`를 사용하고 startup scene을 로드한다.
  - Runtime mode는 editor dockspace/ImGui 없이 전체 창을 GameCamera viewport로 렌더링한다.
  - Runtime mode는 Play 상태로 동작해 script lifecycle과 physics simulation을 editor Play 버튼 없이 실행한다.
- 다음 단계:
  - 별도 Player exe 타깃과 launcher의 Export/Run entry point를 추가한다.
  - runtime package에 engine executable/DLL 복사, icon, app metadata, build profiles를 추가한다.

## 7. 2D / UI / Audio / Navigation / Networking

- 목표: Godot 수렴에 필요한 제작 도메인을 Scene component로 공식화하고 점진적으로 runtime system을 붙인다.
- v1 실행 상태:
  - `Sprite2DComponent`, `UiElementComponent`, `AudioSourceComponent`, `NavigationAgentComponent`, `NetworkIdentityComponent`를 추가했다.
  - Inspector Add Component, enable/remove, Duplicate, scene 저장/로드에 연결했다.
- 다음 단계:
  - 2D renderer, UI layout/render pass, audio mixer, navmesh/query, network session/RPC를 각각 runtime system으로 구현한다.

## 검증 기준

- Debug|x64와 Release|x64 빌드가 경고 0개로 통과해야 한다.
- 새 컴포넌트는 Inspector에서 Add/Enable/Remove가 가능해야 한다.
- 새 컴포넌트를 붙인 scene을 저장/로드했을 때 값과 enabled 상태가 유지되어야 한다.
- 기존 Project Scene, Spider Sample, ECS Benchmark, Physics, Asset Import, RenderGraph smoke path가 깨지면 안 된다.
