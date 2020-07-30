#ifndef _CPU_CLOCK_
#define _CPU_CLOCK_

#include "typedef.h"

#include "clock_hw.h"

typedef enum {
    ///原生时钟源作系统时钟源
    SYS_CLOCK_INPUT_RC,
    SYS_CLOCK_INPUT_BT_OSC,          //BTOSC 双脚(12-26M)
    SYS_CLOCK_INPUT_RTOSCH,
    SYS_CLOCK_INPUT_RTOSCL,
    SYS_CLOCK_INPUT_PAT,

    ///衍生时钟源作系统时钟源
    SYS_CLOCK_INPUT_PLL_BT_OSC,
    SYS_CLOCK_INPUT_PLL_RTOSCH,
    SYS_CLOCK_INPUT_PLL_PAT,
} SYS_CLOCK_INPUT;

typedef enum {
    SYS_ICLOCK_INPUT_BTOSC,          //BTOSC 双脚(12-26M)
    SYS_ICLOCK_INPUT_RTOSCH,
    SYS_ICLOCK_INPUT_RTOSCL,
    SYS_ICLOCK_INPUT_PAT,
} SYS_ICLOCK_INPUT;

typedef enum {
    PB0_CLOCK_OUTPUT = 0,
    PB0_CLOCK_OUT_BT_OSC,
    PB0_CLOCK_OUT_RTOSCH,
    PB0_CLOCK_OUT_RTOSCL,

    PB0_CLOCK_OUT_LSB = 4,
    PB0_CLOCK_OUT_HSB,
    PB0_CLOCK_OUT_SFC,
    PB0_CLOCK_OUT_PLL,
} PB0_CLK_OUT;

typedef enum {
    PA2_CLOCK_OUTPUT = 0,
    PA2_CLOCK_OUT_RC,
    PA2_CLOCK_OUT_LRC,
    PA2_CLOCK_OUT_RCCL,

    PA2_CLOCK_OUT_BT_LO_D32 = 4,
    PA2_CLOCK_OUT_APC,
    PA2_CLOCK_OUT_PLL320,
    PA2_CLOCK_OUT_PLL107,
} PA2_CLK_OUT;

/*
 * system enter critical and exit critical handle
 * */
struct clock_critical_handler {
    void (*enter)();
    void (*exit)();
};

#define CLOCK_CRITICAL_HANDLE_REG(name, enter, exit) \
	const struct clock_critical_handler clock_##name \
		 SEC_USED(.clock_critical_txt) = {enter, exit};

extern struct clock_critical_handler clock_critical_handler_begin[];
extern struct clock_critical_handler clock_critical_handler_end[];

#define list_for_each_loop_clock_critical(h) \
	for (h=clock_critical_handler_begin; h<clock_critical_handler_end; h++)


int clk_early_init(u8 sys_in, u32 input_freq, u32 out_freq);

int clk_get(const char *name);

int clk_set(const char *name, int clk);

void clock_dump(void);

enum sys_clk {
    SYS_24M,
    SYS_48M,
};

enum clk_mode {
    CLOCK_MODE_ADAPTIVE = 0,
    CLOCK_MODE_USR,
};


//clk : SYS_48M / SYS_24M
void sys_clk_set(u8 clk);

void clk_voltage_init(u8 mode, u16 sys_dvdd);

void clk_set_osc_cap(u8 sel_l, u8 sel_r);

u32 clk_get_osc_cap();

#endif

