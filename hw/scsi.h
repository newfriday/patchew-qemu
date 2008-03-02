/* esp.c */
#define ESP_MAX_DEVS 7
typedef void (*espdma_memory_read_write)(void *opaque, uint8_t *buf, int len);
void esp_scsi_attach(void *opaque, BlockDriverState *bd, int id);
void *esp_init(target_phys_addr_t espaddr,
               espdma_memory_read_write dma_memory_read,
               espdma_memory_read_write dma_memory_write,
               void *dma_opaque, qemu_irq irq, qemu_irq *reset);
