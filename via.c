// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// All rights reserved. License: 2-clause BSD

#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <stdlib.h>
#include "via.h"
#include "ps2.h"
#include "memory.h"
//XXX
#include "glue.h"
#include "joystick.h"

#define VIA_IFR_CA2 1
#define VIA_IFR_CA1 2
#define VIA_IFR_SR  4
#define VIA_IFR_CB2 8
#define VIA_IFR_CB1 16
#define VIA_IFR_T2  32
#define VIA_IFR_T1  64

//
// VIA#1
//
// PA0-7 RAM bank
// PB0-2 ROM bank
// PB3   IECATT0
// PB4   IECCLK0
// PB5   IECDAT0
// PB6   IECCLK
// PB7   IECDAT
// CB1   IECSRQ

static uint8_t via1registers[16];

void
via1_init()
{
	srand(time(NULL));

	// default banks are 0
	memory_set_ram_bank(0);
	memory_set_rom_bank(0);
}

void
via1_step()
{
}

bool
via1_get_irq_out()
{
	return false;
}

uint8_t
via1_read(uint8_t reg)
{
	switch (reg) {
		case 0:
			return memory_get_rom_bank(); // PB: ROM bank, IEC
		case 1:
			return memory_get_ram_bank(); // PA: RAM bank
		case 4:
		case 5:
		case 8:
		case 9:
			// timer A and B: return random numbers for RND(0)
			// XXX TODO: these should be real timers :)
			return rand() & 0xff;
		default:
			return via1registers[reg];
	}
}

void
via1_write(uint8_t reg, uint8_t value)
{
	via1registers[reg] = value;
	if (reg == 0) { // PB: ROM bank, IEC
		memory_set_rom_bank(value & 7);
		// TODO: IEC
	} else if (reg == 1) { // PA: RAM bank
		memory_set_ram_bank(value);
	} else {
		// TODO
	}
}

//
// VIA#2
//

static uint8_t via2registers[16];
static uint8_t via2ifr;
static uint8_t via2ier;

static uint8_t via2pa_out;
static uint8_t via2pb_out;
static uint8_t via2pa_pinstate;
static uint8_t via2pb_pinstate;
static uint8_t via2pa_readback;
static uint8_t via2pb_readback;
static uint8_t via2ddra;
static uint8_t via2ddrb;

void
via2_init()
{
	via2ier = 0;

	// DDR to input
	via2ddrb = 0;
	via2ddra = 0;

	ps2_port[0].clk_in = 1;
	ps2_port[0].data_in = 1;
	ps2_port[1].clk_in = 1;
	ps2_port[1].data_in = 1;
}

void
via2_state(uint8_t in, uint8_t out, uint8_t ddr, uint8_t *pinstate, uint8_t *readback)
{
	// driving state (0 = pulled, 1 = passive)
	uint8_t driving = (ddr & out) | ~ddr;

	// mix in internal state (open collector)
	*pinstate = ~(~in | ~driving);

	// value as read on PA register (*out* will read back our own value)
	*readback = (ddr & driving) | (~ddr & *pinstate);
}

void
via2_step()
{
	uint8_t pa_in =
		(ps2_port[0].data_out << 0) | // PA0 PS/2 DAT
		(ps2_port[0].clk_out  << 1) | // PA1 PS/2 CLK
		(1                    << 2) | // PA2 LCD backlight
		(1                    << 3) | // PA3 NESJOY latch (for both joysticks)
		(joystick1_data       << 4) | // PA4 NESJOY joy1 data
		(1                    << 5) | // PA5 NESJOY CLK (for both joysticks)
		(joystick2_data       << 6) | // PA6 NESJOY joy2 data
		(1                    << 7);  // PA7

	via2_state(pa_in, via2pa_out, via2ddra, &via2pa_pinstate, &via2pa_readback);

	ps2_port[0].data_in = !!(via2pa_pinstate >> 0);
	ps2_port[0].clk_in  = !!(via2pa_pinstate >> 1);
	joystick_latch      = !!(via2pa_pinstate >> 3);
	joystick_clock      = !!(via2pa_pinstate >> 5);

	uint8_t pb_in =
		(ps2_port[1].data_out << 0) | // PA0 PS/2 DAT
		(ps2_port[1].clk_out  << 1) | // PA1 PS/2 CLK
		(1                    << 2) | // PA2
		(1                    << 3) | // PA3
		(1                    << 4) | // PA4
		(1                    << 5) | // PA5
		(1                    << 6) | // PA6
		(1                    << 7);  // PA7

	via2_state(pb_in, via2pb_out, via2ddrb, &via2pb_pinstate, &via2pb_readback);

	ps2_port[1].data_in = !!(via2pb_pinstate >> 0);
	ps2_port[1].clk_in  = !!(via2pb_pinstate >> 1);

	static bool old_ca1;
	bool ca1 = ps2_port[0].clk_out;
	if (ca1 != old_ca1) {
		if (!ps2_port[0].clk_in) {
			printf("self-NMI? before: %d, now: %d\n", old_ca1, ca1);
		}
//		printf("KBD IRQ? CLK is now %d\n", ca1);
		if (!ca1) { // falling edge
			printf("NEW KBD IRQ\n");
			via2ifr |= VIA_IFR_CA1;
		}
	}
	old_ca1 = ca1;

//	printf("ps2_port[1].clk_out = %d\n", ps2_port[1].clk_out);
	static bool old_cb1;
	bool cb1 = ps2_port[1].clk_out;
	if (cb1 != old_cb1) {
//		printf("MSE IRQ? CLK is now %d\n", cb1);
		if (!cb1) { // falling edge
			printf("NEW MSE IRQ\n");
			via2ifr |= VIA_IFR_CB1;
		}
	}
	old_cb1 = cb1;
}

bool
via2_get_irq_out()
{
//	if (!!(via2ifr & via2ier)) printf("YYY %d\n", !!(via2ifr & via2ier));
	static int count;
	if (((via2ifr & via2ier) & (VIA_IFR_CA1 | VIA_IFR_CB1)) == (VIA_IFR_CA1 | VIA_IFR_CB1)) {
		printf("BOTH SOURCES!\n");
		count++;
		if (count > 100) {
			printf("BOTHBOTH!\n");
		}
	} else {
		count = 0;
	}
	return !!(via2ifr & via2ier);
}

uint8_t
via2_read(uint8_t reg)
{
	// DDR=0 (input)  -> take input bit
	// DDR=1 (output) -> take output bit

	if (reg == 0) { // PB
		// reading PB clears clear CB1
		if (via2ifr & VIA_IFR_CB1) {
			printf("clearing IRQ\n");
		}
		via2ifr &= ~VIA_IFR_CB1;

		return via2pb_readback;
	} else if (reg == 1) { // PA
		// reading PA clears clear CA1
//		printf("1CLEAR IRQ\n");
		via2ifr &= ~VIA_IFR_CA1;

		return via2pa_readback;
	} else if (reg == 2) { // DDRB
		return via2ddrb;
	} else if (reg == 3) { // DDRA
		return via2ddra;
	} else if (reg == 13) { // IFR
		uint8_t val = via2ifr;
		if (val) {
			val |= 0x80;
		}
		return val;
	} else if (reg == 14) { // IER
		return via2ier;
	} else {
		return via2registers[reg];
	}
}

void
via2_write(uint8_t reg, uint8_t value)
{
	via2registers[reg] = value;

	if (reg == 0) { // PB
		via2pb_out = value;
	} else if (reg == 1) { // PA
		via2pa_out = value;
	} else if (reg == 2) { // DDRB
		via2ddrb = value;
	} else if (reg == 3) { // DDRBA
		via2ddra = value;
	} else if (reg == 13) { // IFR
		// do nothing
	} else if (reg == 14) { // IER
		if (value & 0x80) { // set
			via2ier |= (value & 0x7f);
		} else { // clear
			via2ier &= ~(value & 0x7f);
		}
	}

	// reading PB clears clear CB1
	if (reg == 0) { // PA
		via2ifr &= ~VIA_IFR_CB1;
	}

	// reading PA clears clear CA1
	if (reg == 1) { // PA
//		printf("2CLEAR IRQ\n");
		via2ifr &= ~VIA_IFR_CA1;
	}
}
