/*
htop - NetBSDProcessList.c
(C) 2014 Hisham H. Muhammad
(C) 2015 Michael McConville
(C) 2021 Santhosh Raju
(C) 2021 htop dev team
Released under the GNU GPLv2, see the COPYING file
in the source distribution for its full text.
*/

#include "netbsd/NetBSDProcessList.h"

#include <kvm.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/swap.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <uvm/uvm_extern.h>

#include "CRT.h"
#include "Macros.h"
#include "Object.h"
#include "netbsd/NetBSDProcess.h"
#include "Process.h"
#include "ProcessList.h"
#include "Settings.h"
#include "XUtils.h"


static long fscale;
static int pageSize;
static int pageSizeKB;

ProcessList* ProcessList_new(UsersTable* usersTable, Hashtable* pidMatchList, uid_t userId) {
   const int mib[] = { CTL_HW, HW_NCPU };
   const int fmib[] = { CTL_KERN, KERN_FSCALE };
   int r;
   size_t size;
   char errbuf[_POSIX2_LINE_MAX];

   NetBSDProcessList* opl = xCalloc(1, sizeof(NetBSDProcessList));
   ProcessList* pl = (ProcessList*) opl;
   ProcessList_init(pl, Class(NetBSDProcess), usersTable, pidMatchList, userId);

   size = sizeof(pl->cpuCount);
   r = sysctl(mib, 2, &pl->cpuCount, &size, NULL, 0);
   if (r < 0 || pl->cpuCount < 1) {
      pl->cpuCount = 1;
   }
   opl->cpus = xCalloc(pl->cpuCount + 1, sizeof(CPUData));

   size = sizeof(fscale);
   if (sysctl(fmib, 2, &fscale, &size, NULL, 0) < 0) {
      CRT_fatalError("fscale sysctl call failed");
   }

   if ((pageSize = sysconf(_SC_PAGESIZE)) == -1)
      CRT_fatalError("pagesize sysconf call failed");
   pageSizeKB = pageSize / ONE_K;

   for (unsigned int i = 0; i <= pl->cpuCount; i++) {
      CPUData* d = opl->cpus + i;
      d->totalTime = 1;
      d->totalPeriod = 1;
   }

   opl->kd = kvm_openfiles(NULL, NULL, NULL, KVM_NO_FILES, errbuf);
   if (opl->kd == NULL) {
      CRT_fatalError("kvm_openfiles() failed");
   }

   return pl;
}

void ProcessList_delete(ProcessList* this) {
   NetBSDProcessList* opl = (NetBSDProcessList*) this;

   if (opl->kd) {
      kvm_close(opl->kd);
   }

   free(opl->cpus);

   ProcessList_done(this);
   free(this);
}

static void NetBSDProcessList_scanMemoryInfo(ProcessList* pl) {
   static int uvmexp_mib[] = {CTL_VM, VM_UVMEXP2};
   struct uvmexp_sysctl uvmexp;
   size_t size_uvmexp = sizeof(uvmexp);

   if (sysctl(uvmexp_mib, 2, &uvmexp, &size_uvmexp, NULL, 0) < 0) {
      CRT_fatalError("uvmexp sysctl call failed");
   }

   pl->totalMem = uvmexp.npages * pageSizeKB;

   // These calculations have been taken from NetBSD's top(1)
   // They need review for testing the correctness
   //pl->freeMem = uvmexp.free * pageSizeKB;
   pl->buffersMem = uvmexp.filepages * pageSizeKB;
   pl->cachedMem = (uvmexp.anonpages + uvmexp.filepages + uvmexp.execpages) * pageSizeKB;
   pl->usedMem = (uvmexp.npages - uvmexp.free - uvmexp.paging) * pageSizeKB + pl->buffersMem + pl->cachedMem;

   pl->totalSwap = uvmexp.swpages * pageSizeKB;
   pl->usedSwap = uvmexp.swpginuse * pageSizeKB;
}

static char* NetBSDProcessList_readProcessName(kvm_t* kd, const struct kinfo_proc2* kproc, int* basenameEnd) {
   /*
    * Like NetBSD's top(1), we try to fall back to the command name
    * (argv[0]) if we fail to construct the full command.
    */
   char** arg = kvm_getargv2(kd, kproc, 500);
   if (arg == NULL || *arg == NULL) {
      *basenameEnd = strlen(kproc->p_comm);
      return xStrdup(kproc->p_comm);
   }

   size_t len = 0;
   for (int i = 0; arg[i] != NULL; i++) {
      len += strlen(arg[i]) + 1;   /* room for arg and trailing space or NUL */
   }

   /* don't use xMalloc here - we want to handle huge argv's gracefully */
   char* s;
   if ((s = malloc(len)) == NULL) {
      *basenameEnd = strlen(kproc->p_comm);
      return xStrdup(kproc->p_comm);
   }

   *s = '\0';

   for (int i = 0; arg[i] != NULL; i++) {
      size_t n = strlcat(s, arg[i], len);
      if (i == 0) {
         *basenameEnd = MINIMUM(n, len - 1);
      }
      /* the trailing space should get truncated anyway */
      strlcat(s, " ", len);
   }

   return s;
}

/*
 * Borrowed with modifications from NetBSD's top(1).
 */
static double getpcpu(const struct kinfo_proc2* kp) {
   if (fscale == 0)
      return 0.0;

   return 100.0 * (double)kp->p_pctcpu / fscale;
}

static void NetBSDProcessList_scanProcs(NetBSDProcessList* this) {
   const Settings* settings = this->super.settings;
   bool hideKernelThreads = settings->hideKernelThreads;
   bool hideUserlandThreads = settings->hideUserlandThreads;
   int count = 0;
   int nlwps = 0;

   const struct kinfo_proc2* kprocs = kvm_getproc2(this->kd, KERN_PROC_ALL, 0, sizeof(struct kinfo_proc2), &count);

   for (int i = 0; i < count; i++) {
      const struct kinfo_proc2* kproc = &kprocs[i];

      bool preExisting = false;
      Process* proc = ProcessList_getProcess(&this->super, kproc->p_pid, &preExisting, NetBSDProcess_new);

      proc->show = ! ((hideKernelThreads && Process_isKernelThread(proc)) || (hideUserlandThreads && Process_isUserlandThread(proc)));

      if (!preExisting) {
         proc->ppid = kproc->p_ppid;
         proc->tpgid = kproc->p_tpgid;
         proc->tgid = kproc->p_pid;
         proc->session = kproc->p_sid;
         proc->tty_nr = kproc->p_tdev;
         proc->pgrp = kproc->p__pgid;
         proc->st_uid = kproc->p_uid;
         proc->starttime_ctime = kproc->p_ustart_sec;
         Process_fillStarttimeBuffer(proc);
         proc->user = UsersTable_getRef(this->super.usersTable, proc->st_uid);
         ProcessList_add(&this->super, proc);
         proc->comm = NetBSDProcessList_readProcessName(this->kd, kproc, &proc->basenameOffset);
      } else {
         if (settings->updateProcessNames) {
            free(proc->comm);
            proc->comm = NetBSDProcessList_readProcessName(this->kd, kproc, &proc->basenameOffset);
         }
      }

      proc->m_virt = kproc->p_vm_vsize;
      proc->m_resident = kproc->p_vm_rssize;
      proc->percent_mem = (proc->m_resident * pageSizeKB) / (double)(this->super.totalMem) * 100.0;
      proc->percent_cpu = CLAMP(getpcpu(kproc), 0.0, this->super.cpuCount * 100.0);
      proc->nlwp = kproc->p_nlwps;
      proc->nice = kproc->p_nice - 20;
      proc->time = 100 * (kproc->p_rtime_sec + ((kproc->p_rtime_usec + 500000) / 1000000));
      proc->priority = kproc->p_priority - PZERO;

      struct kinfo_lwp* klwps = kvm_getlwps(this->kd, kproc->p_pid, kproc->p_paddr, sizeof(struct kinfo_lwp), &nlwps);

      switch (kproc->p_realstat) {
      case SIDL:     proc->state = 'I'; break;
      case SACTIVE:
	// We only consider the first LWP with a one of the below states.
        for (int j = 0; j < nlwps; j++) {
          if (klwps) {
            switch (klwps[j].l_stat) {
            case LSONPROC: proc->state = 'P'; break;
            case LSRUN:    proc->state = 'R'; break;
            case LSSLEEP:  proc->state = 'S'; break;
            case LSSTOP:   proc->state = 'T'; break;
            default:       proc->state = '?';
            }
            if (proc->state != '?')
            break;
	  }
	}
        break;
      case SSTOP:    proc->state = 'T'; break;
      case SZOMB:    proc->state = 'Z'; break;
      case SDEAD:    proc->state = 'D'; break;
      default:       proc->state = '?';
      }

      this->super.totalTasks++;
      // SRUN ('R') means runnable, not running
      if (proc->state == 'P') {
         this->super.runningTasks++;
      }
      proc->updated = true;
   }
}

static unsigned long long saturatingSub(unsigned long long a, unsigned long long b) {
   return a > b ? a - b : 0;
}

static void getKernelCPUTimes(int cpuId, u_int64_t* times) {
   const int mib[] = { CTL_KERN, KERN_CP_TIME, cpuId };
   size_t length = sizeof(*times) * CPUSTATES;
   if (sysctl(mib, 3, times, &length, NULL, 0) == -1 || length != sizeof(*times) * CPUSTATES) {
      CRT_fatalError("sysctl kern.cp_time2 failed");
   }
}

static void kernelCPUTimesToHtop(const u_int64_t* times, CPUData* cpu) {
   unsigned long long totalTime = 0;
   for (int i = 0; i < CPUSTATES; i++) {
      totalTime += times[i];
   }

   unsigned long long sysAllTime = times[CP_INTR] + times[CP_SYS];

   cpu->totalPeriod = saturatingSub(totalTime, cpu->totalTime);
   cpu->userPeriod = saturatingSub(times[CP_USER], cpu->userTime);
   cpu->nicePeriod = saturatingSub(times[CP_NICE], cpu->niceTime);
   cpu->sysPeriod = saturatingSub(times[CP_SYS], cpu->sysTime);
   cpu->sysAllPeriod = saturatingSub(sysAllTime, cpu->sysAllTime);
   cpu->intrPeriod = saturatingSub(times[CP_INTR], cpu->intrTime);
   cpu->idlePeriod = saturatingSub(times[CP_IDLE], cpu->idleTime);

   cpu->totalTime = totalTime;
   cpu->userTime = times[CP_USER];
   cpu->niceTime = times[CP_NICE];
   cpu->sysTime = times[CP_SYS];
   cpu->sysAllTime = sysAllTime;
   cpu->intrTime = times[CP_INTR];
   cpu->idleTime = times[CP_IDLE];
}

static void NetBSDProcessList_scanCPUTime(NetBSDProcessList* this) {
   u_int64_t kernelTimes[CPUSTATES] = {0};
   u_int64_t avg[CPUSTATES] = {0};

   for (unsigned int i = 0; i < this->super.cpuCount; i++) {
      getKernelCPUTimes(i, kernelTimes);
      CPUData* cpu = this->cpus + i + 1;
      kernelCPUTimesToHtop(kernelTimes, cpu);

      avg[CP_USER] += cpu->userTime;
      avg[CP_NICE] += cpu->niceTime;
      avg[CP_SYS] += cpu->sysTime;
      avg[CP_INTR] += cpu->intrTime;
      avg[CP_IDLE] += cpu->idleTime;
   }

   for (int i = 0; i < CPUSTATES; i++) {
      avg[i] /= this->super.cpuCount;
   }

   kernelCPUTimesToHtop(avg, this->cpus);
}

void ProcessList_goThroughEntries(ProcessList* super, bool pauseProcessUpdate) {
   NetBSDProcessList* opl = (NetBSDProcessList*) super;

   NetBSDProcessList_scanMemoryInfo(super);
   NetBSDProcessList_scanCPUTime(opl);

   // in pause mode only gather global data for meters (CPU/memory/...)
   if (pauseProcessUpdate) {
      return;
   }

   NetBSDProcessList_scanProcs(opl);
}
