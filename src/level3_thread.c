/*********************************************************************/
/* Copyright 2009, 2010 The University of Texas at Austin.           */
/* All rights reserved.                                              */
/*                                                                   */
/* Redistribution and use in source and binary forms, with or        */
/* without modification, are permitted provided that the following   */
/* conditions are met:                                               */
/*                                                                   */
/*   1. Redistributions of source code must retain the above         */
/*      copyright notice, this list of conditions and the following  */
/*      disclaimer.                                                  */
/*                                                                   */
/*   2. Redistributions in binary form must reproduce the above      */
/*      copyright notice, this list of conditions and the following  */
/*      disclaimer in the documentation and/or other materials       */
/*      provided with the distribution.                              */
/*                                                                   */
/*    THIS  SOFTWARE IS PROVIDED  BY THE  UNIVERSITY OF  TEXAS AT    */
/*    AUSTIN  ``AS IS''  AND ANY  EXPRESS OR  IMPLIED WARRANTIES,    */
/*    INCLUDING, BUT  NOT LIMITED  TO, THE IMPLIED  WARRANTIES OF    */
/*    MERCHANTABILITY  AND FITNESS FOR  A PARTICULAR  PURPOSE ARE    */
/*    DISCLAIMED.  IN  NO EVENT SHALL THE UNIVERSITY  OF TEXAS AT    */
/*    AUSTIN OR CONTRIBUTORS BE  LIABLE FOR ANY DIRECT, INDIRECT,    */
/*    INCIDENTAL,  SPECIAL, EXEMPLARY,  OR  CONSEQUENTIAL DAMAGES    */
/*    (INCLUDING, BUT  NOT LIMITED TO,  PROCUREMENT OF SUBSTITUTE    */
/*    GOODS  OR  SERVICES; LOSS  OF  USE,  DATA,  OR PROFITS;  OR    */
/*    BUSINESS INTERRUPTION) HOWEVER CAUSED  AND ON ANY THEORY OF    */
/*    LIABILITY, WHETHER  IN CONTRACT, STRICT  LIABILITY, OR TORT    */
/*    (INCLUDING NEGLIGENCE OR OTHERWISE)  ARISING IN ANY WAY OUT    */
/*    OF  THE  USE OF  THIS  SOFTWARE,  EVEN  IF ADVISED  OF  THE    */
/*    POSSIBILITY OF SUCH DAMAGE.                                    */
/*                                                                   */
/* The views and conclusions contained in the software and           */
/* documentation are those of the authors and should not be          */
/* interpreted as representing official policies, either expressed   */
/* or implied, of The University of Texas at Austin.                 */
/*********************************************************************/
#include "macro.h"
#include "common.h"

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 8
#endif

#ifndef DIVIDE_RATE
#define DIVIDE_RATE 2
#endif

#ifndef SWITCH_RATIO
#define SWITCH_RATIO 2
#endif

//The array of job_t may overflow the stack.
//Instead, use malloc to alloc job_t.
#if MAX_CPU_NUMBER > BLAS3_MEM_ALLOC_THRESHOLD
#define USE_ALLOC_HEAP
#endif

typedef struct {
  volatile BLASLONG working[MAX_CPU_NUMBER][CACHE_LINE_SIZE * DIVIDE_RATE];
} job_t;

#define BETA_OPERATION(M_FROM, M_TO, N_FROM, N_TO, BETA, C, LDC) \
	GEMM_BETA((M_TO) - (M_FROM), (N_TO - N_FROM), 0, \
		  BETA[0], NULL, 0, NULL, 0, \
		  (FLOAT *)(C) + ((M_FROM) + (N_FROM) * (LDC)) * COMPSIZE, LDC)

#define KERNEL_OPERATION(M, N, K, ALPHA, SA, SB, C, LDC, X, Y) \
	sgemm_kernel(M, N, K, ALPHA[0], SA, SB, (FLOAT *)(C) + ((X) + (Y) * LDC) * COMPSIZE, LDC)


#define KERNEL_OPERATION_FIX12(M, N, K, ALPHA, SA, SB, C, LDC, X, Y) \
	sgemm_kernel_fix12(M, N, K, ALPHA[0], SA, SB, (FLOAT *)(C) + ((X) + (Y) * LDC) * COMPSIZE, LDC)
#define KERNEL_OPERATION_FIX13(M, N, K, ALPHA, SA, SB, C, LDC, X, Y) \
	sgemm_kernel_fix13(M, N, K, ALPHA[0], SA, SB, (FLOAT *)(C) + ((X) + (Y) * LDC) * COMPSIZE, LDC)	
#define KERNEL_OPERATION_FIX14(M, N, K, ALPHA, SA, SB, C, LDC, X, Y) \
	sgemm_kernel_fix14(M, N, K, ALPHA[0], SA, SB, (FLOAT *)(C) + ((X) + (Y) * LDC) * COMPSIZE, LDC)
#define KERNEL_OPERATION_FIX15(M, N, K, ALPHA, SA, SB, C, LDC, X, Y) \
	sgemm_kernel_fix15(M, N, K, ALPHA[0], SA, SB, (FLOAT *)(C) + ((X) + (Y) * LDC) * COMPSIZE, LDC)

#ifndef A
#define A	args -> a
#endif
#ifndef LDA
#define LDA	args -> lda
#endif
#ifndef B
#define B	args -> b
#endif
#ifndef LDB
#define LDB	args -> ldb
#endif
#ifndef C
#define C	args -> c
#endif
#ifndef LDC
#define LDC	args -> ldc
#endif
#ifndef M
#define M	args -> m
#endif
#ifndef N
#define N	args -> n
#endif
#ifndef K
#define K	args -> k
#endif

//#define TIMING

#ifdef TIMING
#define START_RPCC()		rpcc_counter = rpcc()
#define STOP_RPCC(COUNTER)	COUNTER  += rpcc() - rpcc_counter
#else
#define START_RPCC()
#define STOP_RPCC(COUNTER)
#endif

static int inner_thread(blas_arg_t *args, BLASLONG *range_m, BLASLONG *range_n, FLOAT *sa, FLOAT *sb, BLASLONG mypos){

  FLOAT *buffer[DIVIDE_RATE];

  BLASLONG k, lda, ldb, ldc;
  BLASLONG m_from, m_to, n_from, n_to, N_from, N_to;
  int fractions = args->fractions;
  FLOAT *alpha, *beta;
  FLOAT *a, *b, *c;
  job_t *job = (job_t *)args -> common;
  BLASLONG xxx, bufferside;

  BLASLONG ls, min_l, jjs, min_jj;
  BLASLONG is, min_i, div_n;

  BLASLONG i, current;
  BLASLONG l1stride;

#ifdef TIMING
  BLASULONG rpcc_counter;
  BLASULONG copy_A = 0;
  BLASULONG copy_B = 0;
  BLASULONG kernel = 0;
  BLASULONG waiting1 = 0;
  BLASULONG waiting2 = 0;
  BLASULONG waiting3 = 0;
  BLASULONG waiting6[MAX_CPU_NUMBER];
  BLASULONG ops    = 0;

  for (i = 0; i < args -> nthreads; i++) waiting6[i] = 0;
#endif

  k = K;

  a = (FLOAT *)A;
  b = (FLOAT *)B;
  c = (FLOAT *)C;

  lda = LDA;
  ldb = LDB;
  ldc = LDC;

  alpha = (FLOAT *)args -> alpha;
  beta  = (FLOAT *)args -> beta;

  m_from = 0;
  m_to   = M;

  if (range_m) {
    m_from = range_m[0];
    m_to   = range_m[1];
  }

  n_from = 0;
  n_to   = N;

  N_from = 0;
  N_to   = N;

  if (range_n) {
    n_from = range_n[mypos + 0];
    n_to   = range_n[mypos + 1];

    N_from = range_n[0];
    N_to   = range_n[args -> nthreads];
  }

  if (beta) {
#ifndef COMPLEX
    if (beta[0] != ONE)
#else
    if ((beta[0] != ONE) || (beta[1] != ZERO))
#endif
      BETA_OPERATION(m_from, m_to, N_from, N_to, beta, c, ldc);
  }

  if ((k == 0) || (alpha == NULL)) return 0;

  if ((alpha[0] == ZERO)
#ifdef COMPLEX
      && (alpha[1] == ZERO)
#endif
      ) return 0;

  div_n = (n_to - n_from + DIVIDE_RATE - 1) / DIVIDE_RATE;

  buffer[0] = sb;
  for (i = 1; i < DIVIDE_RATE; i++) {
#ifdef FIX_ENABLE
    buffer[i] = (FLOAT *)(((short int*)buffer[i - 1]) + GEMM_Q * ((div_n + GEMM_UNROLL_N - 1)/GEMM_UNROLL_N) * GEMM_UNROLL_N * COMPSIZE);
#else
	buffer[i] = buffer[i - 1] + GEMM_Q * ((div_n + GEMM_UNROLL_N - 1)/GEMM_UNROLL_N) * GEMM_UNROLL_N * COMPSIZE;
#endif
  }


  for(ls = 0; ls < k; ls += min_l){

    min_l = k - ls;

    if (min_l >= GEMM_Q * 2) {
      min_l  = GEMM_Q;
    } else {
      if (min_l > GEMM_Q) min_l = (min_l + 1) / 2;
    }

    l1stride = 1;
    min_i = m_to - m_from;

    if (min_i >= GEMM_P * 2) {
      min_i = GEMM_P;
    } else {
      if (min_i > GEMM_P) {
	min_i = ((min_i / 2 + GEMM_UNROLL_M - 1)/GEMM_UNROLL_M) * GEMM_UNROLL_M;
      } else {
	if (args -> nthreads == 1) l1stride = 0;
      }
    }

    START_RPCC();
	if (12 == fractions)
		sgemm_otcopy_fix12(min_l, min_i, (FLOAT *)(a) + ((m_from) + (ls) * (lda)) * COMPSIZE, lda, sa);
	else if (13 == fractions)
		sgemm_otcopy_fix13(min_l, min_i, (FLOAT *)(a) + ((m_from) + (ls) * (lda)) * COMPSIZE, lda, sa);
	else if (14 == fractions)
		sgemm_otcopy_fix14(min_l, min_i, (FLOAT *)(a) + ((m_from) + (ls) * (lda)) * COMPSIZE, lda, sa);
	else if (15 == fractions)
		sgemm_otcopy_fix15(min_l, min_i, (FLOAT *)(a) + ((m_from) + (ls) * (lda)) * COMPSIZE, lda, sa);
	else if (0 ==fractions)
		sgemm_otcopy(min_l, min_i, (FLOAT *)(a) + ((m_from) + (ls) * (lda)) * COMPSIZE, lda, sa);
	else
		printf("fractions not support\n");
    STOP_RPCC(copy_A);

    div_n = (n_to - n_from + DIVIDE_RATE - 1) / DIVIDE_RATE;

    for (xxx = n_from, bufferside = 0; xxx < n_to; xxx += div_n, bufferside ++) {

      START_RPCC();

      /* Make sure if no one is using buffer */
      for (i = 0; i < args -> nthreads; i++)
	while (job[mypos].working[i][CACHE_LINE_SIZE * bufferside]) {YIELDING;};

      STOP_RPCC(waiting1);

      for(jjs = xxx; jjs < MIN(n_to, xxx + div_n); jjs += min_jj){
	min_jj = MIN(n_to, xxx + div_n) - jjs;

	if (min_jj >= 3*GEMM_UNROLL_N) min_jj = 3*GEMM_UNROLL_N;
	else
		if (min_jj >= 2*GEMM_UNROLL_N) min_jj = 2*GEMM_UNROLL_N;
		else
			if (min_jj > GEMM_UNROLL_N) min_jj = GEMM_UNROLL_N;


	START_RPCC();
	if (12 == fractions)
		sgemm_oncopy_fix12(min_l, min_jj, (FLOAT *)(b) + ((ls) + (jjs) * (ldb)) * COMPSIZE, ldb, (float *)(((short int*)buffer[bufferside]) + min_l * (jjs - xxx) * COMPSIZE * l1stride));
	else if (13 == fractions)
		sgemm_oncopy_fix13(min_l, min_jj, (FLOAT *)(b) + ((ls) + (jjs) * (ldb)) * COMPSIZE, ldb, (float *)(((short int*)buffer[bufferside]) + min_l * (jjs - xxx) * COMPSIZE * l1stride));
	else if (14 == fractions)
		sgemm_oncopy_fix14(min_l, min_jj, (FLOAT *)(b) + ((ls) + (jjs) * (ldb)) * COMPSIZE, ldb, (float *)(((short int*)buffer[bufferside]) + min_l * (jjs - xxx) * COMPSIZE * l1stride));
	else if (15 == fractions)
		sgemm_oncopy_fix15(min_l, min_jj, (FLOAT *)(b) + ((ls) + (jjs) * (ldb)) * COMPSIZE, ldb, (float *)(((short int*)buffer[bufferside]) + min_l * (jjs - xxx) * COMPSIZE * l1stride));
	else if (0 ==fractions)
		sgemm_oncopy(min_l, min_jj, (FLOAT *)(b) + ((ls) + (jjs) * (ldb)) * COMPSIZE, ldb, buffer[bufferside] + min_l * (jjs - xxx) * COMPSIZE * l1stride);
	else
		printf("fractions not support\n");
	STOP_RPCC(copy_B);

	START_RPCC();

	if (12 == fractions)
		KERNEL_OPERATION_FIX12(min_i, min_jj, min_l, alpha,
				 sa, (float *)(((short int*)buffer[bufferside]) + min_l * (jjs - xxx) * COMPSIZE * l1stride),
				 c, ldc, m_from, jjs);
	else if (13 ==fractions)
		KERNEL_OPERATION_FIX13(min_i, min_jj, min_l, alpha,
				 sa, (float *)(((short int*)buffer[bufferside]) + min_l * (jjs - xxx) * COMPSIZE * l1stride),
				 c, ldc, m_from, jjs);
	else if (14 ==fractions)
		KERNEL_OPERATION_FIX14(min_i, min_jj, min_l, alpha,
				 sa, (float *)(((short int*)buffer[bufferside]) + min_l * (jjs - xxx) * COMPSIZE * l1stride),
				 c, ldc, m_from, jjs);
	else if (15 ==fractions)
		KERNEL_OPERATION_FIX15(min_i, min_jj, min_l, alpha,
				 sa, (float *)(((short int*)buffer[bufferside]) + min_l * (jjs - xxx) * COMPSIZE * l1stride),
				 c, ldc, m_from, jjs);
	else if (0 ==fractions)
		KERNEL_OPERATION(min_i, min_jj, min_l, alpha,
				 sa, buffer[bufferside] + min_l * (jjs - xxx) * COMPSIZE * l1stride,
				 c, ldc, m_from, jjs);
	else
		printf("fractions not support\n");

	STOP_RPCC(kernel);

#ifdef TIMING
	  ops += 2 * min_i * min_jj * min_l;
#endif

      }

      for (i = 0; i < args -> nthreads; i++) job[mypos].working[i][CACHE_LINE_SIZE * bufferside] = (BLASLONG)buffer[bufferside];
      WMB;
    }

    current = mypos;

    do {
      current ++;
      if (current >= args -> nthreads) current = 0;

      div_n = (range_n[current + 1]  - range_n[current] + DIVIDE_RATE - 1) / DIVIDE_RATE;

      for (xxx = range_n[current], bufferside = 0; xxx < range_n[current + 1]; xxx += div_n, bufferside ++) {

	if (current != mypos) {

	  START_RPCC();

	  /* thread has to wait */
	  while(job[current].working[mypos][CACHE_LINE_SIZE * bufferside] == 0) {YIELDING;};

	  STOP_RPCC(waiting2);

	  START_RPCC();

	if (12 == fractions)
		KERNEL_OPERATION_FIX12(min_i, MIN(range_n[current + 1]  - xxx,  div_n), min_l, alpha,
						 sa, (FLOAT *)job[current].working[mypos][CACHE_LINE_SIZE * bufferside],
						 c, ldc, m_from, xxx);
	else if (13 == fractions)
		KERNEL_OPERATION_FIX13(min_i, MIN(range_n[current + 1]  - xxx,  div_n), min_l, alpha,
						 sa, (FLOAT *)job[current].working[mypos][CACHE_LINE_SIZE * bufferside],
						 c, ldc, m_from, xxx);
	else if (14 == fractions)
		KERNEL_OPERATION_FIX14(min_i, MIN(range_n[current + 1]  - xxx,  div_n), min_l, alpha,
						 sa, (FLOAT *)job[current].working[mypos][CACHE_LINE_SIZE * bufferside],
						 c, ldc, m_from, xxx);
	else if (15 == fractions)
		KERNEL_OPERATION_FIX15(min_i, MIN(range_n[current + 1]  - xxx,  div_n), min_l, alpha,
						 sa, (FLOAT *)job[current].working[mypos][CACHE_LINE_SIZE * bufferside],
						 c, ldc, m_from, xxx);
	else if (0 ==fractions)
	  KERNEL_OPERATION(min_i, MIN(range_n[current + 1]  - xxx,  div_n), min_l, alpha,
			   sa, (FLOAT *)job[current].working[mypos][CACHE_LINE_SIZE * bufferside],
			   c, ldc, m_from, xxx);
	else
		printf("fractions not support\n");

	STOP_RPCC(kernel);
#ifdef TIMING
	  ops += 2 * min_i * MIN(range_n[current + 1]  - xxx,  div_n) * min_l;
#endif
	}

	if (m_to - m_from == min_i) {
	  job[current].working[mypos][CACHE_LINE_SIZE * bufferside] &= 0;
	}
      }
    } while (current != mypos);


    for(is = m_from + min_i; is < m_to; is += min_i){
      min_i = m_to - is;

      if (min_i >= GEMM_P * 2) {
	min_i = GEMM_P;
      } else
	if (min_i > GEMM_P) {
	  min_i = (((min_i + 1) / 2 + GEMM_UNROLL_M - 1)/GEMM_UNROLL_M) * GEMM_UNROLL_M;
	}

      START_RPCC();

	if (12 == fractions)
		sgemm_otcopy_fix12(min_l, min_i, (FLOAT *)(a) + ((is) + (ls) * (lda)) * COMPSIZE, lda, sa);
	else if (13 == fractions)
		sgemm_otcopy_fix13(min_l, min_i, (FLOAT *)(a) + ((is) + (ls) * (lda)) * COMPSIZE, lda, sa);
	else if (14 == fractions)
		sgemm_otcopy_fix14(min_l, min_i, (FLOAT *)(a) + ((is) + (ls) * (lda)) * COMPSIZE, lda, sa);
	else if (15 == fractions)
		sgemm_otcopy_fix15(min_l, min_i, (FLOAT *)(a) + ((is) + (ls) * (lda)) * COMPSIZE, lda, sa);
	else if (0 ==fractions)
		sgemm_otcopy(min_l, min_i, (FLOAT *)(a) + ((is) + (ls) * (lda)) * COMPSIZE, lda, sa);
	else
		printf("fractions not support\n");

      STOP_RPCC(copy_A);

      current = mypos;
      do {

	div_n = (range_n[current + 1]  - range_n[current] + DIVIDE_RATE - 1) / DIVIDE_RATE;

	for (xxx = range_n[current], bufferside = 0; xxx < range_n[current + 1]; xxx += div_n, bufferside ++) {

	  START_RPCC();


	if (12 == fractions)
		KERNEL_OPERATION_FIX12(min_i, MIN(range_n[current + 1] - xxx, div_n), min_l, alpha,
				 sa, (FLOAT *)job[current].working[mypos][CACHE_LINE_SIZE * bufferside],
				 c, ldc, is, xxx);
	else if (13 == fractions)
		KERNEL_OPERATION_FIX13(min_i, MIN(range_n[current + 1] - xxx, div_n), min_l, alpha,
			 sa, (FLOAT *)job[current].working[mypos][CACHE_LINE_SIZE * bufferside],
			 c, ldc, is, xxx);
	else if (14 == fractions)
		KERNEL_OPERATION_FIX14(min_i, MIN(range_n[current + 1] - xxx, div_n), min_l, alpha,
			 sa, (FLOAT *)job[current].working[mypos][CACHE_LINE_SIZE * bufferside],
			 c, ldc, is, xxx);
	else if (15 == fractions)
		KERNEL_OPERATION_FIX15(min_i, MIN(range_n[current + 1] - xxx, div_n), min_l, alpha,
			 sa, (FLOAT *)job[current].working[mypos][CACHE_LINE_SIZE * bufferside],
			 c, ldc, is, xxx);
	else if (0 ==fractions)
	  KERNEL_OPERATION(min_i, MIN(range_n[current + 1] - xxx, div_n), min_l, alpha,
			   sa, (FLOAT *)job[current].working[mypos][CACHE_LINE_SIZE * bufferside],
			   c, ldc, is, xxx);
	else
		printf("fractions not support\n");

	STOP_RPCC(kernel);

#ifdef TIMING
	ops += 2 * min_i * MIN(range_n[current + 1]  - xxx, div_n) * min_l;
#endif

	if (is + min_i >= m_to) {
	  /* Thread doesn't need this buffer any more */
	  job[current].working[mypos][CACHE_LINE_SIZE * bufferside] &= 0;
	  WMB;
	}
	}

	current ++;
	if (current >= args -> nthreads) current = 0;

      } while (current != mypos);

    }

  }

  START_RPCC();

  for (i = 0; i < args -> nthreads; i++) {
    for (xxx = 0; xxx < DIVIDE_RATE; xxx++) {
      while (job[mypos].working[i][CACHE_LINE_SIZE * xxx] ) {YIELDING;};
    }
  }

  STOP_RPCC(waiting3);

#ifdef TIMING
  BLASLONG waiting = waiting1 + waiting2 + waiting3;
  BLASLONG total = copy_A + copy_B + kernel + waiting;

  fprintf(stderr, "GEMM   [%2ld] Copy_A(tcopy) : %6.2f  Copy_B(ncopy) : %6.2f  Wait1 : %6.2f Wait2 : %6.2f Wait3 : %6.2f Kernel : %6.2f Kernel : %6.2f",
	  mypos, (double)copy_A /(double)total * 100., (double)copy_B /(double)total * 100.,
	  (double)waiting1 /(double)total * 100.,
	  (double)waiting2 /(double)total * 100.,
	  (double)waiting3 /(double)total * 100.,
	  (double)kernel /(double)total * 100.,
	  (double)ops/(double)kernel / 4. * 100.);

#if 0
  fprintf(stderr, "GEMM   [%2ld] Copy_A : %6.2ld  Copy_B : %6.2ld  Wait : %6.2ld\n",
	  mypos, copy_A, copy_B, waiting);

  fprintf(stderr, "Waiting[%2ld] %6.2f %6.2f %6.2f\n",
	  mypos,
	  (double)waiting1/(double)waiting * 100.,
	  (double)waiting2/(double)waiting * 100.,
	  (double)waiting3/(double)waiting * 100.);
#endif
  fprintf(stderr, "\n");
#endif

  return 0;
}

static int gemm_driver(blas_arg_t *args, BLASLONG *range_m, BLASLONG
		       *range_n, FLOAT *sa, FLOAT *sb, BLASLONG mypos){

  blas_arg_t newarg;

#ifndef USE_ALLOC_HEAP
  job_t          job[MAX_CPU_NUMBER];
#else
  job_t *        job = NULL;
#endif

  blas_queue_t queue[MAX_CPU_NUMBER];

  BLASLONG range_M[MAX_CPU_NUMBER + 1];
  BLASLONG range_N[MAX_CPU_NUMBER + 1];

  BLASLONG num_cpu_m, num_cpu_n;

  BLASLONG nthreads = args -> nthreads;

  BLASLONG width, i, j, k, js;
  BLASLONG m, n, n_from, n_to;
  int  mode;

#ifndef COMPLEX
#ifdef XDOUBLE
  mode  =  BLAS_XDOUBLE | BLAS_REAL | BLAS_NODE;
#elif defined(DOUBLE)
  mode  =  BLAS_DOUBLE  | BLAS_REAL | BLAS_NODE;
#else
  mode  =  BLAS_SINGLE  | BLAS_REAL | BLAS_NODE;
#endif
#else
#ifdef XDOUBLE
  mode  =  BLAS_XDOUBLE | BLAS_COMPLEX | BLAS_NODE;
#elif defined(DOUBLE)
  mode  =  BLAS_DOUBLE  | BLAS_COMPLEX | BLAS_NODE;
#else
  mode  =  BLAS_SINGLE  | BLAS_COMPLEX | BLAS_NODE;
#endif
#endif

  newarg.m        = args -> m;
  newarg.n        = args -> n;
  newarg.k        = args -> k;
  newarg.a        = args -> a;
  newarg.b        = args -> b;
  newarg.c        = args -> c;
  newarg.lda      = args -> lda;
  newarg.ldb      = args -> ldb;
  newarg.ldc      = args -> ldc;
  newarg.alpha    = args -> alpha;
  newarg.beta     = args -> beta;
  newarg.nthreads = args -> nthreads;
  newarg.fractions = args->fractions;

#ifdef USE_ALLOC_HEAP
  job = (job_t*)malloc(MAX_CPU_NUMBER * sizeof(job_t));
  if(job==NULL){
    fprintf(stderr, "OpenBLAS: malloc failed in %s\n", __func__);
    exit(1);
  }
#endif

  newarg.common   = (void *)job;

#ifdef PARAMTEST
  newarg.gemm_p  = args -> gemm_p;
  newarg.gemm_q  = args -> gemm_q;
  newarg.gemm_r  = args -> gemm_r;
#endif

  if (!range_m) {
    range_M[0] = 0;
    m          = args -> m;
  } else {
    range_M[0] = range_m[0];
    m          = range_m[1] - range_m[0];
  }

  num_cpu_m  = 0;

  while (m > 0){

    width  = blas_quickdivide(m + nthreads - num_cpu_m - 1, nthreads - num_cpu_m);

    m -= width;
    if (m < 0) width = width + m;

    range_M[num_cpu_m + 1] = range_M[num_cpu_m] + width;
    num_cpu_m ++;
  }

  for (i = 0; i < num_cpu_m; i++) {
    queue[i].mode    = mode;
    queue[i].routine = inner_thread;
    queue[i].args    = &newarg;
    queue[i].range_m = &range_M[i];
    queue[i].range_n = &range_N[0];
    queue[i].sa      = NULL;
    queue[i].sb      = NULL;
    queue[i].next    = &queue[i + 1];
  }

  queue[0].sa = sa;
  queue[0].sb = sb;

  if (!range_n) {
    n_from = 0;
    n_to   = args -> n;
  } else {
    n_from = range_n[0];
    n_to   = range_n[1];
  }

  for(js = n_from; js < n_to; js += GEMM_R * nthreads){
    n = n_to - js;
    if (n > GEMM_R * nthreads) n = GEMM_R * nthreads;

    range_N[0] = js;

    num_cpu_n  = 0;

    while (n > 0){

      width  = blas_quickdivide(n + nthreads - num_cpu_n - 1, nthreads - num_cpu_n);

      n -= width;
      if (n < 0) width = width + n;

      range_N[num_cpu_n + 1] = range_N[num_cpu_n] + width;

      num_cpu_n ++;
    }

    for (j = 0; j < num_cpu_m; j++) {
      for (i = 0; i < num_cpu_m; i++) {
	for (k = 0; k < DIVIDE_RATE; k++) {
	  job[j].working[i][CACHE_LINE_SIZE * k] = 0;
	}
      }
    }

    queue[num_cpu_m - 1].next = NULL;

    exec_blas(num_cpu_m, queue);
  }

#ifdef USE_ALLOC_HEAP
  free(job);
#endif

  return 0;
}

int sgemm_thread_nn(blas_arg_t *args, BLASLONG *range_m, BLASLONG *range_n, FLOAT *sa, FLOAT *sb, BLASLONG mypos, int fractions){
  BLASLONG m = args -> m;
  BLASLONG n = args -> n;
  BLASLONG nthreads = args -> nthreads;
  BLASLONG divN, divT;
  int mode;

	args->fractions = fractions;
	
   if (nthreads  == 1) {
    sgemm_nn(args, range_m, range_n, sa, sb, 0, fractions);
    return 0;
  }


  if (range_m) {
    BLASLONG m_from = *(((BLASLONG *)range_m) + 0);
    BLASLONG m_to   = *(((BLASLONG *)range_m) + 1);

    m = m_to - m_from;
  }

  if (range_n) {
    BLASLONG n_from = *(((BLASLONG *)range_n) + 0);
    BLASLONG n_to   = *(((BLASLONG *)range_n) + 1);

    n = n_to - n_from;
  }

  if ((m < nthreads * SWITCH_RATIO) || (n < nthreads * SWITCH_RATIO)) {
    sgemm_nn(args, range_m, range_n, sa, sb, 0, fractions);
    return 0;
  }

  divT = nthreads;
  divN = 1;

#if 0
  while ((GEMM_P * divT > m * SWITCH_RATIO) && (divT > 1)) {
    do {
      divT --;
      divN = 1;
      while (divT * divN < nthreads) divN ++;
    } while ((divT * divN != nthreads) && (divT > 1));
  }
#endif

  args -> nthreads = divT;

  if (divN == 1){
    gemm_driver(args, range_m, range_n, sa, sb, 0);
  } else {
#ifndef COMPLEX
#ifdef XDOUBLE
    mode  =  BLAS_XDOUBLE | BLAS_REAL;
#elif defined(DOUBLE)
    mode  =  BLAS_DOUBLE  | BLAS_REAL;
#else
    mode  =  BLAS_SINGLE  | BLAS_REAL;
#endif
#else
#ifdef XDOUBLE
    mode  =  BLAS_XDOUBLE | BLAS_COMPLEX;
#elif defined(DOUBLE)
    mode  =  BLAS_DOUBLE  | BLAS_COMPLEX;
#else
    mode  =  BLAS_SINGLE  | BLAS_COMPLEX;
#endif
#endif

#if defined(TN) || defined(TT) || defined(TR) || defined(TC) || \
    defined(CN) || defined(CT) || defined(CR) || defined(CC)
    mode |= (BLAS_TRANSA_T);
#endif
#if defined(NT) || defined(TT) || defined(RT) || defined(CT) || \
    defined(NC) || defined(TC) || defined(RC) || defined(CC)
    mode |= (BLAS_TRANSB_T);
#endif

#ifdef OS_WINDOWS
    gemm_thread_n(mode, args, range_m, range_n, GEMM_LOCAL,  sa, sb, divN);
#else
    gemm_thread_n(mode, args, range_m, range_n, gemm_driver, sa, sb, divN);
#endif
  }

  return 0;
}
