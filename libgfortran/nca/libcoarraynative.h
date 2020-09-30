/* Copyright (C) 2019-2020 Free Software Foundation, Inc.
   Contributed by Nicolas Koenig

This file is part of the GNU Fortran Native Coarray Library (libnca).

Libnca is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

Libnca is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Under Section 7 of GPL version 3, you are granted additional
permissions described in the GCC Runtime Library Exception, version
3.1, as published by the Free Software Foundation.

You should have received a copy of the GNU General Public License and
a copy of the GCC Runtime Library Exception along with this program;
see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
<http://www.gnu.org/licenses/>.  */

#ifndef LIBGFOR_H
#error "Include libgfortran.h before libcoarraynative.h"
#endif

#ifndef COARRAY_NATIVE_HDR
#define COARRAY_NATIVE_HDR

#include "libgfortran.h"

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>


/* This is to create a _nca_gfortrani_ prefix for all variables and
   function used only by nca.  */
#if 0
#define NUM_ADDR_BITS (8 * sizeof (int *))
#endif

#define DEBUG_NATIVE_COARRAY 1

#ifdef DEBUG_NATIVE_COARRAY
#define DEBUG_PRINTF(...) dprintf (2,__VA_ARGS__)
#else
#define DEBUG_PRINTF(...) do {} while(0)
#endif

#include "allocator.h"
#include "hashmap.h"
#include "sync.h"
#include "lock.h"
#include "collective_subroutine.h"

typedef struct {
  pthread_barrier_t barrier;
  int maximg;
} ipcollsub;

typedef enum {
  IMAGE_UNKNOWN = 0,
  IMAGE_OK,
  IMAGE_FAILED
} image_status;

typedef struct {
  image_status status;
  pid_t pid;
} image_tracker;

typedef struct {
  int has_failed_image;
  image_tracker images[];
} master;

typedef struct {
  int image_num;
  master *m;
} image;

extern image this_image;

typedef struct {
  int num_images;
  shared_memory sm;
  alloc_iface ai;
  collsub_iface ci;
  sync_iface si;
} nca_local_data;

extern nca_local_data *local;
internal_proto (local);
void ensure_initialization(void);
internal_proto(ensure_initialization);

void nca_master(void (*)(void));
export_proto (nca_master);

#endif
