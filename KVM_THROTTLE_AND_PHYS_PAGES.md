# KVM 스로틀 재구성 및 페이지 단위 물리 메모리 조회 구현 노트

## 1. 배경
- 기존 `accel/kvm/kvm-throttle.c` 로직은 단순 모노토닉 시계 기반으로 실행 시간을 계산해 스레드 선점이나 장시간 `KVM_RUN`에서 스로틀 유지력이 떨어졌습니다.
- 물리 메모리를 QMP로 직접 확인하려면 기존 `pmemsave`처럼 파일을 거쳐야 했기 때문에, 인터랙티브하게 페이지 단위로 조회할 수 있는 인터페이스가 필요했습니다.

## 2. KVM 스로틀 토큰 버킷화
### 2.1 핵심 구조 (`include/system/kvm-throttle.h`)
- `ThrottleCfg`에 아래 필드를 추가했습니다.
  - `budget_ns`: 현재 주기에서 남은 실행 가능 시간(ns)
  - `last_check_ns`: 마지막으로 모노토닉 시간을 읽어 버짓을 갱신한 시각
  - `thread_last_ns`, `thread_time_valid`: `CLOCK_THREAD_CPUTIME_ID`로 얻은 스레드 CPU 시간 추적용
- 스레드별 타이머(`on_timer`)는 유지하지만, 만료 시 `qemu_cpu_kick()`을 호출해 즉시 `KVM_RUN`이 빠져 나오도록 개선했습니다 (`accel/kvm/kvm-throttle.c:59-66`).

### 2.2 토큰 버킷 흐름 (`accel/kvm/kvm-throttle.c`)
1. `kvm_thr_set_all()`에서 새 스로틀 비율이 들어오면 주기/버짓 상태를 초기화하고 이미 걸린 타이머는 취소합니다.
2. `kvm_thr_tick_before_exec()`이 진입할 때마다 아래 순서로 동작합니다.
   - 현재 모노토닉 시간(`mono_now_ns()`)과 가능하면 스레드 CPU 시간(`thread_time_now_ns()`)을 읽습니다.
   - 타임 윈도가 지나면 `window_start/end`, `budget_ns`를 재설정해 드리프트를 제거합니다.
   - 유효한 스레드 CPU 시간이라면 직전 측정부터의 차이를 버짓에서 차감합니다. 불가능할 경우 모노토닉 시간 차이로 보정합니다.
   - 버짓이 소진되면 `sleep_until_ns()`로 주기 끝까지 절대시간 슬립 후 다음 주기를 시작합니다.
   - 버짓이 남아 있으면 `timer_mod_ns()`로 남은 실행 시간만큼 타이머를 재등록합니다.
3. 타이머 콜백(`kvm_thr_on_expire`)은 `immediate_exit` 플래그를 세우고 `qemu_cpu_kick()`으로 스레드를 깨워 즉시 스로틀 구간으로 진입하게 합니다.

### 2.3 장점
- 스레드 CPU 시간을 우선 사용함으로써 호스트의 선점 지연에 영향 받지 않는 보다 정확한 스로틀이 가능해집니다.
- 하이레졸루션 타이머와 즉시 킥(kick)으로 장시간 `KVM_RUN` 루프에서도 빠르게 빠져나와 스로틀 목표치를 유지합니다.

## 3. 페이지 단위 물리 메모리 QMP 명령
### 3.1 QAPI 확장 (`qapi/machine.json`)
- `PhysMemPage` 구조체와 `query-phys-pages` 명령을 정의했습니다. 기본 요청은 1페이지이며 최대 64페이지로 클램프합니다.
- 각 페이지는 시작 물리 주소, 페이지 크기와 함께 "주소 - 값" 형식의 문자열 리스트(`rows`)를 제공합니다.

### 3.2 명령 처리기 (`system/cpus.c`)
1. 페이지 버퍼를 읽은 뒤 각 바이트를 `"주소 - 값"` 포맷 문자열로 변환하기 위해 QAPI의 `strList`를 채웁니다.
2. `qmp_query_phys_pages()`에서는 입력 검증을 수행합니다.
   - `num-pages`가 0이면 에러.
   - 타겟 페이지 크기(`TARGET_PAGE_SIZE`)가 0이거나 비정렬이면 에러.
   - 시작 주소 정렬 여부, 범위 오버플로(`addr + size`)를 검사합니다.
3. 요청한 페이지 수만큼 반복하며 `cpu_physical_memory_read()`로 내용을 읽고, 각 바이트마다 `StrList`에 `"0x%016PRIx64 - 0x%02x"` 형태의 문자열을 추가한 뒤 `PhysMemPageList` 노드를 구성해 반환합니다.
4. 페이지 수는 `QMP_QUERY_PHYS_PAGES_MAX`(64)로 제한해 QMP 응답 크기를 관리합니다.

### 3.3 사용 예시
```json
{ "execute": "query-phys-pages", "arguments": { "addr": 4096, "num-pages": 2 } }
```
- 응답의 `rows` 배열에는 각 바이트가 `"0x0000000000001000 - 0xff"`와 같은 한 줄 문자열로 담겨 있습니다.
- 더 큰 덤프가 필요할 경우 이 명령을 반복 호출해 페이지 단위로 이어 붙입니다.

## 4. 빌드 및 활용 메모
1. QAPI 스키마 변경 후에는 `meson setup build` (또는 기존 빌드 디렉터리)에서 `ninja -C build qapi-gen`을 실행해 생성 파일을 갱신합니다.
2. 전체 빌드 후 KVM 게스트를 실행하여 스로틀 비율별 CPU 사용률과 `query-phys-pages` 명령 응답을 확인하세요.
