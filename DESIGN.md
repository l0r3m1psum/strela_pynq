# STRELA driver design notes

What the accel-framework driver (`driver/strela_*.c`) assumes, what was
decided and why, and what is still missing. The old char-device driver
(`driver/strela_driver.c`) is still there for reference; this document describes
the new one.

Read this before changing the driver: most of the non-obvious code exists
because of an assumption written down here.

---

## 1. Hardware assumptions

These come from reading `driver/strela_driver.c`, `strela_registers.h` and
`lib/strela.c`. The RTL is not in this repository, so anything marked **unverified**
should be confirmed before it is relied on.

| # | Assumption | Confidence |
|---|---|---|
| H1 | One job at a time per CGRA: a single `CTRL` register, no pipelining. | High — the register layout allows nothing else |
| H2 | A job is two phases, config then exec, with one interrupt each. | High — matches the old driver |
| H3 | The CGRA is configured over the data lines, so its state must be cleared between config and exec. | High — the old driver did the same |
| H4 | DMA is contiguous-only: no scatter/gather, and Zynq-7000 has no IOMMU. | High |
| H5 | DMA addresses are 32-bit. | High (Zynq-7000) |
| H6 | Buffers must be word-aligned; offsets and counts are in 4-byte words. | High |
| H7 | A column with a zero count is inactive and its address is not read. | **Unverified** — see D9 |
| H8 | `STRELA_MKINPSIZE` packs stride in the upper 16 bits and total bytes in the lower 16, so a column can move at most 64 KiB − 1 per job. | **Unverified** — see D8 |
| H9 | The I/O registers can be reprogrammed and exec restarted without reloading the config. | **Unverified** — would enable config caching and cheap batching |
| H10 | Memory is not coherent with the CPU: explicit cache maintenance is required in both directions. | High — the old driver synced on every operation |

Open hardware questions are collected in section 7.

---

## 2. Why the accel framework

The driver was a plain char device. It is now a DRM "compute acceleration"
driver (`DRIVER_COMPUTE_ACCEL`), appearing as `/dev/accel/accelN`.

**What that buys us**, and therefore what is *not* in this driver: buffer
objects with mmap offsets, a handle table and reference counting
(`drm_gem_dma_helper`), ioctl plumbing with argument copying, 32/64-bit compat
and permission checks (`drm_ioctl`), and dma-buf import/export.

**What stays ours**: register programming, the config/exec state machine, cache
maintenance, the job queue and the fences.

**Costs accepted**: `CONFIG_DRM_ACCEL=y` and `CONFIG_DRM_SCHED=y` are required
(both added by `tools/build_kernel.sh arm`), which means the kernel image must be rebuilt
and reinstalled, since DRM is built in. Neither has a Kconfig prompt of its own:
`DRM_ACCEL` is a menuconfig bool, and `DRM_SCHED` is only ever enabled by a
driver's `select`, so the build scripts enable `CONFIG_DRM_ETNAVIV=y` — the
cheapest driver to satisfy, depending only on DRM and MMU — purely to pull it in.
The driver also now tracks DRM's internal APIs, which change faster than
char-device APIs; `drm_sched` in particular changes shape between releases.

### Source layout

Split the way `drivers/accel/rocket` is, with the same file names, so that the
shape is familiar to anyone who has read another accel driver:

| File | Owns |
|---|---|
| `strela_drv.c` | the `drm_driver`, the ioctl table, platform glue, module init, the simulated platform devices |
| `strela_device.c` | `struct strela_device`: bringing the scheduler up and taking it down |
| `strela_core.c` | the CGRA itself: register programming, the interrupt, the simulated stand-in |
| `strela_job.c` | validating a submit, the `drm_sched` backend, fences, the KUnit tests |
| `strela_gem.c` | buffer objects: allocation, freeing, `GEM_NEW` and `GEM_SYNC` |
| `strela_registers.h` | register offsets and bits |

Rocket splits `rocket_device.c` from `rocket_core.c` because an RK3588 has three
NPU cores behind one device node. STRELA has one CGRA per device, so the split
here is by *lifetime versus behaviour* rather than by count: `strela_device.c`
is what exists, `strela_core.c` is what it does. If the second CGRA in the block
design is ever exposed (§7 Q7), rocket's meaning of the split becomes ours too.

`driver/Kconfig` and `driver/Makefile` are in-tree form. Out of tree,
`CONFIG_DRM_ACCEL_STRELA=m` has to be passed on the make command line, since
there is no `.config` to take it from and defaulting it in the Makefile would
override an in-tree choice to leave the driver out. The module is still called
`strela2`, a holdover from coexisting with the old `strela_driver.c`; renaming
it means touching the tests, which read `/sys/module/strela2/parameters`.

---

## 3. Design decisions

### D1 — GEM DMA helper, not the shmem helper
lima and ivpu use `drm_gem_shmem_helper` because their hardware has an MMU and
can take scattered pages. STRELA cannot (H4), so buffers use
`drm_gem_dma_helper`, which is contiguous by construction and hands us a
`dma_addr` ready for the address registers.

### D2 — Cached memory with explicit syncs
`strela_gem_create()` sets `map_noncoherent = true`, so allocation uses
`dma_alloc_noncoherent` and mmap gives a **cached** mapping. The default would
be write-combine, which needs no cache maintenance but makes CPU-side compute on
shared buffers slow — and CPU-side compute on shared buffers is exactly the
zero-copy case this hardware exists to serve.

The cost is that cache maintenance is now our job: `DRM_IOCTL_STRELA_GEM_SYNC`
calls `dma_sync_single_for_device` / `_for_cpu` on a sub-range. Partial syncs
are explicitly allowed by the DMA API.

### D12 — The driver allocates its own buffers, `DMA_BIDIRECTIONAL`
`drm_gem_dma_create()` hardcodes `DMA_TO_DEVICE` in its `dma_alloc_noncoherent()`
call (`drm_gem_dma_helper.c:149`) — the direction is not a parameter. That
declares "the device only ever reads this", which is false for every buffer here,
and it is not merely a description: `check_sync()` waives its direction checks
exactly when the allocation was `DMA_BIDIRECTIONAL` (`kernel/dma/debug.c:1125`),
so under `DMA_TO_DEVICE` the invalidate half of `GEM_SYNC` trips both the
"different direction" and "syncs device read-only DMA memory for cpu" checks.

So `strela_gem_create()` open-codes what the helper does (`__drm_gem_dma_create()`
is static) and allocates `DMA_BIDIRECTIONAL`. Two things follow:

* **`strela_gem_free()` must mirror the direction.** `drm_gem_dma_object_free()`
  hardcodes `DMA_TO_DEVICE` too (`:239`), so it cannot be the `.free` handler.
  The rest of `drm_gem_dma_default_funcs` is repeated verbatim — it is static —
  and those handlers are direction-agnostic.
* **`.gem_create_object` is gone.** It existed only to set `map_noncoherent`
  before the helper allocated. Leaving the hook unset is also what keeps an
  imported dma-buf on the helper's default funcs, so it is freed by the helper's
  own import path and never reaches `strela_gem_free()`.

**Non-goal: per-buffer directions.** Config buffers could honestly be
`DMA_TO_DEVICE` — the CGRA cannot write a bitstream — and it would be free
performance on ARMv7, where `sync_for_cpu` with `DMA_TO_DEVICE` returns without
doing anything (the `dir != DMA_TO_DEVICE` guard in `__dma_page_dev_to_cpu()`,
`arch/arm/mm/dma-mapping.c:702`), saving a full-range invalidate per job. Not
implemented: it means a direction in `drm_strela_gem_new`, a second allocation
path and a rule about which handles may be bound where, for a saving nobody has
measured.

**Tensor buffers stay `DMA_BIDIRECTIONAL` regardless.** In IREE's zero-copy path
one dispatch's output is the next one's input, so the same buffer is read and
written over its life: the direction belongs to the *use*, not to the buffer, and
a buffer cannot be allocated knowing which it will be. Precision about what
actually moved belongs in the per-job pre/post sync TODOs in `strela_run_job()`
and `strela_job_phase_done()`, which narrow the *range* per job — not in the
allocation, which cannot know.

### D3 — Asynchronous submit with fences
The old driver had three blocking ioctls (`CONTROL`, `CONFIG`, `EXEC`). The new
one has a single `SUBMIT` that queues the job and returns a **sync_file fd**
which signals on completion. IREE waits on that fd through its proactor, so no
thread is parked on the device.

Consequences that the code depends on:

- **The fence borrows `sdev->lock`** (passed to `dma_fence_init`). So
  `dma_fence_signal()` takes that lock internally and must never be called while
  holding it — hence the "decide under the lock, signal after unlocking" shape of
  `strela_job_phase_done()`.
- **Every job must signal exactly once**, including failures. A timeout signals
  with `-ETIMEDOUT`, removal with `-ENODEV`. A fence that never signals hangs
  every waiter and breaks the dma-fence contract.
- **One fence context per device** (`dma_fence_context_alloc`), because the queue
  is per-device and strictly ordered. Two devices are unordered with respect to
  each other, which is correct.

### D4 — The DRM GPU scheduler owns the queue
The queue was a hand-written FIFO (an intrusive list, a timeout work item, a
cleanup workqueue). It is now `drm_sched`, with `credit_limit = 1` because the
device runs one job at a time, and one entity per open file.

What the scheduler provides, and what the driver therefore no longer contains:
the queue and its ordering, the job timeout, the workqueue that frees jobs
outside interrupt context, the user-visible fence (`s_fence->finished`), and
arbitration between clients.

What the driver still provides: `run_job()` (program the registers, start the
config phase, return a hardware fence), `timedout_job()`, `free_job()`, and the
interrupt that signals the hardware fence.

Two rules that cost real debugging time:

- **The hardware fence is allocated separately, not embedded in the job.** The
  scheduler still holds a reference to it when `free_job()` frees the job, so an
  embedded fence is a use-after-free. With no `release` op, the last put frees
  the allocation through `dma_fence_free()`. This is the same shape lima uses.
- **`timedout_job()` must call `drm_sched_stop()` and `drm_sched_start()`.** The
  scheduler removes the job from its pending list *before* calling the driver,
  and `drm_sched_stop()` is what puts it back and arranges for it to be freed.
  Returning `NOMINAL` without that dance leaks the job and every buffer it
  references. Failing the fence first is what makes the error reach userspace as
  `-ETIMEDOUT` rather than the scheduler's `-ECANCELED`.
- **The submit workqueue is the driver's, not the scheduler's.**
  `drm_sched_fini()` *cancels* pending work rather than running it, so jobs
  waiting to be freed would leak at removal. Owning the workqueue lets
  `strela_remove()` flush it first.

A fixed-size ring was considered before this and rejected: a ring's usual
advantage is reusing preallocated slots, but a job's fence must outlive its slot,
so a ring of job structs would let a client wedge the device by holding fds, and
a ring of pointers keeps every allocation anyway.

### D5 — No bounded queue (for now)
An earlier version bounded the queue per device and per client, with
`O_NONBLOCK`-aware backpressure. That was dropped when the scheduler took over,
in favour of a smaller driver.

**This is a known gap, not a decision that the risk went away.** `drm_sched`'s
`credit_limit` throttles jobs *in flight on the hardware*, not jobs queued in an
entity, and an entity's queue is unbounded. A process submitting in a loop can
still pin arbitrary memory. If that matters, the bound comes back as a counter
plus an interruptible wait in the submit ioctl; the scheduler will not provide
it.

### D6 — Parameters are fixed at load time
All module parameters are `0444`. A limit that changes under a running queue is a
source of bugs for no benefit, and read-only parameters mean no `READ_ONCE`, no
revalidation and no "what if it changed between these two lines". Tests that need
different values reload the module — see section 5.

### D7 — Teardown runs in process context
- **Job cleanup cannot run in the completion interrupt.** Dropping the last GEM
  reference reaches `dma_free_pages()`, which may sleep, and takes
  `mgr->vm_lock`, a lock the ioctl path takes with interrupts enabled. This was a
  real bug, caught by `CONFIG_PROVE_LOCKING` before the scheduler existed here;
  `free_job()` now runs on the scheduler's workqueue, which is process context,
  so the rule is satisfied by construction.
- **`strela_remove()` drains what the scheduler cannot see.** `strela_drain()`
  sets `dying` under the lock, resets the hardware and fails the fence of the job
  that was executing. Queued jobs are the scheduler's problem: entities are
  destroyed at `postclose`, and the module cannot be unloaded while a file is
  open. Removal then flushes the submit workqueue, calls `drm_sched_fini()` and
  destroys the workqueue, in that order (see D4).

### D8 — Binding validation is a pure function
`strela_binding_check()` takes a size and a binding and returns 0 or `-EINVAL`,
with no locks, lookups or side effects, so KUnit can exercise it. It computes in
`u64` because the inputs are attacker-controlled `u32`s, and it enforces the
64 KiB column limit from H8 as well as the buffer bounds.

### D9 — Unused columns
A binding with handle 0 means "column unused", which is how a kernel that uses
fewer than four columns is expressed (the relu test in `tools/test_strela.c` uses
one input and one output). Handle 0 is never a valid GEM handle: DRM allocates
from 1.

Unused columns are currently programmed with address 0 and size 0. The old driver
wrote the region base with a zero count. If H7 turns out to be false — if the DMA
engine ever touches a zero-length column — address 0 is physical DDR, and an
output column writing there would corrupt memory. Pointing unused columns at a
known-safe address would be the cheap fix.

### D11 — Fences are sync objects, in and out
`DRM_IOCTL_STRELA_SUBMIT` takes two sync object handles: `out_syncobj` receives
the job's completion fence (`drm_syncobj_replace_fence()`), and `in_syncobj`,
when `DRM_STRELA_SUBMIT_WAIT_SYNCOBJ` is set, becomes a dependency through
`drm_sched_job_add_syncobj_dependency()`. Userspace creates and waits with the
core ioctls (`DRM_IOCTL_SYNCOBJ_CREATE`, `_WAIT`), so the driver contains no fd
plumbing at all.

An earlier version returned a `sync_file` fd instead. Sync objects were chosen
because they make the driver smaller — no `get_unused_fd_flags()`,
`sync_file_create()`, `fd_install()` or their error paths — and because timeline
points are a natural extension later. Three consequences to know:

- **A wait does not report failure.** `drm_syncobj` never looks at
  `fence->error`, so `SYNCOBJ_WAIT` says only "signalled". To see that a job
  failed, export the object with
  `DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE` and read `SYNC_IOC_FILE_INFO`
  — which is what the timeout test does. A driver-side status query would be the
  alternative, at the cost of more code.
- **Handles are per open file.** Passing a fence to another device needs
  `HANDLE_TO_FD` then `FD_TO_HANDLE`; a sync_file fd crossed devices by itself.
- **IREE needs an fd** for its proactor, so it must export one per submit
  (`EXPORT_SYNC_FILE`) or register an eventfd (`SYNCOBJ_EVENTFD`). The work moved
  out of the kernel rather than disappearing.

A **flag** marks the in-fence field as meaningful rather than a sentinel value,
because the ioctl layer zero-fills the tail of a short struct from older
userspace and handle 0 would otherwise be ambiguous.

### D10 — Simulation is a mode of the same driver
`sim_dev_count=N` registers N platform devices with no device-tree node;
`strela_device.is_sim` is derived from the absence of that node, and the hardware
functions check it.

The simulated device is a work item on `system_unbound_wq`, not a timer.
`strela_sim_work()` runs one whole job — configure, then execute — sleeping out
each phase in `strela_sim_wait()` with up to 50% jitter so completion order is
not accidentally deterministic. Three consequences:

* **It runs in process context with no lock held**, which is what lets the exec
  phase copy data at all.
* **A job can be interrupted partway.** `strela_hw_reset()` raises `sim_abort`
  and wakes the worker; the exec phase waits once per column, so a reset lands
  between columns and leaves the output half written, like stopping a DMA engine
  mid-transfer. Because that reset is asynchronous, both callers must call
  `strela_sim_settle()` (a `cancel_work_sync()`) *before* signalling the fence —
  signalling is what lets `free_job()` run, and the worker still holds the job.
* **The worker runs both phases itself**, so `strela_hw_start()` queues it only
  for the config phase. Queueing on the exec phase would mean queueing the work
  item from inside its own execution.

Execution is a **bypass kernel**: every enabled column is copied from its input
to its output, honouring offsets, counts and the input stride; a column disabled
at either end is skipped; the configuration is never read and nothing is
computed. `drm_warn()` says so on every job, so no output of a simulated device
can be mistaken for something the CGRA produced.

The bypass does the cache maintenance a real DMA master would: invalidate before
reading a column, clean after writing one. Both are no-ops under QEMU, so this is
not for the test targets — it is for the simulator running on the PYNQ itself
(`insmod` with `sim_dev_count` to exercise the IREE stack without the CGRA),
where the caches are real and the copy would otherwise hand back stale data. It
is legal only because of the `DMA_BIDIRECTIONAL` allocation in D12.

This keeps one code path for both modes and makes everything except real DMA and
interrupts testable without an FPGA — including the whole IREE stack.

---

## 4. Concurrency rules

| Context | Code | May sleep? |
|---|---|---|
| process | ioctls, `strela_drain` | yes |
| hard IRQ / timer | `strela_irq`, `strela_sim_irq` → `strela_job_phase_done` | no |
| scheduler workqueue | `run_job`, `free_job` | yes |
| timeout workqueue | `timedout_job` | yes |

- `sdev->lock` (a spinlock, taken with `_irqsave`) protects the active job and
  the `dying` flag, and doubles as every hardware fence's lock.
- Never call a `dma_fence_*` function that takes `fence->lock` while holding
  `sdev->lock`.
- Never free GEM objects or DMA memory from the completion interrupt (D7).
- `sdev->active` is set by `run_job()` and cleared by whoever finishes the job:
  the interrupt, the timeout, or the drain.
- `job->phase` is only meaningful while the job is active.

---

## 5. Testing

Three layers, all runnable without hardware:

1. **KUnit** (`#if IS_ENABLED(CONFIG_KUNIT)` at the end of the driver) for pure
   logic: binding validation and register encoding. Runs at module load and
   reports TAP to dmesg.
2. **Userspace tests** (`tools/test_strela2.c`) for everything needing real
   ioctls, fds and processes. TAP output via the kernel's `kselftest.h`.
3. **QEMU harness** (`tools/run_qemu.sh` plus the static init in
   `tools/qemu_init.c`) which boots a kernel with an initramfs, loads the module
   and runs the suites.

Because parameters are load-time only (D6), the harness runs the module three
times with different parameters, one suite each:

| Suite | Module parameters | Covers |
|---|---|---|
| `core` | `sim_dev_count=2` | basic submit, the bypass copy, ordering, buffer lifetime, bad handles, two processes |
| `queue` | two devices, `sim_job_delay_us=100000` | async submit, device independence, in-fences across devices, and leaving work in flight for teardown to fail |
| `timeout` | `sim_job_delay_us=900000` | jobs exceeding the 500 ms timeout |

Reloading between suites also exercises unload, and the `queue` suite ends by
deliberately leaving jobs queued so that removal has something to drain.

Two targets:

```sh
tools/run_qemu.sh              # the board kernel, emulated (~40 s)
ARCH=x86_64 tools/run_qemu.sh  # native + KVM, with KUnit and the debug options (~5 s)
```

The x86 kernel (`tools/build_kernel.sh x86_64`) lives in its own git worktree of the
same commit, because an `O=` build refuses a dirty source tree and the ARM kernel
is built in-tree. It enables `PROVE_LOCKING`, `DMA_API_DEBUG`, `KMEMLEAK`,
`UBSAN` and `KUNIT`; `DRM_GEM_DMA_HELPER` is pulled in via `COMPILE_TEST` plus
`DRM_LOGICVC`, so no Kconfig patch is needed.

**Known benign warning**: `DMA-API: ... device driver failed to check map error`.
`dma_alloc_pages()` registers its debug entry as `MAP_ERR_NOT_CHECKED` and only
`dma_mapping_error()` clears it, but the allocation API's contract is to check the
returned pointer, which the GEM helper does. It fires for every
`dma_alloc_noncoherent` user with `CONFIG_DMA_API_DEBUG`.

---

## 6. Still pending

Roughly in the order I would do them.

1. **Per-job pre/post sync.** The flush and invalidate ranges should ride along
   with the job and be applied by the driver around execution, which is where
   IREE's `stream.cmd.flush` should eventually land, instead of a separate ioctl
   round trip.
2. **Out-fences into `drm_syncobj`.** In-fences are done (D11); timeline syncobjs
   would be the next step if IREE's timeline semaphores start mattering more than
   one-shot fds.
3. **Runtime side of the IREE flush work.** `hal.command_buffer.flush_buffer`
   exists in the compiler but has no runtime export, so any module using it fails
   to load. See the IREE-side notes.
4. **Fence lifetime versus device lifetime.** `fence->lock` points into
   `strela_device`. A sync_file fd can outlive the device, and
   `dma_fence_default_wait()` takes that lock even for an already-signalled fence.
   The drain reduces the window; a per-job lock would close it.
5. **Submission is unbounded.** See D5: nothing limits how much a client can
   queue, so a submit loop can pin arbitrary memory. A byte budget rather than a
   job count would be the honest bound.
6. **Overflow checks against the RTL.** H7, H8 and H9 need confirming; the 64 KiB
   limit in particular is currently enforced on a guess.
7. **`queue_dealloca` and friends in the IREE HAL driver** are still
   `UNIMPLEMENTED`, and `dispatch`/`copy_buffer`/`queue_execute` there are no-ops.
8. **Error reporting per job.** The hardware may be able to report how many words
   each output wrote, or an overflow bit; nothing is read back today.
9. **Performance counters.** `CNTR_CONF`, `CNTR_EXEC` and `CNTR_STALL` are unused;
    they would feed IREE's profiling hooks.

---

## 7. Open hardware questions

1. Can the I/O registers be reprogrammed and exec restarted **without**
   `LOAD_CONFIG`? (H9 — decides config caching and batching.)
2. Does the hardware report per-output word counts, or an error/overflow bit?
3. Does `EXEC_DONE` fire only when every output has reached its count?
4. What exactly do `OUT_ARB_HOLD` and `RESET_DMA` do, and is `RESET_DMA` a safe
   recovery from a hung job?
5. Is a zero-size column genuinely inert, address included? (H7 — decides D9.)
6. Is the 64 KiB per-column limit real? (H8.)
7. The block design has two CGRAs but `pl_gold.dts` lists only
   `cgra_axi_lite_0`. Should the second be exposed?
