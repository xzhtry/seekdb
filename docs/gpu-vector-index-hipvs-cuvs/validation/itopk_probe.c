/* itopk_probe: reproduce exactly what the seekdb bridge does on a CAGRA index
 * and dump the raw neighbour ids cuVS returns.
 *
 * Mirrors the bridge: metric=L2(0), build_algo=NN_DESCENT(2), gd, igd, niter,
 * search_width=4, max_queries left at 0 (auto), algo=AUTO(3) / MULTI_CTA(1).
 *
 * usage: itopk_probe base.f32 query.f32 n dim nq topk gd igd niter
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif

static double now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static DLManagedTensor make_tensor(void *data, int device, int dtype_code, int bits,
                                   long r0, long r1)
{
  DLManagedTensor t;
  memset(&t, 0, sizeof(t));
  long *shape = (long *)malloc(sizeof(long) * 2);
  shape[0] = r0;
  shape[1] = r1;
  t.dl_tensor.data = data;
  t.dl_tensor.device.device_type = device ? kDLCUDA : kDLCPU;
  t.dl_tensor.device.device_id = 0;
  t.dl_tensor.ndim = 2;
  t.dl_tensor.dtype.code = dtype_code;
  t.dl_tensor.dtype.bits = bits;
  t.dl_tensor.dtype.lanes = 1;
  t.dl_tensor.shape = shape;
  t.dl_tensor.strides = NULL;
  t.dl_tensor.byte_offset = 0;
  return t;
}

int main(int argc, char **argv)
{
  if (argc < 10) {
    fprintf(stderr, "usage: %s base query n dim nq topk gd igd niter\n", argv[0]);
    return 2;
  }
  const char *base_p = argv[1];
  const char *query_p = argv[2];
  long n = atol(argv[3]), dim = atol(argv[4]);
  long nq = atol(argv[5]), topk = atol(argv[6]);
  long gd = atol(argv[7]), igd = atol(argv[8]), niter = atol(argv[9]);

  float *base = (float *)malloc(sizeof(float) * (size_t)n * dim);
  float *q = (float *)malloc(sizeof(float) * (size_t)nq * dim);
  FILE *f = fopen(base_p, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", base_p); return 4; }
  if (fread(base, sizeof(float), (size_t)n * dim, f) != (size_t)n * dim) { fprintf(stderr, "short base\n"); return 5; }
  fclose(f);
  f = fopen(query_p, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", query_p); return 4; }
  if (fread(q, sizeof(float), (size_t)nq * dim, f) != (size_t)nq * dim) { fprintf(stderr, "short query\n"); return 5; }
  fclose(f);

  cuvsResources_t res;
  if (cuvsResourcesCreate(&res) != CUVS_SUCCESS) { fprintf(stderr, "res create failed\n"); return 5; }

  cuvsCagraIndexParams_t ip;
  if (cuvsCagraIndexParamsCreate(&ip) != CUVS_SUCCESS) { fprintf(stderr, "ip create failed\n"); return 5; }
  ip->metric = (cuvsDistanceType)0;              /* L2 */
  ip->build_algo = (enum cuvsCagraGraphBuildAlgo)2; /* NN_DESCENT */
  ip->graph_degree = (size_t)gd;
  ip->intermediate_graph_degree = (size_t)igd;
  ip->nn_descent_niter = (size_t)niter;

  cuvsCagraIndex_t idx;
  if (cuvsCagraIndexCreate(&idx) != CUVS_SUCCESS) { fprintf(stderr, "idx create failed\n"); return 5; }
  DLManagedTensor dset = make_tensor(base, 0, kDLFloat, 32, n, dim);
  double t0 = now_ms();
  cuvsError_t be = cuvsCagraBuild(res, ip, &dset, idx);
  printf("{\"build_ms\":%.1f,\"err\":%d}\n", now_ms() - t0, (int)be);
  fflush(stdout);
  if (be != CUVS_SUCCESS) { fprintf(stderr, "build failed %d\n", (int)be); return 7; }

  void *qd = NULL, *nd = NULL, *dd = NULL;
  cuvsRMMAlloc(res, &qd, sizeof(float) * (size_t)nq * dim);
  cuvsRMMAlloc(res, &nd, sizeof(unsigned int) * (size_t)nq * topk);
  cuvsRMMAlloc(res, &dd, sizeof(float) * (size_t)nq * topk);
  cudaMemcpy(qd, q, sizeof(float) * (size_t)nq * dim, cudaMemcpyDefault);

  DLManagedTensor qt = make_tensor(qd, 1, kDLFloat, 32, nq, dim);
  DLManagedTensor nt = make_tensor(nd, 1, kDLUInt, 32, nq, topk);
  DLManagedTensor dt = make_tensor(dd, 1, kDLFloat, 32, nq, topk);

  const size_t itopks[] = {512, 1024};
  const int salgos[] = {3 /*AUTO*/, 1 /*MULTI_CTA*/, 0 /*SINGLE_CTA*/};
  const long nqs[] = {1, nq};

  for (size_t qi = 0; qi < sizeof(nqs) / sizeof(nqs[0]); qi++) {
    long qn = nqs[qi];
    if (qn > nq) continue;
    qt.dl_tensor.shape[0] = qn;
    nt.dl_tensor.shape[0] = qn;
    dt.dl_tensor.shape[0] = qn;
    for (size_t wi = 0; wi < sizeof(itopks) / sizeof(itopks[0]); wi++) {
      for (size_t si = 0; si < sizeof(salgos) / sizeof(salgos[0]); si++) {
        cuvsCagraSearchParams_t sp;
        if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { fprintf(stderr, "sp create failed\n"); return 5; }
        sp->itopk_size = itopks[wi];
        sp->search_width = 4;
        sp->algo = (enum cuvsCagraSearchAlgo)salgos[si];
        /* max_queries left at 0 == auto, exactly like the bridge */
        cuvsFilter flt; flt.type = NO_FILTER; flt.addr = (uintptr_t)0;
        double s0 = now_ms();
        cuvsError_t se = cuvsCagraSearch(res, sp, idx, &qt, &nt, &dt, flt);
        double s1 = now_ms();
        if (se != CUVS_SUCCESS) {
          printf("{\"nq\":%ld,\"itopk\":%zu,\"algo\":%d,\"err\":%d}\n", qn, itopks[wi], salgos[si], (int)se);
          fflush(stdout);
          cuvsCagraSearchParamsDestroy(sp);
          continue;
        }
        unsigned int *ids = (unsigned int *)malloc(sizeof(unsigned int) * (size_t)qn * topk);
        cudaMemcpy(ids, nd, sizeof(unsigned int) * (size_t)qn * topk, cudaMemcpyDefault);
        long invalid = 0, u32max = 0, distinct = 0;
        unsigned long mn = 0xFFFFFFFFFFFFFFFFUL, mx = 0;
        for (long r = 0; r < qn; r++) {
          for (long k = 0; k < topk; k++) {
            unsigned int id = ids[r * topk + k];
            if ((unsigned long)id >= (unsigned long)n) invalid++;
            if (id == 0xFFFFFFFFu) u32max++;
            if (id < mn) mn = id;
            if (id > mx) mx = id;
          }
        }
        /* distinct count for the first row */
        {
          unsigned int *seen = (unsigned int *)malloc(sizeof(unsigned int) * (size_t)topk);
          long dcount = 0;
          for (long k = 0; k < topk; k++) {
            int dup = 0;
            for (long j = 0; j < dcount; j++) if (seen[j] == ids[k]) { dup = 1; break; }
            if (!dup) seen[dcount++] = ids[k];
          }
          distinct = dcount;
          free(seen);
        }
        printf("{\"nq\":%ld,\"itopk\":%zu,\"algo\":%d,\"ms\":%.3f,\"invalid\":%ld,\"u32max\":%ld,\"min\":%lu,\"max\":%lu,\"distinct_row0\":%ld,\"head\":[%u,%u,%u,%u,%u]}\n",
               qn, itopks[wi], salgos[si], s1 - s0, invalid, u32max, mn, mx, distinct,
               ids[0], ids[1], ids[2], ids[3], ids[4]);
        fflush(stdout);
        free(ids);
        cuvsCagraSearchParamsDestroy(sp);
      }
    }
  }
  printf("{\"done\":1}\n");
  return 0;
}
