// Shared scaffolding for the bd_pe_ps_*_clamp posteff shaders: XenosRecomp
// helper library, the posteff SharedConstants descriptor-index layout, and the
// generated entry-point boilerplate. Bindings must match the recompiled
// shaders these replace at link time (b1/b2 space4, bindless heaps t0/s0).

#pragma once

#include "thirdparty/XenosRecomp/XenosRecomp/shader_common.h"

#ifndef __spirv__
// Alpha test is never enabled on posteff quads.
uint g_SpecConstants() { return 0u; }
#endif

#ifdef __spirv__

#define ScreenTexture_Texture2DDescriptorIndex REBLUE_SC_U32(0)
#define ScreenTexture_Texture3DDescriptorIndex REBLUE_SC_U32(64)
#define ScreenTexture_TextureCubeDescriptorIndex REBLUE_SC_U32(128)
#define ScreenTexture_SamplerDescriptorIndex REBLUE_SC_U32(192)
#define s1_Texture2DDescriptorIndex REBLUE_SC_U32(4)
#define s1_Texture3DDescriptorIndex REBLUE_SC_U32(68)
#define s1_TextureCubeDescriptorIndex REBLUE_SC_U32(132)
#define s1_SamplerDescriptorIndex REBLUE_SC_U32(196)
#define s2_Texture2DDescriptorIndex REBLUE_SC_U32(8)
#define s2_Texture3DDescriptorIndex REBLUE_SC_U32(72)
#define s2_TextureCubeDescriptorIndex REBLUE_SC_U32(136)
#define s2_SamplerDescriptorIndex REBLUE_SC_U32(200)
#define s3_Texture2DDescriptorIndex REBLUE_SC_U32(12)
#define s3_Texture3DDescriptorIndex REBLUE_SC_U32(76)
#define s3_TextureCubeDescriptorIndex REBLUE_SC_U32(140)
#define s3_SamplerDescriptorIndex REBLUE_SC_U32(204)
#define s4_Texture2DDescriptorIndex REBLUE_SC_U32(16)
#define s4_Texture3DDescriptorIndex REBLUE_SC_U32(80)
#define s4_TextureCubeDescriptorIndex REBLUE_SC_U32(144)
#define s4_SamplerDescriptorIndex REBLUE_SC_U32(208)
#define s5_Texture2DDescriptorIndex REBLUE_SC_U32(20)
#define s5_Texture3DDescriptorIndex REBLUE_SC_U32(84)
#define s5_TextureCubeDescriptorIndex REBLUE_SC_U32(148)
#define s5_SamplerDescriptorIndex REBLUE_SC_U32(212)
#define s6_Texture2DDescriptorIndex REBLUE_SC_U32(24)
#define s6_Texture3DDescriptorIndex REBLUE_SC_U32(88)
#define s6_TextureCubeDescriptorIndex REBLUE_SC_U32(152)
#define s6_SamplerDescriptorIndex REBLUE_SC_U32(216)
#define s7_Texture2DDescriptorIndex REBLUE_SC_U32(28)
#define s7_Texture3DDescriptorIndex REBLUE_SC_U32(92)
#define s7_TextureCubeDescriptorIndex REBLUE_SC_U32(156)
#define s7_SamplerDescriptorIndex REBLUE_SC_U32(220)
#define s8_Texture2DDescriptorIndex REBLUE_SC_U32(32)
#define s8_Texture3DDescriptorIndex REBLUE_SC_U32(96)
#define s8_TextureCubeDescriptorIndex REBLUE_SC_U32(160)
#define s8_SamplerDescriptorIndex REBLUE_SC_U32(224)
#define s9_Texture2DDescriptorIndex REBLUE_SC_U32(36)
#define s9_Texture3DDescriptorIndex REBLUE_SC_U32(100)
#define s9_TextureCubeDescriptorIndex REBLUE_SC_U32(164)
#define s9_SamplerDescriptorIndex REBLUE_SC_U32(228)
#define s10_Texture2DDescriptorIndex REBLUE_SC_U32(40)
#define s10_Texture3DDescriptorIndex REBLUE_SC_U32(104)
#define s10_TextureCubeDescriptorIndex REBLUE_SC_U32(168)
#define s10_SamplerDescriptorIndex REBLUE_SC_U32(232)
#define s11_Texture2DDescriptorIndex REBLUE_SC_U32(44)
#define s11_Texture3DDescriptorIndex REBLUE_SC_U32(108)
#define s11_TextureCubeDescriptorIndex REBLUE_SC_U32(172)
#define s11_SamplerDescriptorIndex REBLUE_SC_U32(236)
#define s12_Texture2DDescriptorIndex REBLUE_SC_U32(48)
#define s12_Texture3DDescriptorIndex REBLUE_SC_U32(112)
#define s12_TextureCubeDescriptorIndex REBLUE_SC_U32(176)
#define s12_SamplerDescriptorIndex REBLUE_SC_U32(240)
#define s13_Texture2DDescriptorIndex REBLUE_SC_U32(52)
#define s13_Texture3DDescriptorIndex REBLUE_SC_U32(116)
#define s13_TextureCubeDescriptorIndex REBLUE_SC_U32(180)
#define s13_SamplerDescriptorIndex REBLUE_SC_U32(244)
#define s14_Texture2DDescriptorIndex REBLUE_SC_U32(56)
#define s14_Texture3DDescriptorIndex REBLUE_SC_U32(120)
#define s14_TextureCubeDescriptorIndex REBLUE_SC_U32(184)
#define s14_SamplerDescriptorIndex REBLUE_SC_U32(248)
#define s15_Texture2DDescriptorIndex REBLUE_SC_U32(60)
#define s15_Texture3DDescriptorIndex REBLUE_SC_U32(124)
#define s15_TextureCubeDescriptorIndex REBLUE_SC_U32(188)
#define s15_SamplerDescriptorIndex REBLUE_SC_U32(252)

#else

cbuffer SharedConstants : register(b2, space4)
{
	uint ScreenTexture_Texture2DDescriptorIndex : packoffset(c0.x);
	uint ScreenTexture_Texture3DDescriptorIndex : packoffset(c4.x);
	uint ScreenTexture_TextureCubeDescriptorIndex : packoffset(c8.x);
	uint ScreenTexture_SamplerDescriptorIndex : packoffset(c12.x);
	uint s1_Texture2DDescriptorIndex : packoffset(c0.y);
	uint s1_Texture3DDescriptorIndex : packoffset(c4.y);
	uint s1_TextureCubeDescriptorIndex : packoffset(c8.y);
	uint s1_SamplerDescriptorIndex : packoffset(c12.y);
	uint s2_Texture2DDescriptorIndex : packoffset(c0.z);
	uint s2_Texture3DDescriptorIndex : packoffset(c4.z);
	uint s2_TextureCubeDescriptorIndex : packoffset(c8.z);
	uint s2_SamplerDescriptorIndex : packoffset(c12.z);
	uint s3_Texture2DDescriptorIndex : packoffset(c0.w);
	uint s3_Texture3DDescriptorIndex : packoffset(c4.w);
	uint s3_TextureCubeDescriptorIndex : packoffset(c8.w);
	uint s3_SamplerDescriptorIndex : packoffset(c12.w);
	uint s4_Texture2DDescriptorIndex : packoffset(c1.x);
	uint s4_Texture3DDescriptorIndex : packoffset(c5.x);
	uint s4_TextureCubeDescriptorIndex : packoffset(c9.x);
	uint s4_SamplerDescriptorIndex : packoffset(c13.x);
	uint s5_Texture2DDescriptorIndex : packoffset(c1.y);
	uint s5_Texture3DDescriptorIndex : packoffset(c5.y);
	uint s5_TextureCubeDescriptorIndex : packoffset(c9.y);
	uint s5_SamplerDescriptorIndex : packoffset(c13.y);
	uint s6_Texture2DDescriptorIndex : packoffset(c1.z);
	uint s6_Texture3DDescriptorIndex : packoffset(c5.z);
	uint s6_TextureCubeDescriptorIndex : packoffset(c9.z);
	uint s6_SamplerDescriptorIndex : packoffset(c13.z);
	uint s7_Texture2DDescriptorIndex : packoffset(c1.w);
	uint s7_Texture3DDescriptorIndex : packoffset(c5.w);
	uint s7_TextureCubeDescriptorIndex : packoffset(c9.w);
	uint s7_SamplerDescriptorIndex : packoffset(c13.w);
	uint s8_Texture2DDescriptorIndex : packoffset(c2.x);
	uint s8_Texture3DDescriptorIndex : packoffset(c6.x);
	uint s8_TextureCubeDescriptorIndex : packoffset(c10.x);
	uint s8_SamplerDescriptorIndex : packoffset(c14.x);
	uint s9_Texture2DDescriptorIndex : packoffset(c2.y);
	uint s9_Texture3DDescriptorIndex : packoffset(c6.y);
	uint s9_TextureCubeDescriptorIndex : packoffset(c10.y);
	uint s9_SamplerDescriptorIndex : packoffset(c14.y);
	uint s10_Texture2DDescriptorIndex : packoffset(c2.z);
	uint s10_Texture3DDescriptorIndex : packoffset(c6.z);
	uint s10_TextureCubeDescriptorIndex : packoffset(c10.z);
	uint s10_SamplerDescriptorIndex : packoffset(c14.z);
	uint s11_Texture2DDescriptorIndex : packoffset(c2.w);
	uint s11_Texture3DDescriptorIndex : packoffset(c6.w);
	uint s11_TextureCubeDescriptorIndex : packoffset(c10.w);
	uint s11_SamplerDescriptorIndex : packoffset(c14.w);
	uint s12_Texture2DDescriptorIndex : packoffset(c3.x);
	uint s12_Texture3DDescriptorIndex : packoffset(c7.x);
	uint s12_TextureCubeDescriptorIndex : packoffset(c11.x);
	uint s12_SamplerDescriptorIndex : packoffset(c15.x);
	uint s13_Texture2DDescriptorIndex : packoffset(c3.y);
	uint s13_Texture3DDescriptorIndex : packoffset(c7.y);
	uint s13_TextureCubeDescriptorIndex : packoffset(c11.y);
	uint s13_SamplerDescriptorIndex : packoffset(c15.y);
	uint s14_Texture2DDescriptorIndex : packoffset(c3.z);
	uint s14_Texture3DDescriptorIndex : packoffset(c7.z);
	uint s14_TextureCubeDescriptorIndex : packoffset(c11.z);
	uint s14_SamplerDescriptorIndex : packoffset(c15.z);
	uint s15_Texture2DDescriptorIndex : packoffset(c3.w);
	uint s15_Texture3DDescriptorIndex : packoffset(c7.w);
	uint s15_TextureCubeDescriptorIndex : packoffset(c11.w);
	uint s15_SamplerDescriptorIndex : packoffset(c15.w);
	DEFINE_SHARED_CONSTANTS();
};

#endif

#ifdef __spirv__
#define BD_PE_PS_FACE_TYPE bool
#else
#define BD_PE_PS_FACE_TYPE uint
#endif

// XenosRecomp PS signature: full interpolator set, used or not.
#define BD_PE_PS_PARAMS \
	in float4 iPos : SV_Position, \
	in float4 iTexCoord0 : TEXCOORD0, \
	in float4 iTexCoord1 : TEXCOORD1, \
	in float4 iTexCoord2 : TEXCOORD2, \
	in float4 iTexCoord3 : TEXCOORD3, \
	in float4 iTexCoord4 : TEXCOORD4, \
	in float4 iTexCoord5 : TEXCOORD5, \
	in float4 iTexCoord6 : TEXCOORD6, \
	in float4 iTexCoord7 : TEXCOORD7, \
	in float4 iTexCoord8 : TEXCOORD8, \
	in float4 iTexCoord9 : TEXCOORD9, \
	in float4 iTexCoord10 : TEXCOORD10, \
	in float4 iTexCoord11 : TEXCOORD11, \
	in float4 iTexCoord12 : TEXCOORD12, \
	in float4 iTexCoord13 : TEXCOORD13, \
	in float4 iTexCoord14 : TEXCOORD14, \
	in float4 iTexCoord15 : TEXCOORD15, \
	in float4 iColor0 : COLOR0, \
	in float4 iColor1 : COLOR1, \
	in BD_PE_PS_FACE_TYPE iFace : SV_IsFrontFace, \
	out float4 oC0 : SV_Target0

// Xenos GPR file. r0 is seeded from TEXCOORD0 by both bd_pe_ps variants.
#define BD_PE_PS_REGISTERS \
	float4 r0 = iTexCoord0; \
	float4 r1 = 0.0; \
	float4 r2 = 0.0; \
	float4 r3 = 0.0; \
	float4 r4 = 0.0; \
	float4 r5 = 0.0; \
	float4 r6 = 0.0; \
	float4 r7 = 0.0; \
	float4 r8 = 0.0; \
	float4 r9 = 0.0; \
	float4 r10 = 0.0; \
	float4 r11 = 0.0; \
	float4 r12 = 0.0; \
	float4 r13 = 0.0; \
	float4 r14 = 0.0; \
	float4 r15 = 0.0; \
	float4 r16 = 0.0; \
	float4 r17 = 0.0; \
	float4 r18 = 0.0; \
	float4 r19 = 0.0; \
	float4 r20 = 0.0; \
	float4 r21 = 0.0; \
	float4 r22 = 0.0; \
	float4 r23 = 0.0; \
	float4 r24 = 0.0; \
	float4 r25 = 0.0; \
	float4 r26 = 0.0; \
	float4 r27 = 0.0; \
	float4 r28 = 0.0; \
	float4 r29 = 0.0; \
	float4 r30 = 0.0; \
	float4 r31 = 0.0; \
	int a0 = 0; \
	int aL = 0; \
	bool p0 = false; \
	float ps = 0.0; \
	CubeMapData cubeMapData = (CubeMapData)0
