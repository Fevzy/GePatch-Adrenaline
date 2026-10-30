#ifndef __GERSHADERS_H__
#define __GERSHADERS_H__

/*
  The GE renderer's shaders. libshacccg cannot be loaded into ScePspemu
  (0x8002D0F3), so they are compiled once by the GE shader baker app and
  stored in GR_SHADER_PACK, which the renderer loads.

  Fragment variant key: bit 0 textured, bits 1-3 texture function (0-4),
  bit 4 texture alpha used, bit 5 colour doubling, bits 6-9 alpha test
  function (0-7, 8 = off). Untextured variants have function, alpha and
  doubling zero.
*/

#include <stdint.h>
#include <stdio.h>

#define GR_SHADER_PACK "ux0:data/AdrenalineGE/shaders.bin"
#define GR_SHADER_MAGIC 0x48535247 /* "GRSH" */
#define GR_SHADER_VERSION 2
#define GR_VARIANT_KEYS 1024
#define GR_VERTEX_KEY 0xFFFF

typedef struct {
	uint32_t magic, version, count;
} GrShaderPackHeader;

typedef struct {
	uint32_t key, offset, size; /* offset from the start of the pack */
} GrShaderPackEntry;

static const char gr_vertex_source[] =
	"void main(float4 aPosition, float2 aTexcoord, float4 aColor, float aFog,\n"
	"  out float4 vPosition : POSITION, out float2 vTexcoord : TEXCOORD0, out float4 vColor : COLOR,\n"
	"  out float vFog : TEXCOORD1)\n"
	"{ vPosition = aPosition; vTexcoord = aTexcoord; vColor = aColor; vFog = aFog; }\n";

static const char gr_fragment_source[] =
	"uniform float4 uEnv;\n"
	"uniform float uAref;\n"
	"uniform float4 uFog;\n"
	"float4 main(float2 vTexcoord : TEXCOORD0, float4 vColor : COLOR, float vFog : TEXCOORD1,\n"
	"  uniform sampler2D uTex : TEXUNIT0) : COLOR\n"
	"{\n"
	"  float4 c = vColor;\n"
	"#if TEX\n"
	"  float4 t = tex2D(uTex, vTexcoord);\n"
	"#if FUNC == 0\n"
	"  c.rgb = c.rgb * t.rgb;\n"
	"  if (RGBA) c.a = c.a * t.a;\n"
	"#elif FUNC == 1\n"
	"  if (RGBA) c.rgb = lerp(c.rgb, t.rgb, t.a); else c.rgb = t.rgb;\n"
	"#elif FUNC == 2\n"
	"  c.rgb = lerp(c.rgb, uEnv.rgb, t.rgb);\n"
	"  if (RGBA) c.a = c.a * t.a;\n"
	"#elif FUNC == 3\n"
	"  c.rgb = t.rgb;\n"
	"  if (RGBA) c.a = t.a;\n"
	"#else\n"
	"  c.rgb = c.rgb + t.rgb;\n"
	"  if (RGBA) c.a = c.a * t.a;\n"
	"#endif\n"
	"  if (DOUBLE) c.rgb = c.rgb * 2.0;\n"
	"#endif\n"
	"  c.rgb = lerp(uFog.rgb, c.rgb, vFog);\n"
	"  float a = floor(c.a * 255.0 + 0.5);\n"
	"#if ATEST == 0\n"
	"  discard;\n"
	"#elif ATEST == 2\n"
	"  if (a != uAref) discard;\n"
	"#elif ATEST == 3\n"
	"  if (a == uAref) discard;\n"
	"#elif ATEST == 4\n"
	"  if (a >= uAref) discard;\n"
	"#elif ATEST == 5\n"
	"  if (a > uAref) discard;\n"
	"#elif ATEST == 6\n"
	"  if (a <= uAref) discard;\n"
	"#elif ATEST == 7\n"
	"  if (a < uAref) discard;\n"
	"#endif\n"
	"  return c;\n"
	"}\n";

static inline uint32_t grVariantKey(uint32_t textured, uint32_t func, uint32_t rgba, uint32_t dbl, uint32_t atest) {
	if (!textured)
		func = rgba = dbl = 0;
	if (func > 4)
		func = 0;
	return (textured ? 1 : 0) | (func << 1) | (rgba << 4) | (dbl << 5) | (atest << 6);
}

/* Whether a key is one grVariantKey can produce. */
static inline int grVariantValid(uint32_t key) {
	uint32_t func = (key >> 1) & 7, atest = (key >> 6) & 15;
	if (key >= GR_VARIANT_KEYS || func > 4 || atest > 8)
		return 0;
	return (key & 1) || (key & 0x3E) == 0;
}

/* The preprocessor definitions of a variant: five strings of 16 bytes. */
static inline void grVariantMacros(uint32_t key, char macros[5][16]) {
	snprintf(macros[0], 16, "TEX=%u", (unsigned)(key & 1));
	snprintf(macros[1], 16, "FUNC=%u", (unsigned)((key >> 1) & 7));
	snprintf(macros[2], 16, "RGBA=%u", (unsigned)((key >> 4) & 1));
	snprintf(macros[3], 16, "DOUBLE=%u", (unsigned)((key >> 5) & 1));
	snprintf(macros[4], 16, "ATEST=%u", (unsigned)((key >> 6) & 15));
}

#endif
