
#ifndef MTLC_TARGET_H
#define MTLC_TARGET_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  MTLC_ARCH_X86_64 = 0,
  MTLC_ARCH_ARM64,
  MTLC_ARCH_PTX,
  MTLC_ARCH_SPIRV,
  MTLC_ARCH_METAL
} MtlcArch;

typedef enum {
  MTLC_OBJ_COFF = 0,
  MTLC_OBJ_ELF
} MtlcObjectFormat;

typedef enum {
  MTLC_LINK_PE = 0,
  MTLC_LINK_ELF
} MtlcLinkTarget;

MtlcObjectFormat mtlc_host_object_format(void);
MtlcLinkTarget mtlc_host_link_target(void);
const char *mtlc_arch_name(MtlcArch arch);

#ifdef __cplusplus
}
#endif

#endif
