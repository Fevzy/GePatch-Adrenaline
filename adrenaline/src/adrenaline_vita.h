#ifndef ADRENALINE_VITA_H
#define ADRENALINE_VITA_H

// NOTE: these definitions already exist in PSPSDK, but not on VITASDK
// since they are needed by Vita-side, but not anymore by PSP-side, I've moved them to this file

#define REBOOTEX_CONFIG 0x88FB0000
#define REBOOTEX_TEXT 0x88FC0000

#define ADRENALINE_TITLEID "PSPEMUCFW"

#define SCE_PSPEMU_FLASH0_PACKAGE_SIZE 0x920000
#define SCE_PSPEMU_EXTRA_MEMORY 0x8B000000
#define SCE_PSPEMU_FLASH0_RAMFS 0x8B000000
#define SCE_PSPEMU_SCRATCHPAD 0x8BD00000
#define SCE_PSPEMU_VRAM 0x8BE00000

#define SCE_PSPEMU_FRAMEBUFFER 0x74000000
#define SCE_PSPEMU_FRAMEBUFFER_SIZE 0x88000

#define PSP_EXTRA_RAM 0x8A000000
#define PSP_EXTRA_RAM_SIZE (32 * 1024 *1024)
#define PSP_RAM_SIZE (64 * 1024 * 1024)

#define PSP_SCREEN_WIDTH 480
#define PSP_SCREEN_HEIGHT 272
#define PSP_SCREEN_LINE 512

#define EPI_CONFIG_ADDR 0x08800010

#define ADRENALINE_SIZE 0x2000
#define ADRENALINE_ADDRESS 0xABCDE000

#define DRAW_NATIVE 0xABCDEF00
#define NATIVE_FRAMEBUFFER 0x0A400000
/* GePatch's second display buffer and the word naming the one done (0 or 1,
   written by the PSP's GE right after each copy), with NATIVE_VIDEO_TWO_BUFFERS */
#define NATIVE_FRAMEBUFFER2 0x0A500000
#define NATIVE_FRONT 0x0A5FF000
#define NATIVE_VIDEO_TWO_BUFFERS 0x20
/* GePatch: the frame in NATIVE_FRAMEBUFFER (written by the PSP side) */
#define NATIVE_FRAME_SIZE 0xABCDEF24   /* width | height << 16 */
#define NATIVE_FRAME_FORMAT 0xABCDEF28 /* 0 565, 1 5551, 2 4444, 3 8888; | NATIVE_FRAME_SMOOTH */
#define NATIVE_FRAME_SMOOTH 0x10       /* a 480x272 frame (a movie) to scale smoothly */
#define NATIVE_FRAME_COUNT 0xABCDEF2C  /* frames GePatch has shown (it adds one each) */
#define NATIVE_PARSE_US 0xABCDEF30     /* (old GePatch: its parse time; no FPS counter now) */
#define NATIVE_WAIT_US 0xABCDEF34
#define NATIVE_COMMANDS 0xABCDEF38
#define NATIVE_PARSES 0xABCDEF3C
#define NATIVE_FRAME_READY 0xABCDEF30  /* GePatch: frames it left in PSP VRAM for us to copy (NATIVE_VIDEO bit 3) */
#define NATIVE_FRAME_TAKEN 0xABCDEF80  /* ours: the last of them copied (a cache line of its own) */
/* GePatch: Adrenaline's settings for it (written by the Vita side, own cache line) */
#define NATIVE_MODE 0xABCDEF40         /* Rendering Resolution: 0 original, 1 720x408, 2 960x544 */
#define NATIVE_VIDEO 0xABCDEF44        /* bit 1 Native Video (experimental) on, bit 2 smooth, bit 0 a movie decodes */

#define CONVERT_ADDRESS(addr) (0x68000000 + (addr & 0x0FFFFFFF))

enum SaveStateModes {
	SAVESTATE_MODE_NONE,
	SAVESTATE_MODE_SAVE,
	SAVESTATE_MODE_LOAD,
};

enum AdrenalinePspCommands {
	ADRENALINE_PSP_CMD_NONE,
	ADRENALINE_PSP_CMD_REINSERT_MS,
	ADRENALINE_PSP_CMD_SAVESTATE,
	ADRENALINE_PSP_CMD_LOADSTATE,
	ADRENALINE_PSP_CMD_PAUSE_WORLD,
	ADRENALINE_PSP_CMD_RESUME_WORLD,
	ADRENALINE_PSP_CMD_REINSERT_EF,
};

enum AdrenalineVitaCommands {
	ADRENALINE_VITA_CMD_NONE,
	ADRENALINE_VITA_CMD_SAVESTATE,
	ADRENALINE_VITA_CMD_LOADSTATE,
	ADRENALINE_VITA_CMD_GET_USB_STATE,
	ADRENALINE_VITA_CMD_START_USB,
	ADRENALINE_VITA_CMD_STOP_USB,
	ADRENALINE_VITA_CMD_PAUSE_POPS,
	ADRENALINE_VITA_CMD_RESUME_POPS,
	ADRENALINE_VITA_CMD_POWER_SHUTDOWN,
	ADRENALINE_VITA_CMD_POWER_REBOOT,
	ADRENALINE_VITA_CMD_PRINT,
	ADRENALINE_VITA_CMD_UPDATE,
	ADRENALINE_VITA_CMD_APP_STARTED,
	ADRENALINE_VITA_CMD_POWER_TICK,
	ADRENALINE_VITA_CMD_IS_EF_ENABLED,
	ADRENALINE_VITA_CMD_EF_DEVINFO,
};

enum AdrenalineVitaResponse {
	ADRENALINE_VITA_RESPONSE_NONE,
	ADRENALINE_VITA_RESPONSE_SAVED,
	ADRENALINE_VITA_RESPONSE_LOADED,
};

typedef struct {
	int savestate_mode;
	int num;
	unsigned int sp;
	unsigned int ra;

	int pops_mode;
	int draw_psp_screen_in_pops;
	char title[128];
	char titleid[12];
	char filename[256];

	int psp_cmd;
	int vita_cmd;
	int psp_response;
	int vita_response;

	char printbuf[1024];
	int app_type;
} SceAdrenaline;

enum SEUmdModes
{
    MODE_UMD = 0,
    MODE_OE_LEGACY, // not available anymore, will default to inferno
    MODE_MARCH33,
    MODE_NP9660,
    MODE_INFERNO,
    MODE_ME,
    MODE_VSHUMD,
    MODE_UPDATERUMD,
    MODE_RECOVERY,
};

/**
 * Application types of an executable.
 */
enum SceApplicationType {
    /** The application is a VSH application (i.e. VSH modules). */
    SCE_APP_TYPE_VSH        = 0x100,
    /** The application is an updater. */
    SCE_APP_TYPE_UPDATER    = 0x110,
    /** The application is a PSP game. */
    SCE_APP_TYPE_GAME       = 0x200,
    /** The application is a Playstation One game. */
    SCE_APP_TYPE_POPS       = 0x300,
    /** The application is a PSP application (i.e. Skype). */
    SCE_APP_TYPE_APP        = 0x400,
};


enum KermitModes {
	KERMIT_MODE_NONE, // 0x0
	KERMIT_MODE_UNK_1, // 0x1
	KERMIT_MODE_UNK_2, // 0x2
	KERMIT_MODE_MSFS, // 0x3
	KERMIT_MODE_FLASHFS, // 0x4
	KERMIT_MODE_AUDIOOUT, // 0x5
	KERMIT_MODE_ME, // 0x6
	KERMIT_MODE_LOWIO, // 0x7
	KERMIT_MODE_POCS_USBPSPCM, // 0x8
	KERMIT_MODE_PERIPHERAL, // 0x9
	KERMIT_MODE_WLAN, // 0xa
	KERMIT_MODE_AUDIOIN, // 0xb
	KERMIT_MODE_USB, // 0xc
	KERMIT_MODE_UTILITY, // 0xd
	KERMIT_MODE_EXTRA_1, // 0x0
	KERMIT_MODE_EXTRA_2, // 0x0
};

enum KermitVirtualInterrupts {
	KERMIT_VIRTUAL_INTR_NONE,
	KERMIT_VIRTUAL_INTR_AUDIO_CH1,
	KERMIT_VIRTUAL_INTR_AUDIO_CH2,
	KERMIT_VIRTUAL_INTR_AUDIO_CH3,
	KERMIT_VIRTUAL_INTR_ME_DMA_CH1,
	KERMIT_VIRTUAL_INTR_ME_DMA_CH2,
	KERMIT_VIRTUAL_INTR_ME_DMA_CH3,
	KERMIT_VIRTUAL_INTR_WLAN_CH1,
	KERMIT_VIRTUAL_INTR_WLAN_CH2,
	KERMIT_VIRTUAL_INTR_IMPOSE_CH1,
	KERMIT_VIRTUAL_INTR_POWER_CH1,
	KERMIT_VIRTUAL_INTR_CAMERA_CH1,	// <- used after settings
	KERMIT_VIRTUAL_INTR_USBGPS_CH1,
	KERMIT_VIRTUAL_INTR_USBPSPCM_CH1,
};

enum KermitArgumentModes {
  KERMIT_INPUT_MODE = 0x1,
  KERMIT_OUTPUT_MODE = 0x2,
};

typedef struct {
	uint32_t cmd; //0x0
	SceUID sema_id; //0x4
	uint64_t *response; //0x8
	uint32_t padding; //0xC
	uint64_t args[14]; // 0x10
} SceKermitRequest; //0x80

// 0xBFC00800
typedef struct {
	uint32_t cmd; //0x00
	SceKermitRequest *request; //0x04
} SceKermitCommand; //0x8

// 0xBFC00840
typedef struct {
	uint64_t result; //0x0
	SceUID sema_id; //0x8
	int32_t unk_C; //0xC
	uint64_t *response; //0x10
	uint64_t unk_1C; //0x1C
} SceKermitResponse; //0x24 or 0x30????

// 0xBFC008C0
typedef struct {
	int32_t unk_0; //0x0
	int32_t unk_4; //0x4
} SceKermitInterrupt; //0x8

typedef struct {
	SceSize size;
	char shortFileName[13];
	char __padding__[3];
	char longFileName[1024];
} SceFatMsDirent;

typedef struct {
	unsigned int max_clusters;
	unsigned int free_clusters;
	unsigned int max_sectors;
	unsigned int sector_size;
	unsigned int sector_count;
} ScePspemuIoDevInfo;

#endif