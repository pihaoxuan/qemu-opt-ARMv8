/*
 *  x86 misc helpers
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/cputlb.h"
#include "helper-tcg.h"


/* MXCSR bits that affect FMA computation results */
#define MXCSR_FP_NONDEFAULT  0xE040U   /* RC[14:13], FTZ[15], DAZ[6] */

/*
 * NOTE: the translator must set DisasContext.cc_op to CC_OP_EFLAGS
 * after generating a call to a helper that uses this.
 */
void cpu_load_eflags(CPUX86State *env, int eflags, int update_mask)
{
    CC_SRC = eflags & (CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C);
    CC_OP = CC_OP_EFLAGS;
    env->df = 1 - (2 * ((eflags >> 10) & 1));
    env->eflags = (env->eflags & ~update_mask) |
        (eflags & update_mask) | 0x2;
}

void helper_into(CPUX86State *env, int next_eip_addend)
{
    int eflags;

    eflags = cpu_cc_compute_all(env);
    if (eflags & CC_O) {
        raise_interrupt(env, EXCP04_INTO, next_eip_addend);
    }
}

void helper_cpuid(CPUX86State *env)
{
    uint32_t eax, ebx, ecx, edx;

    cpu_svm_check_intercept_param(env, SVM_EXIT_CPUID, 0, GETPC());

    cpu_x86_cpuid(env, (uint32_t)env->regs[R_EAX], (uint32_t)env->regs[R_ECX],
                  &eax, &ebx, &ecx, &edx);
    env->regs[R_EAX] = eax;
    env->regs[R_EBX] = ebx;
    env->regs[R_ECX] = ecx;
    env->regs[R_EDX] = edx;
}

void helper_rdtsc(CPUX86State *env)
{
    uint64_t val;

    if ((env->cr[4] & CR4_TSD_MASK) && ((env->hflags & HF_CPL_MASK) != 0)) {
        raise_exception_ra(env, EXCP0D_GPF, GETPC());
    }
    cpu_svm_check_intercept_param(env, SVM_EXIT_RDTSC, 0, GETPC());

    val = cpu_get_tsc(env) + env->tsc_offset;
    env->regs[R_EAX] = (uint32_t)(val);
    env->regs[R_EDX] = (uint32_t)(val >> 32);
}

G_NORETURN void helper_rdpmc(CPUX86State *env)
{
    if (((env->cr[4] & CR4_PCE_MASK) == 0 ) &&
        ((env->hflags & HF_CPL_MASK) != 0)) {
        raise_exception_ra(env, EXCP0D_GPF, GETPC());
    }
    cpu_svm_check_intercept_param(env, SVM_EXIT_RDPMC, 0, GETPC());

    /* currently unimplemented */
    qemu_log_mask(LOG_UNIMP, "x86: unimplemented rdpmc\n");
    raise_exception_err(env, EXCP06_ILLOP, 0);
}

G_NORETURN void helper_pause(CPUX86State *env)
{
    CPUState *cs = env_cpu(env);

    /* Do gen_eob() tasks before going back to the main loop.  */
    do_end_instruction(env);
    helper_rechecking_single_step(env);

    /* Just let another CPU run.  */
    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cs);
}

uint64_t helper_rdpkru(CPUX86State *env, uint32_t ecx)
{
    if ((env->cr[4] & CR4_PKE_MASK) == 0) {
        raise_exception_err_ra(env, EXCP06_ILLOP, 0, GETPC());
    }
    if (ecx != 0) {
        raise_exception_err_ra(env, EXCP0D_GPF, 0, GETPC());
    }

    return env->pkru;
}

void helper_wrpkru(CPUX86State *env, uint32_t ecx, uint64_t val)
{
    CPUState *cs = env_cpu(env);

    if ((env->cr[4] & CR4_PKE_MASK) == 0) {
        raise_exception_err_ra(env, EXCP06_ILLOP, 0, GETPC());
    }
    if (ecx != 0 || (val & 0xFFFFFFFF00000000ull)) {
        raise_exception_err_ra(env, EXCP0D_GPF, 0, GETPC());
    }

    env->pkru = val;
    tlb_flush(cs);
}

target_ulong HELPER(rdpid)(CPUX86State *env)
{
#if !defined CONFIG_USER_ONLY
    return env->tsc_aux;
#elif defined CONFIG_LINUX && defined CONFIG_GETCPU
    unsigned cpu, node;
    getcpu(&cpu, &node);
    return (node << 12) | (cpu & 0xfff);
#elif defined CONFIG_SCHED_GETCPU
    return sched_getcpu();
#else
    return 0;
#endif
}


#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "accel/tcg/cpu-ldst.h"
#include <math.h>

//void helper_custom_fast_fma(CPUX86State *env, uint32_t dest, uint32_t src1, uint32_t src2, target_ulong vaddr, uint32_t flags) {
//    // 获取宿主机的指令返回地址，极速访问内存发生缺页时，能精准恢复虚拟机上下文
//    uintptr_t ra = GETPC(); 
//    
//    // 解析标志位
//    uint32_t oprsz = flags & 0xFFFF;
//    uint32_t opcode = (flags >> 16) & 0xFF;
//    uint32_t w_bit = (flags >> 24) & 1;
//    
//    int element_count = oprsz / ((w_bit == 1) ? 8 : 4);
//
//    if (w_bit == 1) { // 64位 double (pd)
//        double * __restrict dest_ptr = (double *)&env->xmm_regs[dest];
//        double * __restrict src1_ptr = (double *)&env->xmm_regs[src1];
//        double * __restrict src2_ptr;
//        double mem_src2_pd[4] = {0};
//
//        if (src2 != 0xFFFFFFFF) {
//            src2_ptr = (double *)&env->xmm_regs[src2];
//        } else {
//            for (int i = 0; i < element_count; i++) {
//                union { uint64_t i; double d; } conv;
//                conv.i = cpu_ldq_data_ra(env, vaddr + i * 8, ra);
//                mem_src2_pd[i] = conv.d;
//            }
//            src2_ptr = mem_src2_pd;
//        }
//
//        for (int i = 0; i < element_count; i++) {
//            if (opcode == 0xB8) dest_ptr[i] = (src1_ptr[i] * src2_ptr[i]) + dest_ptr[i];
//            else if (opcode == 0xA8) dest_ptr[i] = (dest_ptr[i] * src1_ptr[i]) + src2_ptr[i];
//            else if (opcode == 0x98) dest_ptr[i] = (dest_ptr[i] * src2_ptr[i]) + src1_ptr[i];
//        }
//    } else { // 32位 float (ps)
//        float * __restrict dest_ptr = (float *)&env->xmm_regs[dest];
//        float * __restrict src1_ptr = (float *)&env->xmm_regs[src1];
//        float * __restrict src2_ptr;
//        float mem_src2_ps[8] = {0};
//
//        if (src2 != 0xFFFFFFFF) {
//            src2_ptr = (float *)&env->xmm_regs[src2];
//        } else {
//            for (int i = 0; i < element_count; i++) {
//                union { uint32_t i; float f; } conv;
//                conv.i = cpu_ldl_data_ra(env, vaddr + i * 4, ra);
//                mem_src2_ps[i] = conv.f;
//            }
//            src2_ptr = mem_src2_ps;
//        }
//	
//#pragma GCC ivdep
//        for (int i = 0; i < element_count; i++) {
//            if (opcode == 0xB8) dest_ptr[i] = (src1_ptr[i] * src2_ptr[i]) + dest_ptr[i];
//            else if (opcode == 0xA8) dest_ptr[i] = (dest_ptr[i] * src1_ptr[i]) + src2_ptr[i];
//            else if (opcode == 0x98) dest_ptr[i] = (dest_ptr[i] * src2_ptr[i]) + src1_ptr[i];
//        }
//    }
//
//    // 清除 YMM 高 128 位
//    if (oprsz == 16) {
//        env->xmm_regs[dest].ZMM_Q(2) = 0;
//        env->xmm_regs[dest].ZMM_Q(3) = 0;
//    }
//}
//
//
//
//
//
//uint64_t helper_custom_fma_chunk(uint64_t d, uint64_t a, uint64_t b, uint32_t flags) {
//    uint32_t opcode = flags & 0xFF;
//    uint32_t w_bit = (flags >> 8) & 1;
//    uint32_t chunk_idx = (flags >> 16) & 0xFF;
//
//    bool is_scalar = (opcode == 0x99 || opcode == 0xA9 || opcode == 0xB9);
//
//    if (w_bit == 1) { 
//        // 64位数据包含 1 个 double
//        union { uint64_t i; double f; } vd, va, vb, vres;
//        vd.i = d; va.i = a; vb.i = b;
//        
//	if(is_scalar && chunk_idx > 0){
//	    vres.f = vd.f;
//	}else{
//            if (opcode == 0xB8) vres.f = fma(va.f, vb.f, vd.f);
//            else if (opcode == 0xA8) vres.f = fma(vd.f, va.f, vb.f);
//            else if (opcode == 0x98) vres.f = fma(vd.f, vb.f, va.f);
//            else vres.f = vd.f;
//	}
//        return vres.i;
//    } else { 
//        // 64位数据包含 2 个 float
//        union { uint64_t i; float f[2]; } vd, va, vb, vres;
//        vd.i = d; va.i = a; vb.i = b;
//        
//        for (int j = 0; j < 2; j++) {
//	    if(is_scalar && (chunk_idx > 0 || j > 0)){
//		vres.f[j] = vd.f[j];
//	    }else{
//                if (opcode == 0xB8) vres.f[j] = fmaf(va.f[j], vb.f[j], vd.f[j]);
//                else if (opcode == 0xA8) vres.f[j] = fmaf(vd.f[j], va.f[j], vb.f[j]);
//                else if (opcode == 0x98) vres.f[j] = fmaf(vd.f[j], vb.f[j], va.f[j]);
//                else vres.f[j] = vd.f[j];
//	    }
//        }
//        
//        return vres.i;
//    }
//}


#include <arm_neon.h>
#include <string.h>
#include "fpu/softfloat.h"
// #include "exec/cpu_ldst.h" // 需要用到内存访问接口和 GETPC()

/* NEON vfmaq_* 参数顺序为 acc + x * y。 */
static inline float32x4_t fma_ps(uint32_t opcode, float32x4_t a, float32x4_t b, float32x4_t d)
{
    switch (opcode) {
    case 0xB8: return vfmaq_f32(d, a, b); /* VFMADD231: a*b+d */
    case 0xA8: return vfmaq_f32(b, d, a); /* VFMADD213: d*a+b */
    case 0x98: return vfmaq_f32(a, d, b); /* VFMADD132: d*b+a */
    default:   return d;
    }
}

static inline float64x2_t fma_pd(uint32_t opcode, float64x2_t a, float64x2_t b, float64x2_t d)
{
    switch (opcode) {
    case 0xB8: return vfmaq_f64(d, a, b);
    case 0xA8: return vfmaq_f64(b, d, a);
    case 0x98: return vfmaq_f64(a, d, b);
    default:   return d;
    }
}

/* VEX 标量 FMA 的非低 lane 从 src1（此处为 a）复制。 */
static inline float32x4_t merge_scalar_ps(float32x4_t result, float32x4_t a)
{
    result = vsetq_lane_f32(vgetq_lane_f32(a, 1), result, 1);
    result = vsetq_lane_f32(vgetq_lane_f32(a, 2), result, 2);
    return vsetq_lane_f32(vgetq_lane_f32(a, 3), result, 3);
}

static inline float64x2_t merge_scalar_pd(float64x2_t result, float64x2_t a)
{
    return vsetq_lane_f64(vgetq_lane_f64(a, 1), result, 1);
}

/* 在一个或两个 128 位块上计算，覆盖 XMM 与 YMM。 */
static void custom_fma_apply(void *vd, const void *va, const void *vb, bool is_double, bool is_256, bool is_scalar, uint32_t opcode)
{
    uint8_t *pd = (uint8_t *)vd;
    const uint8_t *pa = (const uint8_t *)va;
    const uint8_t *pb = (const uint8_t *)vb;
    unsigned blocks = is_256 ? 2 : 1;

    for (unsigned i = 0; i < blocks; ++i) {
        uint8_t *d_ptr = pd + i * 16;
        const uint8_t *a_ptr = pa + i * 16;
        const uint8_t *b_ptr = pb + i * 16;

        if (is_double) {
            float64x2_t d = vld1q_f64((const float64_t *)d_ptr);
            float64x2_t a = vld1q_f64((const float64_t *)a_ptr);
            float64x2_t b = vld1q_f64((const float64_t *)b_ptr);
            float64x2_t result = fma_pd(opcode, a, b, d);
            if (is_scalar) {
                result = merge_scalar_pd(result, a);
            }
            vst1q_f64((float64_t *)d_ptr, result);
        } else {
            float32x4_t d = vld1q_f32((const float32_t *)d_ptr);
            float32x4_t a = vld1q_f32((const float32_t *)a_ptr);
            float32x4_t b = vld1q_f32((const float32_t *)b_ptr);
            float32x4_t result = fma_ps(opcode, a, b, d);
            if (is_scalar) {
                result = merge_scalar_ps(result, a);
            }
            vst1q_f32((float32_t *)d_ptr, result);
        }
    }
}

/* 标量 memory operand 只读 4/8 字节；packed operand 读 16/32 字节。 */
static void
load_guest_fma_operand(CPUX86State *env, target_ulong vaddr, bool is_double, bool is_256, bool is_scalar, uintptr_t ra, uint8_t *buf)
{
    if (is_scalar) {
        if (is_double) {
            uint64_t value = cpu_ldq_data_ra(env, vaddr, ra);
            memcpy(buf, &value, sizeof(value));
        } else {
            uint32_t value = cpu_ldl_data_ra(env, vaddr, ra);
            memcpy(buf, &value, sizeof(value));
        }
        return;
    }

    unsigned bytes = is_256 ? 32 : 16;
    for (unsigned offset = 0; offset < bytes; offset += 8) {
        uint64_t value = cpu_ldq_data_ra(env, vaddr + offset, ra);
        memcpy(buf + offset, &value, sizeof(value));
    }
}
// ====================================================================
// 处理纯寄存器操作数的 Helper
// ====================================================================
void helper_custom_fma_vector_reg(CPUX86State *env, uint32_t dofs, uint32_t aofs, uint32_t bofs, uint32_t flags) {
    /* --- MXCSR 守卫：非默认 FP 环境 → 回退至软浮点 --- */
    if (unlikely(env->mxcsr & MXCSR_FP_NONDEFAULT)) {
        uint32_t opcode = flags & 0xFF;
        uint32_t w_bit  = (flags >> 8) & 1;
        uint32_t l_bit  = (flags >> 9) & 1;
        ZMMReg *dreg = (ZMMReg *)((uint8_t *)env + dofs);
        ZMMReg *areg = (ZMMReg *)((uint8_t *)env + aofs);
        ZMMReg *breg = (ZMMReg *)((uint8_t *)env + bofs);
        int n = l_bit ? (w_bit ? 4 : 8) : (w_bit ? 2 : 4);
        for (int i = 0; i < n; i++) {
            if (w_bit) {
                float64 d = dreg->ZMM_Q(i), a = areg->ZMM_Q(i), b = breg->ZMM_Q(i), r;
                if      (opcode == 0xB8) r = float64_muladd(a, b, d, 0, &env->sse_status);
                else if (opcode == 0xA8) r = float64_muladd(d, a, b, 0, &env->sse_status);
                else if (opcode == 0x98) r = float64_muladd(d, b, a, 0, &env->sse_status);
                else r = d;
                dreg->ZMM_Q(i) = r;
            } else {
                float32 d = dreg->ZMM_L(i), a = areg->ZMM_L(i), b = breg->ZMM_L(i), r;
                if      (opcode == 0xB8) r = float32_muladd(a, b, d, 0, &env->sse_status);
                else if (opcode == 0xA8) r = float32_muladd(d, a, b, 0, &env->sse_status);
                else if (opcode == 0x98) r = float32_muladd(d, b, a, 0, &env->sse_status);
                else r = d;
                dreg->ZMM_L(i) = r;
            }
        }
        return;
    }
    /* --- 守卫结束 --- */
    uint32_t opcode = flags & 0xFF;
    bool is_double = ((flags >> 8) & 1) != 0;
    bool is_256 = ((flags >> 9) & 1) != 0;
    bool is_scalar = (opcode == 0x99 || opcode == 0xA9 || opcode == 0xB9);
    uint8_t *env_base = (uint8_t *)env;

    custom_fma_apply(env_base + dofs, env_base + aofs, env_base + bofs, is_double, is_256, is_scalar, opcode);
}

// ====================================================================
// 处理带内存操作数的 Helper
// ====================================================================
void helper_custom_fma_vector_mem(CPUX86State *env, uint32_t dofs, uint32_t aofs, target_ulong vaddr, uint32_t flags) {
    uintptr_t ra = GETPC(); // 捕获异常返回地址，用于引发缺页异常时回溯
    uint32_t opcode = flags & 0xFF;
    bool is_double = ((flags >> 8) & 1) != 0;
    bool is_256 = ((flags >> 9) & 1) != 0;
    bool is_scalar = (opcode == 0x99 || opcode == 0xA9 || opcode == 0xB9);

    // 核心安全点：使用 QEMU API 读取 Guest 内存并存放到宿主机的栈对齐数组中
    uint8_t mem_buf[32] __attribute__((aligned(32))) = {0};
    uint8_t *env_base = (uint8_t *)env;
    load_guest_fma_operand(env, vaddr, is_double, is_256, is_scalar, ra, mem_buf);
    custom_fma_apply(env_base + dofs, env_base + aofs, mem_buf, is_double, is_256, is_scalar, opcode);
}
