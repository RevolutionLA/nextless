// 独立按键注入器（fork 的自检工具）：用 /dev/uinput 发一个真实的按键 press/hold/release
// 用法: uinput_key <linux键码名对应的数字> <按住毫秒> [次数]
//   例: uinput_key 100 900      -> KEY_RIGHTALT 按住 900ms（100 = KEY_RIGHTALT）
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <linux/uinput.h>

static int ev(int fd, unsigned short type, unsigned short code, int value) {
    struct input_event e {};
    e.type = type; e.code = code; e.value = value;
    return write(fd, &e, sizeof(e)) == (ssize_t)sizeof(e) ? 0 : -1;
}
static int syn(int fd) { return ev(fd, EV_SYN, SYN_REPORT, 0); }
static void nap(long ms) {
    struct timespec t { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, nullptr);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "用法: %s <keycode> <hold_ms> [times]\n", argv[0]); return 2; }
    int code = atoi(argv[1]);
    long hold = atol(argv[2]);
    int times = argc > 3 ? atoi(argv[3]) : 1;

    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open /dev/uinput"); return 1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, code);
    struct uinput_setup us {};
    snprintf(us.name, sizeof(us.name), "fork-selftest-kbd");
    us.id.bustype = BUS_USB; us.id.vendor = 0x1; us.id.product = 0x1;
    if (ioctl(fd, UI_DEV_SETUP, &us) || ioctl(fd, UI_DEV_CREATE)) {
        perror("UI_DEV_CREATE"); close(fd); return 1;
    }
    nap(300);  // 等设备被输入子系统接管
    for (int i = 0; i < times; i++) {
        printf("[%d/%d] press keycode=%d, hold %ldms\n", i + 1, times, code, hold);
        if (ev(fd, EV_KEY, (unsigned short)code, 1) || syn(fd)) { perror("press"); break; }
        nap(hold);
        if (ev(fd, EV_KEY, (unsigned short)code, 0) || syn(fd)) { perror("release"); break; }
        nap(500);
    }
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
