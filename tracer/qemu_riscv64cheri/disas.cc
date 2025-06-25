#include "disas.hh"
 
 /* decode opcode */
 
  rv_opcode decode_cheri_two_op(unsigned func) {
     switch (func) {
     case 0b00000: return rv_op_cgetperm;
     case 0b00001: return rv_op_cgettype;
     case 0b00010: return rv_op_cgetbase;
     case 0b00011: return rv_op_cgetlen;
     case 0b00100: return rv_op_cgettag;
     case 0b00101: return rv_op_cgetsealed;
     case 0b00110: return rv_op_cgetoffset;
     case 0b00111: return rv_op_cgetflags;
     case 0b01000: return rv_op_crrl;
     case 0b01001: return rv_op_cram;
     case 0b01010: return rv_op_cmove;
     case 0b01011: return rv_op_ccleartag;
     case 0b01100: return rv_op_cjalr;
     case 0b01111: return rv_op_cgetaddr;
     case 0b10001: return rv_op_csealentry;
     case 0b10010: return rv_op_cloadtags;
     case 0b10111: return rv_op_cgethigh;
     default: return rv_op_illegal;
     }
 }
 
 // From insn32-cheri.decode
 #define CHERI_THREEOP_CASE(name, high_bits, ...)                               \
     case 0b##high_bits:                                                        \
         return rv_op_##name;
 
  rv_opcode decode_cheri_inst(rv_inst inst) {
     int func = ((inst >> 25) & 0b111111);
     switch (func) {
     // 0000000, unused
     CHERI_THREEOP_CASE(cspecialrw,  0000001,  ..... ..... 000 ..... 1011011 @r)
     // 0000010-0000111 unused
     CHERI_THREEOP_CASE(csetbounds,  0001000,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(csetboundsexact, 0001001,  ..... ..... 000 ..... 1011011 @r)
     // 0001010 unused
     CHERI_THREEOP_CASE(cseal,       0001011,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(cunseal,     0001100,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(candperm,    0001101,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(csetflags,   0001110,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(csetoffset,  0001111,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(csetaddr,    0010000,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(cincoffset,  0010001,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(ctoptr,      0010010,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(cfromptr,    0010011,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(csub,        0010100,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(csethigh,    0010110,  ..... ..... 000 ..... 1011011 @r)
     // 0010101-0011100 unused
     CHERI_THREEOP_CASE(cbuildcap,   0011101,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(ccopytype,   0011110,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(ccseal,      0011111,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(ctestsubset, 0100000,  ..... ..... 000 ..... 1011011 @r)
     CHERI_THREEOP_CASE(cseqx,       0100001,  ..... ..... 000 ..... 1011011 @r)
     // 1111011 unused
     // TODO: 1111100 Used for Stores (see below)
     // TODO: 1111101 Used for Loads (see below)
     // TODO: 1111110 Used for two source ops
     // 1111111 Used for Source & Dest ops (see above)
     case 0b111111:
         return decode_cheri_two_op((inst >> 20) & 0b11111);
     default:
         return rv_op_illegal;
     }
 }
 
  void decode_inst_opcode(rv_decode *dec, rv_isa isa, int flags)
 {
     rv_inst inst = dec->inst;
     rv_opcode op = rv_op_illegal;
     switch (((inst >> 0) & 0b11)) {
     case 0:
         switch (((inst >> 13) & 0b111)) {
         case 0: op = rv_op_c_addi4spn; break;
         case 1:
             if (isa == rv128) {
                 op = rv_op_c_lq;
             } else {
                 op = rv_op_c_fld;
             }
             break;
         case 2: op = rv_op_c_lw; break;
         case 3:
             if (isa == rv32) {
                 op = rv_op_c_flw;
             } else {
                 op = rv_op_c_ld;
             }
             break;
         case 5:
             if (isa == rv128) {
                 op = rv_op_c_sq;
             } else {
                 op = rv_op_c_fsd;
             }
             break;
         case 6: op = rv_op_c_sw; break;
         case 7:
             if (isa == rv32) {
                 op = rv_op_c_fsw;
             } else {
                 op = rv_op_c_sd;
             }
             break;
         }
         break;
     case 1:
         switch (((inst >> 13) & 0b111)) {
         case 0:
             switch (((inst >> 2) & 0b11111111111)) {
             case 0: op = rv_op_c_nop; break;
             default: op = rv_op_c_addi; break;
             }
             break;
         case 1:
             if (isa == rv32) {
                 op = rv_op_c_jal;
             } else {
                 op = rv_op_c_addiw;
             }
             break;
         case 2: op = rv_op_c_li; break;
         case 3:
             switch (((inst >> 7) & 0b11111)) {
             case 2: op = rv_op_c_addi16sp; break;
             default: op = rv_op_c_lui; break;
             }
             break;
         case 4:
             switch (((inst >> 10) & 0b11)) {
             case 0:
                 op = rv_op_c_srli;
                 break;
             case 1:
                 op = rv_op_c_srai;
                 break;
             case 2: op = rv_op_c_andi; break;
             case 3:
                 switch (((inst >> 10) & 0b100) | ((inst >> 5) & 0b011)) {
                 case 0: op = rv_op_c_sub; break;
                 case 1: op = rv_op_c_xor; break;
                 case 2: op = rv_op_c_or; break;
                 case 3: op = rv_op_c_and; break;
                 case 4: op = rv_op_c_subw; break;
                 case 5: op = rv_op_c_addw; break;
                 }
                 break;
             }
             break;
         case 5: op = rv_op_c_j; break;
         case 6: op = rv_op_c_beqz; break;
         case 7: op = rv_op_c_bnez; break;
         }
         break;
     case 2:
         switch (((inst >> 13) & 0b111)) {
         case 0:
             op = rv_op_c_slli;
             break;
         case 1:
             if (isa == rv128) {
                 op = rv_op_c_lqsp;
             } else {
                 op = rv_op_c_fldsp;
             }
             break;
         case 2: op = rv_op_c_lwsp; break;
         case 3:
             if (isa == rv32) {
                 op = rv_op_c_flwsp;
             } else {
                 op = rv_op_c_ldsp;
             }
             break;
         case 4:
             switch (((inst >> 12) & 0b1)) {
             case 0:
                 switch (((inst >> 2) & 0b11111)) {
                 case 0: op = rv_op_c_jr; break;
                 default: op = rv_op_c_mv; break;
                 }
                 break;
             case 1:
                 switch (((inst >> 2) & 0b11111)) {
                 case 0:
                     switch (((inst >> 7) & 0b11111)) {
                     case 0: op = rv_op_c_ebreak; break;
                     default: op = rv_op_c_jalr; break;
                     }
                     break;
                 default: op = rv_op_c_add; break;
                 }
                 break;
             }
             break;
         case 5:
             if (isa == rv128) {
                 op = rv_op_c_sqsp;
             } else {
                 op = rv_op_c_fsdsp;
             }
             break;
         case 6: op = rv_op_c_swsp; break;
         case 7:
             if (isa == rv32) {
                 op = rv_op_c_fswsp;
             } else {
                 op = rv_op_c_sdsp;
             }
             break;
         }
         break;
     case 3:
         switch (((inst >> 2) & 0b11111)) {
         case 0:
             switch (((inst >> 12) & 0b111)) {
             case 0: op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_clb : rv_op_lb; break;
             case 1: op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_clh : rv_op_lh; break;
             case 2: op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_clw : rv_op_lw; break;
             case 3:
                 if (isa == rv32 && flags & RISCV_DIS_FLAG_CHERI) {
                     op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_clc : rv_op_lc;
                 } else {
                     op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_cld : rv_op_ld;
                 }
                 break;
             case 4: op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_clbu : rv_op_lbu; break;
             case 5: op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_clhu : rv_op_lhu; break;
             case 6: op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_clwu : rv_op_lwu; break;
             case 7: op = rv_op_ldu; break;
             }
             break;
         case 1:
             switch (((inst >> 12) & 0b111)) {
             case 2: op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_cflw : rv_op_flw; break;
             case 3: op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_cfld : rv_op_fld; break;
             case 4: op = rv_op_flq; break;
             }
             break;
         case 3:
             switch (((inst >> 12) & 0b111)) {
             case 0: op = rv_op_fence; break;
             case 1: op = rv_op_fence_i; break;
             case 2:
                 if (isa == rv64 && flags & RISCV_DIS_FLAG_CHERI) {
                     op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_clc : rv_op_lc;
                 } else {
                     op = rv_op_lq;
                 }
                 break;
             }
             break;
         case 4:
             switch (((inst >> 12) & 0b111)) {
             case 0: op = rv_op_addi; break;
             case 1:
                 switch (((inst >> 27) & 0b11111)) {
                 case 0: op = rv_op_slli; break;
                 }
                 break;
             case 2: op = rv_op_slti; break;
             case 3: op = rv_op_sltiu; break;
             case 4: op = rv_op_xori; break;
             case 5:
                 switch (((inst >> 27) & 0b11111)) {
                 case 0: op = rv_op_srli; break;
                 case 8: op = rv_op_srai; break;
                 }
                 break;
             case 6: op = rv_op_ori; break;
             case 7: op = rv_op_andi; break;
             }
             break;
         case 5:
             op = flags & RISCV_DIS_FLAG_CAPMODE ? rv_op_auipcc : rv_op_auipc;
             break;
         case 6:
             switch (((inst >> 12) & 0b111)) {
             case 0: op = rv_op_addiw; break;
             case 1:
                 switch (((inst >> 25) & 0b1111111)) {
                 case 0: op = rv_op_slliw; break;
                 }
                 break;
             case 5:
                 switch (((inst >> 25) & 0b1111111)) {
                 case 0: op = rv_op_srliw; break;
                 case 32: op = rv_op_sraiw; break;
                 }
                 break;
             }
             break;
         case 8:
             switch (((inst >> 12) & 0b111)) {
             case 0: op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_csb : rv_op_sb; break;
             case 1: op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_csh : rv_op_sh; break;
             case 2: op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_csw : rv_op_sw; break;
             case 3:
                 if (isa == rv32 && flags & RISCV_DIS_FLAG_CHERI) {
                     op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_csc : rv_op_sc;
                 } else {
                     op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_csd : rv_op_sd;
                 }
                 break;
             case 4:
                 if (isa == rv64 && flags & RISCV_DIS_FLAG_CHERI) {
                     op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_csc : rv_op_sc;
                 } else {
                     op = rv_op_sq;
                 };
                 break;
             }
             break;
         case 9:
             switch (((inst >> 12) & 0b111)) {
             case 2: op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_cfsw : rv_op_fsw; break;
             case 3: op = (flags & RISCV_DIS_FLAG_CAPMODE) ? rv_op_cfsd : rv_op_fsd; break;
             case 4: op = rv_op_fsq; break;
             }
             break;
         case 11:
             switch (((inst >> 24) & 0b11111000) | ((inst >> 12) & 0b00000111)) {
             case 2: op = rv_op_amoadd_w; break;
             case 3: op = rv_op_amoadd_d; break;
             case 4: op = rv_op_amoadd_q; break;
             case 10: op = rv_op_amoswap_w; break;
             case 11: op = rv_op_amoswap_d; break;
             case 12: op = rv_op_amoswap_q; break;
             case 18:
                 switch (((inst >> 20) & 0b11111)) {
                 case 0: op = rv_op_lr_w; break;
                 }
                 break;
             case 19:
                 switch (((inst >> 20) & 0b11111)) {
                 case 0: op = rv_op_lr_d; break;
                 }
                 break;
             case 20:
                 switch (((inst >> 20) & 0b11111)) {
                 case 0: op = rv_op_lr_q; break;
                 }
                 break;
             case 26: op = rv_op_sc_w; break;
             case 27: op = rv_op_sc_d; break;
             case 28: op = rv_op_sc_q; break;
             case 34: op = rv_op_amoxor_w; break;
             case 35: op = rv_op_amoxor_d; break;
             case 36: op = rv_op_amoxor_q; break;
             case 66: op = rv_op_amoor_w; break;
             case 67: op = rv_op_amoor_d; break;
             case 68: op = rv_op_amoor_q; break;
             case 98: op = rv_op_amoand_w; break;
             case 99: op = rv_op_amoand_d; break;
             case 100: op = rv_op_amoand_q; break;
             case 130: op = rv_op_amomin_w; break;
             case 131: op = rv_op_amomin_d; break;
             case 132: op = rv_op_amomin_q; break;
             case 162: op = rv_op_amomax_w; break;
             case 163: op = rv_op_amomax_d; break;
             case 164: op = rv_op_amomax_q; break;
             case 194: op = rv_op_amominu_w; break;
             case 195: op = rv_op_amominu_d; break;
             case 196: op = rv_op_amominu_q; break;
             case 226: op = rv_op_amomaxu_w; break;
             case 227: op = rv_op_amomaxu_d; break;
             case 228: op = rv_op_amomaxu_q; break;
             }
             break;
         case 12:
             switch (((inst >> 22) & 0b1111111000) | ((inst >> 12) & 0b0000000111)) {
             case 0: op = rv_op_add; break;
             case 1: op = rv_op_sll; break;
             case 2: op = rv_op_slt; break;
             case 3: op = rv_op_sltu; break;
             case 4: op = rv_op_xor; break;
             case 5: op = rv_op_srl; break;
             case 6: op = rv_op_or; break;
             case 7: op = rv_op_and; break;
             case 8: op = rv_op_mul; break;
             case 9: op = rv_op_mulh; break;
             case 10: op = rv_op_mulhsu; break;
             case 11: op = rv_op_mulhu; break;
             case 12: op = rv_op_div; break;
             case 13: op = rv_op_divu; break;
             case 14: op = rv_op_rem; break;
             case 15: op = rv_op_remu; break;
             case 256: op = rv_op_sub; break;
             case 261: op = rv_op_sra; break;
             }
             break;
         case 13: op = rv_op_lui; break;
         case 14:
             switch (((inst >> 22) & 0b1111111000) | ((inst >> 12) & 0b0000000111)) {
             case 0: op = rv_op_addw; break;
             case 1: op = rv_op_sllw; break;
             case 5: op = rv_op_srlw; break;
             case 8: op = rv_op_mulw; break;
             case 12: op = rv_op_divw; break;
             case 13: op = rv_op_divuw; break;
             case 14: op = rv_op_remw; break;
             case 15: op = rv_op_remuw; break;
             case 256: op = rv_op_subw; break;
             case 261: op = rv_op_sraw; break;
             }
             break;
         case 16:
             switch (((inst >> 25) & 0b11)) {
             case 0: op = rv_op_fmadd_s; break;
             case 1: op = rv_op_fmadd_d; break;
             case 3: op = rv_op_fmadd_q; break;
             }
             break;
         case 17:
             switch (((inst >> 25) & 0b11)) {
             case 0: op = rv_op_fmsub_s; break;
             case 1: op = rv_op_fmsub_d; break;
             case 3: op = rv_op_fmsub_q; break;
             }
             break;
         case 18:
             switch (((inst >> 25) & 0b11)) {
             case 0: op = rv_op_fnmsub_s; break;
             case 1: op = rv_op_fnmsub_d; break;
             case 3: op = rv_op_fnmsub_q; break;
             }
             break;
         case 19:
             switch (((inst >> 25) & 0b11)) {
             case 0: op = rv_op_fnmadd_s; break;
             case 1: op = rv_op_fnmadd_d; break;
             case 3: op = rv_op_fnmadd_q; break;
             }
             break;
         case 20:
             switch (((inst >> 25) & 0b1111111)) {
             case 0: op = rv_op_fadd_s; break;
             case 1: op = rv_op_fadd_d; break;
             case 3: op = rv_op_fadd_q; break;
             case 4: op = rv_op_fsub_s; break;
             case 5: op = rv_op_fsub_d; break;
             case 7: op = rv_op_fsub_q; break;
             case 8: op = rv_op_fmul_s; break;
             case 9: op = rv_op_fmul_d; break;
             case 11: op = rv_op_fmul_q; break;
             case 12: op = rv_op_fdiv_s; break;
             case 13: op = rv_op_fdiv_d; break;
             case 15: op = rv_op_fdiv_q; break;
             case 16:
                 switch (((inst >> 12) & 0b111)) {
                 case 0: op = rv_op_fsgnj_s; break;
                 case 1: op = rv_op_fsgnjn_s; break;
                 case 2: op = rv_op_fsgnjx_s; break;
                 }
                 break;
             case 17:
                 switch (((inst >> 12) & 0b111)) {
                 case 0: op = rv_op_fsgnj_d; break;
                 case 1: op = rv_op_fsgnjn_d; break;
                 case 2: op = rv_op_fsgnjx_d; break;
                 }
                 break;
             case 19:
                 switch (((inst >> 12) & 0b111)) {
                 case 0: op = rv_op_fsgnj_q; break;
                 case 1: op = rv_op_fsgnjn_q; break;
                 case 2: op = rv_op_fsgnjx_q; break;
                 }
                 break;
             case 20:
                 switch (((inst >> 12) & 0b111)) {
                 case 0: op = rv_op_fmin_s; break;
                 case 1: op = rv_op_fmax_s; break;
                 }
                 break;
             case 21:
                 switch (((inst >> 12) & 0b111)) {
                 case 0: op = rv_op_fmin_d; break;
                 case 1: op = rv_op_fmax_d; break;
                 }
                 break;
             case 23:
                 switch (((inst >> 12) & 0b111)) {
                 case 0: op = rv_op_fmin_q; break;
                 case 1: op = rv_op_fmax_q; break;
                 }
                 break;
             case 32:
                 switch (((inst >> 20) & 0b11111)) {
                 case 1: op = rv_op_fcvt_s_d; break;
                 case 3: op = rv_op_fcvt_s_q; break;
                 }
                 break;
             case 33:
                 switch (((inst >> 20) & 0b11111)) {
                 case 0: op = rv_op_fcvt_d_s; break;
                 case 3: op = rv_op_fcvt_d_q; break;
                 }
                 break;
             case 35:
                 switch (((inst >> 20) & 0b11111)) {
                 case 0: op = rv_op_fcvt_q_s; break;
                 case 1: op = rv_op_fcvt_q_d; break;
                 }
                 break;
             case 44:
                 switch (((inst >> 20) & 0b11111)) {
                 case 0: op = rv_op_fsqrt_s; break;
                 }
                 break;
             case 45:
                 switch (((inst >> 20) & 0b11111)) {
                 case 0: op = rv_op_fsqrt_d; break;
                 }
                 break;
             case 47:
                 switch (((inst >> 20) & 0b11111)) {
                 case 0: op = rv_op_fsqrt_q; break;
                 }
                 break;
             case 80:
                 switch (((inst >> 12) & 0b111)) {
                 case 0: op = rv_op_fle_s; break;
                 case 1: op = rv_op_flt_s; break;
                 case 2: op = rv_op_feq_s; break;
                 }
                 break;
             case 81:
                 switch (((inst >> 12) & 0b111)) {
                 case 0: op = rv_op_fle_d; break;
                 case 1: op = rv_op_flt_d; break;
                 case 2: op = rv_op_feq_d; break;
                 }
                 break;
             case 83:
                 switch (((inst >> 12) & 0b111)) {
                 case 0: op = rv_op_fle_q; break;
                 case 1: op = rv_op_flt_q; break;
                 case 2: op = rv_op_feq_q; break;
                 }
                 break;
             case 96:
                 switch (((inst >> 20) & 0b11111)) {
                 case 0: op = rv_op_fcvt_w_s; break;
                 case 1: op = rv_op_fcvt_wu_s; break;
                 case 2: op = rv_op_fcvt_l_s; break;
                 case 3: op = rv_op_fcvt_lu_s; break;
                 }
                 break;
             case 97:
                 switch (((inst >> 20) & 0b11111)) {
                 case 0: op = rv_op_fcvt_w_d; break;
                 case 1: op = rv_op_fcvt_wu_d; break;
                 case 2: op = rv_op_fcvt_l_d; break;
                 case 3: op = rv_op_fcvt_lu_d; break;
                 }
                 break;
             case 99:
                 switch (((inst >> 20) & 0b11111)) {
                 case 0: op = rv_op_fcvt_w_q; break;
                 case 1: op = rv_op_fcvt_wu_q; break;
                 case 2: op = rv_op_fcvt_l_q; break;
                 case 3: op = rv_op_fcvt_lu_q; break;
                 }
                 break;
             case 104:
                 switch (((inst >> 20) & 0b11111)) {
                 case 0: op = rv_op_fcvt_s_w; break;
                 case 1: op = rv_op_fcvt_s_wu; break;
                 case 2: op = rv_op_fcvt_s_l; break;
                 case 3: op = rv_op_fcvt_s_lu; break;
                 }
                 break;
             case 105:
                 switch (((inst >> 20) & 0b11111)) {
                 case 0: op = rv_op_fcvt_d_w; break;
                 case 1: op = rv_op_fcvt_d_wu; break;
                 case 2: op = rv_op_fcvt_d_l; break;
                 case 3: op = rv_op_fcvt_d_lu; break;
                 }
                 break;
             case 107:
                 switch (((inst >> 20) & 0b11111)) {
                 case 0: op = rv_op_fcvt_q_w; break;
                 case 1: op = rv_op_fcvt_q_wu; break;
                 case 2: op = rv_op_fcvt_q_l; break;
                 case 3: op = rv_op_fcvt_q_lu; break;
                 }
                 break;
             case 112:
                 switch (((inst >> 17) & 0b11111000) | ((inst >> 12) & 0b00000111)) {
                 case 0: op = rv_op_fmv_x_s; break;
                 case 1: op = rv_op_fclass_s; break;
                 }
                 break;
             case 113:
                 switch (((inst >> 17) & 0b11111000) | ((inst >> 12) & 0b00000111)) {
                 case 0: op = rv_op_fmv_x_d; break;
                 case 1: op = rv_op_fclass_d; break;
                 }
                 break;
             case 115:
                 switch (((inst >> 17) & 0b11111000) | ((inst >> 12) & 0b00000111)) {
                 case 0: op = rv_op_fmv_x_q; break;
                 case 1: op = rv_op_fclass_q; break;
                 }
                 break;
             case 120:
                 switch (((inst >> 17) & 0b11111000) | ((inst >> 12) & 0b00000111)) {
                 case 0: op = rv_op_fmv_s_x; break;
                 }
                 break;
             case 121:
                 switch (((inst >> 17) & 0b11111000) | ((inst >> 12) & 0b00000111)) {
                 case 0: op = rv_op_fmv_d_x; break;
                 }
                 break;
             case 123:
                 switch (((inst >> 17) & 0b11111000) | ((inst >> 12) & 0b00000111)) {
                 case 0: op = rv_op_fmv_q_x; break;
                 }
                 break;
             }
             break;
         case 22:
             if (isa == rv128) {
                 switch (((inst >> 12) & 0b111)) {
                 case 0:
                     op = rv_op_addid;
                     break;
                 case 1:
                     switch (((inst >> 26) & 0b111111)) {
                     case 0:
                         op = rv_op_sllid;
                         break;
                     }
                     break;
                 case 5:
                     switch (((inst >> 26) & 0b111111)) {
                     case 0:
                         op = rv_op_srlid;
                         break;
                     case 16:
                         op = rv_op_sraid;
                         break;
                     }
                     break;
                 }
             } else if (flags & RISCV_DIS_FLAG_CHERI) {
                 // CHERI instructions:
                 switch (((inst >> 12) & 0b111)) {
                 case 0:
                     op = decode_cheri_inst(inst);
                     break;
                 case 1:
                     op = rv_op_cincoffsetimm;
                     break;
                 case 2:
                     op = rv_op_csetboundsimm;
                     break;
                 }
             }
             break;
         case 24:
             switch (((inst >> 12) & 0b111)) {
             case 0: op = rv_op_beq; break;
             case 1: op = rv_op_bne; break;
             case 4: op = rv_op_blt; break;
             case 5: op = rv_op_bge; break;
             case 6: op = rv_op_bltu; break;
             case 7: op = rv_op_bgeu; break;
             }
             break;
         case 25:
             switch (((inst >> 12) & 0b111)) {
             case 0: op = rv_op_jalr; break;
             }
             break;
         case 27: op = rv_op_jal; break;
         case 28:
             switch (((inst >> 12) & 0b111)) {
             case 0:
                 switch (((inst >> 20) & 0b111111100000) | ((inst >> 7) & 0b000000011111)) {
                 case 0:
                     switch (((inst >> 15) & 0b1111111111)) {
                     case 0: op = rv_op_ecall; break;
                     case 32: op = rv_op_ebreak; break;
                     case 64: op = rv_op_uret; break;
                     }
                     break;
                 case 256:
                     switch (((inst >> 20) & 0b11111)) {
                     case 2:
                         switch (((inst >> 15) & 0b11111)) {
                         case 0: op = rv_op_sret; break;
                         }
                         break;
                     case 4: op = rv_op_sfence_vm; break;
                     case 5:
                         switch (((inst >> 15) & 0b11111)) {
                         case 0: op = rv_op_wfi; break;
                         }
                         break;
                     }
                     break;
                 case 288: op = rv_op_sfence_vma; break;
                 case 512:
                     switch (((inst >> 15) & 0b1111111111)) {
                     case 64: op = rv_op_hret; break;
                     }
                     break;
                 case 768:
                     switch (((inst >> 15) & 0b1111111111)) {
                     case 64: op = rv_op_mret; break;
                     }
                     break;
                 case 1952:
                     switch (((inst >> 15) & 0b1111111111)) {
                     case 576: op = rv_op_dret; break;
                     }
                     break;
                 }
                 break;
             case 1: op = rv_op_csrrw; break;
             case 2: op = rv_op_csrrs; break;
             case 3: op = rv_op_csrrc; break;
             case 5: op = rv_op_csrrwi; break;
             case 6: op = rv_op_csrrsi; break;
             case 7: op = rv_op_csrrci; break;
             }
             break;
         case 30:
             switch (((inst >> 22) & 0b1111111000) | ((inst >> 12) & 0b0000000111)) {
             case 0: op = rv_op_addd; break;
             case 1: op = rv_op_slld; break;
             case 5: op = rv_op_srld; break;
             case 8: op = rv_op_muld; break;
             case 12: op = rv_op_divd; break;
             case 13: op = rv_op_divud; break;
             case 14: op = rv_op_remd; break;
             case 15: op = rv_op_remud; break;
             case 256: op = rv_op_subd; break;
             case 261: op = rv_op_srad; break;
             }
             break;
         }
         break;
     }
     dec->op = op;
 }
 

 /* operand extractors */
 
  uint32_t operand_rd(rv_inst inst)
 {
     return (inst << 52) >> 59;
 }
 
  uint32_t operand_rs1(rv_inst inst)
 {
     return (inst << 44) >> 59;
 }
 
  uint32_t operand_rs2(rv_inst inst)
 {
     return (inst << 39) >> 59;
 }
 
  uint32_t operand_rs3(rv_inst inst)
 {
     return (inst << 32) >> 59;
 }
 
  uint32_t operand_aq(rv_inst inst)
 {
     return (inst << 37) >> 63;
 }
 
  uint32_t operand_rl(rv_inst inst)
 {
     return (inst << 38) >> 63;
 }
 
  uint32_t operand_pred(rv_inst inst)
 {
     return (inst << 36) >> 60;
 }
 
  uint32_t operand_succ(rv_inst inst)
 {
     return (inst << 40) >> 60;
 }
 
  uint32_t operand_rm(rv_inst inst)
 {
     return (inst << 49) >> 61;
 }
 
  uint32_t operand_shamt5(rv_inst inst)
 {
     return (inst << 39) >> 59;
 }
 
  uint32_t operand_shamt6(rv_inst inst)
 {
     return (inst << 38) >> 58;
 }
 
  uint32_t operand_shamt7(rv_inst inst)
 {
     return (inst << 37) >> 57;
 }
 
  uint32_t operand_crdq(rv_inst inst)
 {
     return (inst << 59) >> 61;
 }
 
  uint32_t operand_crs1q(rv_inst inst)
 {
     return (inst << 54) >> 61;
 }
 
  uint32_t operand_crs1rdq(rv_inst inst)
 {
     return (inst << 54) >> 61;
 }
 
  uint32_t operand_crs2q(rv_inst inst)
 {
     return (inst << 59) >> 61;
 }
 
  uint32_t operand_crd(rv_inst inst)
 {
     return (inst << 52) >> 59;
 }
 
  uint32_t operand_crs1(rv_inst inst)
 {
     return (inst << 52) >> 59;
 }
 
  uint32_t operand_crs1rd(rv_inst inst)
 {
     return (inst << 52) >> 59;
 }
 
  uint32_t operand_crs2(rv_inst inst)
 {
     return (inst << 57) >> 59;
 }
 
  uint32_t operand_cimmsh5(rv_inst inst)
 {
     return (inst << 57) >> 59;
 }
 
  uint32_t operand_csr12(rv_inst inst)
 {
     return (inst << 32) >> 52;
 }
 
  int32_t operand_imm12(rv_inst inst)
 {
     return ((int64_t)inst << 32) >> 52;
 }
 
  int32_t operand_imm20(rv_inst inst)
 {
     return (((int64_t)inst << 32) >> 44) << 12;
 }
 
  int32_t operand_jimm20(rv_inst inst)
 {
     return (((int64_t)inst << 32) >> 63) << 20 |
         ((inst << 33) >> 54) << 1 |
         ((inst << 43) >> 63) << 11 |
         ((inst << 44) >> 56) << 12;
 }
 
  int32_t operand_simm12(rv_inst inst)
 {
     return (((int64_t)inst << 32) >> 57) << 5 |
         (inst << 52) >> 59;
 }
 
  int32_t operand_sbimm12(rv_inst inst)
 {
     return (((int64_t)inst << 32) >> 63) << 12 |
         ((inst << 33) >> 58) << 5 |
         ((inst << 52) >> 60) << 1 |
         ((inst << 56) >> 63) << 11;
 }
 
  uint32_t operand_cimmsh6(rv_inst inst)
 {
     return ((inst << 51) >> 63) << 5 |
         (inst << 57) >> 59;
 }
 
  int32_t operand_cimmi(rv_inst inst)
 {
     return (((int64_t)inst << 51) >> 63) << 5 |
         (inst << 57) >> 59;
 }
 
  int32_t operand_cimmui(rv_inst inst)
 {
     return (((int64_t)inst << 51) >> 63) << 17 |
         ((inst << 57) >> 59) << 12;
 }
 
  uint32_t operand_cimmlwsp(rv_inst inst)
 {
     return ((inst << 51) >> 63) << 5 |
         ((inst << 57) >> 61) << 2 |
         ((inst << 60) >> 62) << 6;
 }
 
  uint32_t operand_cimmldsp(rv_inst inst)
 {
     return ((inst << 51) >> 63) << 5 |
         ((inst << 57) >> 62) << 3 |
         ((inst << 59) >> 61) << 6;
 }
 
  uint32_t operand_cimmlqsp(rv_inst inst)
 {
     return ((inst << 51) >> 63) << 5 |
         ((inst << 57) >> 63) << 4 |
         ((inst << 58) >> 60) << 6;
 }
 
  int32_t operand_cimm16sp(rv_inst inst)
 {
     return (((int64_t)inst << 51) >> 63) << 9 |
         ((inst << 57) >> 63) << 4 |
         ((inst << 58) >> 63) << 6 |
         ((inst << 59) >> 62) << 7 |
         ((inst << 61) >> 63) << 5;
 }
 
  int32_t operand_cimmj(rv_inst inst)
 {
     return (((int64_t)inst << 51) >> 63) << 11 |
         ((inst << 52) >> 63) << 4 |
         ((inst << 53) >> 62) << 8 |
         ((inst << 55) >> 63) << 10 |
         ((inst << 56) >> 63) << 6 |
         ((inst << 57) >> 63) << 7 |
         ((inst << 58) >> 61) << 1 |
         ((inst << 61) >> 63) << 5;
 }
 
  int32_t operand_cimmb(rv_inst inst)
 {
     return (((int64_t)inst << 51) >> 63) << 8 |
         ((inst << 52) >> 62) << 3 |
         ((inst << 57) >> 62) << 6 |
         ((inst << 59) >> 62) << 1 |
         ((inst << 61) >> 63) << 5;
 }
 
  uint32_t operand_cimmswsp(rv_inst inst)
 {
     return ((inst << 51) >> 60) << 2 |
         ((inst << 55) >> 62) << 6;
 }
 
  uint32_t operand_cimmsdsp(rv_inst inst)
 {
     return ((inst << 51) >> 61) << 3 |
         ((inst << 54) >> 61) << 6;
 }
 
  uint32_t operand_cimmsqsp(rv_inst inst)
 {
     return ((inst << 51) >> 62) << 4 |
         ((inst << 53) >> 60) << 6;
 }
 
  uint32_t operand_cimm4spn(rv_inst inst)
 {
     return ((inst << 51) >> 62) << 4 |
         ((inst << 53) >> 60) << 6 |
         ((inst << 57) >> 63) << 2 |
         ((inst << 58) >> 63) << 3;
 }
 
  uint32_t operand_cimmw(rv_inst inst)
 {
     return ((inst << 51) >> 61) << 3 |
         ((inst << 57) >> 63) << 2 |
         ((inst << 58) >> 63) << 6;
 }
 
  uint32_t operand_cimmd(rv_inst inst)
 {
     return ((inst << 51) >> 61) << 3 |
         ((inst << 57) >> 62) << 6;
 }
 
  uint32_t operand_cimmq(rv_inst inst)
 {
     return ((inst << 51) >> 62) << 4 |
         ((inst << 53) >> 63) << 8 |
         ((inst << 57) >> 62) << 6;
 }

 /* decode operands */
 
  void decode_inst_operands(rv_decode *dec)
 {
     rv_inst inst = dec->inst;
     dec->codec = opcode_data[dec->op].codec;
     switch (dec->codec) {
     case rv_codec_none:
         dec->rd = dec->rs1 = dec->rs2 = rv_ireg_zero;
         dec->imm = 0;
         break;
     case rv_codec_u:
         dec->rd = operand_rd(inst);
         dec->rs1 = dec->rs2 = rv_ireg_zero;
         dec->imm = operand_imm20(inst);
         break;
     case rv_codec_uj:
         dec->rd = operand_rd(inst);
         dec->rs1 = dec->rs2 = rv_ireg_zero;
         dec->imm = operand_jimm20(inst);
         break;
     case rv_codec_i:
         dec->rd = operand_rd(inst);
         dec->rs1 = operand_rs1(inst);
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_imm12(inst);
         break;
     case rv_codec_i_sh5:
         dec->rd = operand_rd(inst);
         dec->rs1 = operand_rs1(inst);
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_shamt5(inst);
         break;
     case rv_codec_i_sh6:
         dec->rd = operand_rd(inst);
         dec->rs1 = operand_rs1(inst);
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_shamt6(inst);
         break;
     case rv_codec_i_sh7:
         dec->rd = operand_rd(inst);
         dec->rs1 = operand_rs1(inst);
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_shamt7(inst);
         break;
     case rv_codec_i_csr:
         dec->rd = operand_rd(inst);
         dec->rs1 = operand_rs1(inst);
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_csr12(inst);
         break;
     case rv_codec_s:
         dec->rd = rv_ireg_zero;
         dec->rs1 = operand_rs1(inst);
         dec->rs2 = operand_rs2(inst);
         dec->imm = operand_simm12(inst);
         break;
     case rv_codec_sb:
         dec->rd = rv_ireg_zero;
         dec->rs1 = operand_rs1(inst);
         dec->rs2 = operand_rs2(inst);
         dec->imm = operand_sbimm12(inst);
         break;
     case rv_codec_r:
         dec->rd = operand_rd(inst);
         dec->rs1 = operand_rs1(inst);
         dec->rs2 = operand_rs2(inst);
         dec->imm = 0;
         break;
     case rv_codec_r_m:
         dec->rd = operand_rd(inst);
         dec->rs1 = operand_rs1(inst);
         dec->rs2 = operand_rs2(inst);
         dec->imm = 0;
         dec->rm = operand_rm(inst);
         break;
     case rv_codec_r4_m:
         dec->rd = operand_rd(inst);
         dec->rs1 = operand_rs1(inst);
         dec->rs2 = operand_rs2(inst);
         dec->rs3 = operand_rs3(inst);
         dec->imm = 0;
         dec->rm = operand_rm(inst);
         break;
     case rv_codec_r_a:
         dec->rd = operand_rd(inst);
         dec->rs1 = operand_rs1(inst);
         dec->rs2 = operand_rs2(inst);
         dec->imm = 0;
         dec->aq = operand_aq(inst);
         dec->rl = operand_rl(inst);
         break;
     case rv_codec_r_l:
         dec->rd = operand_rd(inst);
         dec->rs1 = operand_rs1(inst);
         dec->rs2 = rv_ireg_zero;
         dec->imm = 0;
         dec->aq = operand_aq(inst);
         dec->rl = operand_rl(inst);
         break;
     case rv_codec_r_f:
         dec->rd = dec->rs1 = dec->rs2 = rv_ireg_zero;
         dec->pred = operand_pred(inst);
         dec->succ = operand_succ(inst);
         dec->imm = 0;
         break;
     case rv_codec_cb:
         dec->rd = rv_ireg_zero;
         dec->rs1 = operand_crs1q(inst) + 8;
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmb(inst);
         break;
     case rv_codec_cb_imm:
         dec->rd = dec->rs1 = operand_crs1rdq(inst) + 8;
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmi(inst);
         break;
     case rv_codec_cb_sh5:
         dec->rd = dec->rs1 = operand_crs1rdq(inst) + 8;
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmsh5(inst);
         break;
     case rv_codec_cb_sh6:
         dec->rd = dec->rs1 = operand_crs1rdq(inst) + 8;
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmsh6(inst);
         break;
     case rv_codec_ci:
         dec->rd = dec->rs1 = operand_crs1rd(inst);
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmi(inst);
         break;
     case rv_codec_ci_sh5:
         dec->rd = dec->rs1 = operand_crs1rd(inst);
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmsh5(inst);
         break;
     case rv_codec_ci_sh6:
         dec->rd = dec->rs1 = operand_crs1rd(inst);
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmsh6(inst);
         break;
     case rv_codec_ci_16sp:
         dec->rd = rv_ireg_sp;
         dec->rs1 = rv_ireg_sp;
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimm16sp(inst);
         break;
     case rv_codec_ci_lwsp:
         dec->rd = operand_crd(inst);
         dec->rs1 = rv_ireg_sp;
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmlwsp(inst);
         break;
     case rv_codec_ci_ldsp:
         dec->rd = operand_crd(inst);
         dec->rs1 = rv_ireg_sp;
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmldsp(inst);
         break;
     case rv_codec_ci_lqsp:
         dec->rd = operand_crd(inst);
         dec->rs1 = rv_ireg_sp;
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmlqsp(inst);
         break;
     case rv_codec_ci_li:
         dec->rd = operand_crd(inst);
         dec->rs1 = rv_ireg_zero;
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmi(inst);
         break;
     case rv_codec_ci_lui:
         dec->rd = operand_crd(inst);
         dec->rs1 = rv_ireg_zero;
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmui(inst);
         break;
     case rv_codec_ci_none:
         dec->rd = dec->rs1 = dec->rs2 = rv_ireg_zero;
         dec->imm = 0;
         break;
     case rv_codec_ciw_4spn:
         dec->rd = operand_crdq(inst) + 8;
         dec->rs1 = rv_ireg_sp;
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimm4spn(inst);
         break;
     case rv_codec_cj:
         dec->rd = dec->rs1 = dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmj(inst);
         break;
     case rv_codec_cj_jal:
         dec->rd = rv_ireg_ra;
         dec->rs1 = dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmj(inst);
         break;
     case rv_codec_cl_lw:
         dec->rd = operand_crdq(inst) + 8;
         dec->rs1 = operand_crs1q(inst) + 8;
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmw(inst);
         break;
     case rv_codec_cl_ld:
         dec->rd = operand_crdq(inst) + 8;
         dec->rs1 = operand_crs1q(inst) + 8;
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmd(inst);
         break;
     case rv_codec_cl_lq:
         dec->rd = operand_crdq(inst) + 8;
         dec->rs1 = operand_crs1q(inst) + 8;
         dec->rs2 = rv_ireg_zero;
         dec->imm = operand_cimmq(inst);
         break;
     case rv_codec_cr:
         dec->rd = dec->rs1 = operand_crs1rd(inst);
         dec->rs2 = operand_crs2(inst);
         dec->imm = 0;
         break;
     case rv_codec_cr_mv:
         dec->rd = operand_crd(inst);
         dec->rs1 = operand_crs2(inst);
         dec->rs2 = rv_ireg_zero;
         dec->imm = 0;
         break;
     case rv_codec_cr_jalr:
         dec->rd = rv_ireg_ra;
         dec->rs1 = operand_crs1(inst);
         dec->rs2 = rv_ireg_zero;
         dec->imm = 0;
         break;
     case rv_codec_cr_jr:
         dec->rd = rv_ireg_zero;
         dec->rs1 = operand_crs1(inst);
         dec->rs2 = rv_ireg_zero;
         dec->imm = 0;
         break;
     case rv_codec_cs:
         dec->rd = dec->rs1 = operand_crs1rdq(inst) + 8;
         dec->rs2 = operand_crs2q(inst) + 8;
         dec->imm = 0;
         break;
     case rv_codec_cs_sw:
         dec->rd = rv_ireg_zero;
         dec->rs1 = operand_crs1q(inst) + 8;
         dec->rs2 = operand_crs2q(inst) + 8;
         dec->imm = operand_cimmw(inst);
         break;
     case rv_codec_cs_sd:
         dec->rd = rv_ireg_zero;
         dec->rs1 = operand_crs1q(inst) + 8;
         dec->rs2 = operand_crs2q(inst) + 8;
         dec->imm = operand_cimmd(inst);
         break;
     case rv_codec_cs_sq:
         dec->rd = rv_ireg_zero;
         dec->rs1 = operand_crs1q(inst) + 8;
         dec->rs2 = operand_crs2q(inst) + 8;
         dec->imm = operand_cimmq(inst);
         break;
     case rv_codec_css_swsp:
         dec->rd = rv_ireg_zero;
         dec->rs1 = rv_ireg_sp;
         dec->rs2 = operand_crs2(inst);
         dec->imm = operand_cimmswsp(inst);
         break;
     case rv_codec_css_sdsp:
         dec->rd = rv_ireg_zero;
         dec->rs1 = rv_ireg_sp;
         dec->rs2 = operand_crs2(inst);
         dec->imm = operand_cimmsdsp(inst);
         break;
     case rv_codec_css_sqsp:
         dec->rd = rv_ireg_zero;
         dec->rs1 = rv_ireg_sp;
         dec->rs2 = operand_crs2(inst);
         dec->imm = operand_cimmsqsp(inst);
         break;
     };
}

uint8_t count_register_operands(uint8_t codec) {
    switch (codec) {
    case rv_codec_none:
    case rv_codec_r_f:
    case rv_codec_ci_none:
    case rv_codec_cj:
        return 0;

    case rv_codec_u:
    case rv_codec_uj:
    case rv_codec_cb:
    case rv_codec_ci:
    case rv_codec_ci_sh5:
    case rv_codec_ci_sh6:
    case rv_codec_ci_16sp:
    case rv_codec_ci_lwsp:
    case rv_codec_ci_ldsp:
    case rv_codec_ci_lqsp:
    case rv_codec_ci_li:
    case rv_codec_ci_lui:
    case rv_codec_ciw_4spn:
    case rv_codec_cj_jal:
    case rv_codec_cr_jalr:
    case rv_codec_cr_jr:
    case rv_codec_css_swsp:
    case rv_codec_css_sdsp:
    case rv_codec_css_sqsp:
        return 1;

    case rv_codec_i:
    case rv_codec_i_sh5:
    case rv_codec_i_sh6:
    case rv_codec_i_sh7:
    case rv_codec_i_csr:
    case rv_codec_s:
    case rv_codec_sb:
    case rv_codec_r_l:
    case rv_codec_cb_imm:
    case rv_codec_cb_sh5:
    case rv_codec_cb_sh6:
    case rv_codec_cl_lw:
    case rv_codec_cl_ld:
    case rv_codec_cl_lq:
    case rv_codec_cr:
    case rv_codec_cr_mv:
    case rv_codec_cs:
    case rv_codec_cs_sw:
    case rv_codec_cs_sd:
    case rv_codec_cs_sq:
        return 2;

    case rv_codec_r:
    case rv_codec_r_m:
    case rv_codec_r_a:
        return 3;

    case rv_codec_r4_m:
        return 4;

    default:
        return 55;  //error
    }
}



/* decompress instruction */

 void decode_inst_decompress_rv32(rv_decode *dec)
{
    int decomp_op = opcode_data[dec->op].decomp_rv32;
    if (decomp_op != rv_op_illegal) {
        if ((opcode_data[dec->op].decomp_data & rvcd_imm_nz)
            && dec->imm == 0) {
            dec->op = rv_op_illegal;
        } else {
            dec->op = decomp_op;
            dec->codec = opcode_data[decomp_op].codec;
        }
    }
}

 void decode_inst_decompress_rv64(rv_decode *dec)
{
    int decomp_op = opcode_data[dec->op].decomp_rv64;
    if (decomp_op != rv_op_illegal) {
        if ((opcode_data[dec->op].decomp_data & rvcd_imm_nz)
            && dec->imm == 0) {
            dec->op = rv_op_illegal;
        } else {
            dec->op = decomp_op;
            dec->codec = opcode_data[decomp_op].codec;
        }
    }
}

 void decode_inst_decompress_rv128(rv_decode *dec)
{
    int decomp_op = opcode_data[dec->op].decomp_rv128;
    if (decomp_op != rv_op_illegal) {
        if ((opcode_data[dec->op].decomp_data & rvcd_imm_nz)
            && dec->imm == 0) {
            dec->op = rv_op_illegal;
        } else {
            dec->op = decomp_op;
            dec->codec = opcode_data[decomp_op].codec;
        }
    }
}

 void decode_inst_decompress(rv_decode *dec, rv_isa isa)
{
    switch (isa) {
    case rv32:
        decode_inst_decompress_rv32(dec);
        break;
    case rv64:
        decode_inst_decompress_rv64(dec);
        break;
    case rv128:
        decode_inst_decompress_rv128(dec);
        break;
    }
}

/* check constraint */

 bool check_constraints(rv_decode *dec, const rvc_constraint *c)
{
    int32_t imm = dec->imm;
    uint8_t rd = dec->rd, rs1 = dec->rs1, rs2 = dec->rs2;
    while (*c != rvc_end) {
        switch (*c) {
        case rvc_rd_eq_ra:
            if (!(rd == 1)) {
                return false;
            }
            break;
        case rvc_rd_eq_x0:
            if (!(rd == 0)) {
                return false;
            }
            break;
        case rvc_rs1_eq_x0:
            if (!(rs1 == 0)) {
                return false;
            }
            break;
        case rvc_rs2_eq_x0:
            if (!(rs2 == 0)) {
                return false;
            }
            break;
        case rvc_rs2_eq_rs1:
            if (!(rs2 == rs1)) {
                return false;
            }
            break;
        case rvc_rs1_eq_ra:
            if (!(rs1 == 1)) {
                return false;
            }
            break;
        case rvc_imm_eq_zero:
            if (!(imm == 0)) {
                return false;
            }
            break;
        case rvc_imm_eq_n1:
            if (!(imm == -1)) {
                return false;
            }
            break;
        case rvc_imm_eq_p1:
            if (!(imm == 1)) {
                return false;
            }
            break;
        case rvc_csr_eq_0x001:
            if (!(imm == 0x001)) {
                return false;
            }
            break;
        case rvc_csr_eq_0x002:
            if (!(imm == 0x002)) {
                return false;
            }
            break;
        case rvc_csr_eq_0x003:
            if (!(imm == 0x003)) {
                return false;
            }
            break;
        case rvc_csr_eq_0xc00:
            if (!(imm == 0xc00)) {
                return false;
            }
            break;
        case rvc_csr_eq_0xc01:
            if (!(imm == 0xc01)) {
                return false;
            }
            break;
        case rvc_csr_eq_0xc02:
            if (!(imm == 0xc02)) {
                return false;
            }
            break;
        case rvc_csr_eq_0xc80:
            if (!(imm == 0xc80)) {
                return false;
            }
            break;
        case rvc_csr_eq_0xc81:
            if (!(imm == 0xc81)) {
                return false;
            }
            break;
        case rvc_csr_eq_0xc82:
            if (!(imm == 0xc82)) {
                return false;
            }
            break;
        default: break;
        }
        c++;
    }
    return true;
}

/* lift instruction to pseudo-instruction */

 void decode_inst_lift_pseudo(rv_decode *dec)
{
    const rv_comp_data *comp_data = opcode_data[dec->op].pseudo;
    if (!comp_data) {
        return;
    }
    while (comp_data->constraints) {
        if (check_constraints(dec, comp_data->constraints)) {
            dec->op = comp_data->op;
            dec->codec = opcode_data[dec->op].codec;
            return;
        }
        comp_data++;
    }
}

rv_decode
disasm_inst(rv_isa isa, uint64_t pc, rv_inst inst, int flags) {
    rv_decode dec = { 0 };
    dec.pc = pc;
    dec.inst = inst;
    decode_inst_opcode(&dec, isa, flags);
    decode_inst_operands(&dec);
    decode_inst_decompress(&dec, isa);
    decode_inst_lift_pseudo(&dec);

    return dec;
}

const char* inst_type_to_str(inst_type_t type) {
    switch (type) {
        case INST_TYPE_UNKNOWN:    return "UNKNOWN";
        case INST_TYPE_BRANCH:     return "BRANCH";
        case INST_TYPE_LOAD:       return "LOAD";
        case INST_TYPE_STORE:      return "STORE";
        case INST_TYPE_CAP_LOAD:   return "CAP_LOAD";
        case INST_TYPE_CAP_STORE:  return "CAP_STORE";
        case INST_TYPE_CAP_OP:     return "CAP_OP";
        case INST_TYPE_ALU:        return "ALU";
        case INST_TYPE_SYSTEM:     return "SYSTEM";
        case INST_TYPE_CSR:        return "CSR";
        case INST_TYPE_ATOMIC:     return "ATOMIC";
        case INST_TYPE_FP:         return "FP";
        case INST_TYPE_FP_LOAD:    return "FP_LOAD";
        case INST_TYPE_FP_STORE:   return "FP_STORE";
        default:                   return "INVALID_TYPE";
    }
}


// Function to classify instruction type
inst_type_t classify_instruction(rv_decode dec) {
    switch (dec.op) {
        // Standard branch instructions
        case rv_op_beq:
        case rv_op_bne:
        case rv_op_blt:
        case rv_op_bge:
        case rv_op_bltu:
        case rv_op_bgeu:
        // Pseudo branch instructions
        case rv_op_beqz:
        case rv_op_bnez:
        case rv_op_blez:
        case rv_op_bgez:
        case rv_op_bltz:
        case rv_op_bgtz:
        case rv_op_ble:
        case rv_op_bleu:
        case rv_op_bgt:
        case rv_op_bgtu:
        // Compressed branches
        case rv_op_c_beqz:
        case rv_op_c_bnez:
        case rv_op_c_j:
        case rv_op_c_jal:
        case rv_op_c_jalr:
        case rv_op_c_jr:
        case rv_op_ret:
        case rv_op_j:
        case rv_op_jal:
        case rv_op_jr:
        case rv_op_jalr:
        // CHERI jalr
        case rv_op_cjalr:
            return INST_TYPE_BRANCH;

 

        // Regular load instructions
        case rv_op_lb:
        case rv_op_lh:
        case rv_op_lw:
        case rv_op_lbu:
        case rv_op_lhu:
        case rv_op_lwu:
        case rv_op_ld:
        case rv_op_ldu:
        case rv_op_lq:
        // Compressed loads
        case rv_op_c_lw:
        case rv_op_c_lwsp:
        case rv_op_c_ld:
        case rv_op_c_ldsp:
        case rv_op_c_lq:
        case rv_op_c_lqsp:
            return INST_TYPE_LOAD;

        // Regular store instructions
        case rv_op_sb:
        case rv_op_sh:
        case rv_op_sw:
        case rv_op_sd:
        case rv_op_sq:
        // Compressed stores
        case rv_op_c_sw:
        case rv_op_c_swsp:
        case rv_op_c_sd:
        case rv_op_c_sdsp:
        case rv_op_c_sq:
        case rv_op_c_sqsp:
            return INST_TYPE_STORE;

        // Capability load instructions
        case rv_op_lc:
        case rv_op_clc:
        case rv_op_clb:
        case rv_op_clbu:
        case rv_op_clh:
        case rv_op_clhu:
        case rv_op_clw:
        case rv_op_clwu:
        case rv_op_cld:
            return INST_TYPE_CAP_LOAD;

        // Capability store instructions
        case rv_op_sc:
        case rv_op_csc:
        case rv_op_csb:
        case rv_op_csh:
        case rv_op_csw:
        case rv_op_csd:
            return INST_TYPE_CAP_STORE;

        // Other capability operations
        case rv_op_auipcc:
        case rv_op_cincoffsetimm:
        case rv_op_csetboundsimm:
        case rv_op_cgetperm:
        case rv_op_cgettype:
        case rv_op_cgetbase:
        case rv_op_cgetlen:
        case rv_op_cgettag:
        case rv_op_cgetsealed:
        case rv_op_cgetoffset:
        case rv_op_cgetflags:
        case rv_op_crrl:
        case rv_op_cram:
        case rv_op_cmove:
        case rv_op_ccleartag:
        case rv_op_cgethigh:
        case rv_op_cgetaddr:
        case rv_op_csealentry:
        case rv_op_cloadtags:
        case rv_op_cspecialrw:
        case rv_op_csetbounds:
        case rv_op_csetboundsexact:
        case rv_op_cseal:
        case rv_op_cunseal:
        case rv_op_candperm:
        case rv_op_csetflags:
        case rv_op_csetoffset:
        case rv_op_csetaddr:
        case rv_op_csethigh:
        case rv_op_cincoffset:
        case rv_op_ctoptr:
        case rv_op_cfromptr:
        case rv_op_csub:
        case rv_op_cbuildcap:
        case rv_op_ccopytype:
        case rv_op_ccseal:
        case rv_op_ctestsubset:
        case rv_op_cseqx:
            return INST_TYPE_CAP_OP;

        // ALU operations (regular)
        case rv_op_lui:
        case rv_op_auipc:
        case rv_op_addi:
        case rv_op_slti:
        case rv_op_sltiu:
        case rv_op_xori:
        case rv_op_ori:
        case rv_op_andi:
        case rv_op_slli:
        case rv_op_srli:
        case rv_op_srai:
        case rv_op_add:
        case rv_op_sub:
        case rv_op_sll:
        case rv_op_slt:
        case rv_op_sltu:
        case rv_op_xor:
        case rv_op_srl:
        case rv_op_sra:
        case rv_op_or:
        case rv_op_and:
        // RV64I ALU operations
        case rv_op_addiw:
        case rv_op_slliw:
        case rv_op_srliw:
        case rv_op_sraiw:
        case rv_op_addw:
        case rv_op_subw:
        case rv_op_sllw:
        case rv_op_srlw:
        case rv_op_sraw:
        // RV128I ALU operations
        case rv_op_addid:
        case rv_op_sllid:
        case rv_op_srlid:
        case rv_op_sraid:
        case rv_op_addd:
        case rv_op_subd:
        case rv_op_slld:
        case rv_op_srld:
        case rv_op_srad:
        // Multiply/Divide operations
        case rv_op_mul:
        case rv_op_mulh:
        case rv_op_mulhsu:
        case rv_op_mulhu:
        case rv_op_div:
        case rv_op_divu:
        case rv_op_rem:
        case rv_op_remu:
        case rv_op_mulw:
        case rv_op_divw:
        case rv_op_divuw:
        case rv_op_remw:
        case rv_op_remuw:
        case rv_op_muld:
        case rv_op_divd:
        case rv_op_divud:
        case rv_op_remd:
        case rv_op_remud:
        // Compressed ALU operations
        case rv_op_c_addi4spn:
        case rv_op_c_addi:
        case rv_op_c_li:
        case rv_op_c_addi16sp:
        case rv_op_c_lui:
        case rv_op_c_srli:
        case rv_op_c_srai:
        case rv_op_c_andi:
        case rv_op_c_sub:
        case rv_op_c_xor:
        case rv_op_c_or:
        case rv_op_c_and:
        case rv_op_c_subw:
        case rv_op_c_addw:
        case rv_op_c_slli:
        case rv_op_c_mv:
        case rv_op_c_add:
        case rv_op_c_addiw:
        // Pseudo operations
        case rv_op_nop:
        case rv_op_mv:
        case rv_op_not:
        case rv_op_neg:
        case rv_op_negw:
        case rv_op_sext_w:
        case rv_op_seqz:
        case rv_op_snez:
        case rv_op_sltz:
        case rv_op_sgtz:
            return INST_TYPE_ALU;

        // System instructions
        case rv_op_fence:
        case rv_op_fence_i:
        case rv_op_ecall:
        case rv_op_ebreak:
        case rv_op_uret:
        case rv_op_sret:
        case rv_op_hret:
        case rv_op_mret:
        case rv_op_dret:
        case rv_op_sfence_vm:
        case rv_op_sfence_vma:
        case rv_op_wfi:
        case rv_op_c_ebreak:
            return INST_TYPE_SYSTEM;

        // CSR operations
        case rv_op_csrrw:
        case rv_op_csrrs:
        case rv_op_csrrc:
        case rv_op_csrrwi:
        case rv_op_csrrsi:
        case rv_op_csrrci:
        // Pseudo CSR operations
        case rv_op_rdcycle:
        case rv_op_rdtime:
        case rv_op_rdinstret:
        case rv_op_rdcycleh:
        case rv_op_rdtimeh:
        case rv_op_rdinstreth:
        case rv_op_frcsr:
        case rv_op_frrm:
        case rv_op_frflags:
        case rv_op_fscsr:
        case rv_op_fsrm:
        case rv_op_fsflags:
        case rv_op_fsrmi:
        case rv_op_fsflagsi:
            return INST_TYPE_CSR;

        // Atomic operations
        case rv_op_lr_w:
        case rv_op_sc_w:
        case rv_op_amoswap_w:
        case rv_op_amoadd_w:
        case rv_op_amoxor_w:
        case rv_op_amoor_w:
        case rv_op_amoand_w:
        case rv_op_amomin_w:
        case rv_op_amomax_w:
        case rv_op_amominu_w:
        case rv_op_amomaxu_w:
        case rv_op_lr_d:
        case rv_op_sc_d:
        case rv_op_amoswap_d:
        case rv_op_amoadd_d:
        case rv_op_amoxor_d:
        case rv_op_amoor_d:
        case rv_op_amoand_d:
        case rv_op_amomin_d:
        case rv_op_amomax_d:
        case rv_op_amominu_d:
        case rv_op_amomaxu_d:
        case rv_op_lr_q:
        case rv_op_sc_q:
        case rv_op_amoswap_q:
        case rv_op_amoadd_q:
        case rv_op_amoxor_q:
        case rv_op_amoor_q:
        case rv_op_amoand_q:
        case rv_op_amomin_q:
        case rv_op_amomax_q:
        case rv_op_amominu_q:
        case rv_op_amomaxu_q:
            return INST_TYPE_ATOMIC;

        // Floating-point loads
        case rv_op_flw:
        case rv_op_fld:
        case rv_op_flq:
        case rv_op_c_flw:
        case rv_op_c_fld:
        case rv_op_c_flwsp:
        case rv_op_c_fldsp:
        case rv_op_cflw:
        case rv_op_cfld:
            return INST_TYPE_FP_LOAD;

        // Floating-point stores
        case rv_op_fsw:
        case rv_op_fsd:
        case rv_op_fsq:
        case rv_op_c_fsw:
        case rv_op_c_fsd:
        case rv_op_c_fswsp:
        case rv_op_c_fsdsp:
        case rv_op_cfsw:
        case rv_op_cfsd:
            return INST_TYPE_FP_STORE;

        // Floating-point operations
        case rv_op_fmadd_s:
        case rv_op_fmsub_s:
        case rv_op_fnmsub_s:
        case rv_op_fnmadd_s:
        case rv_op_fadd_s:
        case rv_op_fsub_s:
        case rv_op_fmul_s:
        case rv_op_fdiv_s:
        case rv_op_fsgnj_s:
        case rv_op_fsgnjn_s:
        case rv_op_fsgnjx_s:
        case rv_op_fmin_s:
        case rv_op_fmax_s:
        case rv_op_fsqrt_s:
        case rv_op_fle_s:
        case rv_op_flt_s:
        case rv_op_feq_s:
        case rv_op_fcvt_w_s:
        case rv_op_fcvt_wu_s:
        case rv_op_fcvt_s_w:
        case rv_op_fcvt_s_wu:
        case rv_op_fmv_x_s:
        case rv_op_fclass_s:
        case rv_op_fmv_s_x:
        case rv_op_fcvt_l_s:
        case rv_op_fcvt_lu_s:
        case rv_op_fcvt_s_l:
        case rv_op_fcvt_s_lu:
        case rv_op_fmadd_d:
        case rv_op_fmsub_d:
        case rv_op_fnmsub_d:
        case rv_op_fnmadd_d:
        case rv_op_fadd_d:
        case rv_op_fsub_d:
        case rv_op_fmul_d:
        case rv_op_fdiv_d:
        case rv_op_fsgnj_d:
        case rv_op_fsgnjn_d:
        case rv_op_fsgnjx_d:
        case rv_op_fmin_d:
        case rv_op_fmax_d:
        case rv_op_fcvt_s_d:
        case rv_op_fcvt_d_s:
        case rv_op_fsqrt_d:
        case rv_op_fle_d:
        case rv_op_flt_d:
        case rv_op_feq_d:
        case rv_op_fcvt_w_d:
        case rv_op_fcvt_wu_d:
        case rv_op_fcvt_d_w:
        case rv_op_fcvt_d_wu:
        case rv_op_fclass_d:
        case rv_op_fcvt_l_d:
        case rv_op_fcvt_lu_d:
        case rv_op_fmv_x_d:
        case rv_op_fcvt_d_l:
        case rv_op_fcvt_d_lu:
        case rv_op_fmv_d_x:
        case rv_op_fmadd_q:
        case rv_op_fmsub_q:
        case rv_op_fnmsub_q:
        case rv_op_fnmadd_q:
        case rv_op_fadd_q:
        case rv_op_fsub_q:
        case rv_op_fmul_q:
        case rv_op_fdiv_q:
        case rv_op_fsgnj_q:
        case rv_op_fsgnjn_q:
        case rv_op_fsgnjx_q:
        case rv_op_fmin_q:
        case rv_op_fmax_q:
        case rv_op_fcvt_s_q:
        case rv_op_fcvt_q_s:
        case rv_op_fcvt_d_q:
        case rv_op_fcvt_q_d:
        case rv_op_fsqrt_q:
        case rv_op_fle_q:
        case rv_op_flt_q:
        case rv_op_feq_q:
        case rv_op_fcvt_w_q:
        case rv_op_fcvt_wu_q:
        case rv_op_fcvt_q_w:
        case rv_op_fcvt_q_wu:
        case rv_op_fclass_q:
        case rv_op_fcvt_l_q:
        case rv_op_fcvt_lu_q:
        case rv_op_fcvt_q_l:
        case rv_op_fcvt_q_lu:
        case rv_op_fmv_x_q:
        case rv_op_fmv_q_x:
        // Pseudo floating-point operations
        case rv_op_fmv_s:
        case rv_op_fabs_s:
        case rv_op_fneg_s:
        case rv_op_fmv_d:
        case rv_op_fabs_d:
        case rv_op_fneg_d:
        case rv_op_fmv_q:
        case rv_op_fabs_q:
        case rv_op_fneg_q:
            return INST_TYPE_FP;

        // Unknown or illegal instruction
        case rv_op_illegal: 
        default:
            return INST_TYPE_UNKNOWN;
    }
}


uint8_t get_memory_access_size(rv_decode dec) {
    switch (dec.op) {
        // 1-byte accesses
        case rv_op_lb:
        case rv_op_lbu:
        case rv_op_sb:
        case rv_op_clb:
        case rv_op_clbu:
        case rv_op_csb:
            return 1;
            
        // 2-byte accesses
        case rv_op_lh:
        case rv_op_lhu:
        case rv_op_sh:
        case rv_op_clh:
        case rv_op_clhu:
        case rv_op_csh:
            return 2;
            
        // 4-byte accesses
        case rv_op_lw:
        case rv_op_lwu:
        case rv_op_sw:
        case rv_op_c_lw:
        case rv_op_c_lwsp:
        case rv_op_c_sw:
        case rv_op_c_swsp:
        case rv_op_clw:
        case rv_op_clwu:
        case rv_op_csw:
        case rv_op_flw:
        case rv_op_fsw:
        case rv_op_c_flw:
        case rv_op_c_flwsp:
        case rv_op_c_fsw:
        case rv_op_c_fswsp:
        case rv_op_cflw:
        case rv_op_cfsw:
        // 4-byte atomics
        case rv_op_lr_w:
        case rv_op_sc_w:
        case rv_op_amoswap_w:
        case rv_op_amoadd_w:
        case rv_op_amoxor_w:
        case rv_op_amoor_w:
        case rv_op_amoand_w:
        case rv_op_amomin_w:
        case rv_op_amomax_w:
        case rv_op_amominu_w:
        case rv_op_amomaxu_w:
            return 4;
            
        // 8-byte accesses
        case rv_op_ld:
        case rv_op_ldu:
        case rv_op_sd:
        case rv_op_c_ld:
        case rv_op_c_ldsp:
        case rv_op_c_sd:
        case rv_op_c_sdsp:
        case rv_op_cld:
        case rv_op_csd:
        case rv_op_fld:
        case rv_op_fsd:
        case rv_op_c_fld:
        case rv_op_c_fldsp:
        case rv_op_c_fsd:
        case rv_op_c_fsdsp:
        case rv_op_cfld:
        case rv_op_cfsd:
        // 8-byte atomics
        case rv_op_lr_d:
        case rv_op_sc_d:
        case rv_op_amoswap_d:
        case rv_op_amoadd_d:
        case rv_op_amoxor_d:
        case rv_op_amoor_d:
        case rv_op_amoand_d:
        case rv_op_amomin_d:
        case rv_op_amomax_d:
        case rv_op_amominu_d:
        case rv_op_amomaxu_d:
            return 8;
            
        // 16-byte accesses (capabilities and quad-word)
        case rv_op_lc:
        case rv_op_sc:
        case rv_op_clc:
        case rv_op_csc:
        case rv_op_lq:
        case rv_op_sq:
        case rv_op_c_lq:
        case rv_op_c_lqsp:
        case rv_op_c_sq:
        case rv_op_c_sqsp:
        case rv_op_flq:
        case rv_op_fsq:
        // 16-byte atomics
        case rv_op_lr_q:
        case rv_op_sc_q:
        case rv_op_amoswap_q:
        case rv_op_amoadd_q:
        case rv_op_amoxor_q:
        case rv_op_amoor_q:
        case rv_op_amoand_q:
        case rv_op_amomin_q:
        case rv_op_amomax_q:
        case rv_op_amominu_q:
        case rv_op_amomaxu_q:
            return 16;
            
        default:
            return 0; // Not a memory instruction
    }
}

bool spans_multiple_cache_lines(uint64_t addr, uint8_t size) {
    
    uint64_t start_line = addr & ~63lu;
    uint64_t end_line = (addr + size - 1) & ~63lu;
    return start_line != end_line;
}