# Editor Convenience Convergence Plan

이 문서는 Ogre, Godot, Unity, Unreal의 에디터 편의 기능을 현재 엔진에 흡수하기 위한 실천 계획이다.

## 기준

- Ogre는 완성형 게임 에디터보다 렌더링 중심 C++ 라이브러리에 가깝다. 따라서 Ogre에서 가져올 것은 “통합 친화적인 모듈형 툴 구조”, “명확한 render/debug sample”, “플러그인식 확장”이다.
- Godot, Unity, Unreal은 제작자가 매일 쓰는 에디터 UX가 강하다. 따라서 여기서는 Scene/Hierarchy/Inspector/Project/Console/Viewport/Play/Export 중심 편의 기능을 우선한다.
- 현재 엔진은 ImGui DockSpace, Scene/Game/Hierarchy/Inspector/Project/Console, project mode, scene save/load, asset hot reload, prefab/script/runtime package v1을 이미 가진다.

## 엔진별 참고 편의 기능

### Ogre

- 렌더러와 툴을 분리해 필요한 기능만 통합하는 구조.
- sample browser / debug visualization / asset conversion tool 같은 개발자용 보조 도구.
- plugin과 addon 중심 확장 철학.
- 현재 엔진 적용 방향:
  - 렌더러 debug view, resource stats, material stats, render graph timing을 계속 독립 패널화한다.
  - 런타임 필수 기능과 에디터 개발 도구를 분리한다.
  - asset import, renderer backend, scripting runtime을 플러그인형 모듈로 점진 분리한다.

### Godot

- Project Manager, FileSystem dock, Scene dock, Inspector dock, script editor, editor output/debugger, editor shortcut reference.
- Scene instance와 resource 중심 재사용.
- editor layout customization과 dock 이동.
- 현재 엔진 적용 방향:
  - Project 패널을 FileSystem dock 수준으로 확장한다.
  - Scene component reflection을 Inspector drawer, serialization, prefab override diff와 공유한다.
  - SceneReference와 PrefabInstance를 nested scene workflow로 확장한다.

### Unity

- Toolbar, Hierarchy, Scene/Game View, Inspector, Project window, status bar.
- Unity Search: asset, object, property, package, menu command 검색과 실행.
- Hierarchy create/duplicate/reorder/parenting, scene visibility, scene pickability.
- Project window search, favorites, two-column/preview, create menu.
- 현재 엔진 적용 방향:
  - Hierarchy / Project / Console 검색 필터부터 도입한다.
  - Command Palette를 추가해 menu command, entity, asset을 한 번에 찾고 실행한다.
  - Hierarchy parent-child, visibility/pickability, default parent를 추가한다.
  - Project favorites, saved search, asset preview를 추가한다.
  - Status bar로 project/scene/render/autosave/source-control 상태를 항상 노출한다.

### Unreal

- Menu Bar, Main Toolbar, Viewport Toolbar, Content Drawer/Browser, Bottom Toolbar, Outliner, Details panel.
- 빠른 Create, Play/Stop/Eject, platform deploy, tracing, output log, revision control status.
- Outliner multi-column/filter/context menu, Details search/lock.
- 현재 엔진 적용 방향:
  - Viewport toolbar를 Scene View 내부 overlay로 분리한다.
  - Content Drawer 스타일 quick asset drawer를 추가한다.
  - Console에 command input, tracing/profiler shortcut, save status를 넣는다.
  - Inspector lock과 property search를 추가한다.

## 우선순위

### v1: 즉시 체감되는 찾기/필터

- Hierarchy entity filter.
- Project asset filter.
- Console asset log filter.
- 현재 구현 상태: 완료.

### v2: Command Palette

- 단축키: `Ctrl+P` 또는 `Ctrl+K`.
- 대상:
  - menu command: Save Scene, Open Scene, Export, Play, Stop, Frame Selected.
  - create command: Cube, Sphere, Capsule, Plane, Camera, Light.
  - entity search: 선택, frame, rename.
  - asset search: open, reveal, load model, reimport.
  - keyboard shortcut cheat sheet.
- 구현:
  - `EditorCommandDescriptor` registry 추가.
  - `EditorLayer` popup UI에서 fuzzy filter, keyboard navigation, Enter 실행.
  - Engine callback으로 command 실행.
- 현재 구현 상태: 완료.
  - Toolbar `Search` 버튼과 `Ctrl+P` / `Ctrl+K`로 Command Palette를 연다.
  - `All`, `Commands`, `Entities`, `Assets` scope filter로 검색 대상을 빠르게 좁히고 프로젝트별 상태로 저장한다.
  - 검색어 prefix `>`, `@`, `/` 또는 `cmd:`, `entity:`, `asset:`으로 저장된 scope를 바꾸지 않고 이번 검색만 Commands/Entities/Assets로 임시 전환할 수 있다.
  - 자주 쓰는 명령을 `Pin` / `Unpin`으로 프로젝트별 고정하고 결과 최상단에 우선 표시하며, `Clear Pins`로 초기화할 수 있다.
  - 최근 실행한 명령을 프로젝트별 `Settings/EditorProjectState.json`에 저장하고 결과 상단에 우선 표시하며, `Clear Recent`로 초기화할 수 있다.
  - `Up/Down`, `PageUp/PageDown`, `Home/End`로 실행 가능한 결과만 이동하고 `Enter`로 선택 결과를 실행한다.
  - File/Edit/Play/Camera/Render/Project 명령을 실행할 수 있다.
  - 최근 scene은 `Open Recent: <Scene>` 명령으로 바로 열 수 있다.
  - Help 메뉴와 `F1`, Command Palette, `shortcuts` Console 명령으로 Keyboard Shortcut Reference 모달을 열 수 있다.
  - Empty Entity, Camera, Light, Cube, Sphere, Capsule, Plane을 생성할 수 있다.
  - 선택 Entity에 Mesh, Camera, Light, Script, Rigidbody, Collider component를 추가할 수 있다.
  - 검색어가 있을 때 Scene entity를 선택하거나 frame selected할 수 있다.
  - 검색어가 있을 때 Project asset을 열기, reveal, model load, reimport할 수 있다.
  - Camera/Light/Empty 생성은 undo command로 기록된다.

### v3: Hierarchy 제작 편의

- parent/child hierarchy.
- reparent drag/drop.
- visibility eye, pickability lock.
- empty parent 생성.
- default parent.
- multi-select delete/duplicate/reorder.
- 현재 구현 상태: 부분 완료.
  - Hierarchy row에 `V` Scene visibility 토글을 추가했다.
  - Hierarchy row에 `P` Scene pickability 토글을 추가했다.
  - 우클릭 메뉴와 Command Palette에서도 hide/show, lock/unlock을 실행할 수 있다.
  - Scene visibility는 Scene View 렌더링과 picking에만 영향을 주며 Game View/runtime 렌더링은 유지한다.
  - Pickability lock은 Scene View picking에서만 제외하고 Hierarchy 선택/Inspector 편집은 유지한다.
  - `editorState` scene 저장/로드와 prefab snapshot/undo snapshot에 보존된다.
  - `SceneHierarchyComponent`를 추가해 Entity parent와 foldout 상태를 저장한다.
  - Hierarchy 패널은 flat list 대신 parent-child tree로 표시된다.
  - Hierarchy filter는 부모/자식 중 하나라도 검색어와 맞으면 해당 경로를 표시한다.
  - Hierarchy 상단 quick filter로 `Mesh`, `Camera`, `Light`, `Physics`, `Script`, `Hidden`, `Locked`, `Nested` Entity를 바로 좁혀 볼 수 있다.
  - Hierarchy 상단 `Expand All` / `Collapse All`로 parent-child tree 전체 foldout 상태를 빠르게 전환할 수 있다.
  - Entity 우클릭 메뉴에서 단일 branch 또는 다중 선택 branch를 재귀적으로 expand/collapse할 수 있다.
  - Entity를 다른 Entity 위로 드래그하면 상단/하단은 sibling reorder, 중앙은 child reparent로 처리한다.
  - Scene 저장/로드, Duplicate, Prefab snapshot, undo snapshot에 hierarchy parent 정보를 보존한다.
  - parent-child transform은 local/world transform으로 분리되어 부모 transform 변경을 자식 world transform에 반영한다.
  - Entity 우클릭 `Create Child` 메뉴로 Empty/Camera/Light/Primitive를 자식으로 생성할 수 있다.
  - Entity 우클릭 `Create Empty Parent`로 선택 Entity를 감싸는 빈 부모를 생성할 수 있다.
  - Entity 우클릭 또는 Command Palette에서 default parent를 지정/해제할 수 있다.
  - 빈 공간 Create와 Command Palette 기본 Create 명령은 default parent 아래에 새 Entity를 만든다.
  - `Ctrl+Click`으로 Hierarchy 다중 선택을 토글하고 `Shift+Click`으로 flat entity order 기준 범위 선택을 할 수 있다.
  - 다중 선택 상태에서 우클릭 `Duplicate Selected` / `Delete Selected`가 동작한다.
  - 다중 선택 상태에서 `Ctrl+D`와 `Delete` 단축키가 선택 묶음 전체에 적용된다.
  - 다중 duplicate/delete는 하나의 복합 undo command로 기록된다.
  - 다중 선택된 Entity 중 하나를 드래그하면 선택 묶음을 before/after/as child 위치로 함께 이동할 수 있다.
  - parent/child가 함께 선택된 경우에는 최상위 선택 Entity만 이동해 중복 reparent를 피한다.
  - 단일/다중 hierarchy move는 undo command로 기록되고, undo 시 이전 parent/index/local transform을 복원한다.
  - Scene View 좌클릭 드래그 marquee selection을 추가해 화면에 투영된 bounds/transform point가 selection rectangle과 겹치는 Entity들을 다중 선택할 수 있다.
  - Ctrl/Shift 상태에서 marquee를 시작하면 기존 Hierarchy selection에 더하는 additive selection으로 동작한다.
  - multi-select property editing polish는 다음 단계로 남긴다.

### v4: Project / Content Browser 고도화

- favorites와 saved search.
- asset thumbnail/preview.
- one-column / two-column layout.
- create asset menu: material, script, prefab, scene.
- asset dependency / references view.
- import settings editor를 Project 패널 오른쪽 detail pane으로 이동.
- 현재 구현 상태: 부분 완료.
  - Project 패널에 one-column / two-column layout 토글을 추가했다.
  - two-column layout에서는 왼쪽 browser, 오른쪽 asset details pane으로 표시한다.
  - Project 패널 상단 breadcrumb와 `Up` 버튼으로 현재 선택 asset의 상위 폴더를 빠르게 재선택할 수 있다.
  - `Scope Folder` 토글을 켜면 선택 폴더를 임시 root처럼 사용해 큰 asset tree에서 현재 작업 폴더만 집중해서 볼 수 있고, 프로젝트별 `projectBrowser.folderScope`에 저장한다.
  - Project details와 폴더 context menu에서 `Select Folder`, `Scope Here`, `Clear Scope`를 제공해 파일 선택에서 폴더 집중 탐색으로 바로 전환할 수 있다.
  - Project details는 선택 asset 기준 `Search Name`, `Filter Type`, `Filter Extension` 액션을 제공해 같은 이름/타입/확장자의 asset으로 빠르게 좁힐 수 있다.
  - Project 패널은 active filter bar로 `Scope`, `Type`, `Text` 필터 상태를 chip처럼 표시하고 각각 또는 전체를 즉시 해제할 수 있다.
  - Project details는 선택 폴더의 `Directory Summary`를 표시해 direct/recursive item 수, 총 indexed size, model/image/scene/material/prefab/source/text/other 개수를 한눈에 볼 수 있다.
  - Directory Summary의 타입별 `Show` 버튼은 현재 폴더를 scope root로 잡고 해당 quick filter를 적용해 폴더 안 특정 asset 종류로 바로 드릴다운한다.
  - Project entry와 details pane에서 favorite toggle을 지원한다.
  - Favorites 섹션에서 즐겨찾기 asset/folder를 빠르게 선택하고 더블클릭으로 열 수 있다.
  - Recent Assets 섹션에서 최근 선택/열기/로드한 asset을 다시 선택하거나 열 수 있다.
  - 즐겨찾기, recent assets, saved search, Project browser layout은 프로젝트별 `Settings/EditorProjectState.json`에 저장된다.
  - Project 패널의 현재 검색어를 saved search로 저장하고, combo에서 다시 불러오거나 삭제할 수 있다.
  - Content Drawer도 같은 saved search 목록을 사용해 빠른 asset drawer 안에서 검색어 저장/불러오기/삭제를 할 수 있다.
  - Project 패널과 Content Drawer는 `Favorites`, `Folders`, `Models`, `Images`, `Scenes`, `Materials`, `Prefabs`, `Source`, `Text` quick filter를 공유한다.
  - quick filter는 텍스트 검색과 함께 적용되고, 프로젝트별 `Settings/EditorProjectState.json`의 `projectBrowser.quickFilter`에 저장된다.
  - Content Drawer는 active filter bar로 공유 quick filter와 drawer 검색어를 표시하고 개별 또는 전체 해제할 수 있다.
  - Content Drawer 결과는 `Up/Down`, `PageUp/PageDown`, `Home/End`로 선택을 이동하고 `Enter`로 열기, `Ctrl+Enter`로 model load를 실행할 수 있다.
  - Content Drawer는 `Path`, `Name`, `Type`, `Size`, `Modified` sort mode와 오름/내림 방향 전환을 제공하고 프로젝트별 `projectBrowser.contentDrawerSort`, `projectBrowser.contentDrawerSortDescending`에 저장한다.
  - Content Drawer는 선택 asset의 details/preview pane을 popup 내부에 표시할 수 있고 프로젝트별 `projectBrowser.contentDrawerDetails`에 표시 여부를 저장한다.
  - Project/Content Drawer에서 asset을 선택, open, reveal, load, reimport하면 `projectBrowser.recentAssets`에 최근 asset으로 기록한다.
  - Project tree, Content Drawer, Details pane에서 absolute path, project-relative path, file name을 클립보드로 복사할 수 있다.
  - Project toolbar와 directory context menu, Command Palette에서 `Folder`, `Scene`, `Material`, `Script`, `Prefab` asset을 생성할 수 있다.
  - 생성 asset은 이름 충돌 시 자동 suffix를 붙이고, 생성 후 Project refresh/resource configure를 요청한다.
  - Project toolbar와 directory context menu의 Create는 Unity/Unreal식 `Create Project Asset` 모달을 열어 asset 이름과 target directory를 확인한 뒤 생성한다.
  - Command Palette의 Create Asset 명령은 빠른 생성 흐름으로 유지되어 기본 이름 + 자동 suffix를 사용한다.
  - Details pane에는 asset type/size/favorite 상태와 텍스트/source preview를 표시한다.
  - 이미지 asset은 backend texture handle 없이도 동작하는 CPU downsample thumbnail, 원본 크기, 채널 수를 Preview에 표시한다.
  - Model asset은 details/context menu에서 Load Model, Reimport, Import Settings로 바로 이어진다.
  - Details pane의 `Dependencies / References`에서 선택 asset이 참조하는 프로젝트 asset과 선택 asset을 참조하는 `.scene/.prefab/.material/source` 파일을 텍스트 스캔 기반으로 표시한다.
  - Dependency/Reference row는 클릭 시 해당 asset을 선택하고, 더블클릭 open, 우클릭 Open/Reveal/Favorite/Load/Reimport/Copy action을 제공한다.
  - 텍스트 reference 분석은 Project snapshot signature 기준으로 캐시되는 reference index를 사용해, 선택 asset이 바뀔 때마다 전체 파일을 다시 읽지 않는다.
  - reference index는 `Assets/AssetDatabase` 서비스로 분리했고, 프로젝트별 `Library/EditorAssetIndex.json`에 저장된다.
  - snapshot signature가 같으면 엔진 재실행 후에도 파일 캐시에서 복원되며, signature에는 파일 size와 last write time이 포함된다.
  - Model asset 선택 시 Assimp inspection으로 scene/root/mesh/material/animation 통계와 material texture slot별 raw/resolved/embedded/auto-match 상태를 표시한다.
  - Assimp inspection에서 resolve된 texture는 선택 model의 dependency 목록에 함께 표시한다.
  - Unreal식 Content Drawer v1을 추가했다.
  - `Ctrl+Space`, Toolbar `Content`, `Window > Content Drawer`, Command Palette, `content` 콘솔 명령으로 빠른 asset drawer를 열 수 있다.
  - Content Drawer는 Project snapshot을 검색해 asset Open/Reveal, model Load/Reimport, drag source를 제공한다.
  - Content Drawer는 details pane을 켜면 Project details와 같은 import settings, dependency/reference, image/text preview를 빠른 asset drawer 안에서 확인할 수 있다.
  - Content Drawer 결과 row와 context menu에서 asset favorite/unfavorite를 바로 토글할 수 있다.
  - Details pane에 editable Import Settings UI를 추가해 model scale/rotation, material/animation/tangent/collider/normal 옵션과 texture sRGB/mip/normal/clamp 옵션을 직접 저장할 수 있다.
  - Model asset은 `Save & Reimport`로 `.import.json` 저장 후 현재 Scene의 loaded instance reload를 바로 요청할 수 있다.
  - GPU texture thumbnail cache와 더 정교한 증분 업데이트는 다음 단계로 남긴다.

### v5: Inspector / Details 고도화

- property search.
- Inspector lock.
- multi-object editing.
- component reorder.
- reset / copy / paste component values.
- reflection 기반 drawer로 수동 UI 축소.

현재 구현 상태:

- Inspector 상단에 `Lock` 토글을 추가해 선택 변경과 별개로 특정 Entity를 계속 편집할 수 있게 했다.
- 잠긴 Entity가 삭제되면 Inspector lock은 자동으로 풀리고 빈 선택 상태로 돌아간다.
- Inspector 검색창을 추가해 `Transform`, `Camera`, `Light`, `Physics`, `Mesh`, `Animator`, `Materials` 같은 컴포넌트 섹션과 주요 property 키워드를 빠르게 필터링한다.
- optional component 헤더의 `...` 메뉴에 `Reset`, `Copy Values`, `Paste Values`를 추가했다.
- reset/paste는 Engine undo command로 실행되어 `Ctrl+Z/Ctrl+Y` 흐름에 포함된다.
- Camera paste는 GameCamera 역할을 복사하지 않고, NetworkIdentity paste는 고유 ID를 복제하지 않도록 보정한다.
- Mesh reset은 렌더 asset을 비우는 위험이 있어 v1에서는 비활성화했다.
- optional component 헤더의 `...` 메뉴에 `Move Up`, `Move Down`을 추가해 Entity별 Inspector 표시 순서를 바꿀 수 있게 했다.
- component reorder는 프로젝트별 `Settings/EditorProjectState.json`의 `inspector.componentOrders`에 저장되어 에디터 재실행 후에도 복원된다.
- optional component 헤더의 `...` 메뉴에 `Pin To Top` / `Unpin From Top`을 추가했다.
- pinned component type은 Inspector 상단에 우선 표시되고, 프로젝트별 `Settings/EditorProjectState.json`의 `inspector.pinnedComponents`에 저장된다.
- Inspector 상단의 `Clear Pins`로 현재 프로젝트의 component pin 상태를 한 번에 초기화할 수 있다.
- Hierarchy multi-select 상태를 Inspector가 읽어 여러 Entity 선택 시 Transform multi-object editing UI를 표시한다.
- multi-object Transform v1은 primary Entity 기준 `Primary Position`, `Shared Rotation`, `Relative Scale`을 제공한다.
- multi-object Transform 변경은 선택된 모든 Entity에 즉시 반영되고, 마우스 릴리즈 시 하나의 undo command로 묶인다.
- multi-object optional component editing은 `Camera`, `Light`, `Rigidbody`, `Collider`, `Physics Material`, `Prefab Instance`, `Scene Reference`, `Script`, `Sprite 2D`, `UI Element`, `Audio Source`, `Navigation Agent`, `Network Identity` property 편집을 선택된 동일 컴포넌트들에 함께 적용한다.
- `Camera/Light/Rigidbody/Collider/Physics Material/Prefab Instance/Scene Reference/Script/Sprite 2D/UI Element/Audio Source/Navigation Agent/Network Identity` multi-edit 변경은 batch component snapshot command로 묶여 undo/redo가 가능하다.
- Camera multi-edit은 FOV/Near/Far만 공유하고 `Game Camera` 역할은 Entity별로 보존한다.
- Sprite 2D, UI Element, Audio Source multi-edit은 각각 Texture/Text/Clip 값을 Entity별로 보존하고 표시/레이아웃/재생 속성만 공유한다.
- Network Identity multi-edit은 Network Id와 Prefab Key를 Entity별로 보존하고 replicate/server authority 설정만 공유한다.
- Script multi-edit은 Script Path를 Entity별로 보존하고 language/class/run-in-editor 설정만 공유한다.
- Prefab Instance/Scene Reference multi-edit은 Prefab Path/Source Name/Scene Path를 Entity별로 보존하고 Track Overrides/Load Additively/Auto Load만 공유한다.
- Inspector에 `Reflection Schema` drawer를 추가해 선택 Entity의 component descriptor와 property metadata coverage를 확인할 수 있게 했다.
- Inspector에 `Reflection Quick Edit` drawer를 추가해 descriptor 기반으로 `Animator`, `Camera`, `Light`, `Rigidbody`, `Collider`, `Physics Material`, `Prefab Instance`, `Scene Reference`, `Script`, `Sprite2D`, `UI Element`, `Audio Source`, `Navigation Agent`, `Network Identity`의 안전한 bool/enum/숫자/vector/color/string/path 필드를 편집할 수 있게 했다.
- `Reflection Quick Edit`는 `Mesh`를 읽기 전용 진단 대상으로 포함해 asset source, primitive kind, vertex/index/submesh/material/animation count와 material row 목록을 표시한다.
- Mesh material row의 `Focus` 액션으로 full `Materials` 섹션의 해당 material을 열고 하이라이트할 수 있다.
- `Reflection Quick Edit`의 `Material Scalar Quick Edit`는 Shading Model, Base Color, Vertex Color, Normal Y Flip, Emissive, Opacity, Metallic/Roughness, Specular/Shininess 값을 기존 material undo/redo 경로로 편집한다.
- Reflection 기반 material scalar edit는 texture slot을 직접 변경하지 않고 full `Materials` 섹션의 texture UI로 분리해 path/embedded texture binding을 안전하게 보존한다.
- Reflection 기반 material scalar edit도 multi-select 상태에서 `Apply Scalars To Selected`를 제공하며, 같은 material index를 가진 선택 Entity들에 하나의 material batch undo command로 적용한다.
- `Material Texture Slot Actions`는 Reflection Quick Edit 안에서 기존 `Browse`, `Clear`, Project drag/drop texture assignment 경로를 재사용해 texture slot을 안전하게 조작한다.
- Reflection Quick Edit의 texture slot action도 prefab source와 다른 slot에 `[Override]` marker와 current/source tooltip을 표시한다.
- Quick Edit에서 물리 component를 수정하면 기존 PhysX actor dirty callback을 호출해 runtime physics state도 재생성 대상이 된다.
- `Reflection Quick Edit` v1은 path property를 직접 타이핑 대신 Project asset drag/drop, `Pick` asset popup, `Clear` 버튼으로 할당한다.
- `SceneReferenceComponent` Inspector에 `Open Scene`, `Reveal`, `Load As Children`, `Unload Loaded` 액션을 추가했다.
- Inspector에서 resolved scene path, loaded 여부, loaded entity 수, auto reload watch 상태를 확인할 수 있다.
- `Load As Children`은 참조 `.scene`의 root entity를 선택 Entity 아래 child로 추가하고, 참조 scene 내부 hierarchy를 유지한다.
- `Reload`는 로드된 child 묶음을 언로드한 뒤 같은 참조 scene에서 다시 생성한다.
- Scene load/restore 시 `Auto Load`가 켜진 enabled SceneReference를 자동으로 확장한다.
- 자동/수동으로 확장된 nested scene child는 scene save/autosave/play snapshot에서 제외해 저장 중복을 피한다.
- 로드된 referenced scene 파일의 `last_write_time`을 1초 간격으로 감지해 변경 시 dirty 상태를 오염시키지 않고 자동 reload한다.
- 현재 scene이 dirty이면 외부 변경 reload를 pending 상태로 보류하고, 저장 후 또는 수동 Reload로 반영하게 했다.
- Hierarchy는 runtime-expanded child에 `[Nested]` 배지를 표시하고, Inspector는 owner SceneReference/source scene/save-excluded 상태를 보여준다.
- Nested child 우클릭 메뉴와 Inspector에서 owner 선택, source scene 열기, source reload/unload를 바로 실행할 수 있다.
- Nested child는 Undo/Redo 가능한 `Make Local`로 SceneReference 추적에서 분리해 현재 씬에 저장되는 일반 Entity/subtree로 전환할 수 있다.
- Command Palette에서도 선택 Entity의 referenced scene open/load/reload/unload 명령을 실행할 수 있다.
- `Prefab Overrides` 진단 패널을 추가해 Prefab Instance의 source prefab과 현재 Entity의 reflected component/property 차이를 표로 확인할 수 있게 했다.
- Prefab Instance 선택 시 Inspector 상단에 reflected override 개수와 영향 component badge를 표시하는 override marker v1을 추가했다.
- Entity 이름 override는 Inspector 상단의 `Revert Name` / `Apply Name` 버튼으로 prefab source와 바로 동기화할 수 있다.
- Transform과 optional component 섹션 헤더에도 `[Override]` 배지를 표시해 어느 섹션이 prefab source와 다른지 바로 찾을 수 있다.
- Transform override는 헤더 옆 `Revert` / `Apply` 버튼으로 prefab source 값으로 되돌리거나 현재 Transform을 prefab source에 반영할 수 있다.
- Optional component 섹션의 `...` 메뉴에서 해당 component override를 prefab 값으로 `Revert`하거나 현재 component 값을 prefab source에 `Apply`할 수 있다.
- `Prefab Overrides`에서 이름과 지원 component override를 prefab source 기준으로 되돌리는 `Revert Name`, `Revert <Component>`, `Revert All Supported` 액션을 추가했다.
- Override 표의 각 row에 `Revert` / `Apply` 액션을 추가해 개별 reflected property를 prefab 값으로 되돌리거나 현재 값으로 prefab에 반영할 수 있게 했다.
- `Apply Current to Prefab` 액션을 추가해 현재 Entity 상태를 source prefab asset에 저장할 수 있게 했다.
- Apply 시 `PrefabInstance` link 자체는 prefab asset에 쓰지 않아 자기 자신을 참조하는 prefab을 피한다.
- `<component>` row의 `Apply`를 지원해 현재 Entity에 추가/삭제된 optional component를 source prefab에 반영할 수 있게 했다.
- Mesh와 PrefabInstance link 자체는 import/link 부작용이 커서 v1 revert/apply 보호 대상으로 둔다.
- Reflection Quick Edit는 prefab source와 다른 reflected property row에 `[Override]` marker를 표시하고 tooltip에서 current/source 값을 비교할 수 있게 했다.
- Mesh 섹션은 prefab source와 다른 asset path/material count를 protected override 진단으로 표시한다.
- `Prefab Overrides` 패널은 material scalar 값과 texture slot 차이를 별도 `Material Overrides` 표로 요약하고, `Focus` 액션으로 full `Materials` 섹션의 해당 material 또는 texture slot으로 바로 이동한다.
- Mesh asset path/material count 변경은 `Apply Mesh To Prefab...` 확인 모달을 통해 current mesh metadata와 material override 배열만 prefab source에 명시 반영할 수 있다.
- `Revert Mesh From Prefab...`은 확인 모달 후 prefab Mesh를 현재 Entity에 되돌리며, 모델 파일은 async import queue를 사용하고 현재 이름/Transform을 유지한다.
- Mesh replacement revert는 undo/redo command로 기록되며, 모델 asset은 captured Mesh snapshot restore job을 다시 큐잉하는 방식으로 복원한다.
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
- `Material Diff`에는 `Pin Focus`와 `Clear Focus`를 추가해 큰 material list에서도 선택한 diff row의 Inspector 위치를 계속 고정해서 볼 수 있다.
- async Mesh restore 상태에는 source model/source prefab path validation을 추가해 파일 존재 여부를 `OK/Missing/Error`로 표시하고, 존재하는 파일은 `Open`/`Reveal`, 누락된 파일은 `Refresh Project`로 이어지게 했다.
- Material texture slot은 저장된 texture path가 누락되면 `Missing texture dependency`와 `Suggested Remaps`를 표시한다.
- Suggested Remaps는 Project snapshot의 이미지 asset 중 같은 filename/stem, material name, slot keyword를 기준으로 후보를 점수화하고, `Assign`으로 기존 undo 가능한 material texture assignment에 연결한다.
- Materials 섹션에는 `Remap Filter`와 `Auto Remap Missing`을 추가해 후보를 텍스트로 좁히고, 현재 material의 missing texture slot들을 하나의 undo command로 일괄 remap할 수 있다.
- Materials 섹션 상단에는 Mesh 전체 `Auto Remap All Missing`과 `Remap Preview`를 추가해 여러 material의 missing texture slot을 하나의 undo command로 일괄 복구할 수 있다.
- Remap 후보는 `High`, `Medium`, `Low`, `Ambiguous` 신뢰도와 score를 표시하며, Mesh 전체 remap에 `Low`/`Ambiguous` 후보가 섞이면 확인 모달에서 위험 후보와 차순위 후보를 검토한 뒤 적용하게 했다.
- `Remap Preview` row에는 candidate combo를 추가해 자동 후보 대신 다른 후보를 수동 선택할 수 있고, `Auto`/`Clear Candidate Choices`로 세션 내 선택을 되돌릴 수 있다.
- Remap scoring은 Assimp importer의 nearby texture matching에 맞춰 same filename/stem, `.png/.tga` preferred variant, material/model token, `textures` folder, source model 주변 경로를 반영한다.
- Materials 섹션은 material row와 texture slot 단위 override marker를 표시하고, source material이 있는 row는 `Revert Material` / `Apply Material To Prefab`을 지원한다.
- Materials 섹션은 multi-select 상태에서 `Apply Scalars To Selected`를 제공해 현재 material row의 Shading Model, color/scalar factor, Use Vertex Color, Normal Y Flip 값을 같은 material index를 가진 선택 Entity들에 하나의 undo command로 적용한다.
- material batch apply는 texture binding/path/embedded texture와 material name을 Entity별로 보존해 대량 재질 보정 중 개별 texture slot이 덮이지 않게 했다.
- importer/editor texture matching은 `Assets/TextureMatching.h`의 slot keyword, non-base-color filter, token ignore 규칙을 공유한다.
- path candidate ordering과 embedded texture fallback까지 포함한 완전 공용 resolver 분리는 다음 단계로 남겼다.
- v5의 남은 큰 작업은 Mesh/material serialization apply/revert 정책을 descriptor 기반으로 더 통합하고, material row 단위 편집의 edge case를 줄이는 것이다.

### v6: Viewport UX

- Scene View overlay toolbar.
- snapping, local/world, pivot/center, grid toggle.
- focus orbit, view cube, camera speed slider.
- measurement/ruler, selection outline, bounding box.
- 현재 구현 상태:
  - Scene View 내부 overlay toolbar를 추가했다.
  - Grid/Gizmos 토글을 Scene View 안에서 바로 조작할 수 있게 했다.
  - Y=0 기준 world grid를 SceneCamera 기준으로 투영해 표시한다.
  - SceneCamera 이동 속도 slider를 추가했다.
  - Transform gizmo translate/rotate/scale에 snap 옵션과 snap 간격 편집을 추가했다.
  - Transform gizmo `World/Local` 전환을 추가했다.
  - Transform gizmo `Pivot/Center` 전환을 추가했다.
  - Translate gizmo에 XY/XZ/YZ plane handle을 추가해 축 하나가 아니라 평면 기준으로 드래그 이동할 수 있게 했다.
  - Scale gizmo 중심에 uniform scale handle을 추가해 축별 scale뿐 아니라 전체 scale을 비율 유지 상태로 드래그할 수 있게 했다.
  - Scale gizmo visual polish를 추가해 uniform/axis handle hover, hand cursor, 현재 scale 값 배지, active drag 값 표시를 Scene View 안에서 확인할 수 있게 했다.
  - Rotate gizmo에 축별 회전 링을 추가해 X/Y/Z 회전 평면을 화면에서 바로 보고 링을 잡아 회전 조작을 시작할 수 있게 했다.
  - 선택 Entity의 `BoundsComponent`를 Scene View에 노란 bounding box로 표시한다.
  - Scene View 오른쪽 위에 `View Cube`를 추가해 카메라 방향 전환을 지원한다.
  - View Cube v2는 카메라 방향에 맞춰 3D 축 endpoint를 투영하고, endpoint 클릭으로 해당 축 방향 view로 전환한다.
  - View Cube `Views` popup을 추가해 Face/Edge/Corner preset으로 Top, Front, Top Front, Top Front Right 같은 시점에 바로 정렬할 수 있게 했다.
  - View Cube 본체에도 face/edge/corner hotspot을 투영해 popup을 열지 않고 미니 3D cube에서 직접 시점 전환할 수 있게 했다.
  - View Cube 빈 영역을 드래그하면 선택 Entity 또는 월드 원점 기준으로 SceneCamera orbit이 동작한다.
  - View Cube hover label/click affordance를 추가해 axis endpoint와 face/edge/corner hotspot 위에서 현재 클릭 대상과 orbit 상태를 즉시 확인할 수 있게 했다.
  - `Orbit` 토글과 `Alt + Left Drag` 기반 Focus Orbit을 추가했다.
  - Focus Orbit은 선택 Entity bounds 중심을 기준으로 돌고, 선택이 없으면 월드 원점을 기준으로 동작한다.
  - `Measure` 토글과 지면(Y=0) 기준 ruler v1을 추가했다.
  - Measure가 켜져 있으면 왼쪽 드래그로 측정선을 만들고, 우클릭으로 측정을 초기화한다.
  - 선택 bounds에 낮은 알파 fill, 코너 포인트, 이름/크기 라벨을 추가해 selection outline polish v1을 구현했다.
  - Measure target을 `Y=0`, `View`, `Bounds`, `Mesh`로 확장했다.
  - `View`는 선택 Entity 중심 또는 카메라 전방 평면에서 측정하고, `Bounds`는 선택 Entity world AABB 표면에서 측정한다.
  - `Mesh`는 선택 Entity의 실제 triangle surface에 ray를 교차시켜 가장 가까운 hit point를 측정한다.
  - Mesh surface 측정은 world AABB 선검사, local-space raycast, triangle budget을 사용해 skinned/large mesh에서 Scene View 입력 지연을 줄인다.
  - Static mesh surface 측정은 local triangle AABB acceleration cache를 사용해 exact triangle test 후보를 줄인다.
  - Animated/skinned mesh surface 측정은 `RenderFrameStats.FrameIndex` 기준 dynamic triangle AABB cache를 사용해 stale vertex cache를 피하면서 같은 프레임 안의 반복 측정 비용을 줄인다.
  - Scene View overlay는 Mesh 측정 중 tested/total triangle 수, static/dynamic acceleration cache 사용, budget 사용, bounds reject/hit/miss 상태를 표시한다.
  - Profiler `Viewport Tools` 섹션은 마지막 Mesh Surface 측정의 hit/bounds/miss, tested/total triangle 수, cache mode, cache rebuild 여부, raycast ms, cache build ms를 표시한다.
  - 남은 작업은 full BVH hierarchy 적용 여부 검토다.

### v7: Production Tooling

- command console.
- profiler/tracing capture.
- source control status.
- unsaved scene list.
- editor layout save/restore.
- crash recovery / autosave.
- package/export profile UI.
- 현재 구현 상태:
  - `Profiler` dock panel을 추가했다.
  - 최근 180프레임의 frame ms, RenderGraph CPU ms, draw call, triangle count 히스토리를 표시한다.
  - Profiler pause/reset을 지원한다.
  - RenderGraph pass별 CPU timing table을 표시한다.
  - Job, Memory, Resource, Script runtime 요약을 한 패널에서 확인할 수 있게 했다.
  - Project Scene dirty 상태를 주기적으로 `Temp/Autosaves/<Scene>.autosave.scene`에 저장하는 autosave v1을 추가했다.
  - Autosave는 명시적 저장이 아니므로 dirty flag를 지우지 않고, Play/Runtime/비 Project Scene 상태에서는 동작하지 않는다.
  - Profiler 패널에서 Autosave on/off, 저장 주기, 진행률, 마지막 autosave 경로와 상태 메시지를 확인할 수 있다.
  - Console command input v1을 추가했다.
  - `help`, `status`, `save`, `play`, `stop`, `frame`, `refresh`, `export`, `api`, `render`, `autosave`, `create`, `select`, `load` 명령을 Console 패널에서 실행할 수 있다.
  - Console 명령은 기존 Editor callback을 재사용해 Toolbar/Command Palette와 같은 코드 경로로 동작한다.
  - Source Control status v1을 추가했다.
  - Console 패널에서 프로젝트 루트의 Git branch, clean/dirty 상태, modified/added/deleted/renamed/untracked/conflict 개수와 변경 파일 목록을 볼 수 있다.
  - `git status` 또는 `scm refresh` 콘솔 명령으로 Git 상태를 수동 갱신할 수 있다.
  - Source Control 패널에서 `Stage All`, 파일별 `Stage/Unstage`, `Unstage All`, `Commit Staged`를 실행할 수 있다.
  - Console 명령으로 `git stage all`, `git unstage all`, `git commit <message>`를 실행할 수 있다.
  - Source Control 패널에 `Push Branch` 확인 모달과 `Set Upstream + Push` 흐름을 추가했다.
  - Console 명령으로 `git push`, `git push set-upstream`을 실행할 수 있다.
  - Conflict Resolver v1을 추가해 충돌 파일을 `Open`/`Reveal`하고, 수동 해결 후 `Mark Resolved`로 `git add -- <path>`를 실행할 수 있다.
  - Console 명령으로 `git resolve <path>`를 실행할 수 있다.
  - destructive conflict 선택지, pull/rebase, force push는 승인/검토 흐름이 필요해 다음 단계로 남긴다.
  - Unsaved Scenes v1을 추가했다.
  - Console 패널에서 현재 Project Scene의 dirty 상태, autosave snapshot 상태, Save/Save As 액션을 확인할 수 있다.
  - `unsaved` 콘솔 명령으로 현재 저장 필요 상태와 autosave snapshot 경로를 확인할 수 있다.
  - Editor layout save/restore/reset v1을 추가했다.
  - `Window > Save Editor Layout` / `Restore Saved Layout` / `Reset Editor Layout`과 Command Palette, `layout save|restore|reset|status` 콘솔 명령으로 프로젝트별 dock layout을 관리할 수 있다.
  - 저장된 layout은 프로젝트별 `Settings/EditorProjectState.json`의 `editorLayout.ini`에 저장되고, 프로젝트 상태 로드 시 자동 복원된다.
  - File 메뉴에 `Open Recent`를 추가했고 최근 scene 목록은 프로젝트별 `Settings/EditorProjectState.json`의 `recentScenes`에 최대 10개까지 저장된다.
  - 하단 Status Bar v1을 추가해 project, scene dirty, play/edit, graphics API, render mode, draw/triangle/entity count, asset scan, autosave, git 상태를 상시 표시한다.
  - Status Bar 항목은 quick action을 제공한다: project reveal, dirty scene save, asset refresh, autosave toggle, git refresh, shortcut reference open.
  - Package/Export profile UI v1을 추가했다.
  - Console 패널에서 Windows Runtime Package 출력 폴더, Assets/Scenes/manifest 포함 여부, 완료 후 reveal 여부를 설정할 수 있다.
  - File 메뉴, Command Palette, `export` 콘솔 명령은 현재 export profile 설정을 우선 사용한다.
  - Export profile은 기존 `ProjectBuildService` 경로를 사용해 `<output>/runtime-package.json`을 생성하고 runtime 실행 인자 예시를 표시한다.
  - Play mode 진입 시 edit scene snapshot을 파일 fallback과 in-memory `LoadSceneResult` clone으로 보관하고, 별도 `m_PlayScene` runtime clone을 만들어 Stop 시 edit scene을 보존해 Play 중 변경이 Edit scene으로 남지 않게 했다.
  - Frame phase의 script/animation/physics 실행 경로와 주요 render read path, picking은 `GetRuntimeScene()` accessor를 통해 Scene을 받으며 Play 중에는 `m_PlayScene`을 사용한다.
  - Play 중 `CanEditProjectScene`은 false로 내려 edit-scene 저장/생성/삭제를 잠그고, `CanControlPlayMode`로 Stop 조작은 유지한다.
  - Hierarchy, Inspector, Status Bar는 `ActiveSceneIsRuntimeClone` 상태를 받아 `Play Runtime Clone` / runtime-only 배지를 표시한다.
  - Inspector runtime-only 카드에는 `Reset Runtime Clone` 빠른 액션을 넣어 Play 실험 상태를 toolbar로 돌아가지 않고 초기화할 수 있다.
  - `BlockEditSceneMutationDuringPlay` guard로 rename/duplicate/delete/create/component/material/transform/undo/redo command가 Play 중 edit scene에 기록되지 않도록 막았다.
  - Play 중 Transform, Material scalar/shading, component property edit commit은 edit stack 대신 `m_RuntimeCommandStack`에 기록해 Ctrl+Z/Ctrl+Y로 runtime clone 안에서만 되돌릴 수 있고, Stop/Reset 시 폐기된다.
  - Play 진입 전 dirty scene은 기존 Unsaved Scene dialog를 재사용해 Save / Don't Save / Cancel 선택을 받고, Cancel이면 Play 진입을 중단한다.
  - Toolbar, Command Palette, Console 명령과 `F5` / `F6` / `F10` 단축키는 Play/Stop, Pause/Resume, Step을 제공하고, Paused 상태에서는 script/update/animation/physics phase만 멈춰 editor/render는 계속 응답한다.
  - Play Reset 버튼과 Command Palette/Console `resetplay` 명령은 현재 runtime clone을 잠긴 edit snapshot에서 다시 생성해 물리/스크립트 실험을 Stop 없이 초기화한다.

## v1 작업 메모

- `Hierarchy` 창 상단에 entity 검색창을 추가했다.
- 검색 대상은 entity name, Camera/Light/Mesh/Physics/Script/Animator/Prefab/SceneReference/2D/UI/Audio/Navigation/Network/Nested 태그다.
- `Hierarchy` 검색창 옆 quick filter는 검색어와 함께 적용되어 컴포넌트/상태별 Entity 탐색을 빠르게 만든다.
- `Project` 창 상단에 asset 검색창을 추가했다.
- 검색 대상은 asset name과 full path이며, 매칭되는 자식이 있는 폴더는 검색 중 자동으로 열린다.
- `Console` 창 상단에 log 검색창을 추가했다.
- 검색어가 없으면 기존처럼 최근 로그만 보여주고, 검색어가 있으면 전체 asset log에서 매칭되는 줄을 보여준다.
