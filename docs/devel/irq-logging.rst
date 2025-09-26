=====================
IRQ Logging Utilities
=====================

``hw/core/irq.c`` now logs additional metadata when the IRQ logger is
enabled: the host thread id (``host-tid=``) and the call site return address
(``caller=``).  The helper script below makes it easy to map those fields back
to friendly names while sifting through large traces.

Running the helper
------------------

.. code-block:: bash

   $ scripts/irqlog-analyze.py --pid $(pgrep qemu-system-x86_64) \
         --binary build/qemu-system-x86_64 irq.log

Arguments:

``--pid``
   Uses ``/proc/<pid>/task/<tid>/comm`` to resolve thread names for the
   recorded host TIDs.  Omit the option on non-Linux hosts or when the process
   has already exited.

``--binary``
   Path to the QEMU binary with symbols.  The tool runs ``addr2line`` to
   resolve ``caller=`` addresses to function names and source locations.  Set
   ``ADDR2LINE=/path/to/eu-addr2line`` if you prefer an alternative resolver.

The output summarises each IRQ log block, preserving the original lines for
reference.  Example (심볼 정보를 사용할 수 없는 빌드에서는 ``caller-symbol``이
공백일 수 있다):

.. code-block:: none

   - time=12508130441234ns level=0 n=0 kind=hardware
     path=/machine/q35/ioapic/unnamed-gpio-in[0]
     irq=0x55fe509e8550 handler=0x55fe0f323c30 opaque=0x55fe509e7630
     tid=41267 (main-loop) caller=0x55fe0f1a4c90 -> pci_update_irq_pending
      /home/user/qemu/hw/pci/pci.c:1832

It is still possible to use ``addr2line`` manually if the helper is not
available:

.. code-block:: bash

   $ addr2line -Cfpe build/qemu-system-x86_64 0x55fe0f1a4c90

Similarly, the raw TID can be matched to a thread name via ``ps -L`` or
``top -H`` if the process is still running.

QMP interface
-------------

When 로깅이 활성화되어 있으면 QMP 명령 ``query-irq-log``를 통해 링 버퍼에
축적된 최신 IRQ 이벤트를 직접 받아볼 수 있다. 예시:

.. code-block:: bash

   (QMP) { "execute": "irq-log-set", "arguments": { "enable": true } }
   (QMP) { "execute": "query-irq-log", "arguments": { "max-entries": 8 } }
   (QMP) { "execute": "irq-log-set", "arguments": { "enable": false } }

``query-irq-log``는 다음 필드를 포함하는 배열을 반환한다. 스레드 이름은
호스트에서 보고하는 값을 그대로 사용하므로(예: Linux의 ``/proc/self/task/.../comm``)
16자 제한이 있으며, 메인 스레드는 대체로 ``qemu-system-x86_64``와 같이 실행 파일
이름이 그대로 보인다.

* ``timestamp-ns``: IRQ가 발생한 호스트 시각 (ns)
* ``level`` / ``irq-line`` / ``kind`` / ``path``: IRQ 상태 정보
* ``host-tid`` / ``thread-name``: IRQ를 발생시킨 호스트 스레드 식별자 및 이름
* ``caller-addr`` / ``caller-symbol``: IRQ를 발생시킨 QEMU 코드 위치 (주소·심볼)

``max-entries``로 반환 길이를 제한할 수 있으며 ``filter-tid`` 또는
``filter-line``을 사용하면 특정 스레드나 IRQ 라인만 선택적으로 조회할 수 있다.
이 명령을 사용하면 외부 `addr2line` 없이도 IRQ의 원인을 즉시 파악할 수 있다.

Windows 스케줄러 추적
----------------------

Windows 게스트에서 vCPU별로 어떤 커널 스레드가 실행되고 있는지 실시간으로
확인하기 위해 새로운 QMP 명령 ``windows-sched-trace-set``과
``query-windows-sched-trace``를 추가했다. 추적이 활성화되면 KVM이 게스트로부터
빠져나올 때마다 현재 커널 스케줄러 상태를 샘플링하고 링 버퍼에 저장한다.

활성화·비활성화:

.. code-block:: bash

   (QMP) { "execute": "windows-sched-trace-set",
            "arguments": { "enable": true } }
   (QMP) { "execute": "query-windows-sched-trace",
            "arguments": { "max-entries": 16 } }
   (QMP) { "execute": "windows-sched-trace-set",
            "arguments": { "enable": false } }

* ``timestamp-ns``: 이벤트가 기록된 호스트 시각
* ``vcpu``: 이벤트가 발생한 vCPU 인덱스
* ``thread-pointer``: 현재 실행 중인 ``KTHREAD/ETHREAD``의 게스트 가상 주소
* ``process-pointer``: 소유 ``EPROCESS`` 주소 (추론 가능할 때)
* ``unique-process-id`` ``unique-thread-id``: Windows 식별자 (CLIENT_ID 기반)
* ``process-image``: ``EPROCESS.ImageFileName``에서 취득한 15바이트 이미지 이름
* ``thread-name``: Windows 11에서 제공하는 UNICODE thread name (있을 때)
* ``kthread-state``: ``KTHREAD.State`` 값 (0=Initialized, 1=Ready, ...)

필터
~~~~

``query-windows-sched-trace``는 다음과 같은 필터를 지원한다.

``filter-vcpu``
   특정 vCPU에서 발생한 전환만 조회한다.

``filter-pid`` / ``filter-tid``
   CLIENT_ID 기반으로 프로세스/스레드 ID를 지정한다. ID 정보가 없으면 해당
   항목은 자동으로 건너뛴다.

자동 구조체 오프셋 감지
~~~~~~~~~~~~~~~~~~~~~~~~

기본적으로 QEMU는 커널이 유지하는 ``KDDEBUGGER_DATA64`` 블록을 읽어 Windows 내부
구조체의 오프셋을 계산한다. 이는 Windows 10/11 계열에서 안정적으로 동작하며,
특정 빌드에서 레이아웃이 변경되면 추적이 비활성화된 상태로 대기한다.
오프셋이 성공적으로 결정되면 ``query-windows-sched-trace``가 유의미한 데이터를
반환한다.

자동 감지가 실패하거나 실험적인 빌드(예: Insider Preview)에서 오프셋이 다를 때는
수동으로 값을 지정할 수 있다. ``windows-sched-trace-set``에 ``auto-detect=false``와
오버라이드를 함께 전달하면 된다.

.. code-block:: bash

   (QMP) { "execute": "windows-sched-trace-set",
            "arguments": {
               "enable": true,
               "auto-detect": false,
               "overrides": {
                 "kpcr-current-prcb": 0x180,
                 "prcb-current-thread": 0x8,
                 "kthread-apc-process": 0x220,
                 "kthread-client-id": 0x878,
                 "kthread-state": 0x32c,
                 "ethread-thread-name": 0x870,
                 "eprocess-image-file-name": 0x5a8
               }
            } }

위 값은 Windows 11 24H2 (빌드 26100) 기준으로 검증되었으며, 다른 빌드에서는
Windbg/LiveKd 등의 도구로 구조체 레이아웃을 확인한 뒤 조정이 필요할 수 있다.

제한 사항
~~~~~~~~~

* 현재 구현은 x86_64 KVM 가상 머신에서만 동작한다. (TCG·WHPX 지원 예정 없음)
* 샘플링은 VM이 KVM을 빠져나오는 시점에 이루어지므로 컨텍스트 전환 순간과
  로그 사이에 약간의 지연이 있을 수 있다.
* 스레드 이름은 Windows 11 API를 통해 명시적으로 설정된 경우에만 등장한다.
* ``process-image``는 ``EPROCESS.ImageFileName``의 첫 15바이트이므로 긴 경로는
  잘린다.

이 기능을 활용하면 WinDbg 같은 외부 디버거 없이도 vCPU가 어떤 커널 스레드를
실행하고 있는지, 어떤 프로세스 컨텍스트로 전환되었는지를 빠르게 파악할 수 있다.
