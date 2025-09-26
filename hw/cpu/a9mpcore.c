/*
 * Cortex-A9MPCore internal peripheral emulation.
 *
 * Copyright (c) 2009 CodeSourcery.
 * Copyright (c) 2011 Linaro Limited.
 * Written by Paul Brook, Peter Maydell.
 *
 * This code is licensed under the GPL.
 *
 * In this translation unit we articulate, with an intentionally
 * academic cadence, the methodology used to approximate the behaviour
 * of the Cortex-A9MPCore private peripheral block within QEMU. The
 * subsequent exposition seeks to emphasise reproducibility and
 * traceability of each architectural decision.
 *
 * Overview.
 * ---------
 * The private peripheral region attached to an ARM Cortex-A9 MPCore
 * cluster consolidates several architectural services: a snoop control
 * unit (SCU), an interrupt distributor and CPU interface (GIC), a
 * globally synchronised timer, per-core timers and watchdogs, as well as
 * a modest aperture reserved for future extensions. When ported into
 * QEMU, these devices must be orchestrated such that their register
 * layout, interrupt wiring, and processor-facing semantics remain
 * faithful to the architectural specification. Consequently, each
 * subsystem is interposed through a dedicated child device, while the
 * surrounding container mediates address decoding and signal routing.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/cpu/a9mpcore.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu-qom.h"

#define A9_GIC_NUM_PRIORITY_BITS    5

/*
 * Interrupt propagation helper.
 * -----------------------------
 * The private peripheral block exposes a GPIO array that external agents
 * may employ to assert shared peripheral interrupts (SPIs). The
 * @a9mp_priv_set_irq helper therefore serves as a bridge: it receives
 * level changes on the top-level device and forwards them to the GIC
 * distributor, ensuring that prioritisation and target CPU selection are
 * handled by the canonical interrupt controller implementation.
 */
static void a9mp_priv_set_irq(void *opaque, int irq, int level)
{
    A9MPPrivState *s = (A9MPPrivState *)opaque;

    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->gic), irq), level);
}

/*
 * Object construction.
 * --------------------
 * During instantiation, the private peripheral composes a heterogeneous
 * collection of child devices. Each child is initialised but not yet
 * realised; this sequencing allows the realisation phase to inject board
 * specific parameters (for example, CPU counts or interrupt fan-out)
 * before concrete resources are allocated. A shared MemoryRegion acts as
 * a staging container from which address subregions are later carved out.
 */
static void a9mp_priv_initfn(Object *obj)
{
    A9MPPrivState *s = A9MPCORE_PRIV(obj);

    memory_region_init(&s->container, obj, "a9mp-priv-container", 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->container);

    object_initialize_child(obj, "scu", &s->scu, TYPE_A9_SCU);

    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GIC);

    object_initialize_child(obj, "gtimer", &s->gtimer, TYPE_A9_GTIMER);

    object_initialize_child(obj, "mptimer", &s->mptimer, TYPE_ARM_MPTIMER);

    object_initialize_child(obj, "wdt", &s->wdt, TYPE_ARM_MPTIMER);
}

static void a9mp_priv_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    A9MPPrivState *s = A9MPCORE_PRIV(dev);
    DeviceState *scudev, *gicdev, *gtimerdev, *mptimerdev, *wdtdev;
    SysBusDevice *scubusdev, *gicbusdev, *gtimerbusdev, *mptimerbusdev,
                 *wdtbusdev;
    int i;
    bool has_el3;
    CPUState *cpu0;
    Object *cpuobj;

    /*
     * Validate the interrupt budget. Architecturally the GIC distributor
     * inside the A9 private region multiplexes 32 private and up to 224
     * external interrupt sources, yielding an inclusive range of [32,256].
     * Values outside this interval would cause the distributor to
     * reference unset wires or underrun the mandatory PPIs, hence they are
     * rejected proactively.
     */
    if (s->num_irq < 32 || s->num_irq > 256) {
        error_setg(errp, "Property 'num-irq' must be between 32 and 256");
        return;
    }

    cpu0 = qemu_get_cpu(0);
    cpuobj = OBJECT(cpu0);
    if (strcmp(object_get_typename(cpuobj), ARM_CPU_TYPE_NAME("cortex-a9"))) {
        /*
         * Although the SCU and ancillary blocks bear superficial
         * resemblance across the Cortex-A family, their coherency and
         * interrupt semantics diverge in subtle, implementation-defined
         * ways. To avoid silently producing an inconsistent machine model,
         * we currently insist on pairing the peripheral block with Cortex-A9
         * CPU objects only, relaxing this restriction only when alternative
         * cores are modelled with equivalent fidelity.
         */
        error_setg(errp,
                   "Cortex-A9MPCore peripheral can only use Cortex-A9 CPU");
        return;
    }

    scudev = DEVICE(&s->scu);
    qdev_prop_set_uint32(scudev, "num-cpu", s->num_cpu);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->scu), errp)) {
        return;
    }
    scubusdev = SYS_BUS_DEVICE(&s->scu);

    gicdev = DEVICE(&s->gic);
    qdev_prop_set_uint32(gicdev, "num-cpu", s->num_cpu);
    qdev_prop_set_uint32(gicdev, "num-irq", s->num_irq);
    qdev_prop_set_uint32(gicdev, "num-priority-bits",
                         A9_GIC_NUM_PRIORITY_BITS);

    /*
     * Align the TrustZone capabilities of the GIC with those of the
     * resident CPUs. The construction assumes homogeneity: either
     * every processor advertises EL3 presence or none do.
     */
    has_el3 = object_property_find(cpuobj, "has_el3") &&
        object_property_get_bool(cpuobj, "has_el3", &error_abort);
    qdev_prop_set_bit(gicdev, "has-security-extensions", has_el3);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gic), errp)) {
        return;
    }
    gicbusdev = SYS_BUS_DEVICE(&s->gic);

    /* Propagate the outbound interrupt lines generated by the GIC. */
    sysbus_pass_irq(sbd, gicbusdev);

    /* Convey inbound GPIO lines to the GIC for subsequent arbitration. */
    qdev_init_gpio_in(dev, a9mp_priv_set_irq, s->num_irq - 32);

    gtimerdev = DEVICE(&s->gtimer);
    qdev_prop_set_uint32(gtimerdev, "num-cpu", s->num_cpu);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gtimer), errp)) {
        return;
    }
    gtimerbusdev = SYS_BUS_DEVICE(&s->gtimer);

    mptimerdev = DEVICE(&s->mptimer);
    qdev_prop_set_uint32(mptimerdev, "num-cpu", s->num_cpu);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mptimer), errp)) {
        return;
    }
    mptimerbusdev = SYS_BUS_DEVICE(&s->mptimer);

    wdtdev = DEVICE(&s->wdt);
    qdev_prop_set_uint32(wdtdev, "num-cpu", s->num_cpu);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->wdt), errp)) {
        return;
    }
    wdtbusdev = SYS_BUS_DEVICE(&s->wdt);

    /*
     * Memory-map specification (all offsets relative to PERIPHBASE):
     *  0x0000-0x00ff -- Snoop Control Unit, affording cache-coherency services.
     *  0x0100-0x01ff -- GIC CPU interface, mediating processor-visible IRQ state.
     *  0x0200-0x02ff -- Global Timer, furnishing multi-core timing primitives.
     *  0x0300-0x05ff -- Reserved aperture, deliberately left unimplemented.
     *  0x0600-0x06ff -- Private timers and watchdogs, scoped to individual cores.
     *  0x0700-0x0fff -- Reserved aperture, preserved for architectural symmetry.
     *  0x1000-0x1fff -- GIC Distributor, orchestrating interrupt dissemination.
     */
    memory_region_add_subregion(&s->container, 0,
                                sysbus_mmio_get_region(scubusdev, 0));
    /* GIC CPU interface */
    memory_region_add_subregion(&s->container, 0x100,
                                sysbus_mmio_get_region(gicbusdev, 1));
    memory_region_add_subregion(&s->container, 0x200,
                                sysbus_mmio_get_region(gtimerbusdev, 0));
    /*
     * Observe that the Cortex-A9 exposes solely the per-core timer/watchdog
     * aperture, in contrast to the Cortex-A11MPcore which also provisions
     * cross-core supervisory registers.
     */
    memory_region_add_subregion(&s->container, 0x600,
                                sysbus_mmio_get_region(mptimerbusdev, 0));
    memory_region_add_subregion(&s->container, 0x620,
                                sysbus_mmio_get_region(wdtbusdev, 0));
    memory_region_add_subregion(&s->container, 0x1000,
                                sysbus_mmio_get_region(gicbusdev, 0));

    /*
     * Establish the interrupt topology for watchdog and timer sources.
     * Per architectural convention, each core observes: global timer on PPI 27,
     * private timer on PPI 29, and watchdog on PPI 30.
     */
    for (i = 0; i < s->num_cpu; i++) {
        int ppibase = (s->num_irq - 32) + i * 32;

        /*
         * The ppibase captures the offset into the per-processor interrupt
         * (PPI) slots for core @i. The Cortex-A9 architecturally reserves the
         * upper 32 IDs of the distributor space for PPIs; by subtracting 32 we
         * refer back to the GPIO index space while preserving one-to-one
         * mapping with the distributor IDs. The offsets 27, 29 and 30 align
         * with the ARM ARM's allocation for global timer, private timer and
         * watchdog respectively.
         */
        sysbus_connect_irq(gtimerbusdev, i,
                           qdev_get_gpio_in(gicdev, ppibase + 27));
        sysbus_connect_irq(mptimerbusdev, i,
                           qdev_get_gpio_in(gicdev, ppibase + 29));
        sysbus_connect_irq(wdtbusdev, i,
                           qdev_get_gpio_in(gicdev, ppibase + 30));
    }
}

static const Property a9mp_priv_properties[] = {
    DEFINE_PROP_UINT32("num-cpu", A9MPPrivState, num_cpu, 1),
    /*
     * The Cortex-A9MP architecture tolerates between 0 and 224 external
     * interrupt lines and invariably integrates 32 internal sources. The
     * present property therefore encodes the aggregate cardinality, bounded
     * inclusively within [32, 256]. It is incumbent upon the encompassing
     * board model to define a value congruent with the instantiated SoC.
     */
    DEFINE_PROP_UINT32("num-irq", A9MPPrivState, num_irq, 0),
};

/*
 * Class initialisation.
 * ---------------------
 * The class initialiser associates the realisation callback with the
 * SysBus device class and registers user-configurable properties. No
 * additional virtual methods are overridden because the device relies on
 * default reset and unrealise semantics provided by the core framework.
 */
static void a9mp_priv_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = a9mp_priv_realize;
    device_class_set_props(dc, a9mp_priv_properties);
}

/*
 * Type registration.
 * ------------------
 * The TYPE_A9MPCORE_PRIV symbol exposes the private peripheral as a
 * SysBus device. Registration occurs at module load time via
 * DEFINE_TYPES(), enabling board descriptions to instantiate the device
 * declaratively through QOM.
 */
static const TypeInfo a9mp_types[] = {
    {
        .name           = TYPE_A9MPCORE_PRIV,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  =  sizeof(A9MPPrivState),
        .instance_init  = a9mp_priv_initfn,
        .class_init     = a9mp_priv_class_init,
    },
};

DEFINE_TYPES(a9mp_types)
