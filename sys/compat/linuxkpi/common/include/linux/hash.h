/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 NVIDIA corporation & affiliates.
 * Copyright (c) 2013 François Tigeot
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _LINUXKPI_LINUX_HASH_H_
#define	_LINUXKPI_LINUX_HASH_H_

#include <sys/param.h>
#include <sys/systm.h>

#include <asm/types.h>

#include <linux/bitops.h>

/* Knuth, TAOCP vol 3, sec 6.4: multiply by 2^N * (1 - phi); use high bits. */
#define	GOLDEN_RATIO_32	0x61C88647U
#define	GOLDEN_RATIO_64	0x61C8864680B583EBULL

static inline u32
__hash_32(u32 val)
{

	return (val * GOLDEN_RATIO_32);
}

static inline u32
hash_32(u32 val, unsigned int bits)
{

	return (__hash_32(val) >> (32 - bits));
}

static inline u32
hash_64(u64 val, unsigned int bits)
{
#if BITS_PER_LONG == 64
	return ((u32)((val * GOLDEN_RATIO_64) >> (64 - bits)));
#else
	return (hash_32((u32)val ^ __hash_32(val >> 32), bits));
#endif
}

#if BITS_PER_LONG == 64
#define	hash_long(...) hash_64(__VA_ARGS__)
#else
#define	hash_long(...) hash_32(__VA_ARGS__)
#endif

#endif					/* _LINUXKPI_LINUX_HASH_H_ */
