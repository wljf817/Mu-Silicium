#ifndef _PMIC_GLINK_H_
#define _PMIC_GLINK_H_

//
// SMEM Protocol (Interface of SmemDxe, only the Ex Functions are used)
//
#define EFI_SMEM_PROTOCOL_GUID      { 0xF4E5C7D0, 0xD239, 0x47CB, { 0xAA, 0xCD, 0x7F, 0x66, 0xEF, 0x76, 0x32, 0x38 } }
#define EFI_SMEM_PROTOCOL_REVISION  0x10001

// Same Flags as GLinkDxe uses for its SMEM Items
#define SMEM_ALLOC_FLAGS_GLINK  0x40000000

typedef struct {
  UINT16  RemoteHost;
  UINT16  Reserved;
  UINT32  SmemType;
  UINT32  Size;
  UINT32  Reserved2;
  VOID   *Buffer;
  UINT32  Flags;
  UINT32  Reserved3;
} SMEM_ALLOC_PARAMS;

typedef
EFI_STATUS
(EFIAPI *EFI_SMEM_ALLOC_EX)(
  IN OUT SMEM_ALLOC_PARAMS *Params
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SMEM_GET_ADDR_EX)(
  IN OUT SMEM_ALLOC_PARAMS *Params
  );

typedef struct {
  UINT64               Revision;
  VOID                *SmemAlloc;
  VOID                *SmemGetAddr;
  EFI_SMEM_ALLOC_EX    SmemAllocEx;
  EFI_SMEM_GET_ADDR_EX SmemGetAddrEx;
} EFI_SMEM_PROTOCOL;

//
// IPCC (used to kick the ADSP, Interrupts are not used)
//
#define IPCC_BASE_ADDR     0x00406000
#define IPCC_REG_SEND_ID   0x0C
#define IPCC_CLIENT_LPASS  3
#define IPCC_SIGNAL_GLINK  0

//
// GLink SMEM Transport to the ADSP
//
#define GLINK_REMOTE_HOST_ADSP             2
#define SMEM_GLINK_NATIVE_XPRT_DESCRIPTOR  478
#define SMEM_GLINK_NATIVE_XPRT_FIFO_0      479
#define SMEM_GLINK_NATIVE_XPRT_FIFO_1      480
#define GLINK_DESCRIPTOR_SIZE              32
#define GLINK_FIFO_SIZE                    SIZE_16KB
#define GLINK_FIFO_RESERVE                 16

typedef struct {
  volatile UINT32 *Tail;
  volatile UINT32 *Head;
  volatile UINT8  *Fifo;
  UINT32           Length;
} GLINK_PIPE;

//
// GLink Native Protocol
//
#define GLINK_VERSION_1               1
#define GLINK_FEATURE_INTENT_REUSE    BIT0

#define GLINK_CMD_VERSION             0
#define GLINK_CMD_VERSION_ACK         1
#define GLINK_CMD_OPEN                2
#define GLINK_CMD_CLOSE               3
#define GLINK_CMD_OPEN_ACK            4
#define GLINK_CMD_INTENT              5
#define GLINK_CMD_RX_DONE             6
#define GLINK_CMD_RX_INTENT_REQ       7
#define GLINK_CMD_RX_INTENT_REQ_ACK   8
#define GLINK_CMD_TX_DATA             9
#define GLINK_CMD_CLOSE_ACK           11
#define GLINK_CMD_TX_DATA_CONT        12
#define GLINK_CMD_READ_NOTIF          13
#define GLINK_CMD_RX_DONE_W_REUSE     14
#define GLINK_CMD_SIGNALS             15

#define GLINK_NAME_SIZE               32
#define GLINK_LOCAL_CHANNEL_ID        1
#define GLINK_INTENT_SIZE             SIZE_1KB
#define GLINK_LOCAL_INTENTS           4
#define GLINK_MAX_REMOTE_INTENTS      8

#pragma pack(1)
typedef struct {
  UINT16 Cmd;
  UINT16 Param1;
  UINT32 Param2;
} GLINK_MSG;

typedef struct {
  GLINK_MSG Msg;
  UINT32    ChunkSize;
  UINT32    LeftSize;
} GLINK_TX_DATA_HDR;

typedef struct {
  UINT32 Size;
  UINT32 Id;
} GLINK_INTENT_PAIR;

typedef struct {
  GLINK_MSG         Msg;
  GLINK_INTENT_PAIR Intent;
} GLINK_INTENT_MSG;
#pragma pack()

typedef struct {
  UINT32  Id;
  UINT32  Size;
  BOOLEAN Valid;
  BOOLEAN InUse;
} GLINK_REMOTE_INTENT;

typedef struct {
  BOOLEAN             VersionReceived;
  BOOLEAN             VersionAcked;
  BOOLEAN             RemoteOpened;
  BOOLEAN             OpenAcked;
  UINT16              RemoteChannelId;
  UINT32              NextIntentId;
  GLINK_REMOTE_INTENT RemoteIntents[GLINK_MAX_REMOTE_INTENTS];
  UINT32              RxLength;
  UINT32              SeqNum;
  UINT32              PendingPropId;
  BOOLEAN             ResponseReceived;
  UINT32              ResponseResult;
} GLINK_STATE;

typedef
BOOLEAN
(*GLINK_CONDITION)(
  VOID
  );

//
// PMIC GLink Channel (Charger Firmware in the ADSP)
//
#define PMIC_GLINK_CHANNEL          "PMIC_RTR_ADSP_APPS"
#define PMIC_GLINK_MSG_TYPE_REQ_RESP  1
#define PMIC_GLINK_MSG_TYPE_NOTIFY    2

#pragma pack(1)
typedef struct {
  UINT32 Owner;
  UINT32 Type;
  UINT32 Opcode;
} PMIC_GLINK_HDR;
#pragma pack()

//
// Xiaomi Properties (same Messages as mca_adsp_glink.ko sends)
//
#define XM_GLINK_OWNER             0x800A
#define XM_GLINK_OPCODE_WRITE_PROP 2
#define XM_GLINK_OPCODE_NOTIFY     0x12
#define XM_PROP_DATA_SIZE          256

#define XM_PROP_OTG_CFG            0x201A
#define XM_PROP_CID_CFG            0x201B
#define XM_PROP_CP_STATE           0x201C
#define XM_PROP_EU_MODEL           0x2028

// Notifications of the ADSP (Data[0]), Android enables the OVP Gate of the Charge Pump on 5
// and puts it into Reverse Mode on 10 (platform_class_cp_set_revchg)
#define XM_NOTIFY_BOOST            3
#define XM_NOTIFY_OVPGATE          5
#define XM_NOTIFY_CP_REVERT        9
#define XM_NOTIFY_OTG_CP_CONFIG    10

#pragma pack(1)
typedef struct {
  PMIC_GLINK_HDR Hdr;
  UINT32         PropId;
  UINT32         SeqNum;
  UINT8          Data[XM_PROP_DATA_SIZE];
} XM_PROP_WRITE_REQ;

typedef struct {
  PMIC_GLINK_HDR Hdr;
  UINT32         PropId;
  UINT32         RetCode;
  UINT32         SeqNum;
} XM_PROP_WRITE_RESP;

typedef struct {
  INT32 OtgCfgSrc;
  INT32 GpioChipType;
  INT32 VddBoostGpio;
  INT32 OvpEnGpio;
} XM_OTG_CFG;
#pragma pack()

//
// Functions
//
EFI_STATUS
PmicGlinkPrepare ();

VOID
PmicGlinkStart ();

VOID
PmicGlinkStop ();

#endif /* _PMIC_GLINK_H_ */
