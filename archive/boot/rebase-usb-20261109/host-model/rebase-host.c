/** @file
  Copyright (C) 2016, The HermitCrabs Lab. All rights reserved.

  All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/
/* Host adapters below; only table functions are under test, not full UEFI. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef uint8_t UINT8; typedef uint16_t UINT16; typedef uint32_t UINT32; typedef char CHAR8; typedef int BOOLEAN; typedef int EFI_STATUS;
#define STATIC static
#define VOID void
#define IN
#define OUT
#define CONST const
#define OPTIONAL
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_OUT_OF_RESOURCES 1
#define OC_ACPI_NAME_SIZE 4
#define AML_EXT_OP 0x5B
#define AML_EXT_REGION_OP 0x80
#define AML_DWORD_PREFIX 0x0C
#define AML_WORD_PREFIX 0x0B
#define AML_NAME_OP 0x08
#define EFI_ACPI_6_2_SYSTEM_MEMORY 0
#define DEBUG(x) ((void)0)
#define AllocatePool malloc
#define FreePool free
#define CopyMem(d,s,n) do { if ((n)>0) memcpy((d),(s),(n)); } while(0)
#define AsciiStrCmp strcmp
typedef struct { UINT32 Signature,Length; } EFI_ACPI_COMMON_HEADER;
#pragma pack(push,1)
typedef struct { UINT32 Signature,Length; UINT8 Revision,Checksum; UINT8 Rest[26]; } EFI_ACPI_DESCRIPTION_HEADER;
#pragma pack(pop)
typedef struct { UINT32 Address; CHAR8 Name[5]; } OC_ACPI_REGION;
typedef struct { OC_ACPI_REGION *Regions; UINT32 NumberOfRegions,AllocatedRegions; } OC_ACPI_CONTEXT;
static UINT8 CalculateCheckSum8(const UINT8 *p, UINT32 n) { UINT8 s=0; for(UINT32 i=0;i<n;i++)s+=p[i];return (UINT8)(0-s); }
STATIC
BOOLEAN
AcpiReadName (
  IN  CONST UINT8  *Data,
  OUT CHAR8        *Name,
  OUT UINT32       *NameOffset OPTIONAL
  )
{
  UINT32  Index;
  UINT32  Off;

  //
  // Skip \ in \NAME.
  //
  Off = Data[0] == '\\' ? 1 : 0;

  for (Index = Off; Index < Off + OC_ACPI_NAME_SIZE; ++Index) {
    if (  (Data[Index] < '/')
       || ((Data[Index] > '9') && (Data[Index] < 'A'))
       || ((Data[Index] > 'Z') && (Data[Index] != '_')))
    {
      return FALSE;
    }

    Name[Index - Off] = Data[Index];
  }

  Name[OC_ACPI_NAME_SIZE] = 0;

  if (NameOffset != NULL) {
    *NameOffset = Off;
  }

  return TRUE;
}

STATIC
UINT32
AcpiFindName (
  IN CONST UINT8  *Data,
  IN UINT32       Length,
  IN CONST CHAR8  *Name
  )
{
  UINT32  Index;

  //
  // Lookup from offset 1 and after.
  //
  if (Length < OC_ACPI_NAME_SIZE + 1) {
    return 0;
  }

  for (Index = 0; Index < Length - OC_ACPI_NAME_SIZE; ++Index) {
    if (  (Data[Index] == AML_NAME_OP)
       && (Data[Index+1] == Name[0])
       && (Data[Index+2] == Name[1])
       && (Data[Index+3] == Name[2])
       && (Data[Index+4] == Name[3]))
    {
      return Index+1;
    }
  }

  return 0;
}

STATIC
EFI_STATUS
AcpiLoadTableRegions (
  IN OUT OC_ACPI_CONTEXT         *Context,
  IN     EFI_ACPI_COMMON_HEADER  *Table
  )
{
  UINT32          Index;
  UINT32          Index2;
  UINT32          NameOffset;
  UINT8           *Buffer;
  UINT32          BufferLen;
  CHAR8           Name[OC_ACPI_NAME_SIZE+1];
  CHAR8           NameAddr[OC_ACPI_NAME_SIZE+1];
  OC_ACPI_REGION  *NewRegions;
  UINT32          Address;

  Buffer    = (UINT8 *)Table;
  BufferLen = Table->Length;

  if (BufferLen < sizeof (EFI_ACPI_DESCRIPTION_HEADER)) {
    return EFI_SUCCESS;
  }

  for (Index = sizeof (EFI_ACPI_DESCRIPTION_HEADER); Index < BufferLen - 0xF; ++Index) {
    if (  (Buffer[Index] == AML_EXT_OP)
       && (Buffer[Index+1] == AML_EXT_REGION_OP)
       && AcpiReadName (&Buffer[Index+2], &Name[0], &NameOffset)
       && (Buffer[Index+OC_ACPI_NAME_SIZE+2+NameOffset] == EFI_ACPI_6_2_SYSTEM_MEMORY))
    {
      //
      // This is SystemMemory region. Try to save it.
      //
      Address = 0;
      if (Buffer[Index+OC_ACPI_NAME_SIZE+3+NameOffset] == AML_DWORD_PREFIX) {
        CopyMem (&Address, &Buffer[Index+OC_ACPI_NAME_SIZE+4+NameOffset], sizeof (UINT32));
      } else if (Buffer[Index+OC_ACPI_NAME_SIZE+3+NameOffset] == AML_WORD_PREFIX) {
        CopyMem (&Address, &Buffer[Index+OC_ACPI_NAME_SIZE+4+NameOffset], sizeof (UINT16));
      } else if (AcpiReadName (&Buffer[Index+OC_ACPI_NAME_SIZE+3+NameOffset], &NameAddr[0], NULL)) {
        Index2 = AcpiFindName (Buffer, BufferLen, &NameAddr[0]);
        if ((Index2 > 0) && (Index2 < BufferLen - 0xF)) {
          if (Buffer[Index2+OC_ACPI_NAME_SIZE] == AML_DWORD_PREFIX) {
            CopyMem (&Address, &Buffer[Index2+OC_ACPI_NAME_SIZE+1], sizeof (UINT32));
          } else if (Buffer[Index2+OC_ACPI_NAME_SIZE] == AML_WORD_PREFIX) {
            CopyMem (&Address, &Buffer[Index2+OC_ACPI_NAME_SIZE+1], sizeof (UINT16));
          }
        }
      }

      for (Index2 = 0; Index2 < Context->NumberOfRegions; ++Index2) {
        if (Context->Regions[Index2].Address == Address) {
          Address = 0;
          break;
        }
      }

      if (Address != 0) {
        if (Context->AllocatedRegions == Context->NumberOfRegions) {
          NewRegions = AllocatePool ((Context->AllocatedRegions + 2) * sizeof (Context->Regions[0]));
          if (NewRegions == NULL) {
            DEBUG ((DEBUG_WARN, "OCA: Failed to allocate memory for %u regions\n", Context->AllocatedRegions+2));
            return EFI_OUT_OF_RESOURCES;
          }

          CopyMem (NewRegions, Context->Regions, Context->NumberOfRegions * sizeof (Context->Regions[0]));
          FreePool (Context->Regions);

          Context->Regions           = NewRegions;
          Context->AllocatedRegions += 2;
        }

        DEBUG ((DEBUG_INFO, "OCA: Found OperationRegion %a at %08X\n", Name, Address));
        Context->Regions[Context->NumberOfRegions].Address = Address;
        CopyMem (&Context->Regions[Context->NumberOfRegions].Name[0], &Name[0], sizeof (Name));
        ++Context->NumberOfRegions;
      }
    }
  }

  return EFI_SUCCESS;
}

STATIC
VOID
AcpiRelocateTableRegions (
  IN OUT OC_ACPI_CONTEXT         *Context,
  IN     EFI_ACPI_COMMON_HEADER  *Table
  )
{
  UINT32   Index;
  UINT32   Index2;
  UINT32   RegionIndex;
  UINT32   NameOffset;
  UINT8    *Buffer;
  UINT32   BufferLen;
  CHAR8    Name[OC_ACPI_NAME_SIZE+1];
  CHAR8    NameAddr[OC_ACPI_NAME_SIZE+1];
  UINT32   OldAddress;
  BOOLEAN  Modified;

  Buffer    = (UINT8 *)Table;
  BufferLen = Table->Length;
  Modified  = FALSE;

  if (BufferLen < sizeof (EFI_ACPI_DESCRIPTION_HEADER)) {
    return;
  }

  for (Index = sizeof (EFI_ACPI_DESCRIPTION_HEADER); Index < BufferLen - 0xF; ++Index) {
    if (  (Buffer[Index] == AML_EXT_OP)
       && (Buffer[Index+1] == AML_EXT_REGION_OP)
       && AcpiReadName (&Buffer[Index+2], &Name[0], &NameOffset)
       && (Buffer[Index+OC_ACPI_NAME_SIZE+2+NameOffset] == EFI_ACPI_6_2_SYSTEM_MEMORY))
    {
      //
      // This is region. Compare to current BIOS tables and relocate.
      //
      for (RegionIndex = 0; RegionIndex < Context->NumberOfRegions; ++RegionIndex) {
        if (AsciiStrCmp (Context->Regions[RegionIndex].Name, Name) == 0) {
          OldAddress = 0;
          if (Buffer[Index+OC_ACPI_NAME_SIZE+3+NameOffset] == AML_DWORD_PREFIX) {
            CopyMem (&OldAddress, &Buffer[Index+OC_ACPI_NAME_SIZE+4+NameOffset], sizeof (UINT32));
            CopyMem (&Buffer[Index+OC_ACPI_NAME_SIZE+4+NameOffset], &Context->Regions[RegionIndex].Address, sizeof (UINT32));
            Modified = TRUE;
          } else if (Buffer[Index+OC_ACPI_NAME_SIZE+3+NameOffset] == AML_WORD_PREFIX) {
            CopyMem (&OldAddress, &Buffer[Index+OC_ACPI_NAME_SIZE+4+NameOffset], sizeof (UINT16));
            CopyMem (&Buffer[Index+OC_ACPI_NAME_SIZE+4+NameOffset], &Context->Regions[RegionIndex].Address, sizeof (UINT16));
            Modified = TRUE;
          } else if (AcpiReadName (&Buffer[Index+OC_ACPI_NAME_SIZE+3+NameOffset], &NameAddr[0], NULL)) {
            Index2 = AcpiFindName (Buffer, BufferLen, &NameAddr[0]);
            if ((Index2 > 0) && (Index2 < BufferLen - 0xF)) {
              if (Buffer[Index2+OC_ACPI_NAME_SIZE] == AML_DWORD_PREFIX) {
                CopyMem (&OldAddress, &Buffer[Index2+OC_ACPI_NAME_SIZE+1], sizeof (UINT32));
                CopyMem (&Buffer[Index2+OC_ACPI_NAME_SIZE+1], &Context->Regions[RegionIndex].Address, sizeof (UINT32));
                Modified = TRUE;
              } else if (Buffer[Index2+OC_ACPI_NAME_SIZE] == AML_WORD_PREFIX) {
                CopyMem (&OldAddress, &Buffer[Index2+OC_ACPI_NAME_SIZE+1], sizeof (UINT16));
                CopyMem (&Buffer[Index2+OC_ACPI_NAME_SIZE+1], &Context->Regions[RegionIndex].Address, sizeof (UINT16));
                Modified = TRUE;
              }
            }
          }

          if (Modified && (OldAddress != Context->Regions[RegionIndex].Address)) {
            DEBUG ((
              DEBUG_INFO,
              "OCA: Region %a address relocated from %08X to %08X\n",
              Context->Regions[RegionIndex].Name,
              OldAddress,
              Context->Regions[RegionIndex].Address
              ));
          }

          break;
        }
      }
    }
  }

  //
  // Update checksum
  //
  if (Modified) {
    ((EFI_ACPI_DESCRIPTION_HEADER *)Table)->Checksum = 0;
    ((EFI_ACPI_DESCRIPTION_HEADER *)Table)->Checksum = CalculateCheckSum8 (
                                                         (UINT8 *)Table,
                                                         Table->Length
                                                         );
  }
}

static UINT8 *readtable(const char *path, size_t *len) {
 FILE *f=fopen(path,"rb");assert(f);assert(fseek(f,0,SEEK_END)==0);long n=ftell(f);assert(n>=36);rewind(f);
 UINT8 *b=malloc(n);assert(b);assert(fread(b,1,n,f)==(size_t)n);fclose(f);*len=n;
 assert(((EFI_ACPI_COMMON_HEADER*)b)->Length==n);assert(CalculateCheckSum8(b,n)==0);return b;
}
int main(int argc,char **argv) {
 assert(argc>=5);assert(sizeof(EFI_ACPI_DESCRIPTION_HEADER)==36);
 OC_ACPI_CONTEXT c={0};c.AllocatedRegions=8;c.Regions=malloc(sizeof(OC_ACPI_REGION)*8);assert(c.Regions);
 for(int i=3;i<argc;i++) {size_t n;UINT8 *b=readtable(argv[i],&n);assert(AcpiLoadTableRegions(&c,(EFI_ACPI_COMMON_HEADER*)b)==0);free(b);}
 printf("REGION_COUNT=%u\n",c.NumberOfRegions);
 for(UINT32 i=0;i<c.NumberOfRegions;i++)printf("REGION %.4s %08X\n",c.Regions[i].Name,c.Regions[i].Address);
 size_t n;UINT8 *b=readtable(argv[2],&n);AcpiRelocateTableRegions(&c,(EFI_ACPI_COMMON_HEADER*)b);assert(CalculateCheckSum8(b,n)==0);
 FILE *out=fopen(argv[1],"wb");assert(out);assert(fwrite(b,1,n,out)==n);fclose(out);free(b);
 for(int i=3;i<argc;i++) {size_t k;UINT8 *t=readtable(argv[i],&k),*old=malloc(k);assert(old);memcpy(old,t,k);AcpiRelocateTableRegions(&c,(EFI_ACPI_COMMON_HEADER*)t);if(memcmp(old,t,k))printf("OEM_CHANGED %s\n",argv[i]);free(t);free(old);}
 free(c.Regions);return 0;
}
