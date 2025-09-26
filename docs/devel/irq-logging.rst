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
reference.  Example:

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
