/* Small file helpers shared by the host's report writer (report.c) and recorder (recorder.c). */
#pragma once
#include <fcntl.h>
#include <unistd.h>

/* from copied to to (created or truncated); 1 when all of it was copied */
static inline int copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY | O_CLOEXEC); if (in < 0) return 0;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (out < 0) { close(in); return 0; }
    char buf[1 << 16]; ssize_t n; int ok = 1;
    while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) { ok = 0; break; }
    close(in); close(out); return ok && n == 0;
}
