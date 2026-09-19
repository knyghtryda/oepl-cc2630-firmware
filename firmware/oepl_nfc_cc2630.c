// -----------------------------------------------------------------------------
//  NFC tag chip access — see oepl_nfc_cc2630.h for the wiring and why it is
//  worth having.
// -----------------------------------------------------------------------------

#include "oepl_nfc_cc2630.h"
#include "oepl_hw_abstraction_cc2630.h"
#include "rtt.h"

#include "i2c.h"
#include "ioc.h"
#include "gpio.h"
#include "prcm.h"
#include "sys_ctrl.h"
#include "hw_memmap.h"

#define NFC_SDA_DIO  24
#define NFC_SCL_DIO  25
#define NFC_PWR_DIO  5       // shared peripheral rail (panel + NFC)

static bool nfc_found;

static bool nfc_wait(void)
{
    uint32_t guard = 0;
    while (I2CMasterBusy(I2C0_BASE) && ++guard < 2000000) { }
    return (guard < 2000000) && (I2CMasterErr(I2C0_BASE) == I2C_MASTER_ERR_NONE);
}

bool oepl_nfc_read_block(uint8_t blk, uint8_t *out)
{
    I2CMasterSlaveAddrSet(I2C0_BASE, NFC_I2C_ADDR, false);
    I2CMasterDataPut(I2C0_BASE, blk);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_SINGLE_SEND);
    if (!nfc_wait()) return false;

    I2CMasterSlaveAddrSet(I2C0_BASE, NFC_I2C_ADDR, true);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_RECEIVE_START);
    for (uint8_t i = 0; i < NFC_BLOCK_SIZE; i++) {
        if (!nfc_wait()) return false;
        out[i] = (uint8_t)I2CMasterDataGet(I2C0_BASE);
        I2CMasterControl(I2C0_BASE, (i == NFC_BLOCK_SIZE - 2)
                                        ? I2C_MASTER_CMD_BURST_RECEIVE_FINISH
                                        : I2C_MASTER_CMD_BURST_RECEIVE_CONT);
    }
    return true;
}

static bool nfc_write_block(uint8_t blk, const uint8_t *data)
{
    I2CMasterSlaveAddrSet(I2C0_BASE, NFC_I2C_ADDR, false);
    I2CMasterDataPut(I2C0_BASE, blk);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_START);
    if (!nfc_wait()) return false;
    for (uint8_t i = 0; i < NFC_BLOCK_SIZE; i++) {
        I2CMasterDataPut(I2C0_BASE, data[i]);
        I2CMasterControl(I2C0_BASE, (i == NFC_BLOCK_SIZE - 1)
                                        ? I2C_MASTER_CMD_BURST_SEND_FINISH
                                        : I2C_MASTER_CMD_BURST_SEND_CONT);
        if (!nfc_wait()) return false;
    }
    oepl_hw_delay_ms(10);        // the chip needs ~5 ms to commit a block
    return true;
}

// The NFC chip shares the panel's supply rail, so it is powered only while
// we are talking to it. A phone can still read the chip with the rail down --
// it harvests from the reader's field, which is why a flat or bricked tag
// still answers a tap.
static void nfc_power(bool on)
{
    IOCPinTypeGpioOutput(NFC_PWR_DIO);
    GPIO_setOutputEnableDio(NFC_PWR_DIO, GPIO_OUTPUT_ENABLE);
    if (on) {
        GPIO_setDio(NFC_PWR_DIO);
        oepl_hw_delay_ms(10);
        IOCPinTypeI2c(I2C0_BASE, NFC_SDA_DIO, NFC_SCL_DIO);
        I2CMasterInitExpClk(I2C0_BASE, SysCtrlClockGet(), false);   // 100 kHz
    } else {
        GPIO_clearDio(NFC_PWR_DIO);
    }
}

bool oepl_nfc_init(void)
{
    PRCMPowerDomainOn(PRCM_DOMAIN_SERIAL);
    for (uint32_t i = 0; i < 500000; i++)
        if (PRCMPowerDomainStatus(PRCM_DOMAIN_SERIAL) == PRCM_DOMAIN_POWER_ON) break;
    PRCMPeripheralRunEnable(PRCM_PERIPH_I2C0);
    PRCMLoadSet();
    for (uint32_t i = 0; i < 500000; i++)
        if (PRCMLoadGet()) break;

    nfc_power(true);

    uint8_t hdr[NFC_BLOCK_SIZE];
    nfc_found = oepl_nfc_read_block(0, hdr);
    if (nfc_found) {
        rtt_puts("NFC: UID ");
        for (uint8_t i = 0; i < 7; i++) rtt_put_hex8(hdr[i]);
        rtt_puts(" CC ");
        for (uint8_t i = 12; i < 16; i++) rtt_put_hex8(hdr[i]);
        rtt_puts("\r\n");
    } else {
        rtt_puts("NFC: no chip (not populated on this board)\r\n");
    }
    nfc_power(false);
    return nfc_found;
}

bool oepl_nfc_present(void) { return nfc_found; }

bool oepl_nfc_write_user(const uint8_t *data, uint16_t len)
{
    if (!nfc_found || len == 0) return false;

    nfc_power(true);
    uint8_t blk[NFC_BLOCK_SIZE];
    uint16_t off = 0;
    uint8_t index = NFC_FIRST_USER_BLK;
    while (off < len) {
        uint8_t n = (uint8_t)((len - off > NFC_BLOCK_SIZE) ? NFC_BLOCK_SIZE : (len - off));
        for (uint8_t i = 0; i < NFC_BLOCK_SIZE; i++)
            blk[i] = (i < n) ? data[off + i] : 0x00;
        if (!nfc_write_block(index, blk)) {
            rtt_puts("NFC: write failed at block ");
            rtt_put_hex8(index);
            rtt_puts("\r\n");
            nfc_power(false);
            return false;
        }
        off += n;
        index++;
    }
    nfc_power(false);
    return true;
}

uint16_t oepl_nfc_make_text(uint8_t *out, uint16_t out_size, const char *text,
                            uint8_t text_len)
{
    // 03 <tlv len> D1 01 <payload len> 'T' 02 'e' 'n' <text> FE
    uint16_t need = (uint16_t)(text_len + 11);
    if (need > out_size) return 0;

    uint8_t payload_len = (uint8_t)(3 + text_len);      // status byte + "en"
    uint16_t n = 0;
    out[n++] = 0x03;
    out[n++] = (uint8_t)(payload_len + 4);
    out[n++] = 0xD1;                 // MB | ME | SR, TNF = NFC Forum well-known
    out[n++] = 0x01;                 // type length
    out[n++] = payload_len;
    out[n++] = 'T';
    out[n++] = 0x02;                 // UTF-8, 2-byte language code
    out[n++] = 'e';
    out[n++] = 'n';
    for (uint8_t i = 0; i < text_len; i++) out[n++] = (uint8_t)text[i];
    out[n++] = 0xFE;                 // terminator TLV
    return n;
}
