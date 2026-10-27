#ifndef _CHARGE_PUMP_H_
#define _CHARGE_PUMP_H_

//
// QCOM I2C Protocol (Interface of I2CDxe)
//
#define EFI_QCOM_I2C_PROTOCOL_GUID      { 0xB27AE8B1, 0x3E10, 0x4D07, { 0xAB, 0x5C, 0xEB, 0x9A, 0x6D, 0xC6, 0xFA, 0x8F } }
#define EFI_QCOM_I2C_PROTOCOL_REVISION  0x10000

typedef struct {
  UINT32 BusFrequencyKhz;
  UINT32 SlaveAddress;
  UINT32 Mode;
  UINT32 SlaveMaxClockStretchUs;
  UINT32 CoreConfiguration1;
  UINT32 CoreConfiguration2;
} I2C_SLAVE_CONFIG;

typedef
UINT16
(EFIAPI *EFI_QCOM_I2C_OPEN)(
  IN  UINT32   Instance,
  OUT VOID   **Handle
  );

typedef
UINT32
(EFIAPI *EFI_QCOM_I2C_READ)(
  IN  VOID             *Handle,
  IN  I2C_SLAVE_CONFIG *Config,
  IN  UINT16            Offset,
  IN  UINT16            OffsetLength,
  OUT UINT8            *Buffer,
  IN  UINT16            BufferLength,
  OUT UINT32           *Read,
  IN  UINT32            TimeoutMs
  );

typedef
UINT32
(EFIAPI *EFI_QCOM_I2C_WRITE)(
  IN  VOID             *Handle,
  IN  I2C_SLAVE_CONFIG *Config,
  IN  UINT16            Offset,
  IN  UINT16            OffsetLength,
  IN  UINT8            *Buffer,
  IN  UINT16            BufferLength,
  OUT UINT32           *Written,
  IN  UINT32            TimeoutMs
  );

typedef struct {
  UINT64             Revision;
  EFI_QCOM_I2C_OPEN  Open;
  EFI_QCOM_I2C_READ  Read;
  EFI_QCOM_I2C_WRITE Write;
  VOID              *Transfer;
  VOID              *Close;
} EFI_QCOM_I2C_PROTOCOL;

//
// SC8581 Charge Pump (I2C Instance = 1 + 10 SEs of QUP 0 + SE 5 of QUP 1)
//
#define SC8581_I2C_INSTANCE      16
#define SC8581_I2C_ADDRESS       0x6E
#define SC8581_I2C_FREQUENCY     100
#define SC8581_I2C_TIMEOUT       100

#define SC8581_DEVICE_ID         0x85

// Registers (Names follow sc8581_sy6541.ko of Android, Bit 7 of the Threshold Registers disables the Protection)
#define SC8581_REG_BAT_OVP       0x01
#define SC8581_REG_USB_OVP       0x03
#define SC8581_REG_WPC_OVP       0x04
#define SC8581_REG_BUS_OVP       0x05
#define SC8581_REG_BUS_OCP       0x06
#define SC8581_REG_BUS_UCP       0x07
#define SC8581_REG_PMID2OUT_OVP  0x08
#define SC8581_REG_PMID2OUT_UVP  0x09
#define SC8581_REG_CONTROL       0x0B
#define SC8581_REG_FSW           0x0C
#define SC8581_REG_CONTROL2      0x0D
#define SC8581_REG_MODE          0x0E
#define SC8581_REG_INT_MASK1     0x15
#define SC8581_REG_INT_MASK2     0x16
#define SC8581_REG_42            0x42
#define SC8581_REG_BAT_OVP_ALARM 0x6C
#define SC8581_REG_ALARM_6D      0x6D
#define SC8581_REG_DEVICE_ID     0x6E
#define SC8581_REG_KEY           0x76
#define SC8581_REG_VOUT2OUT_UVP  0x84

#define SC8581_REG_OVPGATE       SC8581_REG_CONTROL

// Control Register
#define SC8581_CHG_EN            BIT7
#define SC8581_QB_EN             BIT6
#define SC8581_ACDRV_MANUAL      BIT5
#define SC8581_WPCGATE_EN        BIT4
#define SC8581_OVPGATE_EN        BIT3

// Operation Modes of Android (CP_MODE_*), the Mode Register holds 0-2 and 4-6
#define SC8581_MODE_FORWARD_2_1  1
#define SC8581_MODE_REVERSE_1_2  5

// nLPM Pin, low puts the Charge Pump into Low Power Mode (it then only works with VBUS present)
#define SC8581_NLPM_GPIO         110

//
// TLMM GPIO Registers (TLMM_REG of the XBL DT)
//
#define TLMM_GPIO_BASE_ADDR      0x0F100000
#define TLMM_GPIO_CFG(n)         (TLMM_GPIO_BASE_ADDR + (n) * 0x1000)
#define TLMM_GPIO_IN_OUT(n)      (TLMM_GPIO_BASE_ADDR + (n) * 0x1000 + 0x4)
#define TLMM_GPIO_CFG_PULL_MASK  0x03
#define TLMM_GPIO_CFG_FUNC_MASK  0x3C
#define TLMM_GPIO_CFG_OE         BIT9
#define TLMM_GPIO_OUT            BIT1

//
// Functions
//
EFI_STATUS
ChargePumpInit ();

EFI_STATUS
ChargePumpEnableOvpGate ();

EFI_STATUS
ChargePumpSetReverseCharge (
  IN BOOLEAN Enable
  );

#endif /* _CHARGE_PUMP_H_ */
