#pragma once
#include "nvn_protos.h"
typedef struct { NVNshaderData shader; const void *metadata; } NvnShaderDataExt;
typedef int (*NVNT_nvnTextureGetRawStorageClass)(const NVNtexture *);
typedef NVNboolean (*NVNT_nvnProgramSetShadersExt)(NVNprogram *, int, const NvnShaderDataExt *);
typedef void (*NVNT_nvnWindowBuilderSetNumActiveTextures)(NVNwindowBuilder *, int);
typedef void (*NVNT_nvnCommandBufferSetCommandMemoryCallbackEnabled)(NVNcommandBuffer *, NVNboolean);
typedef void (*NVNT_nvnCommandBufferBindTexturesDeferred)(NVNcommandBuffer *, NVNshaderStage, int, int, const NVNtextureHandle *);
