#ifndef _ADSP_LOADER_DXE_H_
#define _ADSP_LOADER_DXE_H_

//
// PIL Protocol (Interface of PILDxe)
//
#define EFI_PIL_PROTOCOL_GUID      { 0x1F4F1DA3, 0x4D8D, 0x4894, { 0xBA, 0x2A, 0x13, 0xD3, 0x20, 0x7F, 0x97, 0x39 } }
#define EFI_PIL_PROTOCOL_REVISION  0x10005

typedef
EFI_STATUS
(EFIAPI *EFI_PIL_PROCESS_IMAGE)(
  IN CHAR16 *FwName
  );

typedef
EFI_STATUS
(EFIAPI *EFI_PIL_SHUTDOWN_IMAGE)(
  IN UINT32 SubsysId
  );

typedef struct {
  UINT64                 Revision;
  EFI_PIL_PROCESS_IMAGE  ProcessPilImage;
  VOID                  *ProcessPilImageExt;
  VOID                  *Reserved;
  EFI_PIL_SHUTDOWN_IMAGE ShutdownPilImage;
} EFI_PIL_PROTOCOL;

//
// PMIC SCHG Protocol (Interface of PmicDxe, only SetOS is used)
//
#define EFI_PMIC_SCHG_PROTOCOL_GUID      { 0xAE6AE96E, 0x483F, 0x42AE, { 0x9C, 0xC1, 0x9F, 0xAC, 0x1B, 0x58, 0x47, 0x28 } }
#define EFI_PMIC_SCHG_PROTOCOL_REVISION  0x10026

typedef
EFI_STATUS
(EFIAPI *EFI_PMIC_SCHG_SET_OS)(
  IN UINT32 OsType
  );

typedef struct {
  UINT64               Revision;
  UINT8                Reserved[0x1C0];
  EFI_PMIC_SCHG_SET_OS SetOs;
} EFI_PMIC_SCHG_PROTOCOL;

//
// ADSP Details (from the PIL Config in the XBL DT)
//
#define ADSP_FW_NAME    L"FULL_ADSP"
#define ADSP_SUBSYS_ID  1

//
// A/B Slot Attribute of GPT Partitions
//
#define PART_ATT_ACTIVE_VAL  BIT50

#endif /* _ADSP_LOADER_DXE_H_ */
