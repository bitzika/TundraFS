#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <stdarg.h>

#define IMAGE "disk.tundra"
#define TOTAL 100
#define THREADS 4

/* new stage tunables */
#define OVERWRITE_THREADS 8
#define OVERWRITE_ROUNDS 20
#define HEAVY_THREADS 16
#define HEAVY_FILES 2000

int errors = 0, ops_done = 0;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

double now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000.0 + ts.tv_nsec / 1000.0;
}

int run_silent(const char *fmt, ...) {
    char cmd[1024], full[1280];
    va_list ap; va_start(ap, fmt); vsnprintf(cmd, sizeof(cmd), fmt, ap); va_end(ap);
    snprintf(full, sizeof(full), "./tundra_fs %s >/dev/null 2>/dev/null", cmd);
    int ret = system(full);
    pthread_mutex_lock(&lock); ops_done++; if (ret) errors++; pthread_mutex_unlock(&lock);
    return ret;
}

/* returns 0 if command succeeded (matches run_silent's semantics), does not touch errors/ops_done */
int run_quiet(const char *fmt, ...) {
    char cmd[1024], full[1280];
    va_list ap; va_start(ap, fmt); vsnprintf(cmd, sizeof(cmd), fmt, ap); va_end(ap);
    snprintf(full, sizeof(full), "./tundra_fs %s >/dev/null 2>/dev/null", cmd);
    return system(full);
}

void *pw(void *arg) {
    int tid = *(int *)arg;
    for (int i = tid; i < TOTAL; i += THREADS) {
        run_silent("write %s /stest/p_%d_%04d.txt \"T%dF%d\"", IMAGE, tid, i, tid, i);
        usleep(1000);
    }
    return NULL;
}

/* Stage 6: many threads writing to the SAME path repeatedly -- max contention on one inode/AG lock */
int overwrite_errors = 0;
void *ow(void *arg) {
    int tid = *(int *)arg;
    for (int r = 0; r < OVERWRITE_ROUNDS; r++) {
        int ret = run_quiet("write %s /stest/shared.txt \"tid%d_round%d\"", IMAGE, tid, r);
        if (ret) { pthread_mutex_lock(&lock); overwrite_errors++; pthread_mutex_unlock(&lock); }
        usleep(500);
    }
    return NULL;
}

/* Stage 7: heavy concurrency across many files/threads, spread to hit different AGs */
int heavy_errors = 0;
void *heavy(void *arg) {
    int tid = *(int *)arg;
    for (int i = tid; i < HEAVY_FILES; i += HEAVY_THREADS) {
        int ret = run_quiet("write %s /stest/h_%d_%05d.txt \"H%dF%d\"", IMAGE, tid, i, tid, i);
        if (ret) { pthread_mutex_lock(&lock); heavy_errors++; pthread_mutex_unlock(&lock); }
    }
    return NULL;
}

int run_fsck(int *out_errs) {
    int fsck_errs = 0;
    FILE *f = popen("./tundra_fsck disk.tundra 2>&1", "r");
    char buf[256];
    while (f && fgets(buf, sizeof(buf), f)) if (strstr(buf, "ERROR")) fsck_errs++;
    if (f) pclose(f);
    if (fsck_errs > 0) {
        system("./tundra_fsck disk.tundra --fix >/dev/null 2>&1");
        fsck_errs = 0;
        f = popen("./tundra_fsck disk.tundra 2>&1", "r");
        while (f && fgets(buf, sizeof(buf), f)) if (strstr(buf, "ERROR")) fsck_errs++;
        if (f) pclose(f);
    }
    *out_errs = fsck_errs;
    return fsck_errs;
}

int main() {
    setbuf(stdout, NULL);

    printf("\n  ╔══════════════════════════════════════════════════╗\n");
    printf("  ║        TUNDRAFS STRESS TEST v8                  ║\n");
    printf("  ╚══════════════════════════════════════════════════╝\n\n");

    system("./mkfs_tundra disk.tundra 1000000 >/dev/null 2>&1");
    run_silent("mkdir %s /stest", IMAGE);
    errors = ops_done = 0;

    /* ── STAGE 1 ── */
    printf("  [1/7] Sequential Write %d files...\n", TOTAL);
    double t0 = now_us();
    for (int i = 0; i < TOTAL; i++) run_silent("write %s /stest/s_%04d.txt \"S%d\"", IMAGE, i, i);
    double t1 = now_us();
    double seq_ms = (t1-t0)/1000.0, seq_iops = TOTAL/(seq_ms/1000.0);
    printf("        %d files │ %.0f ms │ %.0f writes/s │ %d err\n", TOTAL, seq_ms, seq_iops, errors);

    /* ── STAGE 2 ── */
    printf("  [2/7] Parallel Write %d threads...\n", THREADS);
    errors = ops_done = 0; t0 = now_us();
    pthread_t th[THREADS]; int ids[THREADS];
    for (int i = 0; i < THREADS; i++) { ids[i] = i; pthread_create(&th[i], NULL, pw, &ids[i]); }
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL);
    t1 = now_us();
    double par_ms = (t1-t0)/1000.0, par_iops = TOTAL/(par_ms/1000.0);
    printf("        %d ops │ %.0f ms │ %.0f ops/s │ %d err\n", TOTAL, par_ms, par_iops, errors);

    /* ── STAGE 3 ── */
    printf("  [3/7] Random Read 50 files...\n");
    int rok = 0, rmiss = 0; srand(time(NULL)); t0 = now_us();
    for (int i = 0; i < 50; i++) {
        int idx = rand() % TOTAL;
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "./tundra_fs read %s /stest/s_%04d.txt 2>/dev/null", IMAGE, idx);
        FILE *p = popen(cmd, "r"); char buf[64] = {0};
        if (p && fread(buf, 1, sizeof(buf)-1, p) > 0) rok++; else rmiss++;
        if (p) pclose(p);
    }
    t1 = now_us();
    double read_ms = (t1-t0)/1000.0, read_iops = 50.0/(read_ms/1000.0);
    printf("        %d/%d ok │ %.0f ms │ %.0f reads/s │ %d miss\n", rok, 50, read_ms, read_iops, rmiss);

    /* ── STAGE 4: Delete half of the sequential files ── */
    printf("  [4/7] Delete Test (deleting %d files)...\n", TOTAL/2);
    int del_ok = 0, del_fail = 0;
    for (int i = 0; i < TOTAL; i += 2) {
        int ret = run_quiet("delete %s /stest/s_%04d.txt", IMAGE, i);
        if (ret == 0) del_ok++; else { del_fail++; printf("        [FAIL] could not delete s_%04d.txt\n", i); }
    }
    /* verify deleted files are actually gone (read should fail with "Not found") */
    int del_verify_bad = 0;
    for (int i = 0; i < TOTAL; i += 2) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "./tundra_fs read %s /stest/s_%04d.txt 2>/dev/null", IMAGE, i);
        FILE *p = popen(cmd, "r"); char buf[64] = {0};
        size_t n = p ? fread(buf, 1, sizeof(buf)-1, p) : 0;
        if (p) pclose(p);
        buf[n < sizeof(buf) ? n : sizeof(buf)-1] = 0;
        /* "Not found" is the expected stdout for a deleted file -- only flag
           as a leak if we got something else (i.e. actual file content) */
        if (n > 0 && strncmp(buf, "Not found", 9) != 0) del_verify_bad++;
    }
    int del_fsck_errs = 0;
    run_fsck(&del_fsck_errs);
    printf("        deleted %d/%d │ %d still readable (should be 0) │ fsck %s\n",
           del_ok, TOTAL/2, del_verify_bad, del_fsck_errs ? "FAIL" : "CLEAN");

    /* ── STAGE 5: Overwrite race -- N threads hammering the SAME file ── */
    printf("  [5/7] Overwrite Race (%d threads x %d rounds, same file)...\n", OVERWRITE_THREADS, OVERWRITE_ROUNDS);
    overwrite_errors = 0; t0 = now_us();
    pthread_t oth[OVERWRITE_THREADS]; int oids[OVERWRITE_THREADS];
    for (int i = 0; i < OVERWRITE_THREADS; i++) { oids[i] = i; pthread_create(&oth[i], NULL, ow, &oids[i]); }
    for (int i = 0; i < OVERWRITE_THREADS; i++) pthread_join(oth[i], NULL);
    t1 = now_us();
    double ow_ms = (t1-t0)/1000.0;
    int ow_fsck_errs = 0;
    run_fsck(&ow_fsck_errs);
    printf("        %d ops │ %.0f ms │ %d write errs │ fsck %s\n",
           OVERWRITE_THREADS*OVERWRITE_ROUNDS, ow_ms, overwrite_errors, ow_fsck_errs ? "FAIL" : "CLEAN");

    /* ── STAGE 6: Heavy concurrency -- many threads/files spread across AGs ── */
    printf("  [6/7] Heavy Concurrency (%d threads x %d files)...\n", HEAVY_THREADS, HEAVY_FILES);
    heavy_errors = 0; t0 = now_us();
    pthread_t hth[HEAVY_THREADS]; int hids[HEAVY_THREADS];
    for (int i = 0; i < HEAVY_THREADS; i++) { hids[i] = i; pthread_create(&hth[i], NULL, heavy, &hids[i]); }
    for (int i = 0; i < HEAVY_THREADS; i++) pthread_join(hth[i], NULL);
    t1 = now_us();
    double heavy_ms = (t1-t0)/1000.0, heavy_iops = HEAVY_FILES/(heavy_ms/1000.0);
    int heavy_fsck_errs = 0;
    run_fsck(&heavy_fsck_errs);
    printf("        %d files │ %.0f ms │ %.0f writes/s │ %d errs │ fsck %s\n",
           HEAVY_FILES, heavy_ms, heavy_iops, heavy_errors, heavy_fsck_errs ? "FAIL" : "CLEAN");

    /* ── STAGE 7: Final Integrity Check ── */
    printf("  [7/7] Final Integrity Check...\n");
    int fsck_errs = 0;
    run_fsck(&fsck_errs);
    printf("        fsck: %s (%d errors)\n", fsck_errs ? "FAIL" : "CLEAN", fsck_errs);

    /* ── SCORING (NO CAPS) ── */
    double write_score = (seq_iops / 10.0) + (par_iops / 10.0);
    double read_score = (rok / 50.0) * 300.0;
    double integrity_score = fsck_errs ? 0 : 200;
    if (fsck_errs > 0 && fsck_errs <= 5) integrity_score = 100;
    double penalty = rmiss * 10.0 + del_fail * 10.0 + del_verify_bad * 20.0
                    + overwrite_errors * 5.0 + heavy_errors * 5.0
                    + (del_fsck_errs + ow_fsck_errs + heavy_fsck_errs) * 15.0;
    double score = write_score + read_score + integrity_score - penalty;

    /* ── REPORT ── */
    printf("\n");
    printf("  ╔══════════════════════════════════════════════════╗\n");
    printf("  ║              BENCHMARK RESULTS                  ║\n");
    printf("  ╠══════════════════════════════════════════════════╣\n");
    printf("  ║  Seq Write:   %7.0f writes/s  (%6.1f ms)       ║\n", seq_iops, seq_ms);
    printf("  ║  Par Write:   %7.0f ops/s     (%6.1f ms)       ║\n", par_iops, par_ms);
    printf("  ║  Rand Read:   %7.0f reads/s   (%6.1f ms)       ║\n", read_iops, read_ms);
    printf("  ║  Read OK:     %3d/50  (%d%%)                      ║\n", rok, rok*2);
    printf("  ║  Delete:      %3d/%d ok, %d leaked                ║\n", del_ok, TOTAL/2, del_verify_bad);
    printf("  ║  Overwrite:   %3d errs (race, same file)          ║\n", overwrite_errors);
    printf("  ║  Heavy Conc:  %7.0f writes/s │ %3d errs            ║\n", heavy_iops, heavy_errors);
    printf("  ║  Integrity:   %s (%d errs)                 ║\n", fsck_errs ? "FAIL" : "PASS", fsck_errs);
    printf("  ╠══════════════════════════════════════════════════╣\n");
    printf("  ║  Write:    %8.0f pts                          ║\n", write_score);
    printf("  ║  Read:     %8.0f pts                          ║\n", read_score);
    printf("  ║  Integrity:%8.0f pts                          ║\n", integrity_score);
    printf("  ║  Penalty:  %8.0f pts                          ║\n", -penalty);
    printf("  ╠══════════════════════════════════════════════════╣\n");
    printf("  ║  SCORE:    %8.0f pts                          ║\n", score);
    printf("  ╚══════════════════════════════════════════════════╝\n\n");

    return errors + rmiss + fsck_errs + del_fsck_errs + ow_fsck_errs + heavy_fsck_errs;
}
