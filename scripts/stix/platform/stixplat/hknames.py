# SPDX-License-Identifier: GPL-2.0-or-later
"""The HK ADC channel names (PL-11h, O6).

Transcribed from the C comments over stixctrl_hk_adc_table
(hw/sparc/grlib_stixctrl.c:650-694): the identifier in each row's
trailing comment, e.g. "DPU_1V5_C" for ch 0. The one exception is ch 8
(ATT_C), whose table row carries no name comment; its name comes from
the same file's protection-line table stixctrl_hk_prot_ch
(grlib_stixctrl.c:828-838). The names ride the additive "label" key of
the instrument.hk_adc.<ch> state leaves (state.py); the self-test
parses the C table and checks this dict covers exactly its non-zero
entries with these names (R10).
"""

HK_NAMES = {
    0: "DPU_1V5_C", 1: "DPU_2V5_C", 2: "DPU_3V3_C", 3: "DPU_SPW_C",
    4: "DET_C",
    8: "ATT_C",                    # stixctrl_hk_prot_ch, not the table
    12: "ATT_V", 16: "HV_17_32_V", 20: "HV_01_16_V",
    24: "DPU_1V5_V", 28: "DPU_2V5_V",
    32: "DET_Q2_T", 33: "DET_Q1_T", 34: "DET_Q4_T", 35: "DET_Q3_T",
    36: "ATT_T", 40: "PSU_T", 44: "DPU_2V9_V",
    48: "SPW0_V", 52: "SPW1_V", 56: "DPU_PCB_T", 60: "DPU_FPGA_T",
    64: "ASP_PHOTOA0_V", 65: "ASP_PHOTOA1_V", 66: "ASP_TIM05_T",
    67: "ASP_TIM06_T", 68: "ASP_REF_2V5A_V", 69: "ASP_TIM01_T",
    70: "ASP_TIM03_T", 71: "ASP_VSENSA_V", 72: "ASP_PHOTOB0_V",
    73: "ASP_PHOTOB1_V", 74: "ASP_TIM08_T", 75: "ASP_TIM07_T",
    76: "ASP_REF_2V5B_V", 77: "ASP_TIM02_T", 78: "ASP_TIM04_T",
    79: "ASP_VSENSB_V",
}
