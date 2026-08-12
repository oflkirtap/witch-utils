#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>

#define PORTS_DIR   "/usr/ports"
#define INSTALL_DIR "/usr/local/bin"
#define DB_DIR      INSTALL_DIR "/.alchemy_db"   // tiny per-package metadata store
#define PATH_LEN    1024

// small helpers

// extract filename/binary name from a relative path ("main/neofetch" -> "neofetch")
const char *get_base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// validate a port path against directory traversal and other forms
int is_safe_port_name(const char *name) {
    if (!name || name[0] == '\0') return 0;
    if (name[0] == '/' || name[0] == '.') return 0;      // no absolute or hidden/relative starts
    if (strstr(name, "..") != NULL) return 0;            // no directory traversal
    if (strstr(name, "//") != NULL) return 0;            // no empty path components
    if (name[strlen(name) - 1] == '/') return 0;         // no trailing slash
    return 1;
}

static void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) s[--len] = '\0';
}

// read the first line of a file into buf, newline stripped. returns 1 on success.
static int read_first_line(const char *path, char *buf, size_t bufsize) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, bufsize, f)) { fclose(f); return 0; }
    fclose(f);
    trim_newline(buf);
    return 1;
}

// copy a binary file and mark it executable 
int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        perror("Error opening source binary");
        return -1;
    }

    FILE *out = fopen(dst, "wb");
    if (!out) {
        perror("Error opening destination path for writing");
        fclose(in);
        return -1;
    }

    char buffer[8192];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, bytes, out) != bytes) {
            perror("Error writing binary data to destination");
            fclose(in);
            fclose(out);
            return -1;
        }
    }

    fclose(in);
    fclose(out);

    if (chmod(dst, 0755) != 0) {
        perror("Warning: Failed to set executable permissions");
    }

    return 0;
}

// read and print a files contents, indented, under a heading
void print_file_contents(const char *filepath, const char *prefix) {
    FILE *f = fopen(filepath, "r");
    if (!f) return;

    char line[256];
    int first_line = 1;
    int at_start_of_line = 1;

    while (fgets(line, sizeof(line), f)) {
        if (first_line) {
            printf("%s", prefix);
            first_line = 0;
        }
        if (at_start_of_line) printf("  ");
        printf("%s", line);

        size_t len = strlen(line);
        at_start_of_line = (len > 0 && line[len - 1] == '\n');
    }

    if (!first_line && !at_start_of_line) printf("\n");

    fclose(f);
}

// script execution, shared by brew / mix / sublimate

static int script_exists(const char *dir, const char *script_name) {
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", dir, script_name);
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IXUSR);
}

// fork chdir into dir and execute ./script_name [extra_arg].
// returns the childs exit code on normal exit, or -1 if it couldnt run
// or exited abnormally (killed by a signal, fork/wait failure)
static int run_script(const char *dir, const char *script_name, const char *extra_arg) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("Fork failed");
        return -1;
    }

    if (pid == 0) {
        if (chdir(dir) != 0) {
            perror("Chdir failed");
            _exit(1);
        }
        char rel_path[64];
        snprintf(rel_path, sizeof(rel_path), "./%s", script_name);
        char *args[3] = { rel_path, (char *)extra_arg, NULL };
        execv(rel_path, args);
        perror("Failed to execute script");
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) == -1) {
        perror("Waitpid failed");
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "Process killed by signal %d\n", WTERMSIG(status));
    }
    return -1;
}

// ---------------------------------------------------------------------
// installed-package bookkeeping
// each metadata file is named after the installed binary and holds two
// lines: the installed version, then the source "category/port" path
// ---------------------------------------------------------------------

static void ensure_db_dir(void) {
    struct stat st;
    if (stat(DB_DIR, &st) != 0) mkdir(DB_DIR, 0755);
}

static void record_install(const char *base_name, const char *port_name) {
    ensure_db_dir();

    char version_file[PATH_LEN];
    snprintf(version_file, sizeof(version_file), "%s/%s/version", PORTS_DIR, port_name);
    char version[128] = "unknown";
    read_first_line(version_file, version, sizeof(version));

    char meta_path[PATH_LEN];
    snprintf(meta_path, sizeof(meta_path), "%s/%s", DB_DIR, base_name);

    FILE *f = fopen(meta_path, "w");
    if (!f) return; // best-effort bookkeeping shouldnt fail install
    fprintf(f, "%s\n%s\n", version, port_name);
    fclose(f);
}

static int lookup_installed_version(const char *base_name, char *buf, size_t bufsize) {
    char meta_path[PATH_LEN];
    snprintf(meta_path, sizeof(meta_path), "%s/%s", DB_DIR, base_name);
    return read_first_line(meta_path, buf, bufsize);
}

static void remove_meta(const char *base_name) {
    char meta_path[PATH_LEN];
    snprintf(meta_path, sizeof(meta_path), "%s/%s", DB_DIR, base_name);
    unlink(meta_path);
}

// commands

// forward declaration for transmute_all so transmute_port can use it
int transmute_all(const char *unused);

// alchemy brew (build a port from source)
int brew_port(const char *port_name) {
    if (!is_safe_port_name(port_name)) {
        fprintf(stderr, "Error: Invalid or unsafe port path '%s'\n", port_name);
        return 1;
    }

    char target_dir[PATH_LEN];
    int len = snprintf(target_dir, sizeof(target_dir), "%s/%s", PORTS_DIR, port_name);
    if (len < 0 || len >= (int)sizeof(target_dir)) {
        fprintf(stderr, "Error: Port path too long.\n");
        return 1;
    }

    struct stat st;
    if (stat(target_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: Port '%s' not found at %s\n", port_name, target_dir);
        return 1;
    }

    printf("Brewing %s...\n", port_name);

    char version_file[PATH_LEN], depends_file[PATH_LEN];
    snprintf(version_file, sizeof(version_file), "%s/version", target_dir);
    snprintf(depends_file, sizeof(depends_file), "%s/depends", target_dir);
    print_file_contents(version_file, "Age (Version):\n");
    print_file_contents(depends_file, "Ingredients required (Dependencies):\n");

    if (!script_exists(target_dir, "build")) {
        fprintf(stderr, "Error: No build scroll found for %s\n", port_name);
        return 1;
    }

    printf("Following build scrolls...\n");
    int rc = run_script(target_dir, "build", NULL);

    if (rc == 0) {
        printf("Successfully brewed: %s\n", port_name);
        return 0;
    }
    if (rc > 0) {
        fprintf(stderr, "Brewing failed: Build failed for %s (exit code %d)\n", port_name, rc);
        return rc;
    }
    fprintf(stderr, "Brewing halted: Build process for %s terminated abnormally\n", port_name);
    return 1;
}

// alchemy mix (install a built port)
int mix_port(const char *port_name) {
    if (!is_safe_port_name(port_name)) {
        fprintf(stderr, "Error: Invalid or unsafe port path '%s'\n", port_name);
        return 1;
    }

    printf("Mixing in %s...\n", port_name);

    char target_dir[PATH_LEN];
    snprintf(target_dir, sizeof(target_dir), "%s/%s", PORTS_DIR, port_name);

    struct stat st;
    if (stat(target_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: Port directory '%s' does not exist.\n", target_dir);
        return 1;
    }

    const char *base_name = get_base_name(port_name);

    if (script_exists(target_dir, "install")) {
        printf("Executing custom install scroll...\n");
        int rc = run_script(target_dir, "install", INSTALL_DIR);
        if (rc == 0) {
            record_install(base_name, port_name);
            printf("Successfully mixed and installed %s into %s\n", port_name, INSTALL_DIR);
            return 0;
        }
        fprintf(stderr, "Error: Installation scroll failed for %s\n", port_name);
        return rc > 0 ? rc : 1;
    }

    // fallback: copy the built binary straight into install_dir
    char src_binary[PATH_LEN], dest_binary[PATH_LEN];
    snprintf(src_binary, sizeof(src_binary), "%s/%s", target_dir, base_name);
    snprintf(dest_binary, sizeof(dest_binary), "%s/%s", INSTALL_DIR, base_name);

    if (stat(src_binary, &st) != 0) {
        fprintf(stderr, "Error: Compiled binary '%s' not found. Did you run 'brew' first?\n", src_binary);
        return 1;
    }

    if (copy_file(src_binary, dest_binary) != 0) {
        fprintf(stderr, "Error: Failed to install %s into %s\n", base_name, INSTALL_DIR);
        return 1;
    }

    record_install(base_name, port_name);
    printf("Successfully mixed (installed) %s -> %s\n", base_name, dest_binary);
    return 0;
}

// alchemy study (search ports, or view details on one)
int study_port(const char *query) {
    if (!query) query = "";

    if (query[0] == '\0') {
        printf("Studying all known scrolls...\n\n");
    } else {
        printf("Studying scrolls... Searching for: %s\n", query);
    }

    // direct lookup: query looks like a real "category/port" path
    if (query[0] != '\0' && is_safe_port_name(query)) {
        char port_dir[PATH_LEN];
        snprintf(port_dir, sizeof(port_dir), "%s/%s", PORTS_DIR, query);

        struct stat st;
        if (stat(port_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            printf("\n--- Port Details: %s ---\n", query);

            char version_file[PATH_LEN], depends_file[PATH_LEN], desc_file[PATH_LEN];
            snprintf(version_file, sizeof(version_file), "%s/version", port_dir);
            snprintf(depends_file, sizeof(depends_file), "%s/depends", port_dir);
            snprintf(desc_file, sizeof(desc_file), "%s/description", port_dir);

            print_file_contents(version_file, "Version:\n");
            print_file_contents(depends_file, "Dependencies:\n");
            print_file_contents(desc_file, "Description:\n");

            char installed_version[128];
            if (lookup_installed_version(get_base_name(query), installed_version, sizeof(installed_version))) {
                printf("Status: Installed (version %s)\n", installed_version);
            } else {
                printf("Status: Not installed\n");
            }
            return 0;
        }
    }

    // otherwise, search every category for a substring match
    DIR *dir = opendir(PORTS_DIR);
    if (!dir) {
        perror("Error reading ports directory");
        return 1;
    }

    struct dirent *cat_entry;
    int matches = 0;

    while ((cat_entry = readdir(dir)) != NULL) {
        if (cat_entry->d_name[0] == '.') continue;

        char cat_path[PATH_LEN];
        snprintf(cat_path, sizeof(cat_path), "%s/%s", PORTS_DIR, cat_entry->d_name);

        DIR *cat_dir = opendir(cat_path);
        if (!cat_dir) continue;

        struct dirent *port_entry;
        while ((port_entry = readdir(cat_dir)) != NULL) {
            if (port_entry->d_name[0] == '.') continue;

            int is_match = query[0] == '\0' ||
                           strstr(port_entry->d_name, query) != NULL ||
                           strstr(cat_entry->d_name, query) != NULL;
            if (!is_match) continue;

            char installed_version[128];
            const char *marker = lookup_installed_version(port_entry->d_name, installed_version,
                                                            sizeof(installed_version)) ? " [installed]" : "";
            printf("  %s/%s%s\n", cat_entry->d_name, port_entry->d_name, marker);
            matches++;
        }
        closedir(cat_dir);
    }
    closedir(dir);

    if (matches == 0) {
        printf("No matching scrolls or alchemy ingredients found for '%s'.\n", query);
    } else {
        printf("\nFound %d matching port(s).\n", matches);
    }

    return 0;
}

// alchemy sublimate (remove an installed package)
int sublimate_port(const char *port_name) {
    if (!is_safe_port_name(port_name)) {
        fprintf(stderr, "Error: Invalid or unsafe port path '%s'\n", port_name);
        return 1;
    }

    printf("Sublimating potion %s...\n", port_name);

    char target_dir[PATH_LEN];
    snprintf(target_dir, sizeof(target_dir), "%s/%s", PORTS_DIR, port_name);
    const char *base_name = get_base_name(port_name);

    if (script_exists(target_dir, "uninstall")) {
        printf("Executing custom uninstall scroll...\n");
        int rc = run_script(target_dir, "uninstall", INSTALL_DIR);
        if (rc == 0) {
            remove_meta(base_name);
            printf("Successfully sublimated %s from system.\n", port_name);
            return 0;
        }
        fprintf(stderr, "Error: Uninstall scroll failed for %s\n", port_name);
        return rc > 0 ? rc : 1;
    }

    // default removal: unlink the binary in install_dir
    char installed_path[PATH_LEN];
    snprintf(installed_path, sizeof(installed_path), "%s/%s", INSTALL_DIR, base_name);

    if (unlink(installed_path) == 0) {
        remove_meta(base_name);
        printf("Successfully sublimated (removed) %s from %s\n", base_name, INSTALL_DIR);
        return 0;
    }

    perror("Sublimation failed");
    return 1;
}

// alchemy catalog (list installed potions)
int list_installed(const char *unused) {
    (void)unused;

    DIR *dir = opendir(DB_DIR);
    if (!dir) {
        printf("No packages installed yet.\n");
        return 0;
    }

    struct dirent *entry;
    int count = 0;
    printf("Installed potions:\n");

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char meta_path[PATH_LEN];
        snprintf(meta_path, sizeof(meta_path), "%s/%s", DB_DIR, entry->d_name);

        char version[128] = "unknown", source[PATH_LEN] = "";
        FILE *f = fopen(meta_path, "r");
        if (f) {
            if (fgets(version, sizeof(version), f)) trim_newline(version);
            if (fgets(source, sizeof(source), f)) trim_newline(source);
            fclose(f);
        }

        if (source[0]) {
            printf("  %-20s %-10s (%s)\n", entry->d_name, version, source);
        } else {
            printf("  %-20s %s\n", entry->d_name, version);
        }
        count++;
    }
    closedir(dir);

    if (count == 0) printf("  (none)\n");
    else printf("\n%d package(s) installed.\n", count);

    return 0;
}

// alchemy transmute (upgrade): rebuilds + reinstalls only when the ports
// tree actually has a newer version than whats currently installed
int transmute_port(const char *port_name) {
    if (strcmp(port_name, "all") == 0) {
        return transmute_all("");
    }

    if (!is_safe_port_name(port_name)) {
        fprintf(stderr, "Error: Invalid or unsafe port path '%s'\n", port_name);
        return 1;
    }

    char target_dir[PATH_LEN];
    snprintf(target_dir, sizeof(target_dir), "%s/%s", PORTS_DIR, port_name);

    struct stat st;
    if (stat(target_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: Port '%s' not found at %s\n", port_name, target_dir);
        return 1;
    }

    const char *base_name = get_base_name(port_name);

    char version_file[PATH_LEN];
    snprintf(version_file, sizeof(version_file), "%s/version", target_dir);
    char available[128] = "unknown";
    read_first_line(version_file, available, sizeof(available));

    char installed[128];
    if (!lookup_installed_version(base_name, installed, sizeof(installed))) {
        fprintf(stderr, "'%s' is not installed. Run 'mix %s' first.\n", base_name, port_name);
        return 1;
    }

    if (strcmp(installed, available) == 0) {
        printf("%s is already at the latest version (%s).\n", base_name, installed);
        return 0;
    }

    printf("Transmuting %s: %s -> %s\n", base_name, installed, available);

    int rc = brew_port(port_name);
    if (rc != 0) {
        fprintf(stderr, "Transmutation failed: could not brew %s\n", port_name);
        return rc;
    }

    rc = mix_port(port_name);
    if (rc != 0) {
        fprintf(stderr, "Transmutation failed: could not mix %s\n", port_name);
        return rc;
    }

    printf("Successfully transmuted %s to version %s\n", base_name, available);
    return 0;
}

// alchemy transmute all (upgrade all packages)
int transmute_all(const char *unused) {
    (void)unused;
    DIR *dir = opendir(DB_DIR);
    if (!dir) {
        printf("No packages installed yet.\n");
        return 0;
    }

    struct dirent *entry;
    int count = 0;
    int success = 0;

    printf("Transmuting all installed packages...\n");

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char meta_path[PATH_LEN];
        snprintf(meta_path, sizeof(meta_path), "%s/%s", DB_DIR, entry->d_name);

        char version[128] = "unknown", source[PATH_LEN] = "";
        FILE *f = fopen(meta_path, "r");
        if (f) {
            if (fgets(version, sizeof(version), f)) trim_newline(version);
            if (fgets(source, sizeof(source), f)) trim_newline(source);
            fclose(f);
        }

        if (source[0]) {
            printf("\n--- Checking %s ---\n", source);
            if (transmute_port(source) == 0) {
                success++;
            }
            count++;
        }
    }
    closedir(dir);

    printf("\nTransmuted %d/%d package(s) successfully.\n", success, count);
    return (success == count) ? 0 : 1;
}

// command dispatch

typedef struct {
    const char *name;
    int (*handler)(const char *);
    int requires_arg;
    const char *description;
} Command;

static const Command COMMANDS[] = {
    {"brew",          brew_port,      1, "Build a port from source"},
    {"mix",           mix_port,       1, "Install a built port to " INSTALL_DIR},
    {"study",         study_port,     0, "Search ports, or view details (no arg = list all)"},
    {"sublimate",     sublimate_port, 1, "Remove an installed package"},
    {"transmute",     transmute_port, 1, "Upgrade a package to the latest version (or 'all')"},
    {"transmute-all", transmute_all,  0, "Upgrade all installed packages"},
    {"list",          list_installed, 0, "List installed packages"},
};
#define NUM_COMMANDS (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s <command> [<category>/<port>]\n\n", prog_name);
    fprintf(stderr, "Commands:\n");
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        fprintf(stderr, "  %-14s - %s\n", COMMANDS[i].name, COMMANDS[i].description);
    }
    fprintf(stderr, "\nExample: %s brew main/neofetch\n", prog_name);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(cmd, COMMANDS[i].name) != 0) continue;

        if (COMMANDS[i].requires_arg && argc < 3) {
            fprintf(stderr, "Error: '%s' requires a <category/port> argument.\n", cmd);
            return 1;
        }

        const char *cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(cmd, COMMANDS[i].name) != 0) continue;

        if (COMMANDS[i].requires_arg && argc < 3) {
            fprintf(stderr, "Error: '%s' requires a <category/port> argument.\n", cmd);
            return 1;
        }
        return COMMANDS[i].handler(argc >= 3 ? argv[2] : "");
    }

    fprintf(stderr, "Unknown alchemy command: %s\n", cmd);
    print_usage(argv[0]);
    return 1;
	}
}
