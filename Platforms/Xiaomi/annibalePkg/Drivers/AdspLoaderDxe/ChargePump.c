/**
  Copyright (c) 2026, wljf817. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

  SC8581 Charge Pump, its OVP Gate connects VBUS to the USB Port.
  Android (sc8581_sy6541.ko) initializes it at boot and puts it into
  Reverse 1:2 Mode when the ADSP configures OTG, the same is done here.
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "ChargePump.h"

//
// Thresholds of the Device Tree Node sc8585_master@6e of the K90 (mV / mA)
//
#define SC8581_BAT_OVP_MV        4750
#define SC8581_BAT_OVP_ALARM_MV  4650
#define SC8581_WPC_OVP_MV        21000
#define SC8581_OUT_OVP_MV        5000
#define SC8581_PMID2OUT_UVP_MV   100
#define SC8581_PMID2OUT_OVP_MV   600
#define SC8581_FSW_KHZ           600

// Per Work Mode (Div 4, Div 2, Div 1)
STATIC CONST UINT32 mBusOvpMv[3]     = { 21000, 11000, 6000 };
STATIC CONST UINT32 mUsbOvpMv[3]     = { 21000, 14000, 6500 };
STATIC CONST UINT32 mBusOcpMa[3]     = { 5100, 5850, 5000 };

// Range of the Bus OVP Register per Work Mode (sc8581_set_busovp_th)
STATIC CONST UINT32 mBusOvpMinMv[3]  = { 15000, 7500, 3750 };
STATIC CONST UINT32 mBusOvpMaxMv[3]  = { 22000, 13300, 5500 };
STATIC CONST UINT32 mBusOvpStepMv[3] = { 800, 400, 200 };

// Operation Mode -> Work Mode and Mode Register Value (sc8581_set_operation_mode)
STATIC CONST UINT8  mWorkMode[8]     = { 0, 1, 2, 2, 0, 1, 2, 2 };
STATIC CONST UINT8  mModeReg[8]      = { 0, 1, 2, 2, 4, 5, 6, 6 };

//
// Global Variables
//
STATIC EFI_GUID               mI2cProtocolGuid  = EFI_QCOM_I2C_PROTOCOL_GUID;
STATIC EFI_QCOM_I2C_PROTOCOL *mI2cProtocol      = NULL;
STATIC VOID                  *mI2cHandle        = NULL;
STATIC I2C_SLAVE_CONFIG       mI2cConfig;
STATIC BOOLEAN                mOvpGateDelayDone = FALSE;

STATIC
EFI_STATUS
ChargePumpRead (
  IN  UINT8  Register,
  OUT UINT8 *Value)
{
  UINT32 Status;
  UINT32 Read = 0;

  if (mI2cHandle == NULL) {
    return EFI_NOT_READY;
  }

  Status = mI2cProtocol->Read (mI2cHandle, &mI2cConfig, Register, 1, Value, 1, &Read, SC8581_I2C_TIMEOUT);
  if (Status != 0) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Read Register 0x%02x! Status = %u\n", __FUNCTION__, Register, Status));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ChargePumpWrite (
  IN UINT8 Register,
  IN UINT8 Value)
{
  UINT32 Status;
  UINT32 Written = 0;

  if (mI2cHandle == NULL) {
    return EFI_NOT_READY;
  }

  Status = mI2cProtocol->Write (mI2cHandle, &mI2cConfig, Register, 1, &Value, 1, &Written, SC8581_I2C_TIMEOUT);
  if (Status != 0) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Write Register 0x%02x! Status = %u\n", __FUNCTION__, Register, Status));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((EFI_D_INFO, "%a: Register 0x%02x <- 0x%02x\n", __FUNCTION__, Register, Value));

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ChargePumpUpdate (
  IN UINT8 Register,
  IN UINT8 ClearMask,
  IN UINT8 SetMask)
{
  EFI_STATUS Status;
  UINT8      Value;

  Status = ChargePumpRead (Register, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return ChargePumpWrite (Register, (UINT8)((Value & ~ClearMask) | SetMask));
}

STATIC
EFI_STATUS
ChargePumpSetKey (
  IN BOOLEAN Enter)
{
  STATIC CONST UINT8 Sequence[6] = { 0x44, 0x41, 0x47, 0x41, 0x4D, 0x41 };
  EFI_STATUS         Status;
  UINT8              Value;

  // Same Sequence as sc8581_set_key of Android, Bit 0 of the Key Register shows the Key Mode
  for (UINTN i = 0; i < 10; i++) {
    Status = ChargePumpRead (SC8581_REG_KEY, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (((Value & BIT0) != 0) == Enter) {
      return EFI_SUCCESS;
    }

    for (UINTN j = 0; j < ARRAY_SIZE (Sequence); j++) {
      ChargePumpWrite (SC8581_REG_KEY, Sequence[j]);
    }
  }

  DEBUG ((EFI_D_ERROR, "%a: Key Mode not %a!\n", __FUNCTION__, Enter ? "entered" : "exited"));

  return EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
ChargePumpSetOperationMode (
  IN UINT8 Mode)
{
  UINT8  WorkMode;
  UINT32 BusOvp;
  INT32  UsbOvp;

  if (Mode >= ARRAY_SIZE (mWorkMode)) {
    return EFI_INVALID_PARAMETER;
  }

  WorkMode = mWorkMode[Mode];

  // Same as sc8581_set_operation_mode of Android for the SC8581 with the Thresholds of the K90, first enable the Protections
  ChargePumpUpdate (SC8581_REG_BAT_OVP, BIT7, 0);
  ChargePumpUpdate (SC8581_REG_BUS_OCP, BIT7, 0);
  ChargePumpUpdate (SC8581_REG_BUS_UCP, BIT7 | BIT3 | BIT1, 0);
  ChargePumpUpdate (SC8581_REG_PMID2OUT_OVP, BIT7, 0);
  ChargePumpUpdate (SC8581_REG_PMID2OUT_UVP, BIT7, 0);
  ChargePumpUpdate (SC8581_REG_BAT_OVP_ALARM, BIT7, 0);
  ChargePumpUpdate (SC8581_REG_ALARM_6D, BIT7, 0);

  // Battery OVP (4450 mV Base, 25 mV Steps)
  ChargePumpUpdate (SC8581_REG_BAT_OVP, 0x1F, (SC8581_BAT_OVP_MV - 4450) / 25);
  ChargePumpUpdate (SC8581_REG_BAT_OVP_ALARM, 0x1F, (SC8581_BAT_OVP_ALARM_MV - 4450) / 25);

  // Bus OVP of the Work Mode
  BusOvp = MIN (MAX (mBusOvpMv[WorkMode], mBusOvpMinMv[WorkMode]), mBusOvpMaxMv[WorkMode]);
  ChargePumpUpdate (SC8581_REG_BUS_OVP, 0x3C, (UINT8)((((BusOvp - mBusOvpMinMv[WorkMode]) / mBusOvpStepMv[WorkMode]) << 2) & 0x3C));

  // USB OVP (11 V Base, 1 V Steps, 7.5 V is the highest Register Value)
  UsbOvp = (mUsbOvpMv[WorkMode] == 7500) ? 0xF : ((INT32)mUsbOvpMv[WorkMode] - 11000) / 1000;
  ChargePumpUpdate (SC8581_REG_USB_OVP, 0x0F, (UINT8)(UsbOvp & 0x0F));

  // Bus OCP (2100 mA Base, 150 mA Steps)
  ChargePumpUpdate (SC8581_REG_BUS_OCP, 0x1F, (UINT8)((MAX (mBusOcpMa[WorkMode], 2100) - 2100) / 150));

  // Wireless OVP (11 V Base, 1 V Steps)
  ChargePumpUpdate (SC8581_REG_WPC_OVP, 0x0F, (SC8581_WPC_OVP_MV - 11000) / 1000);

  // Output OVP (4800 mV Base, 200 mV Steps)
  ChargePumpUpdate (SC8581_REG_BUS_OVP, 0x03, (SC8581_OUT_OVP_MV - 4800) / 200);

  // PMID2OUT UVP, the SC8581 only knows two Values, and VOUT2OUT UVP high (Key Mode) because Reverse Quick Charge is supported
  ChargePumpUpdate (SC8581_REG_PMID2OUT_UVP, 0x07, (SC8581_PMID2OUT_UVP_MV < 8) ? 0 : 5);
  ChargePumpSetKey (TRUE);
  ChargePumpWrite (SC8581_REG_VOUT2OUT_UVP, 0x03);
  ChargePumpSetKey (FALSE);

  // PMID2OUT OVP (200 mV Base, 100 mV Steps)
  ChargePumpUpdate (SC8581_REG_PMID2OUT_OVP, 0x07, (SC8581_PMID2OUT_OVP_MV - 200) / 100);

  // Finally the Mode
  return ChargePumpUpdate (SC8581_REG_MODE, 0x07, mModeReg[Mode]);
}

STATIC
EFI_STATUS
ChargePumpInitDevice ()
{
  EFI_STATUS Status;

  // Same as sc8581_init_device of Android for the SC8581
  ChargePumpUpdate (SC8581_REG_MODE, 0, BIT3);
  ChargePumpUpdate (SC8581_REG_MODE, BIT7, 0);

  // Gates under Software Control and on
  ChargePumpUpdate (SC8581_REG_CONTROL, 0, SC8581_ACDRV_MANUAL | SC8581_WPCGATE_EN | SC8581_OVPGATE_EN);

  ChargePumpUpdate (SC8581_REG_CONTROL2, BIT5 | BIT4 | BIT3, BIT5 | BIT3);
  ChargePumpUpdate (SC8581_REG_BUS_UCP, BIT5 | BIT4, BIT4);
  ChargePumpUpdate (SC8581_REG_FSW, 0, BIT1 | BIT0);
  ChargePumpUpdate (SC8581_REG_USB_OVP, BIT7, 0);

  // Interrupt Masks
  ChargePumpUpdate (SC8581_REG_INT_MASK1, BIT7 | BIT6 | BIT0, 0);
  ChargePumpUpdate (SC8581_REG_INT_MASK2, BIT7 | BIT6 | BIT5 | BIT4 | BIT3 | BIT0, 0);

  Status = ChargePumpSetOperationMode (SC8581_MODE_FORWARD_2_1);

  ChargePumpWrite (SC8581_REG_42, 0x82);

  // Switching Frequency (180 kHz Base, 60 kHz Steps)
  ChargePumpUpdate (SC8581_REG_FSW, 0x78, (UINT8)((((SC8581_FSW_KHZ - 180) / 60) << 3) & 0x78));

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to set the Operation Mode! Status = %r\n", __FUNCTION__, Status));
  }

  return Status;
}

STATIC
EFI_STATUS
ChargePumpSetOvpGate (
  IN BOOLEAN Enable)
{
  EFI_STATUS Status;
  UINT8      Value;

  // Same as sc8581_enable_ovpgate of Android: Set the Bit, check it and retry
  if (!mOvpGateDelayDone) {
    gBS->Stall (30000);
    mOvpGateDelayDone = TRUE;
  }

  for (UINTN i = 0; i < 10; i++) {
    Status = ChargePumpUpdate (SC8581_REG_CONTROL, SC8581_OVPGATE_EN, Enable ? SC8581_OVPGATE_EN : 0);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = ChargePumpRead (SC8581_REG_CONTROL, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (((Value & SC8581_OVPGATE_EN) != 0) == Enable) {
      return EFI_SUCCESS;
    }

    gBS->Stall (10000);
  }

  DEBUG ((EFI_D_ERROR, "%a: OVP Gate does not turn %a!\n", __FUNCTION__, Enable ? "on" : "off"));

  return EFI_DEVICE_ERROR;
}

EFI_STATUS
ChargePumpEnableOvpGate ()
{
  if (mI2cHandle == NULL) {
    return EFI_NOT_READY;
  }

  return ChargePumpSetOvpGate (TRUE);
}

EFI_STATUS
ChargePumpSetReverseCharge (
  IN BOOLEAN Enable)
{
  EFI_STATUS Status;
  UINT8      Mode;
  UINT8      Control;

  if (mI2cHandle == NULL) {
    return EFI_NOT_READY;
  }

  // Same as ops_cp_set_revchg of Android: Reverse 1:2 Mode with the OVP Gate on, the Charge Pump itself stays off
  for (UINTN i = 0; i < 10; i++) {
    if (Enable) {
      ChargePumpSetOperationMode (SC8581_MODE_REVERSE_1_2);
      ChargePumpSetOvpGate (TRUE);
    } else {
      ChargePumpUpdate (SC8581_REG_CONTROL, SC8581_CHG_EN, 0);
      ChargePumpUpdate (SC8581_REG_CONTROL, SC8581_QB_EN, 0);
      ChargePumpSetOperationMode (SC8581_MODE_FORWARD_2_1);
    }

    Status = ChargePumpRead (SC8581_REG_MODE, &Mode);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = ChargePumpRead (SC8581_REG_CONTROL, &Control);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Enable && (Mode & 0x07) == SC8581_MODE_REVERSE_1_2 && (Control & SC8581_OVPGATE_EN) != 0) ||
        (!Enable && (Mode & 0x07) == SC8581_MODE_FORWARD_2_1 && (Control & SC8581_CHG_EN) == 0)) {
      DEBUG ((EFI_D_WARN, "%a: Reverse Charge %a\n", __FUNCTION__, Enable ? "on" : "off"));
      return EFI_SUCCESS;
    }

    gBS->Stall (20000);
  }

  DEBUG ((EFI_D_ERROR, "%a: Failed to %a Reverse Charge!\n", __FUNCTION__, Enable ? "enable" : "disable"));

  return EFI_DEVICE_ERROR;
}

EFI_STATUS
ChargePumpInit ()
{
  EFI_STATUS Status;
  UINT16     I2cStatus;
  UINT8      DeviceId;

  // Locate QCOM I2C Protocol
  Status = gBS->LocateProtocol (&mI2cProtocolGuid, NULL, (VOID *)&mI2cProtocol);
  if (EFI_ERROR (Status) || mI2cProtocol->Revision != EFI_QCOM_I2C_PROTOCOL_REVISION) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Locate I2C Protocol! Status = %r\n", __FUNCTION__, Status));
    return EFI_NOT_FOUND;
  }

  // Leave Low Power Mode like the Android Driver (GPIO Function, no Pull, Output High)
  MmioOr32 (TLMM_GPIO_IN_OUT (SC8581_NLPM_GPIO), TLMM_GPIO_OUT);
  MmioAndThenOr32 (TLMM_GPIO_CFG (SC8581_NLPM_GPIO), ~(TLMM_GPIO_CFG_PULL_MASK | TLMM_GPIO_CFG_FUNC_MASK), TLMM_GPIO_CFG_OE);

  gBS->Stall (400000);

  // Open I2C Bus
  I2cStatus = mI2cProtocol->Open (SC8581_I2C_INSTANCE, &mI2cHandle);
  if (I2cStatus != 0) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Open I2C Instance %u! Status = %u\n", __FUNCTION__, SC8581_I2C_INSTANCE, I2cStatus));
    mI2cHandle = NULL;
    return EFI_DEVICE_ERROR;
  }

  ZeroMem (&mI2cConfig, sizeof (mI2cConfig));

  mI2cConfig.BusFrequencyKhz        = SC8581_I2C_FREQUENCY;
  mI2cConfig.SlaveAddress           = SC8581_I2C_ADDRESS;
  mI2cConfig.SlaveMaxClockStretchUs = 500;

  // Check Device
  Status = ChargePumpRead (SC8581_REG_DEVICE_ID, &DeviceId);
  if (EFI_ERROR (Status)) {
    mI2cHandle = NULL;
    return Status;
  }

  if (DeviceId != SC8581_DEVICE_ID) {
    DEBUG ((EFI_D_ERROR, "%a: Device ID 0x%02x is not a SC8581, the Register Layout is unknown!\n", __FUNCTION__, DeviceId));
    mI2cHandle = NULL;
    return EFI_UNSUPPORTED;
  }

  // Bring the Charge Pump into the same State as Android
  ChargePumpInitDevice ();

  return EFI_SUCCESS;
}
