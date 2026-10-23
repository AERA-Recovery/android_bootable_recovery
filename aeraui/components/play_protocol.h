/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#define AERA_PLAY_MAGIC 0x41575031u
#define AERA_PLAY_WIDTH 960
#define AERA_PLAY_HEIGHT 540
#define AERA_PLAY_BYTES (960u * 540u * 4u)
#define AERA_PLAY_SHARED (2u * AERA_PLAY_BYTES)
enum { PLAY_FRAME=1, PLAY_ACK, PLAY_KEY, PLAY_BUTTON, PLAY_POINTER,
       PLAY_LOOK, PLAY_RELEASE, PLAY_CLOSE, PLAY_WHEEL };
/* Fixed-size, local, SOCK_SEQPACKET-only channel. No paths or commands. */
struct PlayMessage { uint32_t magic, kind, sequence; int32_t x, y; };
