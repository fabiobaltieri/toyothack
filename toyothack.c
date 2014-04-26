/*
 * Copyright 2013 Fabio Baltieri <fabio.baltieri@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <curses.h>
#include <endian.h>

#define __packed __attribute__((packed))

static int sk;

enum frame_ids {
	TOY_WHEEL_SPEED_A = 0x0b0,
	TOY_WHEEL_SPEED_B = 0x0b2,
	TOY_UNK_B4 = 0x0b4,
	TOY_BRAKE = 0x224,
	TOY_THROTTLE = 0x2c1,
	TOY_ENGINE = 0x2c4,
	TOY_FUEL_USAGE = 0x398,
};

#define UNKNOWN_COUNT 1024
static int unknown[UNKNOWN_COUNT];

union toyoframe {
	struct __packed {
		uint16_t a;
		uint16_t b;
		uint8_t flags;
		uint8_t seq;
	} wheel_speed;
	struct __packed {
		uint32_t _pad;
		uint8_t distance_a;
		uint16_t speed;
		uint8_t distance_b;
	} unkb4;
	struct __packed {
		uint8_t flags;
		uint8_t _pad0;
		uint8_t _pad1;
		uint8_t _pad2;
		uint8_t _pad3;
		uint8_t _pad4;
		uint8_t _pad5;
		uint8_t _pad6;
	} brake;
	struct __packed {
		uint8_t flags0;
		int16_t unk0;
		int16_t unk1;
		uint8_t unk2;
		int16_t throttle;
	} throttle;
	struct __packed {
		uint16_t rpm;
		uint8_t _pad0;
		uint8_t unk0;
		uint8_t _pad1;
		uint8_t _pad2;
		uint8_t unk1;
		int8_t unk2;
	} engine;
	struct __packed {
		int16_t fuel_usage;
	} fuel_usage;
};

static void unknown_frame(int id)
{
	int i;

	for (i = 0; i < UNKNOWN_COUNT; i++)
		if (unknown[i] == 0 || unknown[i] == id)
			break;
	if (i == UNKNOWN_COUNT)
		return;

	unknown[i] = id;

	move(LINES - 3, 1);
	clrtoeol();
	mvprintw(LINES - 3, 1, "unk:");
	for (i = 0; i < UNKNOWN_COUNT; i++) {
		if (unknown[i] == 0)
			break;
		printw(" %02x", unknown[i]);
	}
	printw(" (%d)", i);
}

static void process_one(struct can_frame *frm)
{
	int i;
	union toyoframe *toy;

	toy = (union toyoframe *)frm->data;

	switch (frm->can_id) {
	case TOY_WHEEL_SPEED_A:
	case TOY_WHEEL_SPEED_B:
		i = (frm->can_id == TOY_WHEEL_SPEED_A) ? 1 : 2;
		move(i, 1);
		clrtoeol();
		mvprintw(i, 1, "wheel: a=%5d b=%5d (delta=%5d) flags=%02x seq=%02x",
				be16toh(toy->wheel_speed.a),
				be16toh(toy->wheel_speed.b),
				be16toh(toy->wheel_speed.a) - be16toh(toy->wheel_speed.b),
				toy->wheel_speed.flags,
				toy->wheel_speed.seq);
		break;
	case TOY_UNK_B4:
		move(3, 1);
		clrtoeol();
		mvprintw(3, 1, "unk_b4: distance_a=%3d speed=%5d distance_b=%3d",
				toy->unkb4.distance_a,
				be16toh(toy->unkb4.speed),
				toy->unkb4.distance_b
				);
		break;
		break;
	case TOY_BRAKE:
		move(4, 1);
		clrtoeol();
		mvprintw(4, 1, "brake: flags=%02x [%s]",
				toy->brake.flags,
				(toy->brake.flags) ? "ON" : "  ");
		break;
	case TOY_THROTTLE:
		move(5, 1);
		clrtoeol();
		mvprintw(5, 1, "throttle: flags0=%02x unk0=%5hd unk1=%5hd, unk2=%03hhd throttle=%4hu",
				toy->throttle.flags0, /* bit 3: engine break? */
				be16toh(toy->throttle.unk0),
				be16toh(toy->throttle.unk1),
				toy->throttle.unk2,
				be16toh(toy->throttle.throttle)
				);
		break;
	case TOY_ENGINE:
		move(6, 1);
		clrtoeol();
		mvprintw(6, 1, "engine: rpm=%5hd unk0=%3d unk1=%3d, unk2=%3hhd",
				be16toh(toy->engine.rpm),
				toy->engine.unk0,
				toy->engine.unk1,
				toy->engine.unk2
				);
		break;
	case TOY_FUEL_USAGE:
		move(7, 1);
		clrtoeol();
		mvprintw(7, 1, "fuel_usage: %5hd",
				be16toh(toy->fuel_usage.fuel_usage));
		break;
	default:
		unknown_frame(frm->can_id);
	}

	refresh();
}

static int net_init(char *ifname)
{
	int recv_own_msgs;
	struct sockaddr_can addr;
	struct ifreq ifr;

	sk = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if (sk < 0) {
		perror("socket");
		exit(1);
	}

	memset(&ifr.ifr_name, 0, sizeof(ifr.ifr_name));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ);
	if (ioctl(sk, SIOCGIFINDEX, &ifr) < 0) {
		perror("SIOCGIFINDEX");
		exit(1);
	}

	memset(&addr, 0, sizeof(addr));
	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;
	if (bind(sk, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}

	recv_own_msgs = 0; /* 0 = disabled (default), 1 = enabled */
	setsockopt(sk, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS,
			&recv_own_msgs, sizeof(recv_own_msgs));

	return 0;
}

static void receive_one(void)
{
	struct can_frame frm;
	struct sockaddr_can addr;
	int ret;
	socklen_t len;

	ret = recvfrom(sk, &frm, sizeof(struct can_frame), 0,
			(struct sockaddr *)&addr, &len);
	if (ret < 0) {
		perror("recvfrom");
		exit(1);
	}

	process_one(&frm);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		printf("syntax: %s IFNAME\n", argv[0]);
		exit(1);
	}

	memset(unknown, 0, sizeof(unknown));

	initscr();

	net_init(argv[1]);

	for (;;)
		receive_one();

	endwin();

	return 0;
}
