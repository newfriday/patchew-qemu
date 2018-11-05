 /*
 * QTest testcase for Microbit board using the Nordic Semiconductor nRF51 SoC.
 *
 * nRF51:
 * Reference Manual: http://infocenter.nordicsemi.com/pdf/nRF51_RM_v3.0.pdf
 * Product Spec: http://infocenter.nordicsemi.com/pdf/nRF51822_PS_v3.1.pdf
 *
 * Microbit Board: http://microbit.org/
 *
 * Copyright 2018 Steffen Görtz <contrib@steffen-goertz.de>
 *
 * This code is licensed under the GPL version 2 or later.  See
 * the COPYING file in the top-level directory.
 */


#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "libqtest.h"

#include "hw/arm/nrf51.h"
#include "hw/nvram/nrf51_nvm.h"
#include "hw/gpio/nrf51_gpio.h"
#include "hw/char/nrf51_uart.h"

#include <sys/socket.h>
#include <sys/un.h>

#define FLASH_SIZE          (256 * NRF51_PAGE_SIZE)

static bool wait_for_event(uint32_t event_addr)
{
    int i;

    for (i = 0; i < 1000; i++) {
        if (readl(event_addr) == 1) {
            writel(event_addr, 0x00);
            return true;
        }
        g_usleep(10000);
    }

    return false;
}

static void rw_to_rxd(int sock_fd, const char *in, char *out)
{
    int i;

    g_assert(write(sock_fd, in, strlen(in)) == strlen(in));
    for (i = 0; i < strlen(in); i++) {
        g_assert(wait_for_event(NRF51_UART_BASE + A_UART_RXDRDY));
        out[i] = readl(NRF51_UART_BASE + A_UART_RXD);
    }
    out[i] = '\0';
}

static void w_to_txd(const char *in)
{
    int i;

    for (i = 0; i < strlen(in); i++) {
        writel(NRF51_UART_BASE + A_UART_TXD, in[i]);
        g_assert(wait_for_event(NRF51_UART_BASE + A_UART_TXDRDY));
    }
}

static void test_nrf51_uart(const void *data)
{
    int sock_fd = *((const int *) data);
    char s[10];

    g_assert(write(sock_fd, "c", 1) == 1);
    g_assert(readl(NRF51_UART_BASE + A_UART_RXD) == 0);

    writel(NRF51_UART_BASE + A_UART_ENABLE, 0x04);
    writel(NRF51_UART_BASE + A_UART_STARTRX, 0x01);

    g_assert(wait_for_event(NRF51_UART_BASE + A_UART_RXDRDY));
    writel(NRF51_UART_BASE + A_UART_RXDRDY, 0x00);
    g_assert(readl(NRF51_UART_BASE + A_UART_RXD) == 'c');

    writel(NRF51_UART_BASE + A_UART_INTENSET, 0x04);
    g_assert(readl(NRF51_UART_BASE + A_UART_INTEN) == 0x04);
    writel(NRF51_UART_BASE + A_UART_INTENCLR, 0x04);
    g_assert(readl(NRF51_UART_BASE + A_UART_INTEN) == 0x00);

    rw_to_rxd(sock_fd, "hello", s);
    g_assert(strcmp(s, "hello") == 0);

    writel(NRF51_UART_BASE + A_UART_STARTTX, 0x01);
    w_to_txd("d");
    g_assert(read(sock_fd, s, 10) == 1);
    g_assert(s[0] == 'd');

    writel(NRF51_UART_BASE + A_UART_SUSPEND, 0x01);
    writel(NRF51_UART_BASE + A_UART_TXD, 'h');
    writel(NRF51_UART_BASE + A_UART_STARTTX, 0x01);
    w_to_txd("world");
    g_assert(read(sock_fd, s, 10) == 5);
    g_assert(strcmp(s, "world") == 0);
}


static void fill_and_erase(hwaddr base, hwaddr size, uint32_t address_reg)
{
    /* Fill memory */
    writel(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG, 0x01);
    for (hwaddr i = 0; i < size; ++i) {
        writeb(base + i, i);
        g_assert_cmpuint(readb(base + i), ==, i & 0xFF);
    }
    writel(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG, 0x00);

    /* Erase Page */
    writel(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG, 0x02);
    writel(NRF51_NVMC_BASE + address_reg, base);
    writel(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG, 0x00);

    /* Check memory */
    for (hwaddr i = 0; i < size; ++i) {
        g_assert_cmpuint(readb(base + i), ==, 0xFF);
    }
}

static void test_nrf51_nvmc(void)
{
    uint32_t value;
    /* Test always ready */
    value = readl(NRF51_NVMC_BASE + NRF51_NVMC_READY);
    g_assert_cmpuint(value & 0x01, ==, 0x01);

    /* Test write-read config register */
    writel(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG, 0x03);
    g_assert_cmpuint(readl(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG), ==, 0x03);
    writel(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG, 0x00);
    g_assert_cmpuint(readl(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG), ==, 0x00);

    /* Test PCR0 */
    fill_and_erase(NRF51_FLASH_BASE, NRF51_PAGE_SIZE, NRF51_NVMC_ERASEPCR0);
    fill_and_erase(NRF51_FLASH_BASE + NRF51_PAGE_SIZE,
                   NRF51_PAGE_SIZE, NRF51_NVMC_ERASEPCR0);

    /* Test PCR1 */
    fill_and_erase(NRF51_FLASH_BASE, NRF51_PAGE_SIZE, NRF51_NVMC_ERASEPCR1);
    fill_and_erase(NRF51_FLASH_BASE + NRF51_PAGE_SIZE,
                   NRF51_PAGE_SIZE, NRF51_NVMC_ERASEPCR1);

    /* Erase all */
    writel(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG, 0x01);
    for (hwaddr i = 0; i < FLASH_SIZE / 4; i++) {
        writel(NRF51_FLASH_BASE + i * 4, i);
        g_assert_cmpuint(readl(NRF51_FLASH_BASE + i * 4), ==, i);
    }
    writel(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG, 0x00);

    writel(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG, 0x02);
    writel(NRF51_NVMC_BASE + NRF51_NVMC_ERASEALL, 0x01);
    writel(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG, 0x00);

    for (hwaddr i = 0; i < FLASH_SIZE / 4; i++) {
        g_assert_cmpuint(readl(NRF51_FLASH_BASE + i * 4), ==, 0xFFFFFFFF);
    }

    /* Erase UICR */
    writel(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG, 0x01);
    for (hwaddr i = 0; i < NRF51_UICR_SIZE / 4; i++) {
        writel(NRF51_UICR_BASE + i * 4, i);
        g_assert_cmpuint(readl(NRF51_UICR_BASE + i * 4), ==, i);
    }
    writel(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG, 0x00);

    writel(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG, 0x02);
    writel(NRF51_NVMC_BASE + NRF51_NVMC_ERASEUICR, 0x01);
    writel(NRF51_NVMC_BASE + NRF51_NVMC_CONFIG, 0x00);

    for (hwaddr i = 0; i < NRF51_UICR_SIZE / 4; i++) {
        g_assert_cmpuint(readl(NRF51_UICR_BASE + i * 4), ==, 0xFFFFFFFF);
    }
}

static void test_nrf51_gpio(void)
{
    size_t i;
    uint32_t actual, expected;

    struct {
        hwaddr addr;
        uint32_t expected;
    } reset_state[] = {
        {NRF51_GPIO_REG_OUT, 0x00000000}, {NRF51_GPIO_REG_OUTSET, 0x00000000},
        {NRF51_GPIO_REG_OUTCLR, 0x00000000}, {NRF51_GPIO_REG_IN, 0x00000000},
        {NRF51_GPIO_REG_DIR, 0x00000000}, {NRF51_GPIO_REG_DIRSET, 0x00000000},
        {NRF51_GPIO_REG_DIRCLR, 0x00000000}
    };

    /* Check reset state */
    for (i = 0; i < ARRAY_SIZE(reset_state); i++) {
        expected = reset_state[i].expected;
        actual = readl(NRF51_GPIO_BASE + reset_state[i].addr);
        g_assert_cmpuint(actual, ==, expected);
    }

    for (i = 0; i < NRF51_GPIO_PINS; i++) {
        expected = 0x00000002;
        actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START + i * 4);
        g_assert_cmpuint(actual, ==, expected);
    }

    /* Check dir bit consistency between dir and cnf */
    /* Check set via DIRSET */
    expected = 0x80000001;
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_DIRSET, expected);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_DIR);
    g_assert_cmpuint(actual, ==, expected);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START) & 0x01;
    g_assert_cmpuint(actual, ==, 0x01);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_END) & 0x01;
    g_assert_cmpuint(actual, ==, 0x01);

    /* Check clear via DIRCLR */
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_DIRCLR, 0x80000001);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_DIR);
    g_assert_cmpuint(actual, ==, 0x00000000);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START) & 0x01;
    g_assert_cmpuint(actual, ==, 0x00);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_END) & 0x01;
    g_assert_cmpuint(actual, ==, 0x00);

    /* Check set via DIR */
    expected = 0x80000001;
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_DIR, expected);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_DIR);
    g_assert_cmpuint(actual, ==, expected);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START) & 0x01;
    g_assert_cmpuint(actual, ==, 0x01);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_END) & 0x01;
    g_assert_cmpuint(actual, ==, 0x01);

    /* Reset DIR */
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_DIR, 0x00000000);

    /* Check Input propagates */
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START, 0x00);
    set_irq_in("/machine/nrf51", "unnamed-gpio-in", 0, 0);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_IN) & 0x01;
    g_assert_cmpuint(actual, ==, 0x00);
    set_irq_in("/machine/nrf51", "unnamed-gpio-in", 0, 1);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_IN) & 0x01;
    g_assert_cmpuint(actual, ==, 0x01);
    set_irq_in("/machine/nrf51", "unnamed-gpio-in", 0, -1);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_IN) & 0x01;
    g_assert_cmpuint(actual, ==, 0x01);
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START, 0x02);

    /* Check pull-up working */
    set_irq_in("/machine/nrf51", "unnamed-gpio-in", 0, 0);
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START, 0b0000);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_IN) & 0x01;
    g_assert_cmpuint(actual, ==, 0x00);
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START, 0b1110);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_IN) & 0x01;
    g_assert_cmpuint(actual, ==, 0x01);
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START, 0x02);

    /* Check pull-down working */
    set_irq_in("/machine/nrf51", "unnamed-gpio-in", 0, 1);
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START, 0b0000);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_IN) & 0x01;
    g_assert_cmpuint(actual, ==, 0x01);
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START, 0b0110);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_IN) & 0x01;
    g_assert_cmpuint(actual, ==, 0x00);
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START, 0x02);
    set_irq_in("/machine/nrf51", "unnamed-gpio-in", 0, -1);

    /* Check Output propagates */
    irq_intercept_out("/machine/nrf51");
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START, 0b0011);
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_OUTSET, 0x01);
    g_assert_true(get_irq(0));
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_OUTCLR, 0x01);
    g_assert_false(get_irq(0));

    /* Check self-stimulation */
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START, 0b01);
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_OUTSET, 0x01);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_IN) & 0x01;
    g_assert_cmpuint(actual, ==, 0x01);

    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_OUTCLR, 0x01);
    actual = readl(NRF51_GPIO_BASE + NRF51_GPIO_REG_IN) & 0x01;
    g_assert_cmpuint(actual, ==, 0x00);

    /* Check short-circuit - generates an guest_error which must be checked
       manually as long as qtest can not scan qemu_log messages */
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_CNF_START, 0b01);
    writel(NRF51_GPIO_BASE + NRF51_GPIO_REG_OUTSET, 0x01);
    set_irq_in("/machine/nrf51", "unnamed-gpio-in", 0, 0);
}

int main(int argc, char **argv)
{
    int ret, sock_fd;
    char serialtmpdir[] = "/tmp/qtest-microbit-serial-sXXXXXX";
    char serialtmp[40];
    struct sockaddr_un addr;

    g_assert(mkdtemp(serialtmpdir));
    sprintf(serialtmp, "%s/sock", serialtmpdir);

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    g_assert(sock_fd != -1);

    memset(&addr, 0, sizeof(struct sockaddr_un));

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, serialtmp, sizeof(addr.sun_path) - 1);

    g_test_init(&argc, &argv, NULL);

    global_qtest = qtest_initf("-machine microbit "
                               "-chardev socket,id=s0,path=%s,server,nowait "
                               "-no-shutdown -serial chardev:s0",
                               serialtmp);

    g_assert(connect(sock_fd, (const struct sockaddr *) &addr,
                     sizeof(struct sockaddr_un)) != -1);

    unlink(serialtmp);
    rmdir(serialtmpdir);

    qtest_add_data_func("/microbit/nrf51/uart", &sock_fd, test_nrf51_uart);
    qtest_add_func("/microbit/nrf51/nvmc", test_nrf51_nvmc);
    qtest_add_func("/microbit/nrf51/gpio", test_nrf51_gpio);

    ret = g_test_run();

    qtest_quit(global_qtest);

    close(sock_fd);

    return ret;
}
