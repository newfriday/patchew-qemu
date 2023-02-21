#ifndef APIC_H
#define APIC_H


/* apic.c */
void apic_deliver_irq(uint32_t dest, uint8_t dest_mode, uint8_t delivery_mode,
                      uint8_t vector_num, uint8_t trigger_mode);
int apic_accept_pic_intr(DeviceState *s);
void apic_deliver_pic_intr(DeviceState *s, int level);
void apic_deliver_nmi(DeviceState *d);
int apic_get_interrupt(DeviceState *s);
void cpu_set_apic_base(DeviceState *s, uint64_t val);
uint64_t cpu_get_apic_base(DeviceState *s);
void cpu_set_apic_tpr(DeviceState *s, uint8_t val);
uint8_t cpu_get_apic_tpr(DeviceState *s);
void apic_init_reset(DeviceState *s);
void apic_sipi(DeviceState *s);
void apic_poll_irq(DeviceState *d);
void apic_designate_bsp(DeviceState *d, bool bsp);
int apic_get_highest_priority_irr(DeviceState *dev);
uint64_t apic_register_read(int index);
void apic_register_write(int index, uint64_t val);
bool is_x2apic_mode(DeviceState *d);

/* pc.c */
DeviceState *cpu_get_current_apic(void);

#endif
