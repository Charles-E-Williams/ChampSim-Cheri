/*
 * QEMU RISC-V Disassembler
 *
 * Copyright (c) 2016-2017 Michael Clark <michaeljclark@mac.com>
 * Copyright (c) 2017-2018 SiFive, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */
 /* types */


#ifndef DISAS_HH
#define DISAS_HH 


#include <cstdint>
#include <iostream>
 
 #define RISCV_DIS_FLAG_CHERI 1
 #define RISCV_DIS_FLAG_CAPMODE 2
 typedef uint64_t rv_inst;
 typedef uint16_t rv_opcode;
  
  /* enums */
  
  typedef enum {
      rv32,
      rv64,
      rv128
  } rv_isa;
  
  typedef enum {
      rv_rm_rne = 0,
      rv_rm_rtz = 1,
      rv_rm_rdn = 2,
      rv_rm_rup = 3,
      rv_rm_rmm = 4,
      rv_rm_dyn = 7,
  } rv_rm;
  
  typedef enum {
      rv_fence_i = 8,
      rv_fence_o = 4,
      rv_fence_r = 2,
      rv_fence_w = 1,
  } rv_fence;
  
  typedef enum {
      rv_ireg_zero,
      rv_ireg_ra,
      rv_ireg_sp,
      rv_ireg_gp,
      rv_ireg_tp,
      rv_ireg_t0,
      rv_ireg_t1,
      rv_ireg_t2,
      rv_ireg_s0,
      rv_ireg_s1,
      rv_ireg_a0,
      rv_ireg_a1,
      rv_ireg_a2,
      rv_ireg_a3,
      rv_ireg_a4,
      rv_ireg_a5,
      rv_ireg_a6,
      rv_ireg_a7,
      rv_ireg_s2,
      rv_ireg_s3,
      rv_ireg_s4,
      rv_ireg_s5,
      rv_ireg_s6,
      rv_ireg_s7,
      rv_ireg_s8,
      rv_ireg_s9,
      rv_ireg_s10,
      rv_ireg_s11,
      rv_ireg_t3,
      rv_ireg_t4,
      rv_ireg_t5,
      rv_ireg_t6,
  } rv_ireg;
  
  typedef enum {
      rvc_end,
      rvc_rd_eq_ra,
      rvc_rd_eq_x0,
      rvc_rs1_eq_x0,
      rvc_rs2_eq_x0,
      rvc_rs2_eq_rs1,
      rvc_rs1_eq_ra,
      rvc_imm_eq_zero,
      rvc_imm_eq_n1,
      rvc_imm_eq_p1,
      rvc_csr_eq_0x001,
      rvc_csr_eq_0x002,
      rvc_csr_eq_0x003,
      rvc_csr_eq_0xc00,
      rvc_csr_eq_0xc01,
      rvc_csr_eq_0xc02,
      rvc_csr_eq_0xc80,
      rvc_csr_eq_0xc81,
      rvc_csr_eq_0xc82,
  } rvc_constraint;
  
  typedef enum {
      rv_codec_illegal,
      rv_codec_none,
      rv_codec_u,
      rv_codec_uj,
      rv_codec_i,
      rv_codec_i_sh5,
      rv_codec_i_sh6,
      rv_codec_i_sh7,
      rv_codec_i_csr,
      rv_codec_s,
      rv_codec_sb,
      rv_codec_r,
      rv_codec_r_m,
      rv_codec_r4_m,
      rv_codec_r_a,
      rv_codec_r_l,
      rv_codec_r_f,
      rv_codec_cb,
      rv_codec_cb_imm,
      rv_codec_cb_sh5,
      rv_codec_cb_sh6,
      rv_codec_ci,
      rv_codec_ci_sh5,
      rv_codec_ci_sh6,
      rv_codec_ci_16sp,
      rv_codec_ci_lwsp,
      rv_codec_ci_ldsp,
      rv_codec_ci_lqsp,
      rv_codec_ci_li,
      rv_codec_ci_lui,
      rv_codec_ci_none,
      rv_codec_ciw_4spn,
      rv_codec_cj,
      rv_codec_cj_jal,
      rv_codec_cl_lw,
      rv_codec_cl_ld,
      rv_codec_cl_lq,
      rv_codec_cr,
      rv_codec_cr_mv,
      rv_codec_cr_jalr,
      rv_codec_cr_jr,
      rv_codec_cs,
      rv_codec_cs_sw,
      rv_codec_cs_sd,
      rv_codec_cs_sq,
      rv_codec_css_swsp,
      rv_codec_css_sdsp,
      rv_codec_css_sqsp,
  } rv_codec;
  
  typedef enum {
      rv_op_illegal = 0,
      rv_op_lui = 1,
      rv_op_auipc = 2,
      rv_op_jal = 3,
      rv_op_jalr = 4,
      rv_op_beq = 5,
      rv_op_bne = 6,
      rv_op_blt = 7,
      rv_op_bge = 8,
      rv_op_bltu = 9,
      rv_op_bgeu = 10,
      rv_op_lb = 11,
      rv_op_lh = 12,
      rv_op_lw = 13,
      rv_op_lbu = 14,
      rv_op_lhu = 15,
      rv_op_sb = 16,
      rv_op_sh = 17,
      rv_op_sw = 18,
      rv_op_addi = 19,
      rv_op_slti = 20,
      rv_op_sltiu = 21,
      rv_op_xori = 22,
      rv_op_ori = 23,
      rv_op_andi = 24,
      rv_op_slli = 25,
      rv_op_srli = 26,
      rv_op_srai = 27,
      rv_op_add = 28,
      rv_op_sub = 29,
      rv_op_sll = 30,
      rv_op_slt = 31,
      rv_op_sltu = 32,
      rv_op_xor = 33,
      rv_op_srl = 34,
      rv_op_sra = 35,
      rv_op_or = 36,
      rv_op_and = 37,
      rv_op_fence = 38,
      rv_op_fence_i = 39,
      rv_op_lwu = 40,
      rv_op_ld = 41,
      rv_op_sd = 42,
      rv_op_addiw = 43,
      rv_op_slliw = 44,
      rv_op_srliw = 45,
      rv_op_sraiw = 46,
      rv_op_addw = 47,
      rv_op_subw = 48,
      rv_op_sllw = 49,
      rv_op_srlw = 50,
      rv_op_sraw = 51,
      rv_op_ldu = 52,
      rv_op_lq = 53,
      rv_op_sq = 54,
      rv_op_addid = 55,
      rv_op_sllid = 56,
      rv_op_srlid = 57,
      rv_op_sraid = 58,
      rv_op_addd = 59,
      rv_op_subd = 60,
      rv_op_slld = 61,
      rv_op_srld = 62,
      rv_op_srad = 63,
      rv_op_mul = 64,
      rv_op_mulh = 65,
      rv_op_mulhsu = 66,
      rv_op_mulhu = 67,
      rv_op_div = 68,
      rv_op_divu = 69,
      rv_op_rem = 70,
      rv_op_remu = 71,
      rv_op_mulw = 72,
      rv_op_divw = 73,
      rv_op_divuw = 74,
      rv_op_remw = 75,
      rv_op_remuw = 76,
      rv_op_muld = 77,
      rv_op_divd = 78,
      rv_op_divud = 79,
      rv_op_remd = 80,
      rv_op_remud = 81,
      rv_op_lr_w = 82,
      rv_op_sc_w = 83,
      rv_op_amoswap_w = 84,
      rv_op_amoadd_w = 85,
      rv_op_amoxor_w = 86,
      rv_op_amoor_w = 87,
      rv_op_amoand_w = 88,
      rv_op_amomin_w = 89,
      rv_op_amomax_w = 90,
      rv_op_amominu_w = 91,
      rv_op_amomaxu_w = 92,
      rv_op_lr_d = 93,
      rv_op_sc_d = 94,
      rv_op_amoswap_d = 95,
      rv_op_amoadd_d = 96,
      rv_op_amoxor_d = 97,
      rv_op_amoor_d = 98,
      rv_op_amoand_d = 99,
      rv_op_amomin_d = 100,
      rv_op_amomax_d = 101,
      rv_op_amominu_d = 102,
      rv_op_amomaxu_d = 103,
      rv_op_lr_q = 104,
      rv_op_sc_q = 105,
      rv_op_amoswap_q = 106,
      rv_op_amoadd_q = 107,
      rv_op_amoxor_q = 108,
      rv_op_amoor_q = 109,
      rv_op_amoand_q = 110,
      rv_op_amomin_q = 111,
      rv_op_amomax_q = 112,
      rv_op_amominu_q = 113,
      rv_op_amomaxu_q = 114,
      rv_op_ecall = 115,
      rv_op_ebreak = 116,
      rv_op_uret = 117,
      rv_op_sret = 118,
      rv_op_hret = 119,
      rv_op_mret = 120,
      rv_op_dret = 121,
      rv_op_sfence_vm = 122,
      rv_op_sfence_vma = 123,
      rv_op_wfi = 124,
      rv_op_csrrw = 125,
      rv_op_csrrs = 126,
      rv_op_csrrc = 127,
      rv_op_csrrwi = 128,
      rv_op_csrrsi = 129,
      rv_op_csrrci = 130,
      rv_op_flw = 131,
      rv_op_fsw = 132,
      rv_op_fmadd_s = 133,
      rv_op_fmsub_s = 134,
      rv_op_fnmsub_s = 135,
      rv_op_fnmadd_s = 136,
      rv_op_fadd_s = 137,
      rv_op_fsub_s = 138,
      rv_op_fmul_s = 139,
      rv_op_fdiv_s = 140,
      rv_op_fsgnj_s = 141,
      rv_op_fsgnjn_s = 142,
      rv_op_fsgnjx_s = 143,
      rv_op_fmin_s = 144,
      rv_op_fmax_s = 145,
      rv_op_fsqrt_s = 146,
      rv_op_fle_s = 147,
      rv_op_flt_s = 148,
      rv_op_feq_s = 149,
      rv_op_fcvt_w_s = 150,
      rv_op_fcvt_wu_s = 151,
      rv_op_fcvt_s_w = 152,
      rv_op_fcvt_s_wu = 153,
      rv_op_fmv_x_s = 154,
      rv_op_fclass_s = 155,
      rv_op_fmv_s_x = 156,
      rv_op_fcvt_l_s = 157,
      rv_op_fcvt_lu_s = 158,
      rv_op_fcvt_s_l = 159,
      rv_op_fcvt_s_lu = 160,
      rv_op_fld = 161,
      rv_op_fsd = 162,
      rv_op_fmadd_d = 163,
      rv_op_fmsub_d = 164,
      rv_op_fnmsub_d = 165,
      rv_op_fnmadd_d = 166,
      rv_op_fadd_d = 167,
      rv_op_fsub_d = 168,
      rv_op_fmul_d = 169,
      rv_op_fdiv_d = 170,
      rv_op_fsgnj_d = 171,
      rv_op_fsgnjn_d = 172,
      rv_op_fsgnjx_d = 173,
      rv_op_fmin_d = 174,
      rv_op_fmax_d = 175,
      rv_op_fcvt_s_d = 176,
      rv_op_fcvt_d_s = 177,
      rv_op_fsqrt_d = 178,
      rv_op_fle_d = 179,
      rv_op_flt_d = 180,
      rv_op_feq_d = 181,
      rv_op_fcvt_w_d = 182,
      rv_op_fcvt_wu_d = 183,
      rv_op_fcvt_d_w = 184,
      rv_op_fcvt_d_wu = 185,
      rv_op_fclass_d = 186,
      rv_op_fcvt_l_d = 187,
      rv_op_fcvt_lu_d = 188,
      rv_op_fmv_x_d = 189,
      rv_op_fcvt_d_l = 190,
      rv_op_fcvt_d_lu = 191,
      rv_op_fmv_d_x = 192,
      rv_op_flq = 193,
      rv_op_fsq = 194,
      rv_op_fmadd_q = 195,
      rv_op_fmsub_q = 196,
      rv_op_fnmsub_q = 197,
      rv_op_fnmadd_q = 198,
      rv_op_fadd_q = 199,
      rv_op_fsub_q = 200,
      rv_op_fmul_q = 201,
      rv_op_fdiv_q = 202,
      rv_op_fsgnj_q = 203,
      rv_op_fsgnjn_q = 204,
      rv_op_fsgnjx_q = 205,
      rv_op_fmin_q = 206,
      rv_op_fmax_q = 207,
      rv_op_fcvt_s_q = 208,
      rv_op_fcvt_q_s = 209,
      rv_op_fcvt_d_q = 210,
      rv_op_fcvt_q_d = 211,
      rv_op_fsqrt_q = 212,
      rv_op_fle_q = 213,
      rv_op_flt_q = 214,
      rv_op_feq_q = 215,
      rv_op_fcvt_w_q = 216,
      rv_op_fcvt_wu_q = 217,
      rv_op_fcvt_q_w = 218,
      rv_op_fcvt_q_wu = 219,
      rv_op_fclass_q = 220,
      rv_op_fcvt_l_q = 221,
      rv_op_fcvt_lu_q = 222,
      rv_op_fcvt_q_l = 223,
      rv_op_fcvt_q_lu = 224,
      rv_op_fmv_x_q = 225,
      rv_op_fmv_q_x = 226,
      rv_op_c_addi4spn = 227,
      rv_op_c_fld = 228,
      rv_op_c_lw = 229,
      rv_op_c_flw = 230,
      rv_op_c_fsd = 231,
      rv_op_c_sw = 232,
      rv_op_c_fsw = 233,
      rv_op_c_nop = 234,
      rv_op_c_addi = 235,
      rv_op_c_jal = 236,
      rv_op_c_li = 237,
      rv_op_c_addi16sp = 238,
      rv_op_c_lui = 239,
      rv_op_c_srli = 240,
      rv_op_c_srai = 241,
      rv_op_c_andi = 242,
      rv_op_c_sub = 243,
      rv_op_c_xor = 244,
      rv_op_c_or = 245,
      rv_op_c_and = 246,
      rv_op_c_subw = 247,
      rv_op_c_addw = 248,
      rv_op_c_j = 249,
      rv_op_c_beqz = 250,
      rv_op_c_bnez = 251,
      rv_op_c_slli = 252,
      rv_op_c_fldsp = 253,
      rv_op_c_lwsp = 254,
      rv_op_c_flwsp = 255,
      rv_op_c_jr = 256,
      rv_op_c_mv = 257,
      rv_op_c_ebreak = 258,
      rv_op_c_jalr = 259,
      rv_op_c_add = 260,
      rv_op_c_fsdsp = 261,
      rv_op_c_swsp = 262,
      rv_op_c_fswsp = 263,
      rv_op_c_ld = 264,
      rv_op_c_sd = 265,
      rv_op_c_addiw = 266,
      rv_op_c_ldsp = 267,
      rv_op_c_sdsp = 268,
      rv_op_c_lq = 269,
      rv_op_c_sq = 270,
      rv_op_c_lqsp = 271,
      rv_op_c_sqsp = 272,
      rv_op_nop = 273,
      rv_op_mv = 274,
      rv_op_not = 275,
      rv_op_neg = 276,
      rv_op_negw = 277,
      rv_op_sext_w = 278,
      rv_op_seqz = 279,
      rv_op_snez = 280,
      rv_op_sltz = 281,
      rv_op_sgtz = 282,
      rv_op_fmv_s = 283,
      rv_op_fabs_s = 284,
      rv_op_fneg_s = 285,
      rv_op_fmv_d = 286,
      rv_op_fabs_d = 287,
      rv_op_fneg_d = 288,
      rv_op_fmv_q = 289,
      rv_op_fabs_q = 290,
      rv_op_fneg_q = 291,
      rv_op_beqz = 292,
      rv_op_bnez = 293,
      rv_op_blez = 294,
      rv_op_bgez = 295,
      rv_op_bltz = 296,
      rv_op_bgtz = 297,
      rv_op_ble = 298,
      rv_op_bleu = 299,
      rv_op_bgt = 300,
      rv_op_bgtu = 301,
      rv_op_j = 302,
      rv_op_ret = 303,
      rv_op_jr = 304,
      rv_op_rdcycle = 305,
      rv_op_rdtime = 306,
      rv_op_rdinstret = 307,
      rv_op_rdcycleh = 308,
      rv_op_rdtimeh = 309,
      rv_op_rdinstreth = 310,
      rv_op_frcsr = 311,
      rv_op_frrm = 312,
      rv_op_frflags = 313,
      rv_op_fscsr = 314,
      rv_op_fsrm = 315,
      rv_op_fsflags = 316,
      rv_op_fsrmi = 317,
      rv_op_fsflagsi = 318,
      // CHERI:
      rv_op_auipcc = 319,
      rv_op_lc,
      rv_op_clc,
      rv_op_clb,
      rv_op_clbu,
      rv_op_clh,
      rv_op_clhu,
      rv_op_clw,
      rv_op_clwu,
      rv_op_cld,
  
      rv_op_sc,
      rv_op_csc,
      rv_op_csb,
      rv_op_csh,
      rv_op_csw,
      rv_op_csd,
  
      rv_op_cincoffsetimm,
      rv_op_csetboundsimm,
  
      // Two operand
      rv_op_cgetperm,
      rv_op_cgettype,
      rv_op_cgetbase,
      rv_op_cgetlen,
      rv_op_cgettag,
      rv_op_cgetsealed,
      rv_op_cgetoffset,
      rv_op_cgetflags,
      rv_op_crrl,
      rv_op_cram,
      rv_op_cmove,
      rv_op_ccleartag,
      rv_op_cjalr,
      rv_op_cgethigh,
      rv_op_cgetaddr,
      rv_op_csealentry,
      rv_op_cloadtags,
  
      // Three operand
      rv_op_cspecialrw,
      rv_op_csetbounds,
      rv_op_csetboundsexact,
      rv_op_cseal,
      rv_op_cunseal,
      rv_op_candperm,
      rv_op_csetflags,
      rv_op_csetoffset,
      rv_op_csetaddr,
      rv_op_csethigh,
      rv_op_cincoffset,
      rv_op_ctoptr,
      rv_op_cfromptr,
      rv_op_csub,
      rv_op_cbuildcap,
      rv_op_ccopytype,
      rv_op_ccseal,
      rv_op_ctestsubset,
      rv_op_cseqx,
  
      // FP loads/store
      rv_op_cflw,
      rv_op_cfsw,
      rv_op_cfld,
      rv_op_cfsd,
  
  } rv_op;
  
  /* structures */
  
  typedef struct {
      uint64_t  pc;
      uint64_t  inst;
      int32_t   imm;
      uint16_t  op;
      uint8_t   codec;
      uint8_t   rd;
      uint8_t   rs1;
      uint8_t   rs2;
      uint8_t   rs3;
      uint8_t   rm;
      uint8_t   pred;
      uint8_t   succ;
      uint8_t   aq;
      uint8_t   rl;
  } rv_decode;
  
  typedef struct {
      const int op;
      const rvc_constraint *constraints;
  } rv_comp_data;
  
  enum {
      rvcd_imm_nz = 0x1
  };
  
  typedef struct {
      const char * const name;
      const rv_codec codec;
      const char * const format;
      const rv_comp_data *pseudo;
      const short decomp_rv32;
      const short decomp_rv64;
      const short decomp_rv128;
      const short decomp_data;
  } rv_opcode_data;
  
  /* register names */
  
   const char rv_ireg_name_sym[32][5] = {
      "zero", "ra",   "sp",   "gp",   "tp",   "t0",   "t1",   "t2",
      "s0",   "s1",   "a0",   "a1",   "a2",   "a3",   "a4",   "a5",
      "a6",   "a7",   "s2",   "s3",   "s4",   "s5",   "s6",   "s7",
      "s8",   "s9",   "s10",  "s11",  "t3",   "t4",   "t5",   "t6",
  };
  
   const char rv_creg_name_sym[32][6] = {
      "cnull", "cra",   "csp",   "cgp",   "ctp",   "ct0",   "ct1",   "ct2",
      "cs0",   "cs1",   "ca0",   "ca1",   "ca2",   "ca3",   "ca4",   "ca5",
      "ca6",   "ca7",   "cs2",   "cs3",   "cs4",   "cs5",   "cs6",   "cs7",
      "cs8",   "cs9",   "cs10",  "cs11",  "ct3",   "ct4",   "ct5",   "ct6",
  };
  
   const char rv_freg_name_sym[32][5] = {
      "ft0",  "ft1",  "ft2",  "ft3",  "ft4",  "ft5",  "ft6",  "ft7",
      "fs0",  "fs1",  "fa0",  "fa1",  "fa2",  "fa3",  "fa4",  "fa5",
      "fa6",  "fa7",  "fs2",  "fs3",  "fs4",  "fs5",  "fs6",  "fs7",
      "fs8",  "fs9",  "fs10", "fs11", "ft8",  "ft9",  "ft10", "ft11",
  };
  
  /* instruction formats */
  
  #define rv_fmt_none                   "O\t"
  #define rv_fmt_rs1                    "O\t1"
  #define rv_fmt_offset                 "O\to"
  #define rv_fmt_pred_succ              "O\tp,s"
  #define rv_fmt_rs1_rs2                "O\t1,2"
  #define rv_fmt_rd_imm                 "O\t0,i"
  #define rv_fmt_rd_offset              "O\t0,o"
  #define rv_fmt_cd_offset              "O\tC0,o"
  #define rv_fmt_rd_rs1_rs2             "O\t0,1,2"
  #define rv_fmt_cd_cs1_cs2             "O\tC0,C1,C2"
  #define rv_fmt_cd_cs1_rs2             "O\tC0,C1,2"
  #define rv_fmt_rd_cs1_cs2             "O\t0,C1,C2"
  #define rv_fmt_cd_scr_cs1             "O\tC0,Cs,C1"
  #define rv_fmt_frd_rs1                "O\t3,1"
  #define rv_fmt_rd_frs1                "O\t0,4"
  #define rv_fmt_rd_frs1_frs2           "O\t0,4,5"
  #define rv_fmt_frd_frs1_frs2          "O\t3,4,5"
  #define rv_fmt_rm_frd_frs1            "O\tr,3,4"
  #define rv_fmt_rm_frd_rs1             "O\tr,3,1"
  #define rv_fmt_rm_rd_frs1             "O\tr,0,4"
  #define rv_fmt_rm_frd_frs1_frs2       "O\tr,3,4,5"
  #define rv_fmt_rm_frd_frs1_frs2_frs3  "O\tr,3,4,5,6"
  #define rv_fmt_rd_rs1_imm             "O\t0,1,i"
  #define rv_fmt_cd_cs1_imm             "O\tC0,C1,i"
  #define rv_fmt_rd_rs1_offset          "O\t0,1,i"
  #define rv_fmt_rd_offset_rs1          "O\t0,i(1)"
  #define rv_fmt_rd_offset_cs1          "O\t0,i(C1)"
  #define rv_fmt_cd_offset_rs1          "O\tC0,i(1)"
  #define rv_fmt_cd_offset_cs1          "O\tC0,i(C1)"
  #define rv_fmt_frd_offset_rs1         "O\t3,i(1)"
  #define rv_fmt_frd_offset_cs1         "O\t3,i(C1)"
  #define rv_fmt_rd_csr_rs1             "O\t0,c,1"
  #define rv_fmt_rd_csr_zimm            "O\t0,c,7"
  #define rv_fmt_rs2_offset_rs1         "O\t2,i(1)"
  #define rv_fmt_rs2_offset_cs1         "O\t2,i(C1)"
  #define rv_fmt_cs2_offset_rs1         "O\tC2,i(1)"
  #define rv_fmt_cs2_offset_cs1         "O\tC2,i(C1)"
  #define rv_fmt_frs2_offset_rs1        "O\t5,i(1)"
  #define rv_fmt_frs2_offset_cs1        "O\t5,i(C1)"
  #define rv_fmt_rs1_rs2_offset         "O\t1,2,o"
  #define rv_fmt_rs2_rs1_offset         "O\t2,1,o"
  #define rv_fmt_aqrl_rd_rs2_rs1        "OAR\t0,2,(1)"
  #define rv_fmt_aqrl_rd_rs1            "OAR\t0,(1)"
  #define rv_fmt_rd                     "O\t0"
  #define rv_fmt_rd_zimm                "O\t0,7"
  #define rv_fmt_rd_rs1                 "O\t0,1"
  #define rv_fmt_rd_rs2                 "O\t0,2"
  #define rv_fmt_rd_cs1                 "O\t0,C1"
  #define rv_fmt_cd_cs1                 "O\tC0,C1"
  #define rv_fmt_cd_rs1                 "O\tC0,1"
  #define rv_fmt_rs1_offset             "O\t1,o"
  #define rv_fmt_rs2_offset             "O\t2,o"
  
  /* pseudo-instruction constraints */
  
   const rvc_constraint rvcc_jal[] = { rvc_rd_eq_ra, rvc_end };
   const rvc_constraint rvcc_jalr[] = { rvc_rd_eq_ra, rvc_imm_eq_zero, rvc_end };
   const rvc_constraint rvcc_nop[] = { rvc_rd_eq_x0, rvc_rs1_eq_x0, rvc_imm_eq_zero, rvc_end };
   const rvc_constraint rvcc_mv[] = { rvc_imm_eq_zero, rvc_end };
   const rvc_constraint rvcc_not[] = { rvc_imm_eq_n1, rvc_end };
   const rvc_constraint rvcc_neg[] = { rvc_rs1_eq_x0, rvc_end };
   const rvc_constraint rvcc_negw[] = { rvc_rs1_eq_x0, rvc_end };
   const rvc_constraint rvcc_sext_w[] = { rvc_imm_eq_zero, rvc_end };
   const rvc_constraint rvcc_seqz[] = { rvc_imm_eq_p1, rvc_end };
   const rvc_constraint rvcc_snez[] = { rvc_rs1_eq_x0, rvc_end };
   const rvc_constraint rvcc_sltz[] = { rvc_rs2_eq_x0, rvc_end };
   const rvc_constraint rvcc_sgtz[] = { rvc_rs1_eq_x0, rvc_end };
   const rvc_constraint rvcc_fmv_s[] = { rvc_rs2_eq_rs1, rvc_end };
   const rvc_constraint rvcc_fabs_s[] = { rvc_rs2_eq_rs1, rvc_end };
   const rvc_constraint rvcc_fneg_s[] = { rvc_rs2_eq_rs1, rvc_end };
   const rvc_constraint rvcc_fmv_d[] = { rvc_rs2_eq_rs1, rvc_end };
   const rvc_constraint rvcc_fabs_d[] = { rvc_rs2_eq_rs1, rvc_end };
   const rvc_constraint rvcc_fneg_d[] = { rvc_rs2_eq_rs1, rvc_end };
   const rvc_constraint rvcc_fmv_q[] = { rvc_rs2_eq_rs1, rvc_end };
   const rvc_constraint rvcc_fabs_q[] = { rvc_rs2_eq_rs1, rvc_end };
   const rvc_constraint rvcc_fneg_q[] = { rvc_rs2_eq_rs1, rvc_end };
   const rvc_constraint rvcc_beqz[] = { rvc_rs2_eq_x0, rvc_end };
   const rvc_constraint rvcc_bnez[] = { rvc_rs2_eq_x0, rvc_end };
   const rvc_constraint rvcc_blez[] = { rvc_rs1_eq_x0, rvc_end };
   const rvc_constraint rvcc_bgez[] = { rvc_rs2_eq_x0, rvc_end };
   const rvc_constraint rvcc_bltz[] = { rvc_rs2_eq_x0, rvc_end };
   const rvc_constraint rvcc_bgtz[] = { rvc_rs1_eq_x0, rvc_end };
   const rvc_constraint rvcc_ble[] = { rvc_end };
   const rvc_constraint rvcc_bleu[] = { rvc_end };
   const rvc_constraint rvcc_bgt[] = { rvc_end };
   const rvc_constraint rvcc_bgtu[] = { rvc_end };
   const rvc_constraint rvcc_j[] = { rvc_rd_eq_x0, rvc_end };
   const rvc_constraint rvcc_ret[] = { rvc_rd_eq_x0, rvc_rs1_eq_ra, rvc_end };
   const rvc_constraint rvcc_jr[] = { rvc_rd_eq_x0, rvc_imm_eq_zero, rvc_end };
   const rvc_constraint rvcc_rdcycle[] = { rvc_rs1_eq_x0, rvc_csr_eq_0xc00, rvc_end };
   const rvc_constraint rvcc_rdtime[] = { rvc_rs1_eq_x0, rvc_csr_eq_0xc01, rvc_end };
   const rvc_constraint rvcc_rdinstret[] = { rvc_rs1_eq_x0, rvc_csr_eq_0xc02, rvc_end };
   const rvc_constraint rvcc_rdcycleh[] = { rvc_rs1_eq_x0, rvc_csr_eq_0xc80, rvc_end };
   const rvc_constraint rvcc_rdtimeh[] = { rvc_rs1_eq_x0, rvc_csr_eq_0xc81, rvc_end };
   const rvc_constraint rvcc_rdinstreth[] = { rvc_rs1_eq_x0,
                                                    rvc_csr_eq_0xc82, rvc_end };
   const rvc_constraint rvcc_frcsr[] = { rvc_rs1_eq_x0, rvc_csr_eq_0x003, rvc_end };
   const rvc_constraint rvcc_frrm[] = { rvc_rs1_eq_x0, rvc_csr_eq_0x002, rvc_end };
   const rvc_constraint rvcc_frflags[] = { rvc_rs1_eq_x0, rvc_csr_eq_0x001, rvc_end };
   const rvc_constraint rvcc_fscsr[] = { rvc_csr_eq_0x003, rvc_end };
   const rvc_constraint rvcc_fsrm[] = { rvc_csr_eq_0x002, rvc_end };
   const rvc_constraint rvcc_fsflags[] = { rvc_csr_eq_0x001, rvc_end };
   const rvc_constraint rvcc_fsrmi[] = { rvc_csr_eq_0x002, rvc_end };
   const rvc_constraint rvcc_fsflagsi[] = { rvc_csr_eq_0x001, rvc_end };
  
  /* pseudo-instruction metadata */
  
   const rv_comp_data rvcp_jal[] = {
      { rv_op_j, rvcc_j },
      { rv_op_jal, rvcc_jal },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_jalr[] = {
      { rv_op_ret, rvcc_ret },
      { rv_op_jr, rvcc_jr },
      { rv_op_jalr, rvcc_jalr },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_beq[] = {
      { rv_op_beqz, rvcc_beqz },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_bne[] = {
      { rv_op_bnez, rvcc_bnez },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_blt[] = {
      { rv_op_bltz, rvcc_bltz },
      { rv_op_bgtz, rvcc_bgtz },
      { rv_op_bgt, rvcc_bgt },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_bge[] = {
      { rv_op_blez, rvcc_blez },
      { rv_op_bgez, rvcc_bgez },
      { rv_op_ble, rvcc_ble },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_bltu[] = {
      { rv_op_bgtu, rvcc_bgtu },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_bgeu[] = {
      { rv_op_bleu, rvcc_bleu },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_addi[] = {
      { rv_op_nop, rvcc_nop },
      { rv_op_mv, rvcc_mv },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_sltiu[] = {
      { rv_op_seqz, rvcc_seqz },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_xori[] = {
      { rv_op_not, rvcc_not },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_sub[] = {
      { rv_op_neg, rvcc_neg },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_slt[] = {
      { rv_op_sltz, rvcc_sltz },
      { rv_op_sgtz, rvcc_sgtz },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_sltu[] = {
      { rv_op_snez, rvcc_snez },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_addiw[] = {
      { rv_op_sext_w, rvcc_sext_w },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_subw[] = {
      { rv_op_negw, rvcc_negw },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_csrrw[] = {
      { rv_op_fscsr, rvcc_fscsr },
      { rv_op_fsrm, rvcc_fsrm },
      { rv_op_fsflags, rvcc_fsflags },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_csrrs[] = {
      { rv_op_rdcycle, rvcc_rdcycle },
      { rv_op_rdtime, rvcc_rdtime },
      { rv_op_rdinstret, rvcc_rdinstret },
      { rv_op_rdcycleh, rvcc_rdcycleh },
      { rv_op_rdtimeh, rvcc_rdtimeh },
      { rv_op_rdinstreth, rvcc_rdinstreth },
      { rv_op_frcsr, rvcc_frcsr },
      { rv_op_frrm, rvcc_frrm },
      { rv_op_frflags, rvcc_frflags },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_csrrwi[] = {
      { rv_op_fsrmi, rvcc_fsrmi },
      { rv_op_fsflagsi, rvcc_fsflagsi },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_fsgnj_s[] = {
      { rv_op_fmv_s, rvcc_fmv_s },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_fsgnjn_s[] = {
      { rv_op_fneg_s, rvcc_fneg_s },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_fsgnjx_s[] = {
      { rv_op_fabs_s, rvcc_fabs_s },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_fsgnj_d[] = {
      { rv_op_fmv_d, rvcc_fmv_d },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_fsgnjn_d[] = {
      { rv_op_fneg_d, rvcc_fneg_d },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_fsgnjx_d[] = {
      { rv_op_fabs_d, rvcc_fabs_d },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_fsgnj_q[] = {
      { rv_op_fmv_q, rvcc_fmv_q },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_fsgnjn_q[] = {
      { rv_op_fneg_q, rvcc_fneg_q },
      { rv_op_illegal, NULL }
  };
  
   const rv_comp_data rvcp_fsgnjx_q[] = {
      { rv_op_fabs_q, rvcc_fabs_q },
      { rv_op_illegal, NULL }
  };
  
  /* instruction metadata */
  
  const rv_opcode_data opcode_data[] = {
      { "illegal", rv_codec_illegal, rv_fmt_none, NULL, 0, 0, 0 },
      { "lui", rv_codec_u, rv_fmt_rd_imm, NULL, 0, 0, 0 },
      { "auipc", rv_codec_u, rv_fmt_rd_offset, NULL, 0, 0, 0 },
      { "jal", rv_codec_uj, rv_fmt_rd_offset, rvcp_jal, 0, 0, 0 },
      { "jalr", rv_codec_i, rv_fmt_rd_rs1_offset, rvcp_jalr, 0, 0, 0 },
      { "beq", rv_codec_sb, rv_fmt_rs1_rs2_offset, rvcp_beq, 0, 0, 0 },
      { "bne", rv_codec_sb, rv_fmt_rs1_rs2_offset, rvcp_bne, 0, 0, 0 },
      { "blt", rv_codec_sb, rv_fmt_rs1_rs2_offset, rvcp_blt, 0, 0, 0 },
      { "bge", rv_codec_sb, rv_fmt_rs1_rs2_offset, rvcp_bge, 0, 0, 0 },
      { "bltu", rv_codec_sb, rv_fmt_rs1_rs2_offset, rvcp_bltu, 0, 0, 0 },
      { "bgeu", rv_codec_sb, rv_fmt_rs1_rs2_offset, rvcp_bgeu, 0, 0, 0 },
      { "lb", rv_codec_i, rv_fmt_rd_offset_rs1, NULL, 0, 0, 0 },
      { "lh", rv_codec_i, rv_fmt_rd_offset_rs1, NULL, 0, 0, 0 },
      { "lw", rv_codec_i, rv_fmt_rd_offset_rs1, NULL, 0, 0, 0 },
      { "lbu", rv_codec_i, rv_fmt_rd_offset_rs1, NULL, 0, 0, 0 },
      { "lhu", rv_codec_i, rv_fmt_rd_offset_rs1, NULL, 0, 0, 0 },
      { "sb", rv_codec_s, rv_fmt_rs2_offset_rs1, NULL, 0, 0, 0 },
      { "sh", rv_codec_s, rv_fmt_rs2_offset_rs1, NULL, 0, 0, 0 },
      { "sw", rv_codec_s, rv_fmt_rs2_offset_rs1, NULL, 0, 0, 0 },
      { "addi", rv_codec_i, rv_fmt_rd_rs1_imm, rvcp_addi, 0, 0, 0 },
      { "slti", rv_codec_i, rv_fmt_rd_rs1_imm, NULL, 0, 0, 0 },
      { "sltiu", rv_codec_i, rv_fmt_rd_rs1_imm, rvcp_sltiu, 0, 0, 0 },
      { "xori", rv_codec_i, rv_fmt_rd_rs1_imm, rvcp_xori, 0, 0, 0 },
      { "ori", rv_codec_i, rv_fmt_rd_rs1_imm, NULL, 0, 0, 0 },
      { "andi", rv_codec_i, rv_fmt_rd_rs1_imm, NULL, 0, 0, 0 },
      { "slli", rv_codec_i_sh7, rv_fmt_rd_rs1_imm, NULL, 0, 0, 0 },
      { "srli", rv_codec_i_sh7, rv_fmt_rd_rs1_imm, NULL, 0, 0, 0 },
      { "srai", rv_codec_i_sh7, rv_fmt_rd_rs1_imm, NULL, 0, 0, 0 },
      { "add", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "sub", rv_codec_r, rv_fmt_rd_rs1_rs2, rvcp_sub, 0, 0, 0 },
      { "sll", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "slt", rv_codec_r, rv_fmt_rd_rs1_rs2, rvcp_slt, 0, 0, 0 },
      { "sltu", rv_codec_r, rv_fmt_rd_rs1_rs2, rvcp_sltu, 0, 0, 0 },
      { "xor", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "srl", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "sra", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "or", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "and", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "fence", rv_codec_r_f, rv_fmt_pred_succ, NULL, 0, 0, 0 },
      { "fence.i", rv_codec_none, rv_fmt_none, NULL, 0, 0, 0 },
      { "lwu", rv_codec_i, rv_fmt_rd_offset_rs1, NULL, 0, 0, 0 },
      { "ld", rv_codec_i, rv_fmt_rd_offset_rs1, NULL, 0, 0, 0 },
      { "sd", rv_codec_s, rv_fmt_rs2_offset_rs1, NULL, 0, 0, 0 },
      { "addiw", rv_codec_i, rv_fmt_rd_rs1_imm, rvcp_addiw, 0, 0, 0 },
      { "slliw", rv_codec_i_sh5, rv_fmt_rd_rs1_imm, NULL, 0, 0, 0 },
      { "srliw", rv_codec_i_sh5, rv_fmt_rd_rs1_imm, NULL, 0, 0, 0 },
      { "sraiw", rv_codec_i_sh5, rv_fmt_rd_rs1_imm, NULL, 0, 0, 0 },
      { "addw", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "subw", rv_codec_r, rv_fmt_rd_rs1_rs2, rvcp_subw, 0, 0, 0 },
      { "sllw", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "srlw", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "sraw", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "ldu", rv_codec_i, rv_fmt_rd_offset_rs1, NULL, 0, 0, 0 },
      { "lq", rv_codec_i, rv_fmt_rd_offset_rs1, NULL, 0, 0, 0 },
      { "sq", rv_codec_s, rv_fmt_rs2_offset_rs1, NULL, 0, 0, 0 },
      { "addid", rv_codec_i, rv_fmt_rd_rs1_imm, NULL, 0, 0, 0 },
      { "sllid", rv_codec_i_sh6, rv_fmt_rd_rs1_imm, NULL, 0, 0, 0 },
      { "srlid", rv_codec_i_sh6, rv_fmt_rd_rs1_imm, NULL, 0, 0, 0 },
      { "sraid", rv_codec_i_sh6, rv_fmt_rd_rs1_imm, NULL, 0, 0, 0 },
      { "addd", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "subd", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "slld", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "srld", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "srad", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "mul", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "mulh", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "mulhsu", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "mulhu", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "div", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "divu", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "rem", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "remu", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "mulw", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "divw", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "divuw", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "remw", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "remuw", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "muld", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "divd", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "divud", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "remd", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "remud", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
      { "lr.w", rv_codec_r_l, rv_fmt_aqrl_rd_rs1, NULL, 0, 0, 0 },
      { "sc.w", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoswap.w", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoadd.w", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoxor.w", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoor.w", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoand.w", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amomin.w", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amomax.w", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amominu.w", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amomaxu.w", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "lr.d", rv_codec_r_l, rv_fmt_aqrl_rd_rs1, NULL, 0, 0, 0 },
      { "sc.d", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoswap.d", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoadd.d", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoxor.d", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoor.d", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoand.d", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amomin.d", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amomax.d", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amominu.d", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amomaxu.d", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "lr.q", rv_codec_r_l, rv_fmt_aqrl_rd_rs1, NULL, 0, 0, 0 },
      { "sc.q", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoswap.q", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoadd.q", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoxor.q", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoor.q", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amoand.q", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amomin.q", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amomax.q", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amominu.q", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "amomaxu.q", rv_codec_r_a, rv_fmt_aqrl_rd_rs2_rs1, NULL, 0, 0, 0 },
      { "ecall", rv_codec_none, rv_fmt_none, NULL, 0, 0, 0 },
      { "ebreak", rv_codec_none, rv_fmt_none, NULL, 0, 0, 0 },
      { "uret", rv_codec_none, rv_fmt_none, NULL, 0, 0, 0 },
      { "sret", rv_codec_none, rv_fmt_none, NULL, 0, 0, 0 },
      { "hret", rv_codec_none, rv_fmt_none, NULL, 0, 0, 0 },
      { "mret", rv_codec_none, rv_fmt_none, NULL, 0, 0, 0 },
      { "dret", rv_codec_none, rv_fmt_none, NULL, 0, 0, 0 },
      { "sfence.vm", rv_codec_r, rv_fmt_rs1, NULL, 0, 0, 0 },
      { "sfence.vma", rv_codec_r, rv_fmt_rs1_rs2, NULL, 0, 0, 0 },
      { "wfi", rv_codec_none, rv_fmt_none, NULL, 0, 0, 0 },
      { "csrrw", rv_codec_i_csr, rv_fmt_rd_csr_rs1, rvcp_csrrw, 0, 0, 0 },
      { "csrrs", rv_codec_i_csr, rv_fmt_rd_csr_rs1, rvcp_csrrs, 0, 0, 0 },
      { "csrrc", rv_codec_i_csr, rv_fmt_rd_csr_rs1, NULL, 0, 0, 0 },
      { "csrrwi", rv_codec_i_csr, rv_fmt_rd_csr_zimm, rvcp_csrrwi, 0, 0, 0 },
      { "csrrsi", rv_codec_i_csr, rv_fmt_rd_csr_zimm, NULL, 0, 0, 0 },
      { "csrrci", rv_codec_i_csr, rv_fmt_rd_csr_zimm, NULL, 0, 0, 0 },
      { "flw", rv_codec_i, rv_fmt_frd_offset_rs1, NULL, 0, 0, 0 },
      { "fsw", rv_codec_s, rv_fmt_frs2_offset_rs1, NULL, 0, 0, 0 },
      { "fmadd.s", rv_codec_r4_m, rv_fmt_rm_frd_frs1_frs2_frs3, NULL, 0, 0, 0 },
      { "fmsub.s", rv_codec_r4_m, rv_fmt_rm_frd_frs1_frs2_frs3, NULL, 0, 0, 0 },
      { "fnmsub.s", rv_codec_r4_m, rv_fmt_rm_frd_frs1_frs2_frs3, NULL, 0, 0, 0 },
      { "fnmadd.s", rv_codec_r4_m, rv_fmt_rm_frd_frs1_frs2_frs3, NULL, 0, 0, 0 },
      { "fadd.s", rv_codec_r_m, rv_fmt_rm_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fsub.s", rv_codec_r_m, rv_fmt_rm_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fmul.s", rv_codec_r_m, rv_fmt_rm_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fdiv.s", rv_codec_r_m, rv_fmt_rm_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fsgnj.s", rv_codec_r, rv_fmt_frd_frs1_frs2, rvcp_fsgnj_s, 0, 0, 0 },
      { "fsgnjn.s", rv_codec_r, rv_fmt_frd_frs1_frs2, rvcp_fsgnjn_s, 0, 0, 0 },
      { "fsgnjx.s", rv_codec_r, rv_fmt_frd_frs1_frs2, rvcp_fsgnjx_s, 0, 0, 0 },
      { "fmin.s", rv_codec_r, rv_fmt_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fmax.s", rv_codec_r, rv_fmt_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fsqrt.s", rv_codec_r_m, rv_fmt_rm_frd_frs1, NULL, 0, 0, 0 },
      { "fle.s", rv_codec_r, rv_fmt_rd_frs1_frs2, NULL, 0, 0, 0 },
      { "flt.s", rv_codec_r, rv_fmt_rd_frs1_frs2, NULL, 0, 0, 0 },
      { "feq.s", rv_codec_r, rv_fmt_rd_frs1_frs2, NULL, 0, 0, 0 },
      { "fcvt.w.s", rv_codec_r_m, rv_fmt_rm_rd_frs1, NULL, 0, 0, 0 },
      { "fcvt.wu.s", rv_codec_r_m, rv_fmt_rm_rd_frs1, NULL, 0, 0, 0 },
      { "fcvt.s.w", rv_codec_r_m, rv_fmt_rm_frd_rs1, NULL, 0, 0, 0 },
      { "fcvt.s.wu", rv_codec_r_m, rv_fmt_rm_frd_rs1, NULL, 0, 0, 0 },
      { "fmv.x.s", rv_codec_r, rv_fmt_rd_frs1, NULL, 0, 0, 0 },
      { "fclass.s", rv_codec_r, rv_fmt_rd_frs1, NULL, 0, 0, 0 },
      { "fmv.s.x", rv_codec_r, rv_fmt_frd_rs1, NULL, 0, 0, 0 },
      { "fcvt.l.s", rv_codec_r_m, rv_fmt_rm_rd_frs1, NULL, 0, 0, 0 },
      { "fcvt.lu.s", rv_codec_r_m, rv_fmt_rm_rd_frs1, NULL, 0, 0, 0 },
      { "fcvt.s.l", rv_codec_r_m, rv_fmt_rm_frd_rs1, NULL, 0, 0, 0 },
      { "fcvt.s.lu", rv_codec_r_m, rv_fmt_rm_frd_rs1, NULL, 0, 0, 0 },
      { "fld", rv_codec_i, rv_fmt_frd_offset_rs1, NULL, 0, 0, 0 },
      { "fsd", rv_codec_s, rv_fmt_frs2_offset_rs1, NULL, 0, 0, 0 },
      { "fmadd.d", rv_codec_r4_m, rv_fmt_rm_frd_frs1_frs2_frs3, NULL, 0, 0, 0 },
      { "fmsub.d", rv_codec_r4_m, rv_fmt_rm_frd_frs1_frs2_frs3, NULL, 0, 0, 0 },
      { "fnmsub.d", rv_codec_r4_m, rv_fmt_rm_frd_frs1_frs2_frs3, NULL, 0, 0, 0 },
      { "fnmadd.d", rv_codec_r4_m, rv_fmt_rm_frd_frs1_frs2_frs3, NULL, 0, 0, 0 },
      { "fadd.d", rv_codec_r_m, rv_fmt_rm_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fsub.d", rv_codec_r_m, rv_fmt_rm_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fmul.d", rv_codec_r_m, rv_fmt_rm_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fdiv.d", rv_codec_r_m, rv_fmt_rm_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fsgnj.d", rv_codec_r, rv_fmt_frd_frs1_frs2, rvcp_fsgnj_d, 0, 0, 0 },
      { "fsgnjn.d", rv_codec_r, rv_fmt_frd_frs1_frs2, rvcp_fsgnjn_d, 0, 0, 0 },
      { "fsgnjx.d", rv_codec_r, rv_fmt_frd_frs1_frs2, rvcp_fsgnjx_d, 0, 0, 0 },
      { "fmin.d", rv_codec_r, rv_fmt_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fmax.d", rv_codec_r, rv_fmt_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fcvt.s.d", rv_codec_r_m, rv_fmt_rm_frd_frs1, NULL, 0, 0, 0 },
      { "fcvt.d.s", rv_codec_r_m, rv_fmt_rm_frd_frs1, NULL, 0, 0, 0 },
      { "fsqrt.d", rv_codec_r_m, rv_fmt_rm_frd_frs1, NULL, 0, 0, 0 },
      { "fle.d", rv_codec_r, rv_fmt_rd_frs1_frs2, NULL, 0, 0, 0 },
      { "flt.d", rv_codec_r, rv_fmt_rd_frs1_frs2, NULL, 0, 0, 0 },
      { "feq.d", rv_codec_r, rv_fmt_rd_frs1_frs2, NULL, 0, 0, 0 },
      { "fcvt.w.d", rv_codec_r_m, rv_fmt_rm_rd_frs1, NULL, 0, 0, 0 },
      { "fcvt.wu.d", rv_codec_r_m, rv_fmt_rm_rd_frs1, NULL, 0, 0, 0 },
      { "fcvt.d.w", rv_codec_r_m, rv_fmt_rm_frd_rs1, NULL, 0, 0, 0 },
      { "fcvt.d.wu", rv_codec_r_m, rv_fmt_rm_frd_rs1, NULL, 0, 0, 0 },
      { "fclass.d", rv_codec_r, rv_fmt_rd_frs1, NULL, 0, 0, 0 },
      { "fcvt.l.d", rv_codec_r_m, rv_fmt_rm_rd_frs1, NULL, 0, 0, 0 },
      { "fcvt.lu.d", rv_codec_r_m, rv_fmt_rm_rd_frs1, NULL, 0, 0, 0 },
      { "fmv.x.d", rv_codec_r, rv_fmt_rd_frs1, NULL, 0, 0, 0 },
      { "fcvt.d.l", rv_codec_r_m, rv_fmt_rm_frd_rs1, NULL, 0, 0, 0 },
      { "fcvt.d.lu", rv_codec_r_m, rv_fmt_rm_frd_rs1, NULL, 0, 0, 0 },
      { "fmv.d.x", rv_codec_r, rv_fmt_frd_rs1, NULL, 0, 0, 0 },
      { "flq", rv_codec_i, rv_fmt_frd_offset_rs1, NULL, 0, 0, 0 },
      { "fsq", rv_codec_s, rv_fmt_frs2_offset_rs1, NULL, 0, 0, 0 },
      { "fmadd.q", rv_codec_r4_m, rv_fmt_rm_frd_frs1_frs2_frs3, NULL, 0, 0, 0 },
      { "fmsub.q", rv_codec_r4_m, rv_fmt_rm_frd_frs1_frs2_frs3, NULL, 0, 0, 0 },
      { "fnmsub.q", rv_codec_r4_m, rv_fmt_rm_frd_frs1_frs2_frs3, NULL, 0, 0, 0 },
      { "fnmadd.q", rv_codec_r4_m, rv_fmt_rm_frd_frs1_frs2_frs3, NULL, 0, 0, 0 },
      { "fadd.q", rv_codec_r_m, rv_fmt_rm_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fsub.q", rv_codec_r_m, rv_fmt_rm_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fmul.q", rv_codec_r_m, rv_fmt_rm_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fdiv.q", rv_codec_r_m, rv_fmt_rm_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fsgnj.q", rv_codec_r, rv_fmt_frd_frs1_frs2, rvcp_fsgnj_q, 0, 0, 0 },
      { "fsgnjn.q", rv_codec_r, rv_fmt_frd_frs1_frs2, rvcp_fsgnjn_q, 0, 0, 0 },
      { "fsgnjx.q", rv_codec_r, rv_fmt_frd_frs1_frs2, rvcp_fsgnjx_q, 0, 0, 0 },
      { "fmin.q", rv_codec_r, rv_fmt_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fmax.q", rv_codec_r, rv_fmt_frd_frs1_frs2, NULL, 0, 0, 0 },
      { "fcvt.s.q", rv_codec_r_m, rv_fmt_rm_frd_frs1, NULL, 0, 0, 0 },
      { "fcvt.q.s", rv_codec_r_m, rv_fmt_rm_frd_frs1, NULL, 0, 0, 0 },
      { "fcvt.d.q", rv_codec_r_m, rv_fmt_rm_frd_frs1, NULL, 0, 0, 0 },
      { "fcvt.q.d", rv_codec_r_m, rv_fmt_rm_frd_frs1, NULL, 0, 0, 0 },
      { "fsqrt.q", rv_codec_r_m, rv_fmt_rm_frd_frs1, NULL, 0, 0, 0 },
      { "fle.q", rv_codec_r, rv_fmt_rd_frs1_frs2, NULL, 0, 0, 0 },
      { "flt.q", rv_codec_r, rv_fmt_rd_frs1_frs2, NULL, 0, 0, 0 },
      { "feq.q", rv_codec_r, rv_fmt_rd_frs1_frs2, NULL, 0, 0, 0 },
      { "fcvt.w.q", rv_codec_r_m, rv_fmt_rm_rd_frs1, NULL, 0, 0, 0 },
      { "fcvt.wu.q", rv_codec_r_m, rv_fmt_rm_rd_frs1, NULL, 0, 0, 0 },
      { "fcvt.q.w", rv_codec_r_m, rv_fmt_rm_frd_rs1, NULL, 0, 0, 0 },
      { "fcvt.q.wu", rv_codec_r_m, rv_fmt_rm_frd_rs1, NULL, 0, 0, 0 },
      { "fclass.q", rv_codec_r, rv_fmt_rd_frs1, NULL, 0, 0, 0 },
      { "fcvt.l.q", rv_codec_r_m, rv_fmt_rm_rd_frs1, NULL, 0, 0, 0 },
      { "fcvt.lu.q", rv_codec_r_m, rv_fmt_rm_rd_frs1, NULL, 0, 0, 0 },
      { "fcvt.q.l", rv_codec_r_m, rv_fmt_rm_frd_rs1, NULL, 0, 0, 0 },
      { "fcvt.q.lu", rv_codec_r_m, rv_fmt_rm_frd_rs1, NULL, 0, 0, 0 },
      { "fmv.x.q", rv_codec_r, rv_fmt_rd_frs1, NULL, 0, 0, 0 },
      { "fmv.q.x", rv_codec_r, rv_fmt_frd_rs1, NULL, 0, 0, 0 },
      { "c.addi4spn", rv_codec_ciw_4spn, rv_fmt_rd_rs1_imm, NULL, rv_op_addi,
        rv_op_addi, rv_op_addi, rvcd_imm_nz },
      { "c.fld", rv_codec_cl_ld, rv_fmt_frd_offset_rs1, NULL, rv_op_fld, rv_op_fld, 0 },
      { "c.lw", rv_codec_cl_lw, rv_fmt_rd_offset_rs1, NULL, rv_op_lw, rv_op_lw, rv_op_lw },
      { "c.flw", rv_codec_cl_lw, rv_fmt_frd_offset_rs1, NULL, rv_op_flw, 0, 0 },
      { "c.fsd", rv_codec_cs_sd, rv_fmt_frs2_offset_rs1, NULL, rv_op_fsd, rv_op_fsd, 0 },
      { "c.sw", rv_codec_cs_sw, rv_fmt_rs2_offset_rs1, NULL, rv_op_sw, rv_op_sw, rv_op_sw },
      { "c.fsw", rv_codec_cs_sw, rv_fmt_frs2_offset_rs1, NULL, rv_op_fsw, 0, 0 },
      { "c.nop", rv_codec_ci_none, rv_fmt_none, NULL, rv_op_addi, rv_op_addi, rv_op_addi },
      { "c.addi", rv_codec_ci, rv_fmt_rd_rs1_imm, NULL, rv_op_addi, rv_op_addi,
        rv_op_addi, rvcd_imm_nz },
      { "c.jal", rv_codec_cj_jal, rv_fmt_rd_offset, NULL, rv_op_jal, 0, 0 },
      { "c.li", rv_codec_ci_li, rv_fmt_rd_rs1_imm, NULL, rv_op_addi, rv_op_addi, rv_op_addi },
      { "c.addi16sp", rv_codec_ci_16sp, rv_fmt_rd_rs1_imm, NULL, rv_op_addi,
        rv_op_addi, rv_op_addi, rvcd_imm_nz },
      { "c.lui", rv_codec_ci_lui, rv_fmt_rd_imm, NULL, rv_op_lui, rv_op_lui,
        rv_op_lui, rvcd_imm_nz },
      { "c.srli", rv_codec_cb_sh6, rv_fmt_rd_rs1_imm, NULL, rv_op_srli,
        rv_op_srli, rv_op_srli, rvcd_imm_nz },
      { "c.srai", rv_codec_cb_sh6, rv_fmt_rd_rs1_imm, NULL, rv_op_srai,
        rv_op_srai, rv_op_srai, rvcd_imm_nz },
      { "c.andi", rv_codec_cb_imm, rv_fmt_rd_rs1_imm, NULL, rv_op_andi,
        rv_op_andi, rv_op_andi },
      { "c.sub", rv_codec_cs, rv_fmt_rd_rs1_rs2, NULL, rv_op_sub, rv_op_sub, rv_op_sub },
      { "c.xor", rv_codec_cs, rv_fmt_rd_rs1_rs2, NULL, rv_op_xor, rv_op_xor, rv_op_xor },
      { "c.or", rv_codec_cs, rv_fmt_rd_rs1_rs2, NULL, rv_op_or, rv_op_or, rv_op_or },
      { "c.and", rv_codec_cs, rv_fmt_rd_rs1_rs2, NULL, rv_op_and, rv_op_and, rv_op_and },
      { "c.subw", rv_codec_cs, rv_fmt_rd_rs1_rs2, NULL, rv_op_subw, rv_op_subw, rv_op_subw },
      { "c.addw", rv_codec_cs, rv_fmt_rd_rs1_rs2, NULL, rv_op_addw, rv_op_addw, rv_op_addw },
      { "c.j", rv_codec_cj, rv_fmt_rd_offset, NULL, rv_op_jal, rv_op_jal, rv_op_jal },
      { "c.beqz", rv_codec_cb, rv_fmt_rs1_rs2_offset, NULL, rv_op_beq, rv_op_beq, rv_op_beq },
      { "c.bnez", rv_codec_cb, rv_fmt_rs1_rs2_offset, NULL, rv_op_bne, rv_op_bne, rv_op_bne },
      { "c.slli", rv_codec_ci_sh6, rv_fmt_rd_rs1_imm, NULL, rv_op_slli,
        rv_op_slli, rv_op_slli, rvcd_imm_nz },
      { "c.fldsp", rv_codec_ci_ldsp, rv_fmt_frd_offset_rs1, NULL, rv_op_fld, rv_op_fld, rv_op_fld },
      { "c.lwsp", rv_codec_ci_lwsp, rv_fmt_rd_offset_rs1, NULL, rv_op_lw, rv_op_lw, rv_op_lw },
      { "c.flwsp", rv_codec_ci_lwsp, rv_fmt_frd_offset_rs1, NULL, rv_op_flw, 0, 0 },
      { "c.jr", rv_codec_cr_jr, rv_fmt_rd_rs1_offset, NULL, rv_op_jalr, rv_op_jalr, rv_op_jalr },
      { "c.mv", rv_codec_cr_mv, rv_fmt_rd_rs1_rs2, NULL, rv_op_addi, rv_op_addi, rv_op_addi },
      { "c.ebreak", rv_codec_ci_none, rv_fmt_none, NULL, rv_op_ebreak, rv_op_ebreak, rv_op_ebreak },
      { "c.jalr", rv_codec_cr_jalr, rv_fmt_rd_rs1_offset, NULL, rv_op_jalr, rv_op_jalr, rv_op_jalr },
      { "c.add", rv_codec_cr, rv_fmt_rd_rs1_rs2, NULL, rv_op_add, rv_op_add, rv_op_add },
      { "c.fsdsp", rv_codec_css_sdsp, rv_fmt_frs2_offset_rs1, NULL, rv_op_fsd, rv_op_fsd, rv_op_fsd },
      { "c.swsp", rv_codec_css_swsp, rv_fmt_rs2_offset_rs1, NULL, rv_op_sw, rv_op_sw, rv_op_sw },
      { "c.fswsp", rv_codec_css_swsp, rv_fmt_frs2_offset_rs1, NULL, rv_op_fsw, 0, 0 },
      { "c.ld", rv_codec_cl_ld, rv_fmt_rd_offset_rs1, NULL, 0, rv_op_ld, rv_op_ld },
      { "c.sd", rv_codec_cs_sd, rv_fmt_rs2_offset_rs1, NULL, 0, rv_op_sd, rv_op_sd },
      { "c.addiw", rv_codec_ci, rv_fmt_rd_rs1_imm, NULL, 0, rv_op_addiw, rv_op_addiw },
      { "c.ldsp", rv_codec_ci_ldsp, rv_fmt_rd_offset_rs1, NULL, 0, rv_op_ld, rv_op_ld },
      { "c.sdsp", rv_codec_css_sdsp, rv_fmt_rs2_offset_rs1, NULL, 0, rv_op_sd, rv_op_sd },
      { "c.lq", rv_codec_cl_lq, rv_fmt_rd_offset_rs1, NULL, 0, 0, rv_op_lq },
      { "c.sq", rv_codec_cs_sq, rv_fmt_rs2_offset_rs1, NULL, 0, 0, rv_op_sq },
      { "c.lqsp", rv_codec_ci_lqsp, rv_fmt_rd_offset_rs1, NULL, 0, 0, rv_op_lq },
      { "c.sqsp", rv_codec_css_sqsp, rv_fmt_rs2_offset_rs1, NULL, 0, 0, rv_op_sq },
      { "nop", rv_codec_i, rv_fmt_none, NULL, 0, 0, 0 },
      { "mv", rv_codec_i, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "not", rv_codec_i, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "neg", rv_codec_r, rv_fmt_rd_rs2, NULL, 0, 0, 0 },
      { "negw", rv_codec_r, rv_fmt_rd_rs2, NULL, 0, 0, 0 },
      { "sext.w", rv_codec_i, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "seqz", rv_codec_i, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "snez", rv_codec_r, rv_fmt_rd_rs2, NULL, 0, 0, 0 },
      { "sltz", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "sgtz", rv_codec_r, rv_fmt_rd_rs2, NULL, 0, 0, 0 },
      { "fmv.s", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "fabs.s", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "fneg.s", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "fmv.d", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "fabs.d", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "fneg.d", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "fmv.q", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "fabs.q", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "fneg.q", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "beqz", rv_codec_sb, rv_fmt_rs1_offset, NULL, 0, 0, 0 },
      { "bnez", rv_codec_sb, rv_fmt_rs1_offset, NULL, 0, 0, 0 },
      { "blez", rv_codec_sb, rv_fmt_rs2_offset, NULL, 0, 0, 0 },
      { "bgez", rv_codec_sb, rv_fmt_rs1_offset, NULL, 0, 0, 0 },
      { "bltz", rv_codec_sb, rv_fmt_rs1_offset, NULL, 0, 0, 0 },
      { "bgtz", rv_codec_sb, rv_fmt_rs2_offset, NULL, 0, 0, 0 },
      { "ble", rv_codec_sb, rv_fmt_rs2_rs1_offset, NULL, 0, 0, 0 },
      { "bleu", rv_codec_sb, rv_fmt_rs2_rs1_offset, NULL, 0, 0, 0 },
      { "bgt", rv_codec_sb, rv_fmt_rs2_rs1_offset, NULL, 0, 0, 0 },
      { "bgtu", rv_codec_sb, rv_fmt_rs2_rs1_offset, NULL, 0, 0, 0 },
      { "j", rv_codec_uj, rv_fmt_offset, NULL, 0, 0, 0 },
      { "ret", rv_codec_i, rv_fmt_none, NULL, 0, 0, 0 },
      { "jr", rv_codec_i, rv_fmt_rs1, NULL, 0, 0, 0 },
      { "rdcycle", rv_codec_i_csr, rv_fmt_rd, NULL, 0, 0, 0 },
      { "rdtime", rv_codec_i_csr, rv_fmt_rd, NULL, 0, 0, 0 },
      { "rdinstret", rv_codec_i_csr, rv_fmt_rd, NULL, 0, 0, 0 },
      { "rdcycleh", rv_codec_i_csr, rv_fmt_rd, NULL, 0, 0, 0 },
      { "rdtimeh", rv_codec_i_csr, rv_fmt_rd, NULL, 0, 0, 0 },
      { "rdinstreth", rv_codec_i_csr, rv_fmt_rd, NULL, 0, 0, 0 },
      { "frcsr", rv_codec_i_csr, rv_fmt_rd, NULL, 0, 0, 0 },
      { "frrm", rv_codec_i_csr, rv_fmt_rd, NULL, 0, 0, 0 },
      { "frflags", rv_codec_i_csr, rv_fmt_rd, NULL, 0, 0, 0 },
      { "fscsr", rv_codec_i_csr, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "fsrm", rv_codec_i_csr, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "fsflags", rv_codec_i_csr, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      { "fsrmi", rv_codec_i_csr, rv_fmt_rd_zimm, NULL, 0, 0, 0 },
      { "fsflagsi", rv_codec_i_csr, rv_fmt_rd_zimm, NULL, 0, 0, 0 },
  
      // CHERI extensions
      [rv_op_auipcc] = { "auipcc", rv_codec_u, rv_fmt_cd_offset, NULL, 0, 0, 0 },
      [rv_op_lc] = { "lc", rv_codec_i, rv_fmt_cd_offset_rs1, NULL, 0, 0, 0 },
      [rv_op_clc] = { "clc", rv_codec_i, rv_fmt_cd_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_sc] = { "sc", rv_codec_s, rv_fmt_cs2_offset_rs1, NULL, 0, 0, 0 },
      [rv_op_csc] = { "csc", rv_codec_s, rv_fmt_cs2_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_cincoffsetimm] = { "cincoffset", rv_codec_i, rv_fmt_cd_cs1_imm, NULL, 0, 0, 0 },
      [rv_op_csetboundsimm] = { "csetbounds", rv_codec_i, rv_fmt_cd_cs1_imm, NULL, 0, 0, 0 },
  
      // Two operand
      [rv_op_cgetperm] = { "cgetperm", rv_codec_r, rv_fmt_rd_cs1, NULL, 0, 0, 0 },
      [rv_op_cgettype] = { "cgettype", rv_codec_r, rv_fmt_rd_cs1, NULL, 0, 0, 0 },
      [rv_op_cgetbase] = { "cgetbase", rv_codec_r, rv_fmt_rd_cs1, NULL, 0, 0, 0 },
      [rv_op_cgetlen] = { "cgetlen", rv_codec_r, rv_fmt_rd_cs1, NULL, 0, 0, 0 },
      [rv_op_cgettag] = { "cgettag", rv_codec_r, rv_fmt_rd_cs1, NULL, 0, 0, 0 },
      [rv_op_cgetsealed] = { "cgetsealed", rv_codec_r, rv_fmt_rd_cs1, NULL, 0, 0, 0 },
      [rv_op_cgetoffset] = { "cgetoffset", rv_codec_r, rv_fmt_rd_cs1, NULL, 0, 0, 0 },
      [rv_op_cgetflags] = { "cgetflags", rv_codec_r, rv_fmt_rd_cs1, NULL, 0, 0, 0 },
      [rv_op_crrl] = { "crrl", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      [rv_op_cram] = { "cram", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
      [rv_op_cmove] = { "cmove", rv_codec_r, rv_fmt_cd_cs1, NULL, 0, 0, 0 },
      [rv_op_ccleartag] = { "ccleartag", rv_codec_r, rv_fmt_cd_cs1, NULL, 0, 0, 0 },
      [rv_op_cjalr] = { "cjalr", rv_codec_r, rv_fmt_cd_cs1, NULL, 0, 0, 0 },
      [rv_op_cgetaddr] = { "cgetaddr", rv_codec_r, rv_fmt_rd_cs1, NULL, 0, 0, 0 },
      [rv_op_cgethigh] = { "cgetaddr", rv_codec_r, rv_fmt_rd_cs1, NULL, 0, 0, 0 },
      [rv_op_csealentry] = { "csealentry", rv_codec_r, rv_fmt_cd_cs1, NULL, 0, 0, 0 },
      [rv_op_cloadtags] = { "cloadtags", rv_codec_r, rv_fmt_rd_cs1, NULL, 0, 0, 0 },
  
      // capmode loads:
      [rv_op_clb] = { "clb", rv_codec_i, rv_fmt_rd_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_clh] = { "clh", rv_codec_i, rv_fmt_rd_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_clw] = { "clw", rv_codec_i, rv_fmt_rd_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_cld] = { "cld", rv_codec_i, rv_fmt_rd_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_clbu] = { "clbu", rv_codec_i, rv_fmt_rd_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_clhu] = { "clhu", rv_codec_i, rv_fmt_rd_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_clwu] = { "clwu", rv_codec_i, rv_fmt_rd_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_csb] = { "sb", rv_codec_s, rv_fmt_rs2_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_csh] = { "sh", rv_codec_s, rv_fmt_rs2_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_csw] = { "sw", rv_codec_s, rv_fmt_rs2_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_csd] = { "sd", rv_codec_s, rv_fmt_rs2_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_csd] = { "csd", rv_codec_s, rv_fmt_rs2_offset_cs1, NULL, 0, 0, 0 },
  
      // Three operand
      [rv_op_cspecialrw] = { "cspecialrw", rv_codec_r, rv_fmt_cd_scr_cs1, NULL, 0, 0, 0 },
      [rv_op_csetbounds] = { "csetbounds", rv_codec_r, rv_fmt_cd_cs1_rs2, NULL, 0, 0, 0 },
      [rv_op_csetboundsexact] = { "csetboundsexact", rv_codec_r, rv_fmt_cd_cs1_rs2, NULL, 0, 0, 0 },
      [rv_op_cseal] = { "cseal", rv_codec_r, rv_fmt_cd_cs1_cs2, NULL, 0, 0, 0 },
      [rv_op_cunseal] = { "cunseal", rv_codec_r, rv_fmt_cd_cs1_cs2, NULL, 0, 0, 0 },
      [rv_op_candperm] = { "candperm", rv_codec_r, rv_fmt_cd_cs1_rs2, NULL, 0, 0, 0 },
      [rv_op_csetflags] = { "csetflags", rv_codec_r, rv_fmt_cd_cs1_rs2, NULL, 0, 0, 0 },
      [rv_op_csetoffset] = { "csetoffset", rv_codec_r, rv_fmt_cd_cs1_rs2, NULL, 0, 0, 0 },
      [rv_op_csetaddr] = { "csetaddr", rv_codec_r, rv_fmt_cd_cs1_rs2, NULL, 0, 0, 0 },
      [rv_op_csethigh] = { "csethigh", rv_codec_r, rv_fmt_cd_cs1_rs2, NULL, 0, 0, 0 },
      [rv_op_cincoffset] = { "cincoffset", rv_codec_r, rv_fmt_cd_cs1_rs2, NULL, 0, 0, 0 },
      [rv_op_ctoptr] = { "ctoptr", rv_codec_r, rv_fmt_rd_cs1_cs2, NULL, 0, 0, 0 },
      [rv_op_cfromptr] = { "cfromptr", rv_codec_r, rv_fmt_cd_cs1_rs2, NULL, 0, 0, 0 },
      [rv_op_csub] = { "csub", rv_codec_r, rv_fmt_rd_cs1_cs2, NULL, 0, 0, 0 },
      [rv_op_cbuildcap] = { "cbuildcap", rv_codec_r, rv_fmt_cd_cs1_cs2, NULL, 0, 0, 0 },
      [rv_op_ccopytype] = { "ccopytype", rv_codec_r, rv_fmt_cd_cs1_cs2, NULL, 0, 0, 0 },
      [rv_op_ccseal] = { "ccseal", rv_codec_r, rv_fmt_cd_cs1_cs2, NULL, 0, 0, 0 },
      [rv_op_ctestsubset] = { "ctestsubset", rv_codec_r, rv_fmt_rd_cs1_cs2, NULL, 0, 0, 0 },
      [rv_op_cseqx] = { "cseqx", rv_codec_r, rv_fmt_rd_cs1_cs2, NULL, 0, 0, 0 },
  
      // FP load store
      [rv_op_cflw] = { "cflw", rv_codec_i, rv_fmt_frd_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_cfsw] = { "cfsw", rv_codec_s, rv_fmt_frs2_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_cfld] = { "cfld", rv_codec_i, rv_fmt_frd_offset_cs1, NULL, 0, 0, 0 },
      [rv_op_cfsd] = { "cfsd", rv_codec_s, rv_fmt_frs2_offset_cs1, NULL, 0, 0, 0 },
  };
  




typedef enum {
    BRANCH_DIRECT_JUMP = 0,  // Unconditional direct jumps (j)
    BRANCH_INDIRECT,         // Unconditional indirect jumps (jalr without link)
    BRANCH_CONDITIONAL,      // Conditional branches (beq, bne, etc.)
    BRANCH_DIRECT_CALL,      // Direct calls (jal)
    BRANCH_INDIRECT_CALL,    // Indirect calls (jalr with link)
    BRANCH_RETURN,           // Return instructions (ret/jalr used as return)
    BRANCH_OTHER,            // Other types of branches
    NOT_BRANCH,              // Not a branch instruction
    ERROR
} BranchType;


// Enum for instruction categories
typedef enum {
    INST_TYPE_UNKNOWN = 0,
    INST_TYPE_BRANCH,    // branches and jumps
    INST_TYPE_LOAD,      // Regular loads
    INST_TYPE_STORE,     // Regular stores
    INST_TYPE_CAP_LOAD,  // Capability loads
    INST_TYPE_CAP_STORE, // Capability stores
    INST_TYPE_CAP_OP,    // Other capability operations
    INST_TYPE_ALU,       // Integer ALU operations
    INST_TYPE_SYSTEM,    // System/privileged instructions
    INST_TYPE_CSR,       // Control and Status Register operations
    INST_TYPE_ATOMIC,    // Atomic memory operations
    INST_TYPE_FP,        // Floating-point operations
    INST_TYPE_FP_LOAD,   // Floating-point loads
    INST_TYPE_FP_STORE,  // Floating-point stores
} inst_type_t;


    /* Operand extractor function declarations */

    uint32_t operand_rd(rv_inst inst);
    uint32_t operand_rs1(rv_inst inst);
    uint32_t operand_rs2(rv_inst inst);
    uint32_t operand_rs3(rv_inst inst);
    uint32_t operand_aq(rv_inst inst);
    uint32_t operand_rl(rv_inst inst);
    uint32_t operand_pred(rv_inst inst);
    uint32_t operand_succ(rv_inst inst);
    uint32_t operand_rm(rv_inst inst);
    uint32_t operand_shamt5(rv_inst inst);
    uint32_t operand_shamt6(rv_inst inst);
    uint32_t operand_shamt7(rv_inst inst);
    uint32_t operand_crdq(rv_inst inst);
    uint32_t operand_crs1q(rv_inst inst);
    uint32_t operand_crs1rdq(rv_inst inst);
    uint32_t operand_crs2q(rv_inst inst);
    uint32_t operand_crd(rv_inst inst);
    uint32_t operand_crs1(rv_inst inst);
    uint32_t operand_crs1rd(rv_inst inst);
    uint32_t operand_crs2(rv_inst inst);
    uint32_t operand_cimmsh5(rv_inst inst);
    uint32_t operand_csr12(rv_inst inst);

    int32_t operand_imm12(rv_inst inst);
    int32_t operand_imm20(rv_inst inst);
    int32_t operand_jimm20(rv_inst inst);
    int32_t operand_simm12(rv_inst inst);
    int32_t operand_sbimm12(rv_inst inst);
    int32_t operand_cimmi(rv_inst inst);
    int32_t operand_cimmui(rv_inst inst);
    int32_t operand_cimm16sp(rv_inst inst);
    int32_t operand_cimmj(rv_inst inst);
    int32_t operand_cimmb(rv_inst inst);

    uint32_t operand_cimmsh6(rv_inst inst);
    uint32_t operand_cimmlwsp(rv_inst inst);
    uint32_t operand_cimmldsp(rv_inst inst);
    uint32_t operand_cimmlqsp(rv_inst inst);
    uint32_t operand_cimmswsp(rv_inst inst);
    uint32_t operand_cimmsdsp(rv_inst inst);
    uint32_t operand_cimmsqsp(rv_inst inst);
    uint32_t operand_cimm4spn(rv_inst inst);
    uint32_t operand_cimmw(rv_inst inst);
    uint32_t operand_cimmd(rv_inst inst);
    uint32_t operand_cimmq(rv_inst inst);



    uint8_t count_register_operands(uint8_t);
    void decode_inst_opcode(rv_decode *dec, rv_isa isa, int flags);
    rv_opcode decode_cheri_inst(rv_inst inst);
    rv_opcode decode_cheri_two_op(unsigned func);
    void decode_inst_operands(rv_decode *dec);
    rv_decode disasm_inst(rv_isa isa, uint64_t pc, rv_inst inst, int flags);
    void decode_inst_lift_pseudo(rv_decode *dec);
    bool check_constraints(rv_decode *dec, const rvc_constraint *c);
    void decode_inst_decompress(rv_decode *dec, rv_isa isa);
    void decode_inst_decompress_rv128(rv_decode *dec);
    void decode_inst_decompress_rv64(rv_decode *dec);
    void decode_inst_decompress_rv32(rv_decode *dec);
    inst_type_t classify_instruction(rv_decode dec); 
    const char* inst_type_to_str(inst_type_t type);
    bool spans_multiple_cache_lines(uint64_t addr, uint8_t size);
    uint8_t get_memory_access_size(rv_decode dec);

#endif