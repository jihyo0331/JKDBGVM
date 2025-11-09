# Qt VM Launcher

Qt Widgets 기반의 GUI로 QEMU 가상 머신을 실행하고 VNC 화면, GDB, swtpm 등을 한 번에 붙여 주는 도구입니다. 이 문서는 새 환경에서 필요한 의존성, 환경 변수, 실행 절차를 정리합니다.

## 필수 구성 요소
- Qt 6.2 이상 (`Qt6::Widgets`, `Qt6::Network`)
- CMake 3.16 이상 및 C++17 컴파일러
- `libvncclient` 개발 패키지 (예: Fedora `libvncserver-devel`, Ubuntu `libvncserver-dev`)
- `qemu-system-x86_64`
- `swtpm` (TPM 에뮬레이션이 필요할 때)

## 빌드 방법
```bash
cd qt_vm_launcher
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64
cmake --build build
```
빌드 후 실행 파일은 `qt_vm_launcher/build/qt-vm-launcher`(또는 빌드 타입 디렉터리) 아래에 생성됩니다.

## 환경 변수
런처는 대부분의 설정을 환경 변수에서 읽으며 지정하지 않으면 합리적인 기본값과 자동 탐지를 수행합니다.

| 변수 | 의미 (기본값) |
| --- | --- |
| `QQ_QEMU_BIN` | 사용할 `qemu-system-x86_64` 경로. 기본은 `~/ws/qemu/build/qemu-system-x86_64` → `/home/park/ws/qemu/...` → PATH 검색 순. |
| `QQ_QEMU_MACHINE` | `-machine` 값 (`q35`). |
| `QQ_QEMU_MEMORY` | 메모리 크기 (`4G`). |
| `QQ_VM_DIR` | VM 자산 폴더. 미설정 시 `/home/park/vm/win11` → `~/vm` 순으로 탐색. |
| `QQ_VM_DISK` | 디스크 이미지 경로. 비우면 `QQ_VM_DIR/disk.qcow2`. |
| `QQ_QEMU_OVMF_CODE` / `QQ_QEMU_OVMF_VARS` | OVMF 펌웨어 경로. 지정하지 않으면 `/usr/share/edk2/ovmf/...` 경로들을 자동 검색하고 `QQ_VM_DIR/OVMF_VARS.fd`도 사용. |
| `QQ_QEMU_ACCEL` | 가속기 설정 (`tcg,thread=multi`). `kvm`이면 자동으로 `-enable-kvm`을 붙입니다. |
| `QQ_QEMU_CPU` | `-cpu` 파라미터 (`Westmere,model-id="Intel(R) Xeon(R) CPU E5620 @ 2.40GHz"`). |
| `QQ_QEMU_SMP` | vCPU 개수 (`4`). |
| `QQ_QEMU_KERNEL` / `QQ_QEMU_INITRD` | 커널·initrd 경로. |
| `QQ_QEMU_APPEND` | `-append` 문자열. |
| `QQ_VNC_HOST` | VNC 접속 호스트 (`127.0.0.1`). |
| `QQ_VNC_DISPLAY` | `:1` 형태의 원하는 디스플레이. 비우면 사용 가능한 포트를 자동 탐색. |
| `QQ_ENABLE_TPM` | `1`/`true` 일 때 swtpm을 실행해 가상 TPM 장치를 추가. 기본은 자동 감지(미설정 시 비활성화). |
| `QQ_SWTPM_BIN` | swtpm 바이너리 경로. 기본은 PATH 검색. |
| `QQ_ENABLE_GDB` | `1`이면 QEMU를 `-S -gdb tcp:localhost:PORT`로 띄우고 GUI에서 GDB 터미널을 활성화. 기본은 활성화. |
| `QQ_GDB_BIN` | 사용할 GDB (기본 `gdb`). |
| `QQ_GDB_SYMBOLS` | `file` 명령으로 열 심볼 바이너리. |
| `QQ_GDB_PORT` | GDB 원격 포트 (`1234`). |

모든 경로 변수는 `~/…` 확장을 지원합니다.

## 실행 절차
1. 필요한 환경 변수(특히 `QQ_QEMU_BIN`, `QQ_VM_DIR`, `QQ_VM_DISK`)를 설정합니다.
2. `qt_vm_launcher/build/qt-vm-launcher`를 실행합니다. Qt Creator 내에서도 해당 실행 파일을 Run/Debug 대상으로 지정할 수 있습니다.
3. GUI가 열리면 로그 창에서 준비 상태를 확인하고 **Start**를 클릭합니다.
   - swtpm 활성화 시 `QQ_VM_DIR/tpm/`에 상태가 저장되고, 기존 소켓이 있으면 자동 정리됩니다.
   - `QQ_ENABLE_GDB=1`이면 QEMU가 일시 정지된 채로 뜨고, “Start GDB” 버튼으로 내장 터미널에서 세션을 시작할 수 있습니다.
4. VNC 포트가 열리면 창 가운데에 원격 프레임버퍼가 표시됩니다. 마우스/키보드 이벤트는 그대로 VNC로 전달됩니다.
5. **Stop** 버튼을 누르면 GDB, swtpm, VNC를 모두 종료하고 QEMU 프로세스를 정리합니다.

## 문제 해결
- **QEMU 경로 오류**: 로그에 “Unable to locate qemu-system-x86_64”가 뜨면 `QQ_QEMU_BIN`을 명시적으로 지정합니다.
- **디스크 이미지를 찾지 못함**: `QQ_VM_DIR` 및 `QQ_VM_DISK` 경로에 접근 권한이 있는지 확인합니다.
- **VNC 타임아웃**: QEMU가 그래픽 포트를 열지 못한 경우입니다. QEMU 로그를 확인하고 필요하면 `QQ_VNC_DISPLAY`를 다른 값으로 설정하세요.
- **TPM/스왑 오류**: swtpm 소켓이 남아 있으면 `QQ_VM_DIR/swtpm.sock`을 삭제하거나 `QQ_ENABLE_TPM=0`으로 비활성화합니다.

위 절차만 따르면 새로운 PC에서도 동일한 설정으로 QEMU VM을 빠르게 실행할 수 있습니다.
