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
