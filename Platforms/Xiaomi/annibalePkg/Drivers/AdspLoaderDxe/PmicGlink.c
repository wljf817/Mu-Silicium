/**
  Copyright (c) 2026, wljf817. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

  Minimal GLink Client for the PMIC Channel of the ADSP.

  GLinkDxe can not be used, it crashes Silicium with an Interrupt Storm.
  This only talks to the ADSP over the SMEM Transport, polls the FIFOs
  and kicks the ADSP thru IPCC, no Interrupts are registered.
  The Protocol follows qcom_glink_smem.c and qcom_glink_native.c of Linux.
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include "ChargePump.h"
#include "PmicGlink.h"

//
// Global Variables
//
STATIC EFI_GUID           mSmemProtocolGuid = EFI_SMEM_PROTOCOL_GUID;
STATIC EFI_SMEM_PROTOCOL *mSmemProtocol     = NULL;
STATIC GLINK_PIPE         mTxPipe;
STATIC GLINK_PIPE         mRxPipe;
STATIC GLINK_STATE        mGlink;
STATIC UINT8              mRxBuffer[GLINK_INTENT_SIZE];
STATIC EFI_EVENT          mPollEvent = NULL;

VOID
GlinkKick ()
{
  MmioWrite32 (IPCC_BASE_ADDR + IPCC_REG_SEND_ID, (IPCC_CLIENT_LPASS << 16) | IPCC_SIGNAL_GLINK);
}

UINT32
GlinkRxAvail ()
{
  EFI_STATUS        Status;
  SMEM_ALLOC_PARAMS Params;
  UINT32            Head;
  UINT32            Tail;

  // The ADSP allocates its TX FIFO when it starts
  if (mRxPipe.Fifo == NULL) {
    ZeroMem (&Params, sizeof (Params));

    Params.RemoteHost = GLINK_REMOTE_HOST_ADSP;
    Params.SmemType   = SMEM_GLINK_NATIVE_XPRT_FIFO_1;
    Params.Flags      = SMEM_ALLOC_FLAGS_GLINK;

    Status = mSmemProtocol->SmemGetAddrEx (&Params);
    if (EFI_ERROR (Status) || Params.Buffer == NULL || Params.Size == 0) {
      return 0;
    }

    mRxPipe.Fifo   = Params.Buffer;
    mRxPipe.Length = Params.Size;

    DEBUG ((EFI_D_INFO, "%a: RX FIFO at 0x%p, Size 0x%x\n", __FUNCTION__, Params.Buffer, Params.Size));
  }

  Head = *mRxPipe.Head;
  Tail = *mRxPipe.Tail;

  MemoryFence ();

  if (Head >= mRxPipe.Length || Tail >= mRxPipe.Length) {
    return 0;
  }

  return Head >= Tail ? Head - Tail : mRxPipe.Length - Tail + Head;
}

VOID
GlinkRxPeek (
  IN  UINT32  Offset,
  OUT VOID   *Buffer,
  IN  UINT32  Count)
{
  UINT8  *Data = Buffer;
  UINT32  Tail = *mRxPipe.Tail + Offset;

  for (UINT32 i = 0; i < Count; i++) {
    Data[i] = mRxPipe.Fifo[(Tail + i) % mRxPipe.Length];
  }
}

VOID
GlinkRxAdvance (IN UINT32 Count)
{
  UINT32 Tail = (*mRxPipe.Tail + ALIGN_VALUE (Count, 8)) % mRxPipe.Length;

  MemoryFence ();

  *mRxPipe.Tail = Tail;
}

UINT32
GlinkTxAvail ()
{
  UINT32 Head  = *mTxPipe.Head;
  UINT32 Tail  = *mTxPipe.Tail;
  UINT32 Avail = Tail <= Head ? mTxPipe.Length - Head + Tail : Tail - Head;

  return Avail > GLINK_FIFO_RESERVE ? Avail - GLINK_FIFO_RESERVE : 0;
}

UINT32
GlinkTxWriteOne (
  IN UINT32  Head,
  IN VOID   *Buffer,
  IN UINT32  Count)
{
  UINT8 *Data = Buffer;

  for (UINT32 i = 0; i < Count; i++) {
    mTxPipe.Fifo[(Head + i) % mTxPipe.Length] = Data[i];
  }

  return (Head + Count) % mTxPipe.Length;
}

EFI_STATUS
GlinkTx (
  IN VOID   *Header,
  IN UINT32  HeaderSize,
  IN VOID   *Data,
  IN UINT32  DataSize)
{
  UINT32 Head;

  // Wait for Space in the TX FIFO
  for (UINTN i = 0; GlinkTxAvail () < ALIGN_VALUE (HeaderSize + DataSize, 8); i++) {
    if (i >= 1000) {
      DEBUG ((EFI_D_ERROR, "%a: TX FIFO is Full!\n", __FUNCTION__));
      return EFI_TIMEOUT;
    }

    gBS->Stall (1000);
  }

  // Write Message
  Head = *mTxPipe.Head;
  Head = GlinkTxWriteOne (Head, Header, HeaderSize);
  Head = GlinkTxWriteOne (Head, Data, DataSize);
  Head = ALIGN_VALUE (Head, 8) % mTxPipe.Length;

  // Make the Message visible before the Head
  MemoryFence ();

  *mTxPipe.Head = Head;

  GlinkKick ();

  return EFI_SUCCESS;
}

EFI_STATUS
GlinkSendCommand (
  IN UINT16 Cmd,
  IN UINT16 Param1,
  IN UINT32 Param2)
{
  GLINK_MSG Msg;

  Msg.Cmd    = Cmd;
  Msg.Param1 = Param1;
  Msg.Param2 = Param2;

  return GlinkTx (&Msg, sizeof (Msg), NULL, 0);
}

EFI_STATUS
GlinkSendOpen ()
{
  UINT8      Request[sizeof (GLINK_MSG) + GLINK_NAME_SIZE];
  GLINK_MSG *Msg     = (GLINK_MSG *)Request;
  UINT32     NameLen = AsciiStrSize (PMIC_GLINK_CHANNEL);

  ZeroMem (Request, sizeof (Request));

  Msg->Cmd    = GLINK_CMD_OPEN;
  Msg->Param1 = GLINK_LOCAL_CHANNEL_ID;
  Msg->Param2 = NameLen;

  CopyMem (Request + sizeof (GLINK_MSG), PMIC_GLINK_CHANNEL, NameLen);

  return GlinkTx (Request, ALIGN_VALUE (sizeof (GLINK_MSG) + NameLen, 8), NULL, 0);
}

EFI_STATUS
GlinkAdvertiseIntent ()
{
  GLINK_INTENT_MSG Msg;

  Msg.Msg.Cmd     = GLINK_CMD_INTENT;
  Msg.Msg.Param1  = GLINK_LOCAL_CHANNEL_ID;
  Msg.Msg.Param2  = 1;
  Msg.Intent.Size = GLINK_INTENT_SIZE;
  Msg.Intent.Id   = mGlink.NextIntentId++;

  return GlinkTx (&Msg, sizeof (Msg), NULL, 0);
}

EFI_STATUS
GlinkSendData (
  IN UINT32  IntentId,
  IN VOID   *Data,
  IN UINT32  DataSize)
{
  GLINK_TX_DATA_HDR Hdr;

  Hdr.Msg.Cmd    = GLINK_CMD_TX_DATA;
  Hdr.Msg.Param1 = GLINK_LOCAL_CHANNEL_ID;
  Hdr.Msg.Param2 = IntentId;
  Hdr.ChunkSize  = DataSize;
  Hdr.LeftSize   = 0;

  return GlinkTx (&Hdr, sizeof (Hdr), Data, DataSize);
}

VOID
GlinkAddRemoteIntent (
  IN UINT32 Id,
  IN UINT32 Size)
{
  for (UINTN i = 0; i < GLINK_MAX_REMOTE_INTENTS; i++) {
    if (!mGlink.RemoteIntents[i].Valid) {
      mGlink.RemoteIntents[i].Id    = Id;
      mGlink.RemoteIntents[i].Size  = Size;
      mGlink.RemoteIntents[i].Valid = TRUE;
      mGlink.RemoteIntents[i].InUse = FALSE;
      return;
    }
  }

  DEBUG ((EFI_D_ERROR, "%a: No Space for Remote Intent %u!\n", __FUNCTION__, Id));
}

VOID
GlinkRemoteIntentDone (
  IN UINT32  Id,
  IN BOOLEAN Reuse)
{
  for (UINTN i = 0; i < GLINK_MAX_REMOTE_INTENTS; i++) {
    if (mGlink.RemoteIntents[i].Valid && mGlink.RemoteIntents[i].Id == Id) {
      mGlink.RemoteIntents[i].InUse = FALSE;
      mGlink.RemoteIntents[i].Valid = Reuse;
      return;
    }
  }
}

GLINK_REMOTE_INTENT*
GlinkGetRemoteIntent (IN UINT32 Size)
{
  for (UINTN i = 0; i < GLINK_MAX_REMOTE_INTENTS; i++) {
    if (mGlink.RemoteIntents[i].Valid && !mGlink.RemoteIntents[i].InUse && mGlink.RemoteIntents[i].Size >= Size) {
      return &mGlink.RemoteIntents[i];
    }
  }

  return NULL;
}

VOID
HandlePmicGlinkMessage (
  IN VOID   *Buffer,
  IN UINT32  Length)
{
  PMIC_GLINK_HDR     *Hdr  = Buffer;
  XM_PROP_WRITE_RESP *Resp = Buffer;
  UINT32              Data[4];

  if (Length < sizeof (PMIC_GLINK_HDR)) {
    return;
  }

  ZeroMem (Data, sizeof (Data));
  CopyMem (Data, (UINT8 *)Buffer + sizeof (PMIC_GLINK_HDR), MIN (sizeof (Data), Length - sizeof (PMIC_GLINK_HDR)));

  DEBUG ((EFI_D_INFO, "%a: Owner 0x%x, Type %u, Opcode 0x%x, Length %u: %x %x %x %x\n", __FUNCTION__, Hdr->Owner, Hdr->Type, Hdr->Opcode, Length, Data[0], Data[1], Data[2], Data[3]));

  // The ADSP starts OTG, connect VBUS to the USB Port
  if (Hdr->Owner == XM_GLINK_OWNER && Hdr->Type == PMIC_GLINK_MSG_TYPE_NOTIFY && Hdr->Opcode == XM_GLINK_OPCODE_NOTIFY && Data[0] == XM_NOTIFY_OVPGATE) {
    ChargePumpEnableOvpGate ();
  }

  // The ADSP configures OTG, Android puts the Charge Pump into Reverse Mode (platform_class_cp_set_revchg)
  if (Hdr->Owner == XM_GLINK_OWNER && Hdr->Type == PMIC_GLINK_MSG_TYPE_NOTIFY && Hdr->Opcode == XM_GLINK_OPCODE_NOTIFY && Data[0] == XM_NOTIFY_OTG_CP_CONFIG) {
    ChargePumpSetReverseCharge (Data[1] != 0);
  }

  // Response to a Xiaomi Property Write
  if (Hdr->Owner == XM_GLINK_OWNER && Hdr->Type == PMIC_GLINK_MSG_TYPE_REQ_RESP && Hdr->Opcode == XM_GLINK_OPCODE_WRITE_PROP && Length >= sizeof (XM_PROP_WRITE_RESP)) {
    if (Resp->PropId == mGlink.PendingPropId) {
      mGlink.ResponseResult   = Resp->RetCode;
      mGlink.ResponseReceived = TRUE;
    }
  }
}

VOID
GlinkProcessRx ()
{
  UINT32            Avail;
  UINT32            Size;
  UINT32            Copy;
  GLINK_MSG         Msg;
  GLINK_TX_DATA_HDR DataHdr;
  GLINK_INTENT_PAIR Intent;
  CHAR8             Name[GLINK_NAME_SIZE];

  while ((Avail = GlinkRxAvail ()) >= sizeof (GLINK_MSG)) {
    GlinkRxPeek (0, &Msg, sizeof (Msg));

    switch (Msg.Cmd) {
      case GLINK_CMD_VERSION:
        DEBUG ((EFI_D_INFO, "%a: Remote Version %u, Features 0x%x\n", __FUNCTION__, Msg.Param1, Msg.Param2));

        GlinkRxAdvance (sizeof (Msg));
        GlinkSendCommand (GLINK_CMD_VERSION_ACK, GLINK_VERSION_1, GLINK_FEATURE_INTENT_REUSE);

        mGlink.VersionReceived = TRUE;
        break;

      case GLINK_CMD_VERSION_ACK:
        DEBUG ((EFI_D_INFO, "%a: Version Acked, Features 0x%x\n", __FUNCTION__, Msg.Param2));

        GlinkRxAdvance (sizeof (Msg));

        mGlink.VersionAcked = TRUE;
        break;

      case GLINK_CMD_OPEN:
        Size = ALIGN_VALUE (sizeof (Msg) + Msg.Param2, 8);
        if (Avail < Size) {
          return;
        }

        ZeroMem (Name, sizeof (Name));
        GlinkRxPeek (sizeof (Msg), Name, MIN (Msg.Param2, sizeof (Name) - 1));
        GlinkRxAdvance (Size);

        DEBUG ((EFI_D_INFO, "%a: Remote opened Channel %u \"%a\"\n", __FUNCTION__, Msg.Param1, Name));

        // Only the PMIC Channel is accepted
        if (AsciiStrCmp (Name, PMIC_GLINK_CHANNEL) == 0) {
          mGlink.RemoteChannelId = Msg.Param1;
          mGlink.RemoteOpened    = TRUE;

          GlinkSendCommand (GLINK_CMD_OPEN_ACK, Msg.Param1, 0);
        }

        break;

      case GLINK_CMD_OPEN_ACK:
        DEBUG ((EFI_D_INFO, "%a: Open Acked for Channel %u\n", __FUNCTION__, Msg.Param1));

        GlinkRxAdvance (sizeof (Msg));

        if (Msg.Param1 == GLINK_LOCAL_CHANNEL_ID) {
          mGlink.OpenAcked = TRUE;
        }

        break;

      case GLINK_CMD_CLOSE:
        DEBUG ((EFI_D_WARN, "%a: Remote closed Channel %u\n", __FUNCTION__, Msg.Param1));

        GlinkRxAdvance (sizeof (Msg));

        if (Msg.Param1 == mGlink.RemoteChannelId) {
          mGlink.RemoteOpened = FALSE;
        }

        GlinkSendCommand (GLINK_CMD_CLOSE_ACK, Msg.Param1, 0);
        break;

      case GLINK_CMD_INTENT:
        Size = ALIGN_VALUE (sizeof (Msg) + Msg.Param2 * sizeof (GLINK_INTENT_PAIR), 8);
        if (Avail < Size) {
          return;
        }

        for (UINT32 i = 0; i < Msg.Param2; i++) {
          GlinkRxPeek (sizeof (Msg) + i * sizeof (GLINK_INTENT_PAIR), &Intent, sizeof (Intent));

          DEBUG ((EFI_D_INFO, "%a: Remote Intent %u, Size 0x%x on Channel %u\n", __FUNCTION__, Intent.Id, Intent.Size, Msg.Param1));

          if (Msg.Param1 == mGlink.RemoteChannelId) {
            GlinkAddRemoteIntent (Intent.Id, Intent.Size);
          }
        }

        GlinkRxAdvance (Size);
        break;

      case GLINK_CMD_RX_DONE:
      case GLINK_CMD_RX_DONE_W_REUSE:
        GlinkRxAdvance (sizeof (Msg));
        GlinkRemoteIntentDone (Msg.Param2, Msg.Cmd == GLINK_CMD_RX_DONE_W_REUSE);
        break;

      case GLINK_CMD_RX_INTENT_REQ:
        DEBUG ((EFI_D_INFO, "%a: Remote requested Intent of Size 0x%x\n", __FUNCTION__, Msg.Param2));

        GlinkRxAdvance (sizeof (Msg));
        GlinkAdvertiseIntent ();
        GlinkSendCommand (GLINK_CMD_RX_INTENT_REQ_ACK, GLINK_LOCAL_CHANNEL_ID, 1);
        break;

      case GLINK_CMD_TX_DATA:
      case GLINK_CMD_TX_DATA_CONT:
        if (Avail < sizeof (DataHdr)) {
          return;
        }

        GlinkRxPeek (0, &DataHdr, sizeof (DataHdr));

        Size = ALIGN_VALUE (sizeof (DataHdr) + DataHdr.ChunkSize, 8);
        if (Avail < Size) {
          return;
        }

        if (Msg.Cmd == GLINK_CMD_TX_DATA) {
          mGlink.RxLength = 0;
        }

        Copy = MIN (DataHdr.ChunkSize, (UINT32)sizeof (mRxBuffer) - mGlink.RxLength);

        GlinkRxPeek (sizeof (DataHdr), mRxBuffer + mGlink.RxLength, Copy);
        GlinkRxAdvance (Size);

        mGlink.RxLength += Copy;

        if (DataHdr.LeftSize != 0) {
          break;
        }

        if (Msg.Param1 == mGlink.RemoteChannelId) {
          HandlePmicGlinkMessage (mRxBuffer, mGlink.RxLength);
        }

        // Release the used Intent and give the ADSP a new one
        GlinkSendCommand (GLINK_CMD_RX_DONE, GLINK_LOCAL_CHANNEL_ID, Msg.Param2);
        GlinkAdvertiseIntent ();
        break;

      case GLINK_CMD_READ_NOTIF:
        GlinkRxAdvance (sizeof (Msg));
        GlinkKick ();
        break;

      case GLINK_CMD_RX_INTENT_REQ_ACK:
      case GLINK_CMD_CLOSE_ACK:
      case GLINK_CMD_SIGNALS:
        GlinkRxAdvance (sizeof (Msg));
        break;

      default:
        DEBUG ((EFI_D_ERROR, "%a: Unknown Command %u!\n", __FUNCTION__, Msg.Cmd));

        GlinkRxAdvance (sizeof (Msg));
        break;
    }
  }
}

BOOLEAN
GlinkWait (
  IN GLINK_CONDITION Condition,
  IN UINTN           TimeoutMs)
{
  for (UINTN i = 0; i < TimeoutMs; i++) {
    GlinkProcessRx ();

    if (Condition ()) {
      return TRUE;
    }

    gBS->Stall (1000);
  }

  return FALSE;
}

BOOLEAN
IsLinkUp ()
{
  return mGlink.VersionReceived && mGlink.VersionAcked;
}

BOOLEAN
IsChannelOpen ()
{
  return mGlink.RemoteOpened && mGlink.OpenAcked;
}

BOOLEAN
HasRemoteIntent ()
{
  return GlinkGetRemoteIntent (sizeof (XM_PROP_WRITE_REQ)) != NULL;
}

BOOLEAN
HasResponse ()
{
  return mGlink.ResponseReceived;
}

EFI_STATUS
WriteXiaomiProperty (
  IN UINT32  PropId,
  IN VOID   *Data,
  IN UINT32  DataSize)
{
  EFI_STATUS           Status;
  XM_PROP_WRITE_REQ    Request;
  GLINK_REMOTE_INTENT *Intent;

  // Wait for an Intent of the ADSP
  if (!GlinkWait (HasRemoteIntent, 1000)) {
    DEBUG ((EFI_D_ERROR, "%a: No Remote Intent for Property 0x%x!\n", __FUNCTION__, PropId));
    return EFI_NOT_READY;
  }

  Intent = GlinkGetRemoteIntent (sizeof (Request));

  // Build Request
  ZeroMem (&Request, sizeof (Request));

  Request.Hdr.Owner  = XM_GLINK_OWNER;
  Request.Hdr.Type   = PMIC_GLINK_MSG_TYPE_REQ_RESP;
  Request.Hdr.Opcode = XM_GLINK_OPCODE_WRITE_PROP;
  Request.PropId     = PropId;
  Request.SeqNum     = ++mGlink.SeqNum;

  CopyMem (Request.Data, Data, DataSize);

  mGlink.PendingPropId    = PropId;
  mGlink.ResponseReceived = FALSE;

  // Send Request
  Intent->InUse = TRUE;

  Status = GlinkSendData (Intent->Id, &Request, sizeof (Request));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Wait for the Response
  if (!GlinkWait (HasResponse, 1000)) {
    DEBUG ((EFI_D_ERROR, "%a: No Response for Property 0x%x!\n", __FUNCTION__, PropId));
    return EFI_TIMEOUT;
  }

  if (mGlink.ResponseResult != 0) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to write Property 0x%x! Result = %d\n", __FUNCTION__, PropId, mGlink.ResponseResult));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

VOID
EFIAPI
PollPmicGlink (
  IN EFI_EVENT Event,
  IN VOID     *Context)
{
  GlinkProcessRx ();
}

EFI_STATUS
PmicGlinkPrepare ()
{
  EFI_STATUS        Status;
  SMEM_ALLOC_PARAMS Params;
  volatile UINT32  *Descriptor;

  // Locate SMEM Protocol
  Status = gBS->LocateProtocol (&mSmemProtocolGuid, NULL, (VOID *)&mSmemProtocol);
  if (EFI_ERROR (Status) || mSmemProtocol->Revision != EFI_SMEM_PROTOCOL_REVISION) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Locate SMEM Protocol! Status = %r\n", __FUNCTION__, Status));
    return EFI_NOT_FOUND;
  }

  // Allocate the Descriptor with the FIFO Indexes
  ZeroMem (&Params, sizeof (Params));

  Params.RemoteHost = GLINK_REMOTE_HOST_ADSP;
  Params.SmemType   = SMEM_GLINK_NATIVE_XPRT_DESCRIPTOR;
  Params.Size       = GLINK_DESCRIPTOR_SIZE;
  Params.Flags      = SMEM_ALLOC_FLAGS_GLINK;

  Status = mSmemProtocol->SmemAllocEx (&Params);
  if (EFI_ERROR (Status) || Params.Buffer == NULL) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Allocate GLink Descriptor! Status = %r\n", __FUNCTION__, Status));
    return EFI_OUT_OF_RESOURCES;
  }

  Descriptor = Params.Buffer;

  // Allocate our TX FIFO
  ZeroMem (&Params, sizeof (Params));

  Params.RemoteHost = GLINK_REMOTE_HOST_ADSP;
  Params.SmemType   = SMEM_GLINK_NATIVE_XPRT_FIFO_0;
  Params.Size       = GLINK_FIFO_SIZE;
  Params.Flags      = SMEM_ALLOC_FLAGS_GLINK;

  Status = mSmemProtocol->SmemAllocEx (&Params);
  if (EFI_ERROR (Status) || Params.Buffer == NULL) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Allocate GLink TX FIFO! Status = %r\n", __FUNCTION__, Status));
    return EFI_OUT_OF_RESOURCES;
  }

  mTxPipe.Tail   = &Descriptor[0];
  mTxPipe.Head   = &Descriptor[1];
  mTxPipe.Fifo   = Params.Buffer;
  mTxPipe.Length = Params.Size;
  mRxPipe.Tail   = &Descriptor[2];
  mRxPipe.Head   = &Descriptor[3];
  mRxPipe.Fifo   = NULL;
  mRxPipe.Length = 0;

  DEBUG ((EFI_D_INFO, "%a: Descriptor at 0x%p (%x %x %x %x), TX FIFO at 0x%p\n", __FUNCTION__, Descriptor, Descriptor[0], Descriptor[1], Descriptor[2], Descriptor[3], mTxPipe.Fifo));

  //
  // The Stock UEFI used these Items before, the ADSP is not running yet,
  // so start with empty FIFOs.
  //
  for (UINTN i = 0; i < 4; i++) {
    Descriptor[i] = 0;
  }

  MemoryFence ();

  ZeroMem (&mGlink, sizeof (mGlink));

  mGlink.NextIntentId = 1;

  return EFI_SUCCESS;
}

VOID
PmicGlinkStart ()
{
  EFI_STATUS Status;
  UINT8      CpState = 1;
  XM_OTG_CFG OtgCfg  = { 0, 1, -1, -1 };
  UINT32     CidCfg  = 0x708;
  UINT8      EuModel = 0;

  // Send our Version, the ADSP sends its own
  GlinkSendCommand (GLINK_CMD_VERSION, GLINK_VERSION_1, GLINK_FEATURE_INTENT_REUSE);

  if (!GlinkWait (IsLinkUp, 5000)) {
    DEBUG ((EFI_D_ERROR, "%a: GLink is not up! Version Received %d, Acked %d, RX FIFO 0x%p\n", __FUNCTION__, mGlink.VersionReceived, mGlink.VersionAcked, mRxPipe.Fifo));
    return;
  }

  // Open the PMIC Channel
  GlinkSendOpen ();

  if (!GlinkWait (IsChannelOpen, 3000)) {
    DEBUG ((EFI_D_ERROR, "%a: PMIC Channel is not open! Remote Opened %d, Acked %d\n", __FUNCTION__, mGlink.RemoteOpened, mGlink.OpenAcked));
    return;
  }

  DEBUG ((EFI_D_WARN, "%a: PMIC Channel is open\n", __FUNCTION__));

  // Give the ADSP Intents for its Responses
  for (UINTN i = 0; i < GLINK_LOCAL_INTENTS; i++) {
    GlinkAdvertiseIntent ();
  }

  // Android brings up the Charge Pump before it sends the Config
  if (!EFI_ERROR (ChargePumpInit ())) {
    ChargePumpEnableOvpGate ();
  }

  // Send the Config like qcom_subpmic_sync_cfg_work of Android
  Status = WriteXiaomiProperty (XM_PROP_CP_STATE, &CpState, sizeof (CpState));
  if (!EFI_ERROR (Status)) {
    WriteXiaomiProperty (XM_PROP_OTG_CFG, &OtgCfg, sizeof (OtgCfg));
    WriteXiaomiProperty (XM_PROP_CID_CFG, &CidCfg, sizeof (CidCfg));
    WriteXiaomiProperty (XM_PROP_EU_MODEL, &EuModel, sizeof (EuModel));
  }

  // Keep handling Messages of the ADSP
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, PollPmicGlink, NULL, &mPollEvent);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Create Poll Event! Status = %r\n", __FUNCTION__, Status));
    return;
  }

  gBS->SetTimer (mPollEvent, TimerPeriodic, EFI_TIMER_PERIOD_MILLISECONDS (50));
}

VOID
PmicGlinkStop ()
{
  if (mPollEvent != NULL) {
    gBS->CloseEvent (mPollEvent);
    mPollEvent = NULL;
  }
}
