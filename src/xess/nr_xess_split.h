/* nr_xess_split.h — the command-list cut (nr_xess_split.cpp) for the proxy, which is C.
 *
 * At xessD3D12Execute: nr_split_prepare, then nr_split_snapshot_take before anything of the
 * proxy's is recorded, then the first half's work, then nr_split_cut. Every call on the list
 * from then on, the proxy's own and XeSS's included, goes into the continuation, which starts
 * with the state the snapshot holds. When the game executes the list, `run` is called inside
 * ExecuteCommandLists between the halves, on the queue that runs them; it must leave that
 * queue waiting on the GPU for its work, or do nothing. If the list is reset or released
 * without being executed, `discard` is called instead. Exactly one of them is called once
 * nr_split_cut has returned 1; neither when it returns 0, and then the caller still has
 * `user`, and nr_split_restore puts the snapshot's state back on the list. x86-64 only. */
#ifndef NR_XESS_SPLIT_H
#define NR_XESS_SPLIT_H

#include <stddef.h>
#include <d3d12.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nr_split_snapshot nr_split_snapshot;
typedef void (*nr_split_run)(void *user, ID3D12CommandQueue *queue);
typedef void (*nr_split_discard)(void *user);

/* Where the cut writes what it has to say, a line at a time. */
void nr_split_set_log(void (*say)(const char *line));
/* 1 when the list can be cut now; 0, with the reason, when this frame goes through whole. */
int nr_split_prepare(ID3D12GraphicsCommandList *list, char *why, size_t why_size);
nr_split_snapshot *nr_split_snapshot_take(ID3D12GraphicsCommandList *list);
void nr_split_snapshot_free(nr_split_snapshot *snapshot);
void nr_split_restore(ID3D12GraphicsCommandList *list, const nr_split_snapshot *snapshot);
int nr_split_cut(ID3D12GraphicsCommandList *list, nr_split_run run, nr_split_discard discard, void *user,
                 const nr_split_snapshot *snapshot, char *why, size_t why_size);

/* For the panel (nr_xess_panel.cpp), whose work goes on the queue a swap chain presents on, which
 * DXGI does not name. `nr_split_watch` hooks the device's direct queue class's ExecuteCommandLists
 * now, cut or no cut; from then on every queue that executes command lists is noted.
 * `nr_split_known_queue` says whether `queue` is a direct queue seen executing them, which is how a
 * pointer read out of a swap chain is trusted before it is used; `nr_split_last_queue` is the
 * direct queue this thread last executed on, or NULL. */
int nr_split_watch(ID3D12Device *device, char *why, size_t why_size);
int nr_split_known_queue(const void *queue);
ID3D12CommandQueue *nr_split_last_queue(void);

#ifdef __cplusplus
}
#endif
#endif
