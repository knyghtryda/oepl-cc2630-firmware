# CC2630 CPE Patch for IEEE 802.15.4 - Research Findings

**Date:** 2026-02-06
**Issue:** CC2630 CMD_RADIO_SETUP returns INTERNAL_ERROR for IEEE 802.15.4 mode
**Root Cause:** Missing CPE (Command Processing Engine) patch application before CMD_RADIO_SETUP

---

## Executive Summary

**YES, the CC2630 PG2.3 REQUIRES CPE patches for IEEE 802.15.4 operation.**

The CC26x0 RF core requires firmware patches to be loaded into CPERAM, RFERAM, and MCERAM after boot but **BEFORE** sending CMD_RADIO_SETUP. Without these patches, the RF core will return INTERNAL_ERROR or fail to initialize properly.

---

## Required Patches

Three patches are required for IEEE 802.15.4 operation on CC26x0 devices:

### 1. **CPE Patch** (`rf_patch_cpe_ieee.h`)
- **Purpose:** Command and Packet Engine firmware corrections
- **Target:** CPERAM (0x21000000 base address)
- **Size:** 139 words (32-bit)
- **Patch Vector Offset:** 0x0404
- **Entry Function:** `rf_patch_cpe_ieee()`

### 2. **RFE Patch** (`rf_patch_rfe_ieee.h`)
- **Purpose:** RF Engine firmware corrections (includes RSSI calculation fix)
- **Target:** RFERAM (0x2100C000 base address)
- **Size:** 304 words (32-bit)
- **Entry Function:** `rf_patch_rfe_ieee()`

### 3. **MCE Patch** (`rf_patch_mce_ieee_s.h`)
- **Purpose:** Modem and Coding Engine firmware (for single-ended output)
- **Target:** MCERAM (0x21008000 base address)
- **Size:** 256 words (32-bit)
- **Entry Function:** `rf_patch_mce_ieee_s()` (for single-ended mode)
- **Note:** There's also `rf_patch_mce_ieee.h` for differential mode

---

## Patch Application Sequence

According to TI's RF driver startup sequence:

1. **Power on RF core** (PRCM power domain)
2. **Enable RF core clocks** (all sub-modules: CPE, CPERAM, RFE, RFERAM, MDM, MDMRAM, etc.)
3. **Wait for RF core boot** (BOOT_DONE interrupt)
4. **⚠️ APPLY PATCHES HERE ⚠️** ← **CRITICAL: Must happen BEFORE CMD_RADIO_SETUP**
   - Apply CPE patch: `rf_patch_cpe_ieee()`
   - Apply RFE patch: `rf_patch_rfe_ieee()`
   - Apply MCE patch: `rf_patch_mce_ieee_s()` (or `rf_patch_mce_ieee()` for differential)
5. **Send CMD_RADIO_SETUP** (mode = 0x01 for IEEE 802.15.4)
6. **Configure radio operations** (CMD_FS, CMD_IEEE_RX, CMD_IEEE_TX, etc.)

---

## Patch Structure and Loading Mechanism

### CPE Patch Example (from `rf_patch_cpe_ieee.h`)

```c
// Patch data array (139 words)
CPE_PATCH_TYPE patchImageIeee[] = {
   0x2100050f,
   0x2100041d,
   0x21000539,
   // ... 136 more words
};

// Memory layout
#define _IEEE_CPERAM_START 0x21000000
#define _IEEE_PATCH_VEC_OFFSET 0x0404
#define _IEEE_PATCH_TAB_OFFSET 0x033C
#define _IEEE_IRQPATCH_OFFSET 0x03AC

// Loading functions
void enterIeeeCpePatch(void)
{
   uint32_t *pPatchVec = (uint32_t *)(_IEEE_CPERAM_START + _IEEE_PATCH_VEC_OFFSET);
   memcpy(pPatchVec, patchImageIeee, sizeof(patchImageIeee));
}

void configureIeeePatch(void)
{
   uint8_t *pPatchTab = (uint8_t *)(_IEEE_CPERAM_START + _IEEE_PATCH_TAB_OFFSET);
   uint32_t *pIrqPatch = (uint32_t *)(_IEEE_CPERAM_START + _IEEE_IRQPATCH_OFFSET);

   // Map patch indices to patch table positions
   pPatchTab[5] = 0;
   pPatchTab[52] = 1;
   pPatchTab[103] = 2;
   pPatchTab[60] = 3;
   pPatchTab[38] = 4;
   pPatchTab[43] = 5;

   // Configure IRQ patches
   pIrqPatch[1] = 0x2100044d;  // _IRQ_PATCH_0
   pIrqPatch[9] = 0x2100048d;  // _IRQ_PATCH_1
}

void rf_patch_cpe_ieee(void)
{
   enterIeeeSysPatch();    // Empty for CC26x0
   enterIeeeCpePatch();    // Copy patch to CPERAM
   configureIeeePatch();   // Configure patch table and IRQ handlers
}
```

---

## Source Files Downloaded

All three patch files have been downloaded from the official TI SimpleLink Core SDK (via Contiki-NG repository):

- **Location:** `/home/ratsark/Code/oepl-cc2630-firmware/firmware/rf_patches/`
- **Files:**
  - `rf_patch_cpe_ieee.h` (6.8 KB)
  - `rf_patch_rfe_ieee.h` (8.0 KB)
  - `rf_patch_mce_ieee_s.h` (7.2 KB)

**Source Repository:** [contiki-ng/coresdk_cc13xx_cc26xx](https://github.com/contiki-ng/coresdk_cc13xx_cc26xx/tree/master/source/ti/devices/cc26x0/rf_patches)

**Revision Information:**
- CPE IEEE: Revision 18438 (May 7, 2018)
- RFE IEEE: Revision 18842 (Jan 31, 2019) - includes RSSI calculation fix
- MCE IEEE-S: Revision 18842 (Jan 31, 2019) - for single-ended output

**License:** BSD 3-Clause (Texas Instruments Incorporated)

---

## Integration into Current Code

### Current Code Location of Issue

File: `/home/ratsark/Code/oepl-cc2630-firmware/firmware/oepl_rf_cc2630.c`

Lines 153-157 (current initialization sequence):
```c
// 4. Wait for RF core boot
rf_wait_boot();
rtt_puts("RF: Boot done\r\n");

// 5. Send CMD_RADIO_SETUP for IEEE 802.15.4
memset(&rf_cmd_setup, 0, sizeof(rf_cmd_setup));
```

### Required Modification

**Insert patch application between steps 4 and 5:**

```c
// 4. Wait for RF core boot
rf_wait_boot();
rtt_puts("RF: Boot done\r\n");

// 4a. Apply RF core firmware patches (REQUIRED for IEEE 802.15.4)
rtt_puts("RF: Applying CPE/RFE/MCE patches...\r\n");
rf_patch_cpe_ieee();   // Apply CPE patch
rf_patch_rfe_ieee();   // Apply RFE patch
rf_patch_mce_ieee_s(); // Apply MCE patch (single-ended mode)
rtt_puts("RF: Patches applied\r\n");

// 5. Send CMD_RADIO_SETUP for IEEE 802.15.4
memset(&rf_cmd_setup, 0, sizeof(rf_cmd_setup));
```

### Additional Changes Needed

1. **Include patch headers** in `oepl_rf_cc2630.c`:
   ```c
   #include "rf_patches/rf_patch_cpe_ieee.h"
   #include "rf_patches/rf_patch_rfe_ieee.h"
   #include "rf_patches/rf_patch_mce_ieee_s.h"
   ```

2. **Update Makefile** to include rf_patches directory in include path

3. **Note about MCE patch variant:**
   - Use `rf_patch_mce_ieee_s.h` for **single-ended** RF output
   - Use `rf_patch_mce_ieee.h` for **differential** RF output
   - Current code uses `frontEndMode = 0x00` (differential), but may need single-ended patch depending on hardware

---

## Why This Wasn't Obvious

1. **No explicit documentation:** TI's datasheet doesn't prominently state "patches required"
2. **ROM-based MAC:** CC2630 has IEEE 802.15.4 MAC in ROM, which might suggest no patches needed
3. **Hidden in SDK:** Patches are buried in device-specific directories of the SDK
4. **Works on newer chips:** CC2652/CC1352 (CC13x2/CC26x2 family) may have different requirements
5. **Different from BLE:** BLE patches (`rf_patch_cpe_ble.h`) exist separately and are more documented

---

## Verification Steps After Integration

1. **Compile and flash** with patches included
2. **Check RTT output** for "Patches applied" message
3. **Verify CMD_RADIO_SETUP** returns `DONE_OK` (0x1400) instead of `INTERNAL_ERROR`
4. **Test CMD_FS** (frequency synth setup)
5. **Test CMD_IEEE_TX** and CMD_IEEE_RX** operations

---

## Additional Resources

- [TI SimpleLink CC13xx/CC26xx SDK Documentation](https://software-dl.ti.com/simplelink/esd/simplelink_cc13xx_cc26xx_sdk/7.41.00.17/exports/docs/proprietary-rf/proprietary-rf-users-guide/proprietary-rf-guide/ieee-rf-index-cc13xx_cc26xx.html)
- [Contiki-OS CC26xx RF Core](https://github.com/contiki-os/contiki/tree/master/cpu/cc26xx-cc13xx/rf-core)
- [Contiki-NG CC26x0 Platform Documentation](https://docs.contiki-ng.org/en/master/doc/platforms/cc26x0-cc13x0.html)
- [TI E2E Support Forums](https://e2e.ti.com/support/wireless-connectivity/)
- [CC2630 Datasheet](https://www.ti.com/lit/ds/symlink/cc2630.pdf)

---

## Conclusion

The CC2630 **absolutely requires** CPE/RFE/MCE firmware patches for IEEE 802.15.4 operation. These patches correct silicon bugs and provide necessary functionality that is not present in the base ROM firmware. The patches must be applied after RF core boot but before sending CMD_RADIO_SETUP.

**Next Steps:**
1. Integrate the three patch files into the build system
2. Add patch application calls to `oepl_rf_init()` between boot and CMD_RADIO_SETUP
3. Test and verify that CMD_RADIO_SETUP succeeds
4. Proceed with IEEE 802.15.4 TX/RX testing

---

**Research completed:** 2026-02-06
**Files ready for integration:** ✅
**Patch application point identified:** ✅
**Next action:** Code integration and testing
