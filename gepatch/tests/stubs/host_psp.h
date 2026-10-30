#ifndef GP_HOST_PSP_H
#define GP_HOST_PSP_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int SceUID;
typedef unsigned SceSize;
typedef struct { unsigned Buttons; } SceCtrlData;
typedef int16_t s16;
typedef struct PspGeContext { unsigned int context[512]; } PspGeContext;
/* Same layout as PSPSDK's pspge.h. */
typedef struct { unsigned size; PspGeContext *context; u32 numStacks; void *stacks; } PspGeListArgs;
#define PSP_MODULE_INFO(a,b,c,d)
#define PSP_CTRL_SELECT 0x1
#define PSP_CTRL_DOWN 0x40
#define PSP_CTRL_LTRIGGER 0x100
#define PSP_CTRL_TRIANGLE 0x1000
#define PSP_CTRL_RTRIGGER 0x200
#define PSP_O_WRONLY 1
#define PSP_O_CREAT 0x200
#define PSP_SEEK_END 2
int pspSdkSetK1(int value);
int sceIoOpen(const char*,int,int);
int sceIoLseek(int,int,int);
int sceIoWrite(int,const void*,int);
int sceIoClose(int);
void sceKernelDcacheWritebackInvalidateAll(void);
void sceKernelDcacheWritebackAll(void);
void sceKernelDcacheWritebackRange(const void *, unsigned);
void sceKernelDcacheInvalidateRange(const void *, unsigned);
void sceKernelDcacheWritebackInvalidateRange(const void *, unsigned);
void sceKernelIcacheClearAll(void);
unsigned sceKernelGetSystemTimeLow(void);
int scePowerGetCpuClockFrequencyInt(void);
int scePowerGetBusClockFrequencyInt(void);
int sceCtrlPeekBufferPositive(SceCtrlData*,int);
uintptr_t sctrlHENFindFunction(const char*,const char*,unsigned);
void sctrlHENPatchSyscall(void*,void*);
typedef struct SceModule { struct SceModule *next; unsigned short attribute; unsigned char version[2]; char modname[27]; } SceModule;
typedef int (*STMOD_HANDLER)(SceModule *);
STMOD_HANDLER sctrlHENSetStartModuleHandler(STMOD_HANDLER);
unsigned sceKernelPartitionMaxFreeMemSize(int);
#define PSP_MEMORY_PARTITION_USER 2
#define PSP_SMEM_Low 0
SceUID sceKernelAllocPartitionMemory(int, const char *, int, unsigned, void *);
void *sceKernelGetBlockHeadAddr(SceUID);
typedef unsigned int SceSize;
typedef int (*SceKernelThreadEntry)(SceSize, void *);
int sceKernelCpuSuspendIntr(void);
void sceKernelCpuResumeIntr(int);
SceUID sceKernelCreateThread(const char *, SceKernelThreadEntry, int, int, unsigned, void *);
int sceKernelStartThread(SceUID, SceSize, void *);
int sceKernelDelayThread(unsigned);
int sceGuStart(int,void*);
void sceGuCopyImage(int,int,int,int,int,int,void*,int,int,int,void*);
int sceGuFinish(void);
#endif
