# KVM 스로틀 재구성, 페이지 단위 물리 메모리 조회, IRQ 실시간 로깅에 대한 체계적 설계 문서

본 문서는 QEMU에 다음 세 가지 기능을 연구적으로 설계·구현하기 위한 절차를 세부 단계까지 기술한다. 목표 독자는 QEMU 내부 구조를 처음 접하는 엔지니어라도 본 문서만으로 동일한 기능을 재현할 수 있도록 돕는 것이다.

1. KVM 실행률 제어(스로틀)를 토큰 버킷 기반으로 재구성하여 장시간 `KVM_RUN` 동안에도 일정한 vCPU 실행률을 보장한다.
2. QMP(QEMU Machine Protocol)를 통해 페이지 단위로 게스트 물리 메모리를 직접 조회할 수 있는 인터페이스를 제공한다.
3. IRQ 전달 경로에 실시간 로깅 토글을 도입하여, QMP 요청 한 번으로 IRQ 발생 이력을 추적할 수 있게 한다.

이 문서는 각 기능에 대해 (a) 요구사항 분석, (b) 자료구조와 API 설계, (c) 구현 절차 및 코드 스니펫, (d) 동시성·성능·검증 전략을 순차적으로 다룬다.

---

## 1. 연구 배경 및 문제 정의

### 1.1 KVM 스로틀링 이슈
- 기존 `accel/kvm/kvm-throttle.c` 구현은 기벽적으로 모노토닉(clock_gettime) 기반 타임 차이를 이용해 vCPU 실행을 제한한다.
- 그러나
  1. 스레드가 호스트 스케줄러에 의해 선점되면 모노토닉 시간만으로는 실제 vCPU 실행 시간을 정확히 추정할 수 없다.
  2. 장시간 `KVM_RUN` 호출이 이어지면, 타이머 인터럽트가 지연되어 스로틀 주기가 목표치를 크게 초과한다.
- 목표는 스레드 CPU 시간(`CLOCK_THREAD_CPUTIME_ID`)을 적극 활용하고, 고해상도 타이머 만료 시 즉시 `KVM_RUN`을 빠져나오도록 하여 원하는 실행률을 유지하는 것이다.

### 1.2 QMP 기반 물리 메모리 조회 필요성
- 기존 `pmemsave`는 파일을 경유해야 하므로 대화식 디버깅, 스크립팅 환경에서 사용하기 불편하다.
- 사용자 공간 도구(QMP 클라이언트)가 원하는 주소·페이지 수만큼 즉시 메모리를 확보할 수 있는 JSON 기반 응답이 필요하다.

### 1.3 IRQ 로깅 관측성 문제
- IRQ 전달 경로를 실시간으로 관측하려면 기존에는 트레이스 포인트나 디버거를 사용해야 했다.
- QMP 사용자(예: 자동화 테스트)는 RPC 한 번으로 로깅을 켜고 끄며, 표준 출력(stderr)을 통해 즉시 로그를 수집할 수 있는 경량 배송 메커니즘을 원한다.

---

## 2. KVM 토큰 버킷 스로틀 재설계

### 2.1 기능 요구사항 요약
1. 주기(period)와 실행률(ratio)을 입력으로 받아 토큰 버킷을 구성한다.
2. 스레드 CPU 시간 우선, 모노토닉 시간 보조를 이용해 남은 버짓을 계산한다.
3. 버짓이 고갈되면 `sleep_until_ns()`로 다음 주기 시작 시점까지 슬립한다.
4. 고해상도 타이머 만료 시 `qemu_cpu_kick()`을 호출해 `KVM_RUN`을 즉시 종료하도록 한다.
5. 다중 vCPU 환경에서 각 vCPU 스레드가 독립적인 버짓을 유지하되, 공통 설정은 글로벌 컨텍스트를 이용한다.

### 2.2 자료구조 설계 (`include/system/kvm-throttle.h`)
`ThrottleCfg` 구조체에 아래 필드를 추가한다.

| 필드명 | 타입 | 의미 |
| ------ | ---- | ---- |
| `budget_ns` | `int64_t` | 현 주기에서 남은 실행 가능 시간(ns) |
| `window_start_ns`, `window_end_ns` | `int64_t` | 현재 버킷 주기의 시작과 끝 타임스탬프 |
| `last_check_ns` | `int64_t` | 가장 최근에 모노토닉 시간을 측정한 시각 |
| `thread_last_ns` | `int64_t` | 최근 스레드 CPU 시간 측정값 |
| `thread_time_valid` | `bool` | 스레드 CPU 시간 측정이 유효한지 여부 |
| `timer` | `QEMUTimer *` | 스로틀 타이머 핸들 |
| `immediate_exit` | `bool` | 타이머 만료 시 `kvm_vcpu` 구조체에 설정해 `KVM_RUN`을 빠져나오도록 하는 플래그 |

또한 vCPU당 `ThrottleState`(예: `struct KVMState::throttle_state[i]`)에 위 정보를 담도록 한다.

### 2.3 시간 측정 및 버짓 계산 알고리즘

1. **시간 취득**
   - `mono_now_ns()`는 `clock_gettime(CLOCK_MONOTONIC, …)` 호출로 구현한다.
   - `thread_time_now_ns()`는 `clock_gettime(CLOCK_THREAD_CPUTIME_ID, …)`로 얻는다. POSIX 스레드 스케줄링 정책에 따라 실패할 수 있으므로, 실패 시에는 `thread_time_valid = false`로 설정하고 모노토닉 시간만 사용한다.

2. **버짓 재계산**
   - 새로운 주기가 시작되었는지 확인: if `now >= window_end_ns` -> `window_start_ns = now`, `window_end_ns = now + period_ns`, `budget_ns = period_ns * ratio` (ratio가 0..1 범위라면 실수가 될 수 있으므로 고정 소수점 또는 정수 비율 `(quota_ns, period_ns)` 형태로 유지한다).
   - 지난 측정 이후 경과 시간 `delta = thread_delta`(스레드 CPU 시간) 또는 `mono_delta`(모노토닉 시간). `budget_ns -= delta`.
   - `budget_ns`가 0 이하이면 슬립이 필요하다.

3. **슬립 및 타이머 재등록**
   - 슬립이 필요하다면 `sleep_until_ns(window_end_ns)` 호출 (통상 `qemu_cond_timedwait_iothread` 사용).
   - 슬립하지 않는 경우, 남은 버짓만큼 타이머를 arm: `timer_mod_ns(timer, now + budget_ns)`.

4. **타이머 콜백** (`kvm_thr_on_expire`)
   - 타이머 만료 시 `immediate_exit = true`를 설정하고 `qemu_cpu_kick(vcpu)` 호출. `immediate_exit` 플래그는 `kvm_run` 루프에서 체크되어 `KVM_RUN`을 빠져나오도록 한다.

### 2.4 구현 절차

#### 2.4.1 헤더 확장
1. `include/system/kvm-throttle.h`에 새로운 필드, 헬퍼 함수 프로토타입을 추가한다.
2. 주석에 각 필드의 쓰임, 단위(ns), 초기화 조건을 상세히 기술한다.

#### 2.4.2 초기화 루틴
1. `accel/kvm/kvm-throttle.c`에서 `kvm_thr_set_all()` 내에 다음을 추가한다.
   ```c
   static void kvm_thr_reset_bucket(ThrottleState *ts, int64_t now_ns)
   {
       ts->window_start_ns = now_ns;
       ts->window_end_ns = now_ns + ts->period_ns;
       ts->budget_ns = mult_fraction(ts->quota_ns, ts->period_ns, ts->period_ns);
       ts->last_check_ns = now_ns;
       ts->thread_time_valid = thread_time_now_ns(&ts->thread_last_ns);
   }
   ```
2. `kvm_thr_set_all()` 호출 시 모든 vCPU에 대해 `kvm_thr_reset_bucket()`을 실행하고, 기존에 설치된 타이머가 있다면 `timer_del()`로 해제한다.

#### 2.4.3 실행 전 진입 훅 수정
1. `kvm_thr_tick_before_exec(KVMState *s, CPUState *cpu)`를 아래 의사코드에 맞게 수정한다.
   ```c
   now = mono_now_ns();
   if (now >= ts->window_end_ns) {
       kvm_thr_reset_bucket(ts, now);
   }

   if (thread_time_now_ns(&thread_now)) {
       delta = thread_now - ts->thread_last_ns;
       ts->thread_last_ns = thread_now;
       ts->thread_time_valid = true;
   } else {
       delta = now - ts->last_check_ns;
       ts->thread_time_valid = false;
   }
   ts->last_check_ns = now;
   ts->budget_ns -= delta;

   if (ts->budget_ns <= 0) {
       sleep_until_ns(ts->window_end_ns);
       kvm_thr_reset_bucket(ts, ts->window_end_ns);
   } else {
       timer_mod_ns(ts->timer, now + ts->budget_ns);
   }
   ts->immediate_exit = false;
   ```

#### 2.4.4 `KVM_RUN` 탈출 보장
- `kvm_cpu_exec()` 루프에서 `ts->immediate_exit`를 체크해 true이면 `kvm_vcpu_kick()` 호출 후 루프를 빠져나오도록 한다. 기존 구현이 이미 유사 기능을 제공한다면 플래그만 재사용한다.

### 2.5 동시성 및 오류 처리
- `thread_time_now_ns()`가 실패하면(예: 특정 플랫폼에서 미지원) 모노토닉 기반 fallback 경로가 동작하도록 한다.
- 타이머 콜백과 메인 루프가 동일한 `ThrottleState`를 갱신하므로, 필요한 경우 `qemu_spin_lock` 등을 도입한다. 실험적으로 타이머와 vCPU 스레드가 같은 affinity를 갖는 경우 lock-free로도 안전하나, 코드 가독성을 위해 `ts->lock`을 추가할 수 있다.

### 2.6 검증 시나리오
1. **정량 검증**: host에서 `perf stat` 혹은 `/proc/<pid>/schedstat`으로 vCPU 스레드의 CPU 사용 시간을 측정하여, 목표 ratio 대비 오차가 ±5% 이내인지 확인한다.
2. **선점 시나리오**: `stress-ng --cpu` 등으로 host를 과부하시키고, 새 구현이 기존 대비 오차가 감소하는지 측정한다.
3. **장시간 실행**: 30분 이상 `KVM_RUN`을 유지하면서 타이머가 목표 주기마다 정확히 트리거되는지 로그로 확인한다.

---

## 3. 페이지 단위 물리 메모리 QMP 명령 설계

### 3.1 요구사항
1. QMP 명령 이름: `query-phys-pages`.
2. 입력 인자: `addr`(64비트 정수), `num-pages`(정수, 기본 1, 최대 64).
3. 출력: 각 페이지마다 시작 주소(`base`), 페이지 크기(`size`), 16진수 문자열 리스트(`rows`).
4. 오류 조건: 인자 누락, 정렬되지 않은 주소, 페이지 수 초과, 게스트 메모리 접근 실패.

### 3.2 QAPI 스키마 확장 (`qapi/machine.json`)
```json
{ 'struct': 'PhysMemPage',
  'data': { 'base': 'uint64', 'size': 'size', 'rows': ['str'] } }

{ 'command': 'query-phys-pages',
  'data': { 'addr': 'uint64', '*num-pages': 'uint16' },
  'returns': ['PhysMemPage'] }
```
- `size` 타입은 QAPI에서 `size` alias가 제공되므로 활용한다.
- 문서 블록(`##`)에 각 필드 설명을 기술한다.

스키마 변경 후 `ninja -C build qapi-gen`을 실행해 `qmp_query_phys_pages()` 스텁을 생성한다.

### 3.3 서버 측 구현 절차 (`system/cpus.c`)
1. **입력 검증**
   ```c
   if (!num_pages) {
       error_setg(errp, "num-pages must be greater than zero");
       return NULL;
   }
   if (num_pages > QMP_QUERY_PHYS_PAGES_MAX) {
       error_setg(errp, "num-pages exceeds limit (%d)", QMP_QUERY_PHYS_PAGES_MAX);
       return NULL;
   }
   if (TARGET_PAGE_SIZE == 0 || (TARGET_PAGE_SIZE & (TARGET_PAGE_SIZE - 1))) {
       error_setg(errp, "invalid target page size");
       return NULL;
   }
   if (addr % TARGET_PAGE_SIZE) {
       error_setg(errp, "addr must be page-aligned (%d)", TARGET_PAGE_SIZE);
       return NULL;
   }
   if (addr > UINT64_MAX - TARGET_PAGE_SIZE * num_pages) {
       error_setg(errp, "address range overflow");
       return NULL;
   }
   ```

2. **페이지 반복 및 데이터 수집**
   ```c
   for (i = 0; i < num_pages; ++i) {
       uint8_t buf[TARGET_PAGE_SIZE];
       hwaddr base = addr + i * TARGET_PAGE_SIZE;
       cpu_physical_memory_read(base, buf, TARGET_PAGE_SIZE);

       rows = NULL;
       for (offset = 0; offset < TARGET_PAGE_SIZE; ++offset) {
           char line[64];
           g_snprintf(line, sizeof(line),
                      "0x%016" PRIx64 " - 0x%02x",
                      (uint64_t)(base + offset), buf[offset]);
           QAPI_LIST_APPEND(strList, rows, g_strdup(line));
       }

       page = g_new0(PhysMemPage, 1);
       page->base = base;
       page->size = TARGET_PAGE_SIZE;
       page->rows = rows;
       QAPI_LIST_PREPEND(PhysMemPageList, ret, page);
   }
   return QAPI_LIST_REVERSE(ret);
   ```

3. **메모리 관리**
   - `cpu_physical_memory_read`가 게스트 메모리 접근에 실패할 가능성이 있으므로 예외 처리를 추가한다. 실패 시 `error_setg` 후 `qapi_free_PhysMemPageList(ret)`로 정리한다.

### 3.4 예제 QMP 세션
```bash
socat - UNIX-CONNECT:/path/to/qmp.sock <<'JSON'
{ "execute": "qmp_capabilities" }
{ "execute": "query-phys-pages",
  "arguments": { "addr": 4096, "num-pages": 2 } }
JSON
```
- 응답 예시:
  ```json
  {
    "return": [
      {
        "base": 4096,
        "size": 4096,
        "rows": [
          "0x0000000000001000 - 0xff",
          "0x0000000000001001 - 0x7a",
          "..."
        ]
      },
      {
        "base": 8192,
        "size": 4096,
        "rows": [ "..." ]
      }
    ]
  }
  ```

### 3.5 검증 및 성능 고려
- `num-pages`를 크게 설정하면 JSON 응답이 커지므로 최대값을 64로 제한한다.
- 게스트가 대규모 메모리에 매핑되지 않은 페이지를 요청하는 경우 대비해 `cpu_physical_memory_read`가 0x00으로 채워진 버퍼를 반환하도록 한다.
- 테스트: 게스트 OS 내에서 알려진 패턴(예: `memset`)을 만든 뒤 `query-phys-pages`로 확인하여 정확성을 검증한다.

---

## 4. 실시간 IRQ 로깅 QMP 명령 설계

### 4.1 설계 목표
1. 로깅 토글이 QMP 요청 한 번으로 이루어질 것.
2. 핫패스(`qemu_set_irq`)에 도입되는 오버헤드를 최소화할 것.
3. 로깅 출력은 기본적으로 `stderr`로 제공하고, 필요 시 다른 싱크로 대체할 수 있는 확장점을 마련할 것.

### 4.2 QAPI 명세 (`qapi/machine.json`)
```json
{ 'command': 'irq-log-set',
  'data': { 'enable': 'bool' },
  'returns': 'nothing' }
```
- 문서 블록에 `enable=true`면 로깅 활성화, `false`면 비활성화됨을 명시한다.

### 4.3 시스템 레벨 구현 순서

#### 4.3.1 핸들러 구현 (`system/cpus.c`)
```c
void qmp_irq_log_set(bool enable, Error **errp)
{
    if (qemu_irq_log_is_enabled() == enable) {
        return;
    }
    qemu_irq_log_set_enabled(enable);
}
```
- 동일 상태로의 중복 토글을 허용하지만 불필요한 출력은 방지한다.

#### 4.3.2 IRQ 서브시스템 확장 (`include/hw/irq.h`, `hw/core/irq.c`)
1. 헤더에 다음 프로토타입을 추가한다.
   ```c
   void qemu_irq_log_set_enabled(bool enable);
   bool qemu_irq_log_is_enabled(void);
   ```
2. 구현부에서는 원자적 정수(`int` 또는 `atomic_int`)를 사용한다.
   ```c
   static QAtomicInt irq_log_enabled;

   void qemu_irq_log_set_enabled(bool enable)
   {
       int old = qatomic_xchg(&irq_log_enabled, enable);
       if (old != enable) {
           fprintf(stderr, "irq-log: %s\n", enable ? "enabled" : "disabled");
       }
   }

   bool qemu_irq_log_is_enabled(void)
   {
       return qatomic_read(&irq_log_enabled);
   }
   ```

#### 4.3.3 핫패스 로깅 (`qemu_set_irq`)
```c
void qemu_set_irq(qemu_irq irq, int level)
{
    if (qemu_irq_log_is_enabled()) {
        int64_t ts = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        g_autofree char *path = object_get_canonical_path(OBJECT(irq));

        error_printf("irq-log: time=%" PRId64 "ns irq=%p path=%s kind=%s "
                     "handler=%p opaque=%p n=%d level=%d\n",
                     ts, irq, path ? path : "(anonymous)",
                     irq_classification(irq), irq->handler,
                     irq->opaque, irq->n, level);
    }
    irq->handler(irq->opaque, irq->n, level);
}
```
- `object_get_canonical_path()`를 통해 QOM 경로를 함께 기록하면 인터럽트 출처(디바이스/라인)를 즉시 파악할 수 있다.
- `irq_classification()` 헬퍼는 ARM GIC 계열이면 SGI/PPI/SPI 범위를 이용해 `software (SGI)`, `percpu (PPI)`, `hardware (SPI)`로 표기하고, 그 외 컨트롤러는 기본적으로 `hardware`로 표시한다.
- `QEMU_CLOCK_REALTIME` 대신 `QEMU_CLOCK_HOST`를 선택해도 무방하다. ns 단위 타임스탬프 확보가 목적이다.
- 출력 포맷을 고정 문자열로 유지하면 외부 파서가 손쉽게 분석할 수 있다.

### 4.4 사용 패턴
1. 로깅 활성화
   ```bash
   { "execute": "irq-log-set", "arguments": { "enable": true } }
   ```
   이후 `stderr`에는 모든 IRQ 이벤트가 한 줄씩 출력된다.
2. 로깅 비활성화
   ```bash
   { "execute": "irq-log-set", "arguments": { "enable": false } }
   ```
   즉시 `irq-log: disabled` 메시지가 출력되며, 추가 로그는 발생하지 않는다.

### 4.5 성능 및 확장 고려
- 로깅 비활성화 시 오버헤드는 `qatomic_read` 한 번뿐이다.
- 활성화 시 `fprintf` 비용이 크므로, 장시간 활성화를 예상하면 `stderr`를 파일로 리다이렉션하거나 버퍼링을 사용한다.
- 향후 요구사항에 따라 `qemu_irq_log_set_sink(enum sink)` 같은 API를 도입하여 trace backend, ring buffer 등을 선택할 수 있다.

---

## 5. 통합 빌드 및 검증 절차

1. **코드 변경**: 위에서 언급한 각각의 파일에 수정을 적용한다.
2. **빌드 시스템 갱신**: QAPI 변경 후 `ninja -C build qapi-gen`을 실행한다.
3. **전체 빌드**: `ninja -C build` 또는 해당 메이크 타깃을 수행한다.
4. **단위 검증**
   - 스로틀: vCPU 하나를 가진 간단한 게스트를 실행하고 `taskset`, `perf`로 실행률을 측정한다.
   - 메모리 조회: 게스트에 알려진 데이터 패턴을 써 넣은 후 `query-phys-pages`로 읽어 비교한다.
   - IRQ 로깅: 가상 장치를 트리거하여 IRQ를 발생시키고 `stderr` 로그를 확인한다.
5. **회귀 테스트**: 기존 QEMU test suite (`make check`)를 통과하는지 확인한다.

---

## 6. 배포 및 문서화 권고

- 사용자 문서(`docs/system/monitor.rst`)에 `query-phys-pages`, `irq-log-set` 명령 예시를 추가한다.
- 내부 개발 문서에 스로틀 파라미터(기간, 비율)의 기본값과 권장값을 표로 정리한다.
- 릴리스 노트에 새로운 기능과 사용 방법을 명시해 외부 사용자에게 제공한다.

---

## 7. 향후 연구 과제

1. **스로틀링**: 스레드 간 공정성을 위해 cgroup 기반 CPU share와의 상호 작용을 평가한다.
2. **메모리 조회**: 16진수 문자열 대신 Base64 등 압축된 표현을 지원해 대규모 덤프에도 대응한다.
3. **IRQ 로깅**: 필터링(예: 특정 IRQ 번호만), 레이트 리미터, JSON 기반 로그 전송을 추가해 대규모 시스템에서도 활용 가능하게 확장한다.

---

본 문서에 따라 구현을 진행하면 KVM 실행률 제어, 페이지 단위 메모리 조회, IRQ 로깅 기능을 서로 독립적으로 혹은 동시에 도입할 수 있으며, 각 기능은 QEMU의 기존 인프라스트럭처와 충돌 없이 통합된다.
