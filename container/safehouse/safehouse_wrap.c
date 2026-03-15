/*
 * safehouse_wrap.c
 *
 * Generic command wrapper shim for safehouse.
 * Compile once, then create symlinks for each wrapped command.
 *
 * The binary reads argv[0] to determine which command it is wrapping,
 * loads the policy from /etc/safehouse/policies/<command>.policy,
 * and either blocks or exec's the real binary.
 *
 * Policy directives:
 *   REAL_BINARY /usr/bin/rm       - path to the real binary (required)
 *   BLOCK_FLAG  -f                - block if this exact flag appears
 *   BLOCK_ARG   /etc/             - block if any arg starts with this prefix
 *   ALLOW_FLAG  -l                - always allow if this flag is present
 *                                   (checked before BLOCK rules)
 *
 * All paths are compiled in — no runtime environment variable overrides.
 * This prevents the agent from disabling or redirecting safehouse.
 *
 * Exit codes:
 *   (passthrough) - whatever the real binary returns
 *   0             - blocked by policy (silent — agent sees success)
 *   2             - internal/configuration error
 *
 * No external dependencies beyond libc. No dynamic allocation.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <libgen.h>
#include <sys/types.h>
#include <pwd.h>

/* ---------- limits ---------- */

#define MAX_RULES    64
#define MAX_PATH_LEN 512
#define MAX_LINE     512
#define MAX_ARGS_STR 1024

#define POLICY_DIR "/etc/safehouse/policies"
#define LOG_DIR    "/var/log/safehouse"
#define ALERT_CMD  "/usr/local/lib/safehouse/safehouse_alert.sh"

/* ---------- policy ---------- */

typedef struct {
    char real_binary[MAX_PATH_LEN];

    char block_flags[MAX_RULES][MAX_PATH_LEN];
    int  block_flags_count;

    char block_args[MAX_RULES][MAX_PATH_LEN];
    int  block_args_count;

    char allow_flags[MAX_RULES][MAX_PATH_LEN];
    int  allow_flags_count;
} Policy;

/*
 * Load the policy file for `cmd_name`.
 * Returns 1 if a policy was loaded, 0 if no file exists.
 */
static int load_policy(const char *cmd_name, Policy *p)
{
    const char *dir = POLICY_DIR;

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s.policy", dir, cmd_name);

    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    memset(p, 0, sizeof(*p));

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        /* strip newline */
        line[strcspn(line, "\n")] = '\0';

        /* skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\0')
            continue;

        char directive[64];
        char value[MAX_PATH_LEN];
        int n = sscanf(line, "%63s %511s", directive, value);

        if (n == 2 && strcmp(directive, "REAL_BINARY") == 0) {
            snprintf(p->real_binary, MAX_PATH_LEN, "%s", value);
        } else if (n == 2 && strcmp(directive, "BLOCK_FLAG") == 0) {
            if (p->block_flags_count < MAX_RULES)
                snprintf(p->block_flags[p->block_flags_count++],
                         MAX_PATH_LEN, "%s", value);
        } else if (n == 2 && strcmp(directive, "BLOCK_ARG") == 0) {
            if (p->block_args_count < MAX_RULES)
                snprintf(p->block_args[p->block_args_count++],
                         MAX_PATH_LEN, "%s", value);
        } else if (n == 2 && strcmp(directive, "ALLOW_FLAG") == 0) {
            if (p->allow_flags_count < MAX_RULES)
                snprintf(p->allow_flags[p->allow_flags_count++],
                         MAX_PATH_LEN, "%s", value);
        }
        /* unknown directives are silently ignored */
    }

    fclose(f);
    return 1;
}

/* ---------- argument checking ---------- */

typedef struct {
    int         blocked;
    const char *reason;
    const char *offending;
} CheckResult;

static CheckResult check_args(const Policy *p, int argc, char *argv[])
{
    CheckResult r = {0, NULL, NULL};

    /* ALLOW_FLAG takes priority: if any allow-flag is present, permit the
       entire invocation unconditionally. */
    for (int i = 1; i < argc; i++) {
        for (int j = 0; j < p->allow_flags_count; j++) {
            if (strcmp(argv[i], p->allow_flags[j]) == 0)
                return r; /* allowed */
        }
    }

    /* Check blocked flags (exact match) */
    for (int i = 1; i < argc; i++) {
        for (int j = 0; j < p->block_flags_count; j++) {
            if (strcmp(argv[i], p->block_flags[j]) == 0) {
                r.blocked = 1;
                r.reason  = "blocked flag";
                r.offending = argv[i];
                return r;
            }
        }
    }

    /* Check blocked argument prefixes */
    for (int i = 1; i < argc; i++) {
        for (int j = 0; j < p->block_args_count; j++) {
            size_t plen = strlen(p->block_args[j]);
            if (strncmp(argv[i], p->block_args[j], plen) == 0) {
                r.blocked = 1;
                r.reason  = "blocked argument prefix";
                r.offending = argv[i];
                return r;
            }
        }
    }

    return r;
}

/* ---------- logging ---------- */

static void log_event(const char *cmd, const char *status,
                      const char *reason, const char *offending,
                      int argc, char *argv[])
{
    const char *log_dir = LOG_DIR;

    char log_path[MAX_PATH_LEN];
    snprintf(log_path, sizeof(log_path), "%s/events.log", log_dir);

    FILE *f = fopen(log_path, "a");
    if (!f)
        return;

    /* ISO 8601 timestamp */
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);

    /* username */
    struct passwd *pw = getpwuid(getuid());
    const char *user = pw ? pw->pw_name : "unknown";

    /* flatten argv[1..] into a string */
    char args[MAX_ARGS_STR] = "";
    for (int i = 1; i < argc && strlen(args) < (MAX_ARGS_STR - 64); i++) {
        strncat(args, argv[i], sizeof(args) - strlen(args) - 2);
        if (i < argc - 1)
            strncat(args, " ", sizeof(args) - strlen(args) - 1);
    }

    fprintf(f,
            "%s cmd=%s status=%s user=%s pid=%d "
            "reason=\"%s\" offending=\"%s\" args=\"%s\"\n",
            ts, cmd, status, user, (int)getpid(),
            reason    ? reason    : "",
            offending ? offending : "",
            args);
    fclose(f);
}

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
    /* Determine which command we are wrapping from the symlink name */
    char argv0_buf[MAX_PATH_LEN];
    snprintf(argv0_buf, sizeof(argv0_buf), "%s", argv[0]);
    char *cmd_name = basename(argv0_buf);

    /* Load policy */
    Policy policy;
    int has_policy = load_policy(cmd_name, &policy);

    if (!has_policy || policy.real_binary[0] == '\0') {
        fprintf(stderr,
                "safehouse: no policy or REAL_BINARY for '%s'\n", cmd_name);
        return 2;
    }

    /* Check arguments against policy */
    CheckResult check = check_args(&policy, argc, argv);

    if (check.blocked) {
        /* Log the blocked event (host-side only, never visible to agent) */
        log_event(cmd_name, "BLOCKED", check.reason, check.offending,
                  argc, argv);

        /* Run alert command (writes to IPC for host monitoring).
           Uses fork/execl with env vars instead of system() to avoid
           shell injection via check.reason / check.offending. */
        {
            pid_t pid = fork();
            if (pid == 0) {
                /* child: set env vars and exec the alert script directly */
                setenv("SAFEHOUSE_CMD",    cmd_name, 1);
                setenv("SAFEHOUSE_REASON", check.reason    ? check.reason    : "", 1);
                setenv("SAFEHOUSE_ARG",    check.offending ? check.offending : "", 1);
                execl(ALERT_CMD, ALERT_CMD, (char *)NULL);
                _exit(1); /* exec failed — silent */
            }
            /* parent: best-effort, don't wait */
        }

        /* Silent exit: return 0 and print nothing so the agent
           believes the command succeeded. No stderr, no distinctive
           exit code — safehouse is invisible from the inside. */
        return 0;
    }

    /* Allowed -- log and exec the real binary */
    log_event(cmd_name, "ALLOWED", "", "", argc, argv);

    argv[0] = policy.real_binary;
    execv(policy.real_binary, argv);

    /* execv only returns on error */
    fprintf(stderr, "safehouse: exec %s failed: %s\n",
            policy.real_binary, strerror(errno));
    return 2;
}
