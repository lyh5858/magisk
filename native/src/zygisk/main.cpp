#include <sys/mount.h>
#include <android/dlext.h>
#include <dlfcn.h>

#include <magisk.hpp>
#include <base.hpp>
#include <socket.hpp>
#include <daemon.hpp>
#include <selinux.hpp>
#include <embed.hpp>

#include "zygisk.hpp"

using namespace std;

static bool is_valid_environment_variable(const char* name) {
    // According to the kernel source, by default the kernel uses 32*PAGE_SIZE
    // as the maximum size for an environment variable definition.
    const int MAX_ENV_LEN = 32*4096;

    if (name == nullptr) {
        return false;
    }

    // Parse the string, looking for the first '=' there, and its size.
    int pos = 0;
    int first_equal_pos = -1;
    while (pos < MAX_ENV_LEN) {
        if (name[pos] == '\0') {
            break;
        }
        if (name[pos] == '=' && first_equal_pos < 0) {
            first_equal_pos = pos;
        }
        pos++;
    }

    // Check that it's smaller than MAX_ENV_LEN (to detect non-zero terminated strings).
    if (pos >= MAX_ENV_LEN) {
        return false;
    }

    // Check that it contains at least one equal sign that is not the first character
    if (first_equal_pos < 1) {
        return false;
    }

    return true;
}

static const char* env_match(const char* envstr, const char* name) {
    size_t i = 0;

    while (envstr[i] == name[i] && name[i] != '\0') {
        ++i;
    }

    if (name[i] == '\0' && envstr[i] == '=') {
        return envstr + i + 1;
    }

    return nullptr;
}

static bool is_unsafe_environment_variable(const char* name) {
    // None of these should be allowed when the AT_SECURE auxv
    // flag is set. This flag is set to inform userspace that a
    // security transition has occurred, for example, as a result
    // of executing a setuid program or the result of an SELinux
    // security transition.
    static constexpr const char* UNSAFE_VARIABLE_NAMES[] = {
            "ANDROID_DNS_MODE",
            "GCONV_PATH",
            "GETCONF_DIR",
            "HOSTALIASES",
            "JE_MALLOC_CONF",
            "LD_AOUT_LIBRARY_PATH",
            "LD_AOUT_PRELOAD",
            "LD_AUDIT",
            "LD_CONFIG_FILE",
            "LD_DEBUG",
            "LD_DEBUG_OUTPUT",
            "LD_DYNAMIC_WEAK",
            "LD_LIBRARY_PATH",
            "LD_ORIGIN_PATH",
//            "LD_PRELOAD",
            "LD_PROFILE",
            "LD_SHOW_AUXV",
            "LD_USE_LOAD_BIAS",
            "LIBC_DEBUG_MALLOC_OPTIONS",
            "LIBC_HOOKS_ENABLE",
            "LOCALDOMAIN",
            "LOCPATH",
            "MALLOC_CHECK_",
            "MALLOC_CONF",
            "MALLOC_TRACE",
            "NIS_PATH",
            "NLSPATH",
            "RESOLV_HOST_CONF",
            "RES_OPTIONS",
            "SCUDO_OPTIONS",
            "TMPDIR",
            "TZDIR",
    };
    for (const auto& unsafe_variable_name : UNSAFE_VARIABLE_NAMES) {
        if (env_match(name, unsafe_variable_name) != nullptr) {
            return true;
        }
    }
    return false;
}

static void sanitize_environment_variables(char** env) {
    char** src = env;
    char** dst = env;
    for (; src[0] != nullptr; ++src) {
        if (!is_valid_environment_variable(src[0])) {
            continue;
        }
        // Remove various unsafe environment variables if we're loading a setuid program.
        if (is_unsafe_environment_variable(src[0])) {
            continue;
        }
        dst[0] = src[0];
        ++dst;
    }
    dst[0] = nullptr;
}

// Entrypoint for app_process overlay
int app_process_main(int argc, char *argv[]) {
    android_logging();
    char buf[PATH_MAX];

    bool zygote = false;
    if (!selinux_enabled()) {
        for (int i = 0; i < argc; ++i) {
            if (argv[i] == "--zygote"sv) {
                zygote = true;
                break;
            }
        }
    } else if (auto fp = open_file("/proc/self/attr/current", "r")) {
        fscanf(fp.get(), "%s", buf);
        zygote = (buf == "u:r:zygote:s0"sv);
    }

    if (!zygote) {
        // For the non zygote case, we need to get real app_process via passthrough
        // We have to connect magiskd via exec-ing magisk due to SELinux restrictions

        // This is actually only relevant for calling app_process via ADB shell
        // because zygisk shall already have the app_process overlays unmounted
        // during app process specialization within its private mount namespace.

        int fds[2];
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds);
        if (fork_dont_care() == 0) {
            // This fd has to survive exec
            fcntl(fds[1], F_SETFD, 0);
            ssprintf(buf, sizeof(buf), "%d", fds[1]);
#if defined(__LP64__)
            execlp("magisk", "", "zygisk", "passthrough", buf, "1", (char *) nullptr);
#else
            execlp("magisk", "", "zygisk", "passthrough", buf, "0", (char *) nullptr);
#endif
            exit(-1);
        }

        close(fds[1]);
        if (read_int(fds[0]) != 0) {
            fprintf(stderr, "Failed to connect magiskd, try umount %s or reboot.\n", argv[0]);
            return 1;
        }
        int app_proc_fd = recv_fd(fds[0]);
        if (app_proc_fd < 0)
            return 1;
        close(fds[0]);

        fcntl(app_proc_fd, F_SETFD, FD_CLOEXEC);
        fexecve(app_proc_fd, argv, environ);
        return 1;
    }

    if (int socket = zygisk_request(ZygiskRequest::SETUP); socket >= 0) {
        do {
            if (read_int(socket) != 0)
                break;

            // Send over zygisk loader
            write_int(socket, sizeof(zygisk_ld));
            xwrite(socket, zygisk_ld, sizeof(zygisk_ld));

            int app_proc_fd = recv_fd(socket);
            if (app_proc_fd < 0)
                break;

            string tmp = read_string(socket);
            if (char *ld = getenv("LD_PRELOAD")) {
                string env = ld;
                env += ':';
                env += HIJACK_BIN;
                setenv("LD_PRELOAD", env.data(), 1);
            } else {
                setenv("LD_PRELOAD", HIJACK_BIN, 1);
            }
            setenv(MAGISKTMP_ENV, tmp.data(), 1);

            close(socket);

            fcntl(app_proc_fd, F_SETFD, FD_CLOEXEC);
            fprintf(xopen_file("/proc/self/attr/current", "w").get(), "u:r:init:s0");
            sanitize_environment_variables(environ);
            fexecve(app_proc_fd, argv, environ);
        } while (false);

        close(socket);
    }

    // If encountering any errors, unmount and execute the original app_process
    xreadlink("/proc/self/exe", buf, sizeof(buf));
    xumount2("/proc/self/exe", MNT_DETACH);
    execve(buf, argv, environ);
    return 1;
}

static void zygiskd(int socket) {
    if (getuid() != 0 || fcntl(socket, F_GETFD) < 0)
        exit(-1);

#if defined(__LP64__)
    set_nice_name("zygiskd64");
    LOGI("* Launching zygiskd64\n");
#else
    set_nice_name("zygiskd32");
    LOGI("* Launching zygiskd32\n");
#endif

    // Load modules
    using comp_entry = void(*)(int);
    vector<comp_entry> modules;
    {
        vector<int> module_fds = recv_fds(socket);
        for (int fd : module_fds) {
            comp_entry entry = nullptr;
            struct stat s{};
            if (fstat(fd, &s) == 0 && S_ISREG(s.st_mode)) {
                android_dlextinfo info {
                    .flags = ANDROID_DLEXT_USE_LIBRARY_FD,
                    .library_fd = fd,
                };
                if (void *h = android_dlopen_ext("/jit-cache", RTLD_LAZY, &info)) {
                    *(void **) &entry = dlsym(h, "zygisk_companion_entry");
                } else {
                    LOGW("Failed to dlopen zygisk module: %s\n", dlerror());
                }
            }
            modules.push_back(entry);
            close(fd);
        }
    }

    // ack
    write_int(socket, 0);

    // Start accepting requests
    pollfd pfd = { socket, POLLIN, 0 };
    for (;;) {
        poll(&pfd, 1, -1);
        if (pfd.revents && !(pfd.revents & POLLIN)) {
            // Something bad happened in magiskd, terminate zygiskd
            exit(0);
        }
        int client = recv_fd(socket);
        if (client < 0) {
            // Something bad happened in magiskd, terminate zygiskd
            exit(0);
        }
        int module_id = read_int(client);
        if (module_id >= 0 && module_id < modules.size() && modules[module_id]) {
            exec_task([=, entry = modules[module_id]] {
                struct stat s1;
                fstat(client, &s1);
                entry(client);
                // Only close client if it is the same file so we don't
                // accidentally close a re-used file descriptor.
                // This check is required because the module companion
                // handler could've closed the file descriptor already.
                if (struct stat s2; fstat(client, &s2) == 0) {
                    if (s1.st_dev == s2.st_dev && s1.st_ino == s2.st_ino) {
                        close(client);
                    }
                }
            });
        } else {
            close(client);
        }
    }
}

// Entrypoint where we need to re-exec ourselves
// This should only ever be called internally
int zygisk_main(int argc, char *argv[]) {
    android_logging();

    if (argc == 3 && argv[1] == "companion"sv) {
        zygiskd(parse_int(argv[2]));
    } else if (argc == 4 && argv[1] == "passthrough"sv) {
        int client = parse_int(argv[2]);
        int is_64_bit = parse_int(argv[3]);
        if (fcntl(client, F_GETFD) < 0)
            return 1;
        if (int magiskd = connect_daemon(MainRequest::ZYGISK_PASSTHROUGH); magiskd >= 0) {
            write_int(magiskd, ZygiskRequest::PASSTHROUGH);
            write_int(magiskd, is_64_bit);

            if (read_int(magiskd) != 0) {
                write_int(client, 1);
                return 0;
            }

            write_int(client, 0);
            int real_app_fd = recv_fd(magiskd);
            send_fd(client, real_app_fd);
        } else {
            write_int(client, 1);
            return 0;
        }
    }
    return 0;
}
