/* sweep2: CAGRA build/search cost + recall model via the cuVS C API directly.
 *
 * Usage: sweep2 base.f32 query.f32 n dim nq topk gd igd niter algo metric nbuild
 *
 *   algo   : 1 = IVF_PQ, 2 = NN_DESCENT   (cuvsCagraGraphBuildAlgo)
 *   metric : 0 = L2, 1 = INNER_PRODUCT, 2 = COSINE, 3 = L2Expanded
 *
 * Prints:
 *   {"build":0,"ms":42312.4}
 *   {"itopk":512,"sw":4,"algo":3,"ms":41.2,"recall10":0.982,"recall100":0.913}
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif

#ifdef _OPENMP
#include <omp.h>
#endif

#define CHK(x) do { cuvsError_t e_ = (x); if (e_ != CUVS_SUCCESS) { \
  fprintf(stderr, "CUVS error %d at %s:%d\n", (int)e_, __FILE__, __LINE__); return 5; } } while (0)
#define CHKC(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "CUDA error %d at %s:%d\n", (int)e_, __FILE__, __LINE__); return 6; } } while (0)

static double now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static DLManagedTensor make_tensor(void* data, int device, int dtype_code, int bits,
                                   long r0, long r1, long dim)
{
  DLManagedTensor t;
  memset(&t, 0, sizeof(t));
  /* the shape array must outlive the tensor -> heap allocated (leaked on purpose) */
  long* shape = (long*)malloc(sizeof(long) * 2);
  shape[0] = r0;
  shape[1] = r1;
  t.dl_tensor.data = data;
  t.dl_tensor.device.device_type = device ? kDLCUDA : kDLCPU;
  t.dl_tensor.device.device_id = 0;
  t.dl_tensor.ndim = (dim > 0) ? 2 : 1;
  t.dl_tensor.dtype.code = dtype_code;
  t.dl_tensor.dtype.bits = bits;
  t.dl_tensor.dtype.lanes = 1;
  t.dl_tensor.shape = shape;
  t.dl_tensor.strides = NULL;
  t.dl_tensor.byte_offset = 0;
  return t;
}

int main(int argc, char** argv)
{
  if (argc < 13) {
    fprintf(stderr, "usage: %s base query n dim nq topk gd igd niter algo metric nbuild\n", argv[0]);
    return 2;
  }
  const char* base_p = argv[1];
  const char* query_p = argv[2];
  long n = atol(argv[3]), dim = atol(argv[4]), nq = atol(argv[5]), topk = atol(argv[6]);
  long gd = atol(argv[7]), igd = atol(argv[8]), niter = atol(argv[9]);
  int algo = atoi(argv[10]), metric = atoi(argv[11]);
  int nbuild = atoi(argv[12]);

  float* base = (float*)malloc(sizeof(float) * (size_t)n * dim);
  float* q = (float*)malloc(sizeof(float) * (size_t)nq * dim);
  if (!base || !q) { fprintf(stderr, "oom\n"); return 3; }
  FILE* f = fopen(base_p, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", base_p); return 4; }
  if (fread(base, sizeof(float), (size_t)n * dim, f) != (size_t)n * dim) { fprintf(stderr, "short base\n"); return 5; }
  fclose(f);
  f = fopen(query_p, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", query_p); return 4; }
  if (fread(q, sizeof(float), (size_t)nq * dim, f) != (size_t)nq * dim) { fprintf(stderr, "short query\n"); return 5; }
  fclose(f);

  /* exact top-k ground truth, per-thread scratch (no shared state) */
  int* gt = (int*)malloc(sizeof(int) * (size_t)nq * topk);
  double gt0 = now_ms();
#pragma omp parallel for schedule(dynamic, 1)
  for (long qi = 0; qi < nq; qi++) {
    float* best = (float*)malloc(sizeof(float) * topk);
    int* bi = (int*)malloc(sizeof(int) * topk);
    int cnt = 0;
    double worst = 1e300;
    const float* qq = q + qi * dim;
    for (long i = 0; i < n; i++) {
      const float* r = base + i * dim;
      double s = 0.0;
      for (long d = 0; d < dim; d++) { double df = (double)r[d] - (double)qq[d]; s += df * df; }
      if (cnt < topk) {
        int p = cnt++;
        while (p > 0 && best[p - 1] > s) { best[p] = best[p - 1]; bi[p] = bi[p - 1]; p--; }
        best[p] = (float)s; bi[p] = (int)i; worst = best[cnt - 1];
      } else if (s < worst) {
        int p = (int)topk - 1;
        while (p > 0 && best[p - 1] > s) { best[p] = best[p - 1]; bi[p] = bi[p - 1]; p--; }
        best[p] = (float)s; bi[p] = (int)i; worst = best[topk - 1];
      }
    }
    for (long k = 0; k < topk; k++) gt[qi * topk + k] = bi[k];
    free(best); free(bi);
  }
  printf("{\"gt_ms\":%.1f}\n", now_ms() - gt0);
  fflush(stdout);

  cuvsResources_t res;
  CHK(cuvsResourcesCreate(&res));
  printf("{\"cuvs_success\":%d}\n", (int)CUVS_SUCCESS);
  fflush(stdout);

  void* qd = NULL; void* nd = NULL; void* dd = NULL;
  CHK(cuvsRMMAlloc(res, &qd, sizeof(float) * (size_t)nq * dim));
  CHK(cuvsRMMAlloc(res, &nd, sizeof(unsigned int) * (size_t)nq * topk));
  CHK(cuvsRMMAlloc(res, &dd, sizeof(float) * (size_t)nq * topk));
  CHKC(cudaMemcpy(qd, q, sizeof(float) * (size_t)nq * dim, cudaMemcpyDefault));

  DLManagedTensor qt = make_tensor(qd, 1, kDLFloat, 32, nq, dim, dim);
  DLManagedTensor nt = make_tensor(nd, 1, kDLUInt, 32, nq, topk, dim);
  DLManagedTensor dt = make_tensor(dd, 1, kDLFloat, 32, nq, topk, dim);

  for (int b = 0; b < nbuild; b++) {
    cuvsCagraIndexParams_t ip;
    cuvsCagraIndex_t idx;
    CHK(cuvsCagraIndexParamsCreate(&ip));
    ip->metric = (cuvsDistanceType)metric;
    ip->build_algo = (enum cuvsCagraGraphBuildAlgo)algo;
    ip->graph_degree = (size_t)gd;
    ip->intermediate_graph_degree = (size_t)igd;
    ip->nn_descent_niter = (size_t)niter;
    CHK(cuvsCagraIndexCreate(&idx));

    DLManagedTensor dset = make_tensor(base, 0, kDLFloat, 32, n, dim, dim);
    double t0 = now_ms();
    cuvsError_t be = cuvsCagraBuild(res, ip, &dset, idx);
    double t1 = now_ms();
    printf("{\"build\":%d,\"ms\":%.1f,\"err\":%d,\"metric\":%d,\"errtext\":\"%s\"}\n",
           b, t1 - t0, (int)be, (int)ip->metric, cuvsGetLastErrorText());
    fflush(stdout);
    if (be != CUVS_SUCCESS) { printf("{\"fatal\":\"build\"}\n"); return 7; }

    if (b == 0) {
      const size_t itopks[] = {64, 128, 256, 512, 1024};
      const size_t sws[] = {1, 4};
      const int salgos[] = {3 /*AUTO*/, 1 /*MULTI_CTA*/, 0 /*SINGLE_CTA*/};
      for (size_t si = 0; si < sizeof(salgos) / sizeof(salgos[0]); si++) {
        for (size_t wi = 0; wi < sizeof(itopks) / sizeof(itopks[0]); wi++) {
          for (size_t xi = 0; xi < sizeof(sws) / sizeof(sws[0]); xi++) {
            cuvsCagraSearchParams_t sp;
            CHK(cuvsCagraSearchParamsCreate(&sp));
            sp->itopk_size = itopks[wi];
            sp->search_width = sws[xi];
            sp->algo = (enum cuvsCagraSearchAlgo)salgos[si];
            sp->max_queries = (size_t)nq;
            cuvsFilter flt; flt.type = NO_FILTER; flt.addr = (uintptr_t)0;
            double s0 = now_ms();
            cuvsError_t se = cuvsCagraSearch(res, sp, idx, &qt, &nt, &dt, flt);
            double s1 = now_ms();
            if (se != CUVS_SUCCESS) {
              printf("{\"itopk\":%zu,\"sw\":%zu,\"algo\":%d,\"err\":%d}\n", itopks[wi], sws[xi], salgos[si], (int)se);
              fflush(stdout);
              cuvsCagraSearchParamsDestroy(sp);
              continue;
            }
            unsigned int* ids = (unsigned int*)malloc(sizeof(unsigned int) * (size_t)nq * topk);
            CHKC(cudaMemcpy(ids, nd, sizeof(unsigned int) * (size_t)nq * topk, cudaMemcpyDefault));
            long h10 = 0, h100 = 0;
            int k10 = (topk < 10) ? (int)topk : 10;
            for (long qi = 0; qi < nq; qi++) {
              for (int k = 0; k < k10; k++) {
                int id = (int)ids[qi * topk + k];
                for (int j = 0; j < k10; j++) if (gt[qi * topk + j] == id) { h10++; break; }
              }
              for (int k = 0; k < topk; k++) {
                int id = (int)ids[qi * topk + k];
                for (int j = 0; j < topk; j++) if (gt[qi * topk + j] == id) { h100++; break; }
              }
            }
            printf("{\"itopk\":%zu,\"sw\":%zu,\"algo\":%d,\"ms\":%.2f,\"recall10\":%.6f,\"recall100\":%.6f}\n",
                   itopks[wi], sws[xi], salgos[si], s1 - s0,
                   (double)h10 / (double)(nq * k10), (double)h100 / (double)(nq * topk));
            fflush(stdout);
            free(ids);
            cuvsCagraSearchParamsDestroy(sp);
          }
        }
      }
      /* single-query latency (the platform's throughput probe may issue one query at a time) */
      {
        float* q1 = q;
        long shape0 = 1;
        /* reuse the same device buffers, but shape them for a single query */
        qt.dl_tensor.shape[0] = shape0;
        nt.dl_tensor.shape[0] = shape0;
        dt.dl_tensor.shape[0] = shape0;
        const size_t itopks1[] = {512, 1024};
        const int salgos1[] = {0, 1, 3};
        for (size_t wi = 0; wi < 2; wi++) {
          for (size_t si = 0; si < 3; si++) {
            cuvsCagraSearchParams_t sp;
            CHK(cuvsCagraSearchParamsCreate(&sp));
            sp->itopk_size = itopks1[wi];
            sp->search_width = 4;
            sp->algo = (enum cuvsCagraSearchAlgo)salgos1[si];
            sp->max_queries = 1;
            cuvsFilter flt; flt.type = NO_FILTER; flt.addr = (uintptr_t)0;
            double s0 = now_ms();
            cuvsError_t se = cuvsCagraSearch(res, sp, idx, &qt, &nt, &dt, flt);
            double s1 = now_ms();
            printf("{\"nq\":1,\"itopk\":%zu,\"algo\":%d,\"ms\":%.3f,\"err\":%d}\n",
                   itopks1[wi], salgos1[si], s1 - s0, (int)se);
            fflush(stdout);
            cuvsCagraSearchParamsDestroy(sp);
          }
        }
        qt.dl_tensor.shape[0] = nq;
        nt.dl_tensor.shape[0] = nq;
        dt.dl_tensor.shape[0] = nq;
        (void)q1;
      }
    }
    cuvsCagraIndexDestroy(idx);
    cuvsCagraIndexParamsDestroy(ip);
  }
  cuvsRMMFree(res, qd, sizeof(float) * (size_t)nq * dim);
  cuvsRMMFree(res, nd, sizeof(unsigned int) * (size_t)nq * topk);
  cuvsRMMFree(res, dd, sizeof(float) * (size_t)nq * topk);
  cuvsResourcesDestroy(res);
  return 0;
}
