/*
  Adrenaline GE shader baker: compiles the GE renderer's vertex program and
  every fragment variant with libshacccg and writes them to GR_SHADER_PACK
  (see gershaders.h). Run it once; Adrenaline loads the pack.
*/

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/shacccg.h>
#include <stdlib.h>
#include <string.h>

#include "debugScreen.h"
#include "gershaders.h"

#define printf psvDebugScreenPrintf

static SceShaccCgSourceFile source;

static SceShaccCgSourceFile *openSource(const char *name, const SceShaccCgSourceLocation *from,
		const SceShaccCgCompileOptions *options, const char **error) {
	(void)name; (void)from; (void)options; (void)error;
	return &source;
}

static void *compilerAlloc(unsigned int size) { return malloc(size); }

/* The compiled program in memory of its own, or NULL with the messages printed. */
static void *compile(const char *text, int fragment, uint32_t key, uint32_t *size) {
	char m[5][16];
	const char *macros[5] = { m[0], m[1], m[2], m[3], m[4] };
	SceShaccCgCompileOptions options;
	SceShaccCgCallbackList callbacks;
	source.fileName = "<ge>";
	source.text = text;
	source.size = strlen(text);
	sceShaccCgInitializeCompileOptions(&options);
	options.mainSourceFile = source.fileName;
	options.targetProfile = fragment ? SCE_SHACCCG_PROFILE_FP : SCE_SHACCCG_PROFILE_VP;
	options.entryFunctionName = "main";
	if (fragment) {
		grVariantMacros(key, m);
		options.macroDefinitionCount = 5;
		options.macroDefinitions = macros;
	}
	options.optimizationLevel = 3;
	options.useFastmath = 1;
	sceShaccCgInitializeCallbackList(&callbacks, SCE_SHACCCG_TRIVIAL);
	callbacks.openFile = openSource;
	const SceShaccCgCompileOutput *out = sceShaccCgCompileProgram(&options, &callbacks, 0);
	void *program = NULL;
	if (out && out->programData) {
		program = malloc(out->programSize);
		memcpy(program, out->programData, out->programSize);
		*size = out->programSize;
	} else if (out) {
		for (int i = 0; i < out->diagnosticCount; i++) {
			printf("  %s\n", out->diagnostics[i].message);
			sceClibPrintf("[bake] %04x: %s\n", key, out->diagnostics[i].message);
		}
	}
	if (out)
		sceShaccCgDestroyCompileOutput(out);
	return program;
}

static void finish(int seconds) {
	sceKernelDelayThread(seconds * 1000 * 1000);
	sceKernelExitProcess(0);
}

int main(void) {
	static GrShaderPackEntry entries[GR_VARIANT_KEYS + 1];
	static void *programs[GR_VARIANT_KEYS + 1];
	uint32_t count = 0, bytes = 0, failed = 0;

	psvDebugScreenInit();
	printf("Adrenaline GE shader baker\n\n");
	SceUID mod = sceKernelLoadStartModule("ur0:data/libshacccg.suprx", 0, NULL, 0, NULL, NULL);
	if (mod < 0) {
		printf("ur0:data/libshacccg.suprx could not be loaded: %08x\n", mod);
		sceClibPrintf("[bake] libshacccg %08x\n", mod);
		finish(10);
	}
	sceShaccCgSetDefaultAllocator(compilerAlloc, free);

	uint32_t size = 0;
	programs[0] = compile(gr_vertex_source, 0, GR_VERTEX_KEY, &size);
	if (!programs[0]) {
		printf("vertex program failed\n");
		finish(10);
	}
	entries[count].key = GR_VERTEX_KEY;
	entries[count].size = size;
	count++;
	bytes += (size + 15) & ~15u;
	printf("vertex program: %u bytes\n", size);

	for (uint32_t key = 0; key < GR_VARIANT_KEYS; key++) {
		if (!grVariantValid(key))
			continue;
		programs[count] = compile(gr_fragment_source, 1, key, &size);
		if (!programs[count]) {
			failed++;
			printf("variant %03x failed\n", key);
			continue;
		}
		entries[count].key = key;
		entries[count].size = size;
		count++;
		bytes += (size + 15) & ~15u;
		if (count % 20 == 0)
			printf("%u programs...\n", count);
	}

	uint32_t offset = (sizeof(GrShaderPackHeader) + count * sizeof(GrShaderPackEntry) + 15) & ~15u;
	for (uint32_t i = 0; i < count; i++) {
		entries[i].offset = offset;
		offset += (entries[i].size + 15) & ~15u;
	}
	GrShaderPackHeader header = { GR_SHADER_MAGIC, GR_SHADER_VERSION, count };
	sceIoMkdir("ux0:data/AdrenalineGE", 0777);
	SceUID fd = sceIoOpen(GR_SHADER_PACK, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0) {
		printf("%s could not be written: %08x\n", GR_SHADER_PACK, fd);
		finish(10);
	}
	static const uint8_t zero[16];
	uint32_t at = sizeof(header) + count * sizeof(GrShaderPackEntry);
	sceIoWrite(fd, &header, sizeof(header));
	sceIoWrite(fd, entries, count * sizeof(GrShaderPackEntry));
	sceIoWrite(fd, zero, ((at + 15) & ~15u) - at);
	for (uint32_t i = 0; i < count; i++) {
		sceIoWrite(fd, programs[i], entries[i].size);
		sceIoWrite(fd, zero, ((entries[i].size + 15) & ~15u) - entries[i].size);
	}
	sceIoClose(fd);
	printf("\n%u programs (%u failed), %u KiB written to\n%s\n\nDone, closing.\n", count, failed, offset >> 10, GR_SHADER_PACK);
	sceClibPrintf("[bake] %u programs, %u failed, %u bytes\n", count, failed, offset);
	finish(5);
	return 0;
}
