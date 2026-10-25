/* Platform layer for the PS Vita. */
#include "../platform/platform.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/netctl.h>
#include <stdio.h>
#include <string.h>

#include "vita.h"

#define DATA_DIR "ux0:data/VitaCast"

static FILE *g_log;

void vita_platform_init(const char *log_name)
{
    char path[128];
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DATA_DIR, 0777);
    snprintf(path, sizeof(path), DATA_DIR "/%s", log_name);
    g_log = fopen(path, "w");
}

void vita_platform_shutdown(void)
{
    if (g_log)
        fclose(g_log);
    g_log = NULL;
}

uint64_t plat_ticks_ms(void)
{
    return (uint64_t)sceKernelGetProcessTimeWide() / 1000u;
}

void plat_sleep_ms(int ms)
{
    sceKernelDelayThread((SceUInt)ms * 1000u);
}

const char *plat_data_dir(void)
{
    return DATA_DIR;
}

int plat_local_ipv4(char out[16])
{
    SceNetCtlInfo info;
    int state = 0;
    if (sceNetCtlInetGetState(&state) < 0 || state != SCE_NETCTL_STATE_CONNECTED)
        return -1;
    memset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0)
        return -1;
    if (!info.ip_address[0] || strcmp(info.ip_address, "0.0.0.0") == 0)
        return -1;
    snprintf(out, 16, "%s", info.ip_address);
    return 0;
}

int plat_random(void *buf, size_t len)
{
    unsigned char *p = buf;
    while (len) {
        size_t n = len > 64 ? 64 : len;
        if (sceKernelGetRandomNumber(p, n) < 0)
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

void plat_log_write(const char *line)
{
    if (!g_log)
        return;
    fprintf(g_log, "[%8llu] %s\n", (unsigned long long)plat_ticks_ms(), line);
    fflush(g_log);
}
