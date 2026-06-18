# Editor Convenience Work Resume Notes

작성일: 2026-06-18

이 문서는 긴 에디터 편의 기능 수렴 작업을 잠시 멈추고 나중에 바로 재개하기 위한 메모다.

## 이번에 마무리한 작업

- Command Palette에 `All`, `Commands`, `Entities`, `Assets` scope filter를 추가했다.
- scope filter는 프로젝트별 `Settings/EditorProjectState.json`의 `commandPalette.scope`에 저장/로드된다.
- Command Palette item에 scope를 부여했다.
  - 일반 menu/editor 명령: `Commands`
  - Scene entity 검색 결과: `Entities`
  - Project asset 검색 결과: `Assets`
- `Entities` scope는 검색어가 없어도 현재 Scene entity 목록을 표시한다.
- `Assets` scope는 큰 프로젝트에서 전체 asset tree가 과하게 펼쳐지지 않도록 검색어 입력을 요구한다.
- Command Palette 검색 prefix를 추가했다.
  - `>save`, `cmd:save`, `command:save`, `commands:save`는 `Commands` 검색
  - `@camera`, `entity:camera`, `entities:camera`는 `Entities` 검색
  - `/wolf`, `asset:wolf`, `assets:wolf`는 `Assets` 검색
- prefix 검색은 저장된 scope를 바꾸지 않고 이번 검색에만 적용된다.
- README와 `Docs/EditorConvenienceConvergencePlan.md`에 현재 기능 상태를 반영했다.

## 마지막 검증

다음 검증을 완료했다.

- `git diff --check`
  - 공백 오류 없음
  - 기존 LF/CRLF 안내만 출력
- `Debug|x64` 빌드 성공
  - `MSBuild.exe DX12Eninge.vcxproj /p:Configuration=Debug /p:Platform=x64 /m /v:minimal`
- `Release|x64` 빌드 성공
  - `MSBuild.exe DX12Eninge.vcxproj /p:Configuration=Release /p:Platform=x64 /m /v:minimal`

## 재개 시 먼저 확인할 것

1. 런타임에서 Command Palette를 열고 prefix 검색을 직접 확인한다.
   - `Ctrl+P`
   - `>save`
   - `@camera`
   - `/wolf` 또는 프로젝트에 있는 asset 이름
2. scope combo와 prefix가 함께 있을 때 prefix가 우선 적용되는지 확인한다.
3. `Entities` scope에서 빈 검색어로 entity 목록이 나오는지 확인한다.
4. `Assets` scope에서 빈 검색어일 때 안내 문구가 나오는지 확인한다.
5. pinned/recent command가 prefix scope와 함께 있을 때 현재 결과에 존재하는 항목만 승격되는지 확인한다.

## 남은 업무

현재 큰 목표는 Godot/Unity/Unreal/Ogre 계열 에디터 편의 기능을 계속 수렴시키는 것이다. 바로 이어서 할 만한 작업은 아래 순서가 좋다.

1. Command Palette polish
   - prefix 도움말을 Shortcut Reference에 추가
   - scope별 결과 count 표시
   - `Pinned`, `Recent`, `Normal` section header 분리
   - disabled command를 별도 하단 그룹으로 분리

2. Project / Content Browser
   - GPU texture thumbnail cache
   - asset snapshot 증분 업데이트 고도화
   - importer/editor texture resolver 공용화 강화
   - path candidate ordering과 embedded texture fallback 공용 resolver 정리

3. Inspector / Materials
   - Mesh/material serialization apply/revert 정책을 reflection descriptor 기반으로 더 통합
   - material row 단위 편집 edge case 정리
   - material override diff UX 추가 polish

4. Viewport UX
   - Mesh surface measure cache를 full BVH hierarchy로 확장할지 profiling 기준으로 검토
   - selection outline과 transform gizmo 세부 polish

5. Play/Edit Mode
   - Play 중 runtime-only Inspector 조작을 별도 runtime command로 더 명확히 분리
   - runtime edit과 edit-scene mutation guard UX 문구 정리

## 주의 사항

- 워킹트리는 이미 많은 변경분이 있는 dirty 상태다. 관련 없는 파일을 되돌리지 말 것.
- `PerformanceTests1`은 기존 원칙대로 건드리지 말 것.
- 새 작업 전에는 `git status --short`와 관련 파일 `rg`로 현재 상태를 다시 확인할 것.
- 기능을 추가할 때마다 최소 `Debug|x64`, 가능하면 `Release|x64`까지 빌드 확인할 것.
