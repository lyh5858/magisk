#include <sys/stat.h>
#include <sys/types.h>
#include <libgen.h>
#include <vector>

#include <xz.h>

#include <utils.hpp>
#include <binaries.h>

#include "init.hpp"

using namespace std;

bool unxz(int fd, const uint8_t *buf, size_t size) {
    uint8_t out[8192];
    xz_crc32_init();
    struct xz_dec *dec = xz_dec_init(XZ_DYNALLOC, 1 << 26);
    struct xz_buf b = {
        .in = buf,
        .in_pos = 0,
        .in_size = size,
        .out = out,
        .out_pos = 0,
        .out_size = sizeof(out)
    };
    enum xz_ret ret;
    do {
        ret = xz_dec_run(dec, &b);
        if (ret != XZ_OK && ret != XZ_STREAM_END)
            return false;
        write(fd, out, b.out_pos);
        b.out_pos = 0;
    } while (b.in_pos != size);
    return true;
}

int dump_manager(const char *path, mode_t mode) {
    int fd = xopen(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return 1;
    if (!unxz(fd, manager_xz, sizeof(manager_xz)))
        return 1;
    close(fd);
    return 0;
}

class RecoveryInit : public BaseInit {
public:
    using BaseInit::BaseInit;
    void start() override {
        LOGD("Ramdisk is recovery, abort\n");
        rename(backup_init(), "/init");
        rm_rf("/.backup");
        exec_init();
    }
};

int main(int argc, char *argv[]) {
    umask(0);

    auto name = basename(argv[0]);
    if (name == "magisk"sv)
        return magisk_proxy_main(argc, argv);

    if (argc > 1 && argv[1] == "-x"sv) {
        if (argc > 2 && argv[2] == "manager"sv)
            return dump_manager(argv[3], 0644);
        return 1;
    }

    if (getpid() != 1)
        return 1;

    BaseInit *init;
    BootConfig config{};

    if (argc > 1 && argv[1] == "selinux_setup"sv) {
        init = new SecondStageInit(argv);
    } else {
        // This will also mount /sys and /proc
        load_kernel_info(&config);

        if (config.skip_initramfs)
            init = new LegacySARInit(argv, &config);
        else if (config.force_normal_boot)
            init = new FirstStageInit(argv, &config);
        else if (access("/sbin/recovery", F_OK) == 0 || access("/system/bin/recovery", F_OK) == 0)
            init = new RecoveryInit(argv, &config);
        else if (check_two_stage())
            init = new FirstStageInit(argv, &config);
        else
            init = new RootFSInit(argv, &config);
    }

    // Run the main routine
    init->start();
    exit(1);
}
