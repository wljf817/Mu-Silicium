/**
  Copyright (c) 2026, wljf817. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>

#include "AdspLoaderDxe.h"

//
// Global Variables
//
STATIC EFI_GUID          mPilProtocolGuid      = EFI_PIL_PROTOCOL_GUID;
STATIC EFI_GUID          mPmicSchgProtocolGuid = EFI_PMIC_SCHG_PROTOCOL_GUID;
STATIC EFI_PIL_PROTOCOL *mPilProtocol          = NULL;
STATIC EFI_EVENT         mExitBootServicesEvent;

EFI_PARTITION_ENTRY*
GetPartitionEntry (IN EFI_HANDLE Handle)
{
  EFI_STATUS                   Status;
  EFI_PARTITION_INFO_PROTOCOL *PartitionInfo;
  EFI_PARTITION_ENTRY         *PartitionEntry;

  // Get GPT Entry from Partition Info
  Status = gBS->HandleProtocol (Handle, &gEfiPartitionInfoProtocolGuid, (VOID *)&PartitionInfo);
  if (!EFI_ERROR (Status) && PartitionInfo->Type == PARTITION_TYPE_GPT) {
    return &PartitionInfo->Info.Gpt;
  }

  // Get GPT Entry from Partition Record
  Status = gBS->HandleProtocol (Handle, &gEfiPartitionRecordGuid, (VOID *)&PartitionEntry);
  if (!EFI_ERROR (Status)) {
    return PartitionEntry;
  }

  return NULL;
}

EFI_HANDLE
FindModemPartition ()
{
  EFI_STATUS           Status;
  EFI_HANDLE          *Handles;
  UINTN                HandleCount;
  EFI_HANDLE           Partition = NULL;
  EFI_PARTITION_ENTRY *PartitionEntry;

  // Locate all Block IO Handles
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Locate Block IO Handles! Status = %r\n", __FUNCTION__, Status));
    return NULL;
  }

  // Go thru each Block IO Handle
  for (UINTN i = 0; i < HandleCount; i++) {
    PartitionEntry = GetPartitionEntry (Handles[i]);
    if (PartitionEntry == NULL) {
      continue;
    }

    // Check Partition Name
    if (StrCmp (PartitionEntry->PartitionName, L"modem_a") != 0 && StrCmp (PartitionEntry->PartitionName, L"modem_b") != 0) {
      continue;
    }

    // Prefer the Partition of the active Slot
    if (PartitionEntry->Attributes & PART_ATT_ACTIVE_VAL) {
      Partition = Handles[i];
      break;
    }

    // Fall back to Slot A
    if (StrCmp (PartitionEntry->PartitionName, L"modem_a") == 0) {
      Partition = Handles[i];
    }
  }

  // Free Buffer
  FreePool (Handles);

  return Partition;
}

VOID
ConnectDisks ()
{
  EFI_STATUS  Status;
  EFI_HANDLE *Handles;
  UINTN       HandleCount;

  // Locate all Block IO Handles
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  // Create Partition Handles of whole Disks only
  for (UINTN i = 0; i < HandleCount; i++) {
    if (GetPartitionEntry (Handles[i]) == NULL) {
      gBS->ConnectController (Handles[i], NULL, NULL, FALSE);
    }
  }

  // Free Buffer
  FreePool (Handles);
}

EFI_STATUS
MountModemPartition ()
{
  EFI_STATUS  Status;
  EFI_HANDLE  Partition;
  VOID       *FileSystem;

  // Find Modem Partition
  Partition = FindModemPartition ();
  if (Partition == NULL) {
    ConnectDisks ();

    Partition = FindModemPartition ();
    if (Partition == NULL) {
      DEBUG ((EFI_D_ERROR, "%a: Modem Partition not Found!\n", __FUNCTION__));
      return EFI_NOT_FOUND;
    }
  }

  // Check for an existing File System
  Status = gBS->HandleProtocol (Partition, &gEfiSimpleFileSystemProtocolGuid, &FileSystem);
  if (!EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  // Mount File System
  Status = gBS->ConnectController (Partition, NULL, NULL, TRUE);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Connect Modem Partition! Status = %r\n", __FUNCTION__, Status));
    return Status;
  }

  return gBS->HandleProtocol (Partition, &gEfiSimpleFileSystemProtocolGuid, &FileSystem);
}

VOID
EFIAPI
ShutdownAdsp (
  IN EFI_EVENT Event,
  IN VOID     *Context)
{
  EFI_STATUS Status;

  // Shutdown ADSP
  Status = mPilProtocol->ShutdownPilImage (ADSP_SUBSYS_ID);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Shutdown ADSP! Status = %r\n", __FUNCTION__, Status));
  }
}

VOID
EFIAPI
LoadAdsp (
  IN EFI_EVENT Event,
  IN VOID     *Context)
{
  EFI_STATUS              Status;
  EFI_PMIC_SCHG_PROTOCOL *PmicSchgProtocol;

  // Close Event
  gBS->CloseEvent (Event);

  // Locate PIL Protocol
  Status = gBS->LocateProtocol (&mPilProtocolGuid, NULL, (VOID *)&mPilProtocol);
  if (EFI_ERROR (Status) || mPilProtocol->Revision != EFI_PIL_PROTOCOL_REVISION) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Locate PIL Protocol! Status = %r\n", __FUNCTION__, Status));
    return;
  }

  // Locate PMIC SCHG Protocol
  Status = gBS->LocateProtocol (&mPmicSchgProtocolGuid, NULL, (VOID *)&PmicSchgProtocol);
  if (EFI_ERROR (Status) || PmicSchgProtocol->Revision != EFI_PMIC_SCHG_PROTOCOL_REVISION) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Locate PMIC SCHG Protocol! Status = %r\n", __FUNCTION__, Status));
    return;
  }

  // Set OS Type like Stock before loading the Charger Firmware
  Status = PmicSchgProtocol->SetOs (0);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Set OS Type! Status = %r\n", __FUNCTION__, Status));
    return;
  }

  // Mount Modem Partition, PIL reads the ADSP Firmware from there
  Status = MountModemPartition ();
  if (EFI_ERROR (Status)) {
    return;
  }

  // Load ADSP
  Status = mPilProtocol->ProcessPilImage (ADSP_FW_NAME);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Load ADSP! Status = %r\n", __FUNCTION__, Status));
    return;
  }

  DEBUG ((EFI_D_WARN, "%a: ADSP Loaded\n", __FUNCTION__));

  // Shutdown ADSP before the OS takes over
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ShutdownAdsp, NULL, &gEfiEventExitBootServicesGuid, &mExitBootServicesEvent);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Create Exit Boot Services Event! Status = %r\n", __FUNCTION__, Status));
  }
}

EFI_STATUS
EFIAPI
InitAdspLoader (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_EVENT EndOfDxeEvent;

  //
  // Load ADSP at End of DXE: all Drivers are dispatched, but BDS has not mounted
  // the other FAT Partitions yet, so like Stock only the Modem Partition is mounted.
  //
  return gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, LoadAdsp, NULL, &gEfiEndOfDxeEventGroupGuid, &EndOfDxeEvent);
}
