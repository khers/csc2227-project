/* Copyright (c) 2020 Eric B Munson */

/*
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <kernkv/kernkv.h>

int main(int argc, char **argv)
{
    int ret;
    struct value value;
    unsigned int num = 3;
    /*
    char **ips;
    unsigned int i;
    char *next;
    */
    char *ips[3] = {
        "192.168.122.254",
        "192.168.122.95",
        "192.168.122.124"
    };

    if (argc < 2) {
        printf("USAGE %s <local ip for listening> <num remote nodes> <ip1,ip2,ip3,...>\n", argv[0]);
        return -1;
    }

    /*
    num = (unsigned int)atoi(argv[2]);
    ips = malloc(num * sizeof(char *));

    next = ips[0] = argv[3];
    for (i = 1; i < num; i++) {
        next = strchr(next, ',');
        *next = '\0';
        if (!next) {
            num = i;
            break;
        }
        next++;
        ips[i] = next;
    }
    */

    ret = init_kernkv(argv[1], num,	3, (char**)ips);
    if (ret) {
        printf("Failed to initialize kernkv: '%s'\n", strerror(errno));
        return ret;
    }

    memset(&value, 0, sizeof(struct value));
    value.len = 4;
    value.buf[0] = 0xFF;
    value.buf[1] = 0xFF;
    value.buf[2] = 0xFF;
    value.buf[3] = 0xFF;

    ret = kernkv_put(1, &value);
    if (ret) {
        printf("Failed to send get request: '%s'\n", strerror(errno));
        return ret;
    }

    memset(&value, 0, sizeof(struct value));
    ret = kernkv_get(1, &value);
    if (ret) {
        printf("Failed to send get request: '%s'\n", strerror(errno));
        return ret;
    }

    if (value.buf[0] != 0xFF) {
        printf("Got unknown value back %d, should have been %d\n", value.buf[0], 0XFF);
        return -1;
    }

    ret = kernkv_del(1);

    shutdown_kernkv();

    return ret;
}
