/*

Copyright (c) 2012, Alexandre Girao <alexgirao@gmail.com>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the
   distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 *
 *
 * adpcm_swf2raw
 *
 * decoding of ADPCM (Adaptive Differential Pulse Code Modulation)
 *   according to SWF File Format Specification Version 10
 *
 * reference: http://www.adobe.com/content/dam/Adobe/en/devnet/swf/pdf/swf_file_format_spec_v10.pdf
 * reference: http://www.drdobbs.com/database/algorithm-alley/184410326
 * reference: doc/imaadpcm.cpp and doc/imaadpcm.h
 *
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <stdint.h>

#include "debug0.h"

#include "str.h"

#include "bsd-getopt_long.h"
#include "getopt_x.h"

static const char *options_short = NULL;
static const char *options_mandatory = "io";

static struct option options_long[] = {
	{.val='i', .name="input", .has_arg=1},
	{.val='o', .name="output", .has_arg=1},
	{.val='s', .name="stereo"},
	{.val='h', .name="help"},
	{.name=NULL}
};

struct args {
	struct str input_file[1];
	struct str output_file[1];
	int is_stereo;
} args[1];

/* sub or zero */
#define SOZ(a,b) ((a) > (b) ? (a) - (b) : 0)

static void help(const char *argv0, struct getopt_x *state)
{
	char buf[4096];
	int bufsz = sizeof(buf);
	struct option opt[1];
	int pos = 0;
	int c = 0;

	pos += snprintf(buf + pos, SOZ(bufsz,pos), "\n");
	pos += snprintf(buf + pos, SOZ(bufsz,pos), "  usage: %s [options] ...\n", argv0);
	pos += snprintf(buf + pos, SOZ(bufsz,pos), "  options:\n");
	pos += snprintf(buf + pos, SOZ(bufsz,pos), "\n");

	while ((c = getopt_x_option(state, c, opt)) >= 0) {
		pos += getopt_x_option_format(buf + pos, bufsz - pos, state, opt);
		switch (opt->val) {
		case 'i':
			pos += snprintf(buf + pos, SOZ(bufsz,pos), "input file that has adpcm_swf raw data\n");
			break;
		case 'o':
			pos += snprintf(buf + pos, SOZ(bufsz,pos), "output file, s16le\n");
			break;
		case 's':
			pos += snprintf(buf + pos, SOZ(bufsz,pos), "stereo input? mono is default\n");
			break;
		case 0:
			pos += snprintf(buf + pos, SOZ(bufsz,pos), "this is option --%s and it does such also\n", opt->name);
			break;
		case 'h':
			pos += snprintf(buf + pos, SOZ(bufsz,pos), "\n");
			break;
		default:
			pos += snprintf(buf + pos, SOZ(bufsz,pos), "undocumented\n");
		}
		if (pos >= bufsz) {
			DEBUG("buffer too small");
			exit(1);
		}
	}
	pos += snprintf(buf + pos, SOZ(bufsz,pos), "\n");

	fputs(buf, stderr);
}

static int process_args(struct getopt_x *state, int argc, char **argv)
{
	int c;
	if (getopt_x_prepare(state, argc, argv, options_short, options_long, options_mandatory)) {
		DEBUG("error: failed to parse options");
		exit(1);
	}
	do {
		struct option *opt;
		switch (c = getopt_x_next(state, &opt)) {
		case 'i': str_copyz(args->input_file, optarg); break;
		case 'o': str_copyz(args->output_file, optarg); break;
		case 's': args->is_stereo = 1; break;
		case 'h': help(argv[0], state); exit(0);
		case -1: break;
		default:
			getopt_x_option_debug(state, c, opt);
			return -1;
		}
	} while (c != -1);
	if (!state->got_error) {
		assert(str_len(args->output_file));
	}
	return state->got_error;
}

/*
 */

static const int indexAdjustTable2bit[4] = {
	-1, 2, /* 2#0? */
	-1, 2  /* 2#1? (signal bit set)*/
};

static const int indexAdjustTable3bit[8] = {
	-1, -1, 2, 4, /* 2#0?? */
	-1, -1, 2, 4  /* 2#1?? (signal bit set)*/
};

static const int indexAdjustTable4bit[16] = {
	-1, -1, -1, -1, 2, 4, 6, 8, /* 2#0??? */
	-1, -1, -1, -1, 2, 4, 6, 8 /* 2#1??? (signal bit set)*/
};

static const int indexAdjustTable5bit[32] = {
	-1, -1, -1, -1, -1, -1, -1, -1, 1, 2, 4, 6, 8, 10, 13, 16, /* 2#0???? */
	-1, -1, -1, -1, -1, -1, -1, -1, 1, 2, 4, 6, 8, 10, 13, 16  /* 2#1???? (signal bit set)*/
};

static const int stepSizeTable[89] = {
   7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34,
   37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
   157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
   544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
   1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
   4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
   11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
   27086, 29794, 32767
};

struct adpcm_state {
	int index;
	int sample;
};

static int adpcm_decode2bit(int deltaCode, struct adpcm_state *state)
{
	assert(deltaCode == (deltaCode & 3) /* 2#11 */);

	int step = stepSizeTable[state->index];

	int difference = step >> 1;
	if (deltaCode & 1) difference += step;
	if (deltaCode & 2) difference = -difference;

	state->sample += difference;
	if (state->sample > 32767) state->sample = 32767;
	else if (state->sample < -32768) state->sample = -32768;

	state->index += indexAdjustTable2bit[deltaCode];
	if (state->index < 0) state->index = 0;
	else if (state->index > 88) state->index = 88;

	return state->sample;
}

static int adpcm_decode3bit(int deltaCode, struct adpcm_state *state)
{
	assert(deltaCode == (deltaCode & 7) /* 2#111 */);

	int step = stepSizeTable[state->index];

	int difference = step >> 2;
	if (deltaCode & 1) difference += step >> 1;
	if (deltaCode & 2) difference += step;
	if (deltaCode & 4) difference = -difference;

	state->sample += difference;
	if (state->sample > 32767) state->sample = 32767;
	else if (state->sample < -32768) state->sample = -32768;

	state->index += indexAdjustTable3bit[deltaCode];
	if (state->index < 0) state->index = 0;
	else if (state->index > 88) state->index = 88;

	return state->sample;
}

static int adpcm_decode4bit(int deltaCode, struct adpcm_state *state)
{
	assert(deltaCode == (deltaCode & 15) /* 2#1111 */);

	int step = stepSizeTable[state->index];

	int difference = step >> 3;
	if (deltaCode & 1) difference += step >> 2;
	if (deltaCode & 2) difference += step >> 1;
	if (deltaCode & 4) difference += step;
	if (deltaCode & 8) difference = -difference;

	state->sample += difference;
	if (state->sample > 32767) state->sample = 32767;
	else if (state->sample < -32768) state->sample = -32768;

	state->index += indexAdjustTable4bit[deltaCode];
	if (state->index < 0) state->index = 0;
	else if (state->index > 88) state->index = 88;

	return state->sample;
}

static int adpcm_decode5bit(int deltaCode, struct adpcm_state *state)
{
	assert(deltaCode >= 0);
	assert(deltaCode <= 31 /* 2#11111 */);

	int step = stepSizeTable[state->index];

	int difference = step >> 4;
	if (deltaCode & 1) difference += step >> 3;
	if (deltaCode & 2) difference += step >> 2;
	if (deltaCode & 4) difference += step >> 1;
	if (deltaCode & 8) difference += step;
	if (deltaCode & 16) difference = -difference;

	state->sample += difference;
	if (state->sample > 32767) state->sample = 32767;
	else if (state->sample < -32768) state->sample = -32768;

	state->index += indexAdjustTable5bit[deltaCode];
	if (state->index < 0) state->index = 0;
	else if (state->index > 88) state->index = 88;

	return state->sample;
}

static int write_exact(int fd, void *buf, int len);

int doit(const char *adpcm_path)
{
	DEFINE_STR(input);

	/* prepare input
	 */

	str_from_file(input, adpcm_path);

	assert(input->len);

	const char *in = input->s;
	const char *last = in + input->len;

	DEBUG("input size=%i", input->len);
	DEBUG("first byte=0x%x", *in);

	if (args->is_stereo) {
		DEBUG("ADPCMSTEREOPACKET not implemented yet");
	} else {
		/* ADPCMSOUNDDATA
		 */
		int adpcm_code_size;
		int initial_sample;
		int initial_index;
		long sample_number = 0;

		assert((in + 2) < last);

		adpcm_code_size = *(unsigned char*)in >> 6;              /* get UB[2]*/

		initial_sample =
			((*(unsigned char*)in & ((1<<6)-1)) << (16-6)) | /* SI16 UB[6] */
			(*(unsigned char*)(in+1) << (16-6-8)) |          /* SI16 UB[8] */
			(*(unsigned char*)(in+2) >> 6);                  /* SI16 UB[2] */
		initial_sample = (int16_t)initial_sample;

		initial_index = *(unsigned char*)(in+2) & ((1<<6)-1);    /* UB[6] */

		DEBUG("adpcm_code_size=%i", adpcm_code_size);

		in += 3;

		/* got ADPCMSOUNDDATA and ADPCMMONOPACKET header
		 */

		struct adpcm_state state[1] = {{.index = initial_index, .sample = initial_sample}};

		int bits_per_code = adpcm_code_size + 2;
		DEBUG("bits_per_code=%i", bits_per_code);
		
		//DEBUG("initial_sample=%i (first)", initial_sample);
		//DEBUG("initial_index=%i (first)", initial_index);

		int16_t output[4095], *outputp = output;
		*outputp++ = initial_sample;
		sample_number++;

		switch (adpcm_code_size) {
		case 1: /* 3 bits/sample */
		{
			int count = 0;
			int heading_bits = 0;
			int amount_of_heading_bits = 0;

			while (in < last && count < 4095) {
				assert((in + 2) < last);

				int i0 = *(unsigned char*)in++;
				int i1 = *(unsigned char*)in++;
				int i2 = *(unsigned char*)in++;
				
				int s0 = i0 >> 5;
				int s1 = (i0 >> 2) & 7;
				int s2 = ((i0 & 3) << 1) | (i1 >> 7);
				int s3 = (i1 >> 4) & 7;
				int s4 = (i1 >> 1) & 7;
				int s5 = ((i1 & 1) << 2) | (i2 >> 6);
				int s6 = (i2 >> 3) & 7;
				int s7 = i2 & 7;

				*outputp++ = adpcm_decode3bit(s0, state);
				*outputp++ = adpcm_decode3bit(s1, state);
				*outputp++ = adpcm_decode3bit(s2, state);
				*outputp++ = adpcm_decode3bit(s3, state);
				*outputp++ = adpcm_decode3bit(s4, state);
				*outputp++ = adpcm_decode3bit(s5, state);
				*outputp++ = adpcm_decode3bit(s6, state);
				count += 7; /* 7-samples so far */
				sample_number += 7;

				if (count >= 4095) {
					assert(count == 4095);
					/* i2 holds next packet UB[3] at it's tail
					 */
					heading_bits = i2 & 7;
					amount_of_heading_bits = 3;
					break;
				}

				*outputp++ = adpcm_decode3bit(s7, state);
				count++; /* 8-samples (for each 3-byte input) */
				sample_number++;
			}

			if (in < last && count != 4095) {
				/* this ensures heading_bits got set
				 */
				DEBUG("exhaustion");
				exit(1);
			}

			int output_len = ((unsigned char*)outputp) - ((unsigned char*)output);
			//DEBUG("count=%i, outputp-output=%i", count, output_len);

			/* write first ADPCMMONOPACKET
			 */

			int use_stdout = str_len(args->output_file) == 1 && args->output_file->s[0] == '-';
			int fd;

			if (use_stdout) {
				fd = STDOUT_FILENO;
			} else {
				if ((fd = open(args->output_file->s, O_CREAT | O_WRONLY | O_TRUNC, 0644)) < 0) {
					int save_errno = errno;
					assert(fd == -1);
					DEBUG("open(args->output_file->s=[%s], O_CREAT | O_WRONLY | O_TRUNC, 0644), errno=%i", args->output_file->s, save_errno);
					errno = save_errno;
					perror(args->output_file->s);
					break;
				}
			}
			assert(write_exact(fd, output, output_len) == output_len);

			/* more data?
			 */

			while (in < last) {
				if (amount_of_heading_bits == 0) {
					if ((in + 2) >= last) break;

					/*
					 * [in:8,in1:8] [in2:6] [in2:2]
					 * ---- 16 ---- -- 6 -- ...
					 */

					initial_sample = (*(unsigned char*)(in) << 8) | (*(unsigned char*)(in+1));   /* SI16 */
					initial_sample = (int16_t)initial_sample;
					initial_index = *(unsigned char*)(in+2) >> 2;   /* UB[6] */

					int i0 = *(unsigned char*)(in+2) & 3; /* UB[2] for next delta */

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 3;

					/* got ADPCMMONOPACKET header
					 *
					 * note: we still have UB[2] hanging on i0
					 *
					 *  2] [1|3|3|1] [2|3|3]: 2 bytes to get even
					 *
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					assert((in + 1) < last);

					int i1 = *(unsigned char*)in++;
					int i2 = *(unsigned char*)in++;

					int s2 = ((i0 & 3) << 1) | (i1 >> 7);
					int s3 = (i1 >> 4) & 7;
					int s4 = (i1 >> 1) & 7;
					int s5 = ((i1 & 1) << 2) | (i2 >> 6);
					int s6 = (i2 >> 3) & 7;
					int s7 = i2 & 7;

					*outputp++ = adpcm_decode3bit(s2, state);
					*outputp++ = adpcm_decode3bit(s3, state);
					*outputp++ = adpcm_decode3bit(s4, state);
					*outputp++ = adpcm_decode3bit(s5, state);
					*outputp++ = adpcm_decode3bit(s6, state);
					*outputp++ = adpcm_decode3bit(s7, state);
					count = 6;
					sample_number += 6;
				} else if (amount_of_heading_bits == 3) {
					if ((in + 2) >= last) break;

					/*
					 * [3,in:8,in1:5] [in1:3,in2:3] [in2:5]
					 * ----- 16 ----- ----- 6 ----- ...
					 */

					initial_sample =
						(heading_bits << (16-3)) |            /* SI16 UB[3] */
						(*(unsigned char*)in << (16-3-8)) |   /* SI16 UB[8] */
						(*(unsigned char*)(in+1) >> 3);       /* SI16 UB[5] */
					initial_sample = (int16_t)initial_sample;

					initial_index =
						((*(unsigned char*)(in+1) & 7) << 3) |  /* UB[6] UB[3] */
						(*(unsigned char*)(in+2) >> 5);         /* UB[6] UB[3] */

					int i0 = *(unsigned char*)(in+2) & 31; /* UB[5] for next delta */

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 3;

					/* got ADPCMMONOPACKET header
					 *
					 * note: we still have UB[5] hanging on i0
					 *
					 *  3|2] [1|3|3|1] [2|3|3]: 2 bytes to get even
					 *
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					assert((in + 1) < last);

					int i1 = *(unsigned char*)in++;
					int i2 = *(unsigned char*)in++;

					int s1 = (i0 >> 2) & 7;
					int s2 = ((i0 & 3) << 1) | (i1 >> 7);
					int s3 = (i1 >> 4) & 7;
					int s4 = (i1 >> 1) & 7;
					int s5 = ((i1 & 1) << 2) | (i2 >> 6);
					int s6 = (i2 >> 3) & 7;
					int s7 = i2 & 7;

					*outputp++ = adpcm_decode3bit(s1, state);
					*outputp++ = adpcm_decode3bit(s2, state);
					*outputp++ = adpcm_decode3bit(s3, state);
					*outputp++ = adpcm_decode3bit(s4, state);
					*outputp++ = adpcm_decode3bit(s5, state);
					*outputp++ = adpcm_decode3bit(s6, state);
					*outputp++ = adpcm_decode3bit(s7, state);
					count = 7;
					sample_number += 7;
				} else if (amount_of_heading_bits == 4) {
					if ((in + 2) >= last) break;

					/*
					 * [4,in:8,in1:4] [in1:4,in2:2] [in2:6]
					 * ----- 16 ----- ----- 6 ----- ...
					 */

					initial_sample =
						(heading_bits << (16-4)) |            /* SI16 UB[4] */
						(*(unsigned char*)in << (16-4-8)) |   /* SI16 UB[8] */
						(*(unsigned char*)(in+1) >> 4);       /* SI16 UB[4] */
					initial_sample = (int16_t)initial_sample;

					initial_index =
						((*(unsigned char*)(in+1) & 15) << 2) |  /* UB[6] UB[4] */
						(*(unsigned char*)(in+2) >> 6);          /* UB[6] UB[2] */

					int i0 = *(unsigned char*)(in+2) & 63; /* UB[6] for next delta */

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 3;

					/* got ADPCMMONOPACKET header
					 *
					 * note: we still have UB[6] hanging on i0
					 *
					 *  3|3]: 0 bytes to get even
					 *
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					int s6 = (i0 >> 3) & 7;
					int s7 = i0 & 7;

					*outputp++ = adpcm_decode3bit(s6, state);
					*outputp++ = adpcm_decode3bit(s7, state);
					count = 2;
					sample_number += 2;
				} else if (amount_of_heading_bits == 5) {
					if ((in + 2) >= last) break;

					/*
					 * [5,in:8,in1:3] [in1:5,in2:1] [in2:7]
					 * ----- 16 ----- ----- 6 ----- ...
					 */

					initial_sample =
						(heading_bits << (16-5)) |            /* SI16 UB[5] */
						(*(unsigned char*)in << (16-5-8)) |   /* SI16 UB[8] */
						(*(unsigned char*)(in+1) >> 5);       /* SI16 UB[3] */
					initial_sample = (int16_t)initial_sample;

					initial_index =
						((*(unsigned char*)(in+1) & 31) << 1) |  /* UB[6] UB[5] */
						(*(unsigned char*)(in+2) >> 7);          /* UB[6] UB[1] */

					int i0 = *(unsigned char*)(in+2) & 127; /* UB[7] for next delta */

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 3;

					/* got ADPCMMONOPACKET header
					 *
					 * note: we still have UB[7] hanging on i0
					 *
					 *  3|3|1] [2|3|3]: 1 bytes to get even
					 *
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					assert(in < last);

					int i1 = *(unsigned char*)in++;

					int s3 = (i0 >> 4) & 7;
					int s4 = (i0 >> 1) & 7;
					int s5 = ((i0 & 1) << 2) | (i1 >> 6);
					int s6 = (i1 >> 3) & 7;
					int s7 = i1 & 7;

					*outputp++ = adpcm_decode3bit(s3, state);
					*outputp++ = adpcm_decode3bit(s4, state);
					*outputp++ = adpcm_decode3bit(s5, state);
					*outputp++ = adpcm_decode3bit(s6, state);
					*outputp++ = adpcm_decode3bit(s7, state);
					count = 5;
					sample_number += 5;
				} else if (amount_of_heading_bits == 2) {
					if ((in + 2) >= last) break;

					/*
					 * [2,in:8,in1:6] [in1:2,in2:4] [in2:4]
					 * ----- 16 ----- ----- 6 ----- ...
					 */

					initial_sample =
						(heading_bits << (16-2)) |            /* SI16 UB[2] */
						(*(unsigned char*)in << (16-2-8)) |   /* SI16 UB[8] */
						(*(unsigned char*)(in+1) >> 2);       /* SI16 UB[6] */
					initial_sample = (int16_t)initial_sample;

					initial_index =
						((*(unsigned char*)(in+1) & 3) << 4) |   /* UB[6] UB[2] */
						(*(unsigned char*)(in+2) >> 4);          /* UB[6] UB[4] */

					int i0 = *(unsigned char*)(in+2) & 15;   /* UB[4] for next delta */

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 3;

					/* got ADPCMMONOPACKET header
					 *
					 * note: we still have UB[4] hanging on i0
					 *
					 *  3|1] [2|3|3]: 1 bytes to get even
					 *
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					assert(in < last);

					int i1 = *(unsigned char*)in++;

					int s4 = (i0 >> 1) & 7;
					int s5 = ((i0 & 1) << 2) | (i1 >> 6);
					int s6 = (i1 >> 3) & 7;
					int s7 = i1 & 7;

					*outputp++ = adpcm_decode3bit(s4, state);
					*outputp++ = adpcm_decode3bit(s5, state);
					*outputp++ = adpcm_decode3bit(s6, state);
					*outputp++ = adpcm_decode3bit(s7, state);
					count = 4;
					sample_number += 4;
				} else if (amount_of_heading_bits == 7) {
					if ((in + 1) >= last) break;

					/*
					 * [7,in:8,in1:1] [in1:6] [in1:1]
					 * ----- 16 ----- -- 6 -- ...
					 */

					initial_sample =
						(heading_bits << (16-7)) |            /* SI16 UB[7] */
						(*(unsigned char*)in << (16-7-8)) |   /* SI16 UB[8] */
						(*(unsigned char*)(in+1) >> 7);       /* SI16 UB[1] */
					initial_sample = (int16_t)initial_sample;

					initial_index = ((*(unsigned char*)(in+1) >> 1)) & 63;  /* UB[6] UB[6] (note: UB[1] gone to initial_sample) */

					int i0 = *(unsigned char*)(in+1) & 1;    /* UB[1] for next delta */

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 2;

					/* got ADPCMMONOPACKET header
					 *
					 * note: we still have UB[1] hanging on i0
					 *
					 *  1] [2|3|3]: 1 bytes to get even
					 *
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					assert(in < last);

					int i1 = *(unsigned char*)in++;

					int s5 = ((i0 & 1) << 2) | (i1 >> 6);
					int s6 = (i1 >> 3) & 7;
					int s7 = i1 & 7;

					*outputp++ = adpcm_decode3bit(s5, state);
					*outputp++ = adpcm_decode3bit(s6, state);
					*outputp++ = adpcm_decode3bit(s7, state);
					count = 3;
					sample_number += 3;
				} else if (amount_of_heading_bits == 1) {
					if ((in + 2) >= last) break;

					/*
					 * [1,in:8,in1:7] [in1:1,in2:5] [in2:3]
					 * ----- 16 ----- ----- 6 ----- ...
					 */

					initial_sample =
						(heading_bits << (16-1)) |            /* SI16 UB[1] */
						(*(unsigned char*)in << (16-1-8)) |   /* SI16 UB[8] */
						(*(unsigned char*)(in+1) >> 1);       /* SI16 UB[7] */
					initial_sample = (int16_t)initial_sample;

					initial_index =
						((*(unsigned char*)(in+1) & 1) << 5) |   /* UB[6] UB[1] */
						(*(unsigned char*)(in+2) >> 3);          /* UB[6] UB[5] */

					int i0 = *(unsigned char*)(in+2) & 7;   /* UB[3] for next delta */

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 3;

					/* got ADPCMMONOPACKET header
					 *
					 * note: we still have UB[3] hanging on i0
					 *
					 *  3]: 0 bytes to get even
					 *
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					int s7 = i0 & 7;

					*outputp++ = adpcm_decode3bit(s7, state);
					count = 1;
					sample_number++;
				} else if (amount_of_heading_bits == 6) {
					if ((in + 1) >= last) break;

					/*
					 * [6,in:8,in1:2] [in1:6] (we are even!)
					 * ----- 16 ----- -- 6 -- ...
					 */

					initial_sample =
						(heading_bits << (16-6)) |            /* SI16 UB[6] */
						(*(unsigned char*)in << (16-6-8)) |   /* SI16 UB[8] */
						(*(unsigned char*)(in+1) >> 6);       /* SI16 UB[2] */
					initial_sample = (int16_t)initial_sample;

					initial_index = *(unsigned char*)(in+1) & 63;  /* UB[6] UB[6] (we are even!) */

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 2;

					/* got ADPCMMONOPACKET header
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					count = 0;
				} else {
					DEBUG("exhaustion, amount_of_heading_bits=%i", amount_of_heading_bits);
					exit(1);
				}

				heading_bits = 0;
				amount_of_heading_bits = 0;

				//DEBUG("sample_number=%li", sample_number);

				while (in < last && count < 4095) {
					/* i0
					 */

					int i0 = *(unsigned char*)in++;
					int s0 = i0 >> 5;
					int s1 = (i0 >> 2) & 7;

					*outputp++ = adpcm_decode3bit(s0, state);
					count++;
					sample_number++;
					if (count == 4095) {
						heading_bits = i0 & 31;
						amount_of_heading_bits = 5;
						break;
					}

					*outputp++ = adpcm_decode3bit(s1, state);
					count++;
					sample_number++;
					if (count == 4095) {
						heading_bits = i0 & 3;
						amount_of_heading_bits = 2;
						break;
					}

					/* i1
					 */

					if (in > last) break;
					int i1 = *(unsigned char*)in++;
					int s2 = ((i0 & 3) << 1) | (i1 >> 7);
					int s3 = (i1 >> 4) & 7;
					int s4 = (i1 >> 1) & 7;

					*outputp++ = adpcm_decode3bit(s2, state);
					count++;
					sample_number++;
					if (count == 4095) {
						heading_bits = i1 & 127;
						amount_of_heading_bits = 7;
						break;
					}

					*outputp++ = adpcm_decode3bit(s3, state);
					count++;
					sample_number++;
					if (count == 4095) {
						heading_bits = i1 & 15;
						amount_of_heading_bits = 4;
						break;
					}

					*outputp++ = adpcm_decode3bit(s4, state);
					count++;
					sample_number++;
					if (count == 4095) {
						heading_bits = i1 & 1;
						amount_of_heading_bits = 1;
						break;
					}

					/* i2
					 */

					if (in > last) break;
					int i2 = *(unsigned char*)in++;
					int s5 = ((i1 & 1) << 2) | (i2 >> 6);
					int s6 = (i2 >> 3) & 7;
					int s7 = i2 & 7;

					*outputp++ = adpcm_decode3bit(s5, state);
					count++;
					sample_number++;
					if (count == 4095) {
						heading_bits = i2 & 63;
						amount_of_heading_bits = 6;
						break;
					}

					*outputp++ = adpcm_decode3bit(s6, state);
					count++;
					sample_number++;
					if (count == 4095) {
						heading_bits = i2 & 7;
						amount_of_heading_bits = 3;
						break;
					}

					*outputp++ = adpcm_decode3bit(s7, state);
					count++;
					sample_number++;
				}

				if (in < last && count != 4095) {
					DEBUG("exhaustion, count=%i, last-in=%i", count, (int)(last-in));
					exit(1);
				}

				int output_len = ((unsigned char*)outputp) - ((unsigned char*)output);
				//DEBUG("count=%i, outputp-output=%i", count, output_len);

				assert(write_exact(fd, output, output_len) == output_len);
			}

			/* close output
			 */

			if (fd != STDOUT_FILENO) {
				assert(close(fd) == 0);
				fd = -1;
			}

			break;
		}
		case 3: /* 5 bits/sample */
		{
			int count = 0;
			int heading_bits = 0;
			int amount_of_heading_bits = 0;

			while (in < last && count < 4095) {
				if ((in + 4) >= last) break;

				int i0 = *(unsigned char*)in++;
				int i1 = *(unsigned char*)in++;
				int i2 = *(unsigned char*)in++;
				int i3 = *(unsigned char*)in++;
				int i4 = *(unsigned char*)in++;

				int s0 = i0 >> 3;
				int s1 = ((i0 & 7) << 2) | (i1 >> 6);
				int s2 = (i1 >> 1) & 31;
				int s3 = ((i1 & 1) << 4) | (i2 >> 4);
				int s4 = ((i2 & 15) << 1) | (i3 >> 7);
				int s5 = (i3 >> 2) & 31;
				int s6 = ((i3 & 3) << 3) | (i4 >> 5);
				int s7 = i4 & 31;

				*outputp++ = adpcm_decode5bit(s0, state);
				*outputp++ = adpcm_decode5bit(s1, state);
				*outputp++ = adpcm_decode5bit(s2, state);
				*outputp++ = adpcm_decode5bit(s3, state);
				*outputp++ = adpcm_decode5bit(s4, state);
				*outputp++ = adpcm_decode5bit(s5, state);
				*outputp++ = adpcm_decode5bit(s6, state);
				count += 7; /* 7-samples so far */
				sample_number += 7;

				if (count >= 4095) {
					assert(count == 4095);
					heading_bits = i4 & 31;
					amount_of_heading_bits = 5;
					break;
				}

				*outputp++ = adpcm_decode5bit(s7, state);
				count++; /* 8-samples (for each 3-byte input) */
				sample_number++;
			}

			if (in < last && count != 4095) {
				DEBUG("first packet is incomplete, count=%i, last-in=%i", count, (int)(last-in));
				do {
					if (in >= last) break;
					int i0 = *(unsigned char*)in++;
					int s0 = i0 >> 3;
					*outputp++ = adpcm_decode5bit(s0, state);
					count++;
					sample_number++;

					if (in >= last) break;
					int i1 = *(unsigned char*)in++;
					int s1 = ((i0 & 7) << 2) | (i1 >> 6);
					int s2 = (i1 >> 1) & 31;
					*outputp++ = adpcm_decode5bit(s1, state);
					count++;
					sample_number++;
					*outputp++ = adpcm_decode5bit(s2, state);
					count++;
					sample_number++;

					if (in >= last) break;
					int i2 = *(unsigned char*)in++;
					int s3 = ((i1 & 1) << 4) | (i2 >> 4);
					*outputp++ = adpcm_decode5bit(s3, state);
					count++;
					sample_number++;

					if (in >= last) break;
					int i3 = *(unsigned char*)in++;
					int s4 = ((i2 & 15) << 1) | (i3 >> 7);
					int s5 = (i3 >> 2) & 31;
					*outputp++ = adpcm_decode5bit(s4, state);
					count++;
					sample_number++;
					*outputp++ = adpcm_decode5bit(s5, state);
					count++;
					sample_number++;

					if (in >= last) break;
					int i4 = *(unsigned char*)in++;
					int s6 = ((i3 & 3) << 3) | (i4 >> 5);
					int s7 = i4 & 31;
					*outputp++ = adpcm_decode5bit(s6, state);
					count++;
					sample_number++;
					*outputp++ = adpcm_decode5bit(s7, state);
					count++;
					sample_number++;
				} while (0);
			}

			int output_len = ((unsigned char*)outputp) - ((unsigned char*)output);
			//DEBUG("count=%i, outputp-output=%i", count, output_len);

			/* write first ADPCMMONOPACKET
			 */

			int use_stdout = str_len(args->output_file) == 1 && args->output_file->s[0] == '-';
			int fd;

			if (use_stdout) {
				fd = STDOUT_FILENO;
			} else {
				if ((fd = open(args->output_file->s, O_CREAT | O_WRONLY | O_TRUNC, 0644)) < 0) {
					int save_errno = errno;
					assert(fd == -1);
					DEBUG("open(args->output_file->s=[%s], O_CREAT | O_WRONLY | O_TRUNC, 0644), errno=%i", args->output_file->s, save_errno);
					errno = save_errno;
					perror(args->output_file->s);
					break;
				}
			}
			assert(write_exact(fd, output, output_len) == output_len);

			/* more data?
			 */

			while (in < last) {
				if (amount_of_heading_bits == 5) {
					if ((in + 2) >= last) break;
					
					/*
					 * [5,in:8,in1:3] [in1:5,in2:1] [in2:7]
					 * ----- 16 ----- ----- 6 ----- ...
					 */

					initial_sample =
						(heading_bits << (16-5)) |            /* SI16 UB[5] */
						(*(unsigned char*)in << (16-5-8)) |   /* SI16 UB[8] */
						(*(unsigned char*)(in+1) >> 5);       /* SI16 UB[3] */
					initial_sample = (int16_t)initial_sample;

					initial_index =
						((*(unsigned char*)(in+1) & 31) << 1) |  /* UB[6] UB[5] */
						(*(unsigned char*)(in+2) >> 7);          /* UB[6] UB[1] */

					int i0 = *(unsigned char*)(in+2) & 127; /* UB[7] for next delta */

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 3;

					/* got ADPCMMONOPACKET header
					 *
					 * note: we still have UB[7] hanging on i0
					 *
					 *  5|2] [3|5]: 1 bytes to get even
					 *
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					assert(in < last);

					int i1 = *(unsigned char*)in++;

					int s5 = (i0 >> 2) & 31;
					int s6 = ((i0 & 3) << 3) | (i1 >> 5);
					int s7 = i1 & 31;

					*outputp++ = adpcm_decode5bit(s5, state);
					*outputp++ = adpcm_decode5bit(s6, state);
					*outputp++ = adpcm_decode5bit(s7, state);
					count = 3;
					sample_number += 3;
				} else {
					DEBUG("exhaustion, amount_of_heading_bits=%i", amount_of_heading_bits);
					exit(1);
				}

				heading_bits = 0;
				amount_of_heading_bits = 0;

				//DEBUG("sample_number=%li", sample_number);

				while (in < last && count < 4095) {
					/* i0
					 */

					int i0 = *(unsigned char*)in++;
					int s0 = i0 >> 3;
					*outputp++ = adpcm_decode5bit(s0, state);
					count++;
					sample_number++;
					if (count == 4095) {
						DEBUG("wip"); exit(1);
						break;
					}

					/* i1
					 */

					if (in >= last) break;
					int i1 = *(unsigned char*)in++;
					int s1 = ((i0 & 7) << 2) | (i1 >> 6);
					int s2 = (i1 >> 1) & 31;
					*outputp++ = adpcm_decode5bit(s1, state);
					count++;
					sample_number++;
					if (count == 4095) {
						DEBUG("wip"); exit(1);
						break;
					}

					*outputp++ = adpcm_decode5bit(s2, state);
					count++;
					sample_number++;
					if (count == 4095) {
						DEBUG("wip"); exit(1);
						break;
					}

					/* i2
					 */

					if (in >= last) break;
					int i2 = *(unsigned char*)in++;
					int s3 = ((i1 & 1) << 4) | (i2 >> 4);
					*outputp++ = adpcm_decode5bit(s3, state);
					count++;
					sample_number++;
					if (count == 4095) {
						DEBUG("wip"); exit(1);
						break;
					}

					/* i3
					 */

					if (in >= last) break;
					int i3 = *(unsigned char*)in++;
					int s4 = ((i2 & 15) << 1) | (i3 >> 7);
					int s5 = (i3 >> 2) & 31;
					*outputp++ = adpcm_decode5bit(s4, state);
					count++;
					sample_number++;
					if (count == 4095) {
						DEBUG("wip"); exit(1);
						break;
					}

					*outputp++ = adpcm_decode5bit(s5, state);
					count++;
					sample_number++;
					if (count == 4095) {
						DEBUG("wip"); exit(1);
						break;
					}

					/* i4
					 */

					if (in >= last) break;
					int i4 = *(unsigned char*)in++;
					int s6 = ((i3 & 3) << 3) | (i4 >> 5);
					int s7 = i4 & 31;
					*outputp++ = adpcm_decode5bit(s6, state);
					count++;
					sample_number++;
					if (count == 4095) {
						DEBUG("wip"); exit(1);
						break;
					}

					*outputp++ = adpcm_decode5bit(s7, state);
					count++;
					sample_number++;
					if (count == 4095) {
						DEBUG("wip"); exit(1);
						break;
					}
				}

				if (in < last && count != 4095) {
					DEBUG("exhaustion, count=%i, last-in=%i", count, (int)(last-in));
					exit(1);
				}

				int output_len = ((unsigned char*)outputp) - ((unsigned char*)output);
				//DEBUG("count=%i, outputp-output=%i", count, output_len);

				assert(write_exact(fd, output, output_len) == output_len);
			}

			/* close output
			 */

			if (fd != STDOUT_FILENO) {
				assert(close(fd) == 0);
				fd = -1;
			}

			break;
		}
		case 2: /* 4 bits/sample */
		{
			int count = 0;
			int heading_bits = 0;
			int amount_of_heading_bits = 0;

			while (in < last && count < 4095) {
				int i0 = *(unsigned char*)in++;

				*outputp++ = adpcm_decode4bit(i0 >> 4, state);
				count++;
				sample_number++;

				if (count >= 4095) {
					assert(count == 4095);
					heading_bits = i0 & 15;
					amount_of_heading_bits = 4;
					break;
				}

				*outputp++ = adpcm_decode4bit(i0 & 15, state);
				count++;
				sample_number++;
			}

			if (in < last && count != 4095) {
				DEBUG("first packet is incomplete, count=%i, last-in=%i", count, (int)(last-in));
				exit(1);
			}

			int output_len = ((unsigned char*)outputp) - ((unsigned char*)output);
			//DEBUG("count=%i, outputp-output=%i", count, output_len);

			/* write first ADPCMMONOPACKET
			 */

			int use_stdout = str_len(args->output_file) == 1 && args->output_file->s[0] == '-';
			int fd;

			if (use_stdout) {
				fd = STDOUT_FILENO;
			} else {
				if ((fd = open(args->output_file->s, O_CREAT | O_WRONLY | O_TRUNC, 0644)) < 0) {
					int save_errno = errno;
					assert(fd == -1);
					DEBUG("open(args->output_file->s=[%s], O_CREAT | O_WRONLY | O_TRUNC, 0644), errno=%i", args->output_file->s, save_errno);
					errno = save_errno;
					perror(args->output_file->s);
					break;
				}
			}
			assert(write_exact(fd, output, output_len) == output_len);

			/* more data?
			 */

			while (in < last) {
				//DEBUG("sample_number=%li", sample_number);
				//DEBUG("amount_of_heading_bits=%i", amount_of_heading_bits);

				if (amount_of_heading_bits == 4) {
					if ((in + 2) >= last) break;

					/*
					 * [4,in:8,in1:4] [in1:4,in2:2] [in2:6]
					 * ----- 16 ----- ----- 6 ----- ...
					 */

					initial_sample =
						(heading_bits << (16-4)) |            /* SI16 UB[4] */
						(*(unsigned char*)in << (16-4-8)) |   /* SI16 UB[8] */
						(*(unsigned char*)(in+1) >> 4);       /* SI16 UB[4] */
					initial_sample = (int16_t)initial_sample;

					initial_index =
						((*(unsigned char*)(in+1) & 15) << 2) |  /* UB[6] UB[4] */
						(*(unsigned char*)(in+2) >> 6);          /* UB[6] UB[2] */

					heading_bits = 0;
					amount_of_heading_bits = 0;

					int i0 = *(unsigned char*)(in+2) & 63; /* UB[6] for next delta */

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 3;

					/* got ADPCMMONOPACKET header
					 *
					 * note: we still have UB[6] hanging on i0
					 *
					 *  4|2] [2|4|2] [2|4|2] ...: can't get even
					 *
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					*outputp++ = adpcm_decode4bit(i0 >> 2, state);
					count = 1;
					sample_number++;

					for (;;) {
						if (in >= last) break;
						int i1 = *(unsigned char*)in++;

						/* i0 = 2]
						 * i1 = [2|4|2]
						 */

						*outputp++ = adpcm_decode4bit(((i0 & 3) << 2) | (i1 >> 6), state);
						count++;
						sample_number++;
						if (count >= 4095) {
							assert(count == 4095);
							heading_bits = i0 & 63;
							amount_of_heading_bits = 6;
							break;
						}

						*outputp++ = adpcm_decode4bit((i1 >> 2) & 15, state);
						count++;
						sample_number++;
						if (count >= 4095) {
							assert(count == 4095);
							heading_bits = i1 & 3;
							amount_of_heading_bits = 2;
							break;
						}

						/*
						 */

						i0 = i1;
					}
				} else if (amount_of_heading_bits == 2) {
					if ((in + 2) >= last) break;

					/*
					 * [2,in:8,in1:6] [in1:2,in2:4] [in2:4]
					 * ----- 16 ----- ----- 6 ----- ...
					 */

					initial_sample =
						(heading_bits << (16-2)) |            /* SI16 UB[2] */
						(*(unsigned char*)in << (16-2-8)) |   /* SI16 UB[8] */
						(*(unsigned char*)(in+1) >> 2);       /* SI16 UB[6] */
					initial_sample = (int16_t)initial_sample;

					initial_index =
						((*(unsigned char*)(in+1) & 3) << 4) |   /* UB[6] UB[2] */
						(*(unsigned char*)(in+2) >> 4);          /* UB[6] UB[4] */

					heading_bits = 0;
					amount_of_heading_bits = 0;

					int i0 = *(unsigned char*)(in+2) & 15;   /* UB[4] for next delta */

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 3;

					/* got ADPCMMONOPACKET header
					 *
					 * note: we still have UB[4] hanging on i0
					 *
					 *  4]: 0 bytes to get even
					 *
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					*outputp++ = adpcm_decode4bit(i0, state);
					count = 1;
					sample_number++;

					while (in < last && count < 4095) {
						int i0 = *(unsigned char*)in++;

						*outputp++ = adpcm_decode4bit(i0 >> 4, state);
						count++;
						sample_number++;

						if (count >= 4095) {
							assert(count == 4095);
							heading_bits = i0 & 15;
							amount_of_heading_bits = 4;
							break;
						}

						*outputp++ = adpcm_decode4bit(i0 & 15, state);
						count++;
						sample_number++;
					}
				} else if (amount_of_heading_bits == 0) {
					if ((in + 2) >= last) break;

					/*
					 * [in:8,in1:8] [in2:6] [in2:2]
					 * ---- 16 ---- -- 6 -- ...
					 */

					initial_sample = (*(unsigned char*)(in) << 8) | (*(unsigned char*)(in+1));   /* SI16 */
					initial_sample = (int16_t)initial_sample;
					initial_index = *(unsigned char*)(in+2) >> 2;   /* UB[6] */

					heading_bits = 0;
					amount_of_heading_bits = 0;

					int i0 = *(unsigned char*)(in+2) & 3; /* UB[2] for next delta */

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 3;

					/* got ADPCMMONOPACKET header
					 *
					 * note: we still have UB[2] hanging on i0
					 *
					 *  2] [2|4|2] [2|4|2] ...: can't get even
					 *
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					count = 0;

					for (;;) {
						if (in >= last) break;
						int i1 = *(unsigned char*)in++;

						/* i0 = 2]
						 * i1 = [2|4|2]
						 */

						*outputp++ = adpcm_decode4bit(((i0 & 3) << 2) | (i1 >> 6), state);
						count++;
						sample_number++;
						if (count >= 4095) {
							assert(count == 4095);
							heading_bits = i1 & 63;
							amount_of_heading_bits = 6;
							break;
						}

						*outputp++ = adpcm_decode4bit((i1 >> 2) & 15, state);
						count++;
						sample_number++;
						if (count >= 4095) {
							assert(count == 4095);
							heading_bits = i1 & 3;
							amount_of_heading_bits = 2;
							break;
						}

						/*
						 */

						i0 = i1;
					}
				} else if (amount_of_heading_bits == 6) {
					if ((in + 1) >= last) break;

					/*
					 * [6,in:8,in1:2] [in1:6] (we are even!)
					 * ----- 16 ----- -- 6 -- ...
					 */

					initial_sample =
						(heading_bits << (16-6)) |            /* SI16 UB[6] */
						(*(unsigned char*)in << (16-6-8)) |   /* SI16 UB[8] */
						(*(unsigned char*)(in+1) >> 6);       /* SI16 UB[2] */
					initial_sample = (int16_t)initial_sample;

					initial_index = *(unsigned char*)(in+1) & 63;  /* UB[6] UB[6] (we are even!) */

					heading_bits = 0;
					amount_of_heading_bits = 0;

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 2;

					/* got ADPCMMONOPACKET header
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					count = 0;

					while (in < last && count < 4095) {
						int i0 = *(unsigned char*)in++;

						*outputp++ = adpcm_decode4bit(i0 >> 4, state);
						count++;
						sample_number++;

						if (count >= 4095) {
							assert(count == 4095);
							heading_bits = i0 & 15;
							amount_of_heading_bits = 4;
							break;
						}

						*outputp++ = adpcm_decode4bit(i0 & 15, state);
						count++;
						sample_number++;
					}
				} else {
					DEBUG("exhaustion, amount_of_heading_bits=%i", amount_of_heading_bits);
					exit(1);
				}

				if (in < last && count != 4095) {
					DEBUG("exhaustion, count=%i, last-in=%i", count, (int)(last-in));
					exit(1);
				}

				int output_len = ((unsigned char*)outputp) - ((unsigned char*)output);
				//DEBUG("count=%i, outputp-output=%i", count, output_len);

				assert(write_exact(fd, output, output_len) == output_len);

			}

			/* close output
			 */

			if (fd != STDOUT_FILENO) {
				assert(close(fd) == 0);
				fd = -1;
			}

			break;
		}
		case 0: /* 2 bits/sample */
		{
			int count = 0;
			int heading_bits = 0;
			int amount_of_heading_bits = 0;

			while (in < last && count < 4095) {
				int i0 = *(unsigned char*)in++;

				*outputp++ = adpcm_decode2bit(i0 >> 6, state);
				count++;
				sample_number++;
				if (count >= 4095) {
					assert(count == 4095);
					heading_bits = i0 & 63;
					amount_of_heading_bits = 6;
					break;
				}

				*outputp++ = adpcm_decode2bit((i0 >> 4) & 3, state);
				count++;
				sample_number++;
				if (count >= 4095) {
					assert(count == 4095);
					heading_bits = i0 & 15;
					amount_of_heading_bits = 4;
					break;
				}

				*outputp++ = adpcm_decode2bit((i0 >> 2) & 3, state);
				count++;
				sample_number++;
				if (count >= 4095) {
					assert(count == 4095);
					heading_bits = i0 & 3;
					amount_of_heading_bits = 2;
					break;
				}

				*outputp++ = adpcm_decode2bit(i0 & 3, state);
				count++;
				sample_number++;
			}

			if (in < last && count != 4095) {
				DEBUG("first packet is incomplete, count=%i, last-in=%i", count, (int)(last-in));
				exit(1);
			}

			int output_len = ((unsigned char*)outputp) - ((unsigned char*)output);
			//DEBUG("count=%i, outputp-output=%i", count, output_len);

			/* write first ADPCMMONOPACKET
			 */

			int use_stdout = str_len(args->output_file) == 1 && args->output_file->s[0] == '-';
			int fd;

			if (use_stdout) {
				fd = STDOUT_FILENO;
			} else {
				if ((fd = open(args->output_file->s, O_CREAT | O_WRONLY | O_TRUNC, 0644)) < 0) {
					int save_errno = errno;
					assert(fd == -1);
					DEBUG("open(args->output_file->s=[%s], O_CREAT | O_WRONLY | O_TRUNC, 0644), errno=%i", args->output_file->s, save_errno);
					errno = save_errno;
					perror(args->output_file->s);
					break;
				}
			}
			assert(write_exact(fd, output, output_len) == output_len);

			/* more data?
			 */

			while (in < last) {
				//DEBUG("sample_number=%li", sample_number);
				//DEBUG("amount_of_heading_bits=%i", amount_of_heading_bits);

				if (amount_of_heading_bits == 2) {
					if ((in + 2) >= last) break;

					/*
					 * [2,in:8,in1:6] [in1:2,in2:4] [in2:4]
					 * ----- 16 ----- ----- 6 ----- ...
					 */

					initial_sample =
						(heading_bits << (16-2)) |            /* SI16 UB[2] */
						(*(unsigned char*)in << (16-2-8)) |   /* SI16 UB[8] */
						(*(unsigned char*)(in+1) >> 2);       /* SI16 UB[6] */
					initial_sample = (int16_t)initial_sample;

					initial_index =
						((*(unsigned char*)(in+1) & 3) << 4) |   /* UB[6] UB[2] */
						(*(unsigned char*)(in+2) >> 4);          /* UB[6] UB[4] */

					heading_bits = 0;
					amount_of_heading_bits = 0;

					int i0 = *(unsigned char*)(in+2) & 15;   /* UB[4] for next delta */

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 3;

					/* got ADPCMMONOPACKET header
					 *
					 * note: we still have UB[4] hanging on i0
					 *
					 *  2|2]: 0 bytes to get even
					 *
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					*outputp++ = adpcm_decode2bit(i0 >> 2, state);
					*outputp++ = adpcm_decode2bit(i0 & 3, state);

					count = 2;
					sample_number += 2;
				} else if (amount_of_heading_bits == 6) {
					if ((in + 1) >= last) break;

					/*
					 * [6,in:8,in1:2] [in1:6] (we are even!)
					 * ----- 16 ----- -- 6 -- ...
					 */

					initial_sample =
						(heading_bits << (16-6)) |            /* SI16 UB[6] */
						(*(unsigned char*)in << (16-6-8)) |   /* SI16 UB[8] */
						(*(unsigned char*)(in+1) >> 6);       /* SI16 UB[2] */
					initial_sample = (int16_t)initial_sample;

					initial_index = *(unsigned char*)(in+1) & 63;  /* UB[6] UB[6] (we are even!) */

					heading_bits = 0;
					amount_of_heading_bits = 0;

					//DEBUG("initial_sample=%i", initial_sample);
					//DEBUG("initial_index=%i", initial_index);

					in += 2;

					/* got ADPCMMONOPACKET header
					 */

					state->index = initial_index;
					state->sample = initial_sample;
					outputp = output;
					*outputp++ = initial_sample;
					sample_number++;

					count = 0;
				} else {
					DEBUG("exhaustion, amount_of_heading_bits=%i", amount_of_heading_bits);
					exit(1);
				}

				while (in < last && count < 4095) {
					int i0 = *(unsigned char*)in++;

					*outputp++ = adpcm_decode2bit(i0 >> 6, state);
					count++;
					sample_number++;
					if (count >= 4095) {
						assert(count == 4095);
						heading_bits = i0 & 63;
						amount_of_heading_bits = 6;
						break;
					}

					*outputp++ = adpcm_decode2bit((i0 >> 4) & 3, state);
					count++;
					sample_number++;
					if (count >= 4095) {
						assert(count == 4095);
						heading_bits = i0 & 15;
						amount_of_heading_bits = 4;
						break;
					}

					*outputp++ = adpcm_decode2bit((i0 >> 2) & 3, state);
					count++;
					sample_number++;
					if (count >= 4095) {
						assert(count == 4095);
						heading_bits = i0 & 3;
						amount_of_heading_bits = 2;
						break;
					}

					*outputp++ = adpcm_decode2bit(i0 & 3, state);
					count++;
					sample_number++;
				}

				if (in < last && count != 4095) {
					DEBUG("exhaustion, count=%i, last-in=%i", count, (int)(last-in));
					exit(1);
				}

				int output_len = ((unsigned char*)outputp) - ((unsigned char*)output);
				//DEBUG("count=%i, outputp-output=%i", count, output_len);

				assert(write_exact(fd, output, output_len) == output_len);

			}

			/* close output
			 */

			if (fd != STDOUT_FILENO) {
				assert(close(fd) == 0);
				fd = -1;
			}

			break;
		}
		default:
			DEBUG("exhaustion/not-implemented");
			exit(1);
		}
	}

	/* cleanup
	 */
	str_free(input);

	return 0;
}

int main(int argc, char **argv)
{
	struct getopt_x state[1];

	if (process_args(state, argc, argv)) {
		help(argv[0], state);
		exit(1);
	}

	if (argc == 1) {
		help(argv[0], state);
		exit(0);
	}

	if (doit(args->input_file->s)) {
		return 1;
	}

	return 0;
}

#if 0
static int read_exact(int fd, void *buf, int len)
{
	int i, got=0;
	do {
		if ((i = read(fd, buf + got, len - got)) <= 0) return i;
		got += i;
	} while (got < len);
	return len;
}
#endif

static int write_exact(int fd, void *buf, int len)
{
	int i, wrote = 0;
	do {
		if ((i = write(fd, buf + wrote, len - wrote)) <= 0) return i;
		wrote += i;
	} while (wrote < len);
	return len;
}
