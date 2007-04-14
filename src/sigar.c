/*
 * Copyright (C) [2004, 2005, 2006], Hyperic, Inc.
 * This file is part of SIGAR.
 * 
 * SIGAR is free software; you can redistribute it and/or modify
 * it under the terms version 2 of the GNU General Public License as
 * published by the Free Software Foundation. This program is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA.
 */

#include <errno.h>
#ifndef WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

#include "sigar.h"
#include "sigar_private.h"
#include "sigar_util.h"
#include "sigar_os.h"

SIGAR_DECLARE(int) sigar_open(sigar_t **sigar)
{
    int status = sigar_os_open(sigar);

    if (status == SIGAR_OK) {
        (*sigar)->pid = 0;
        (*sigar)->ifconf_buf = NULL;
        (*sigar)->ifconf_len = 0;
        (*sigar)->log_level = -1; /* log nothing by default */
        (*sigar)->log_impl = NULL;
        (*sigar)->log_data = NULL;
        (*sigar)->ptql_re_impl = NULL;
        (*sigar)->ptql_re_data = NULL;
        (*sigar)->self_path = NULL;
        (*sigar)->proc_cpu = NULL;
        (*sigar)->net_listen = NULL;
    }

    return status;
}

SIGAR_DECLARE(int) sigar_close(sigar_t *sigar)
{
    if (sigar->ifconf_buf) {
        free(sigar->ifconf_buf);
    }
    if (sigar->self_path) {
        free(sigar->self_path);
    }
    if (sigar->proc_cpu) {
        sigar_cache_destroy(sigar->proc_cpu);
    }
    if (sigar->net_listen) {
        sigar_cache_destroy(sigar->net_listen);
    }

    return sigar_os_close(sigar);
}

#ifndef __linux__ /* linux has a special case */
SIGAR_DECLARE(sigar_pid_t) sigar_pid_get(sigar_t *sigar)
{
    if (!sigar->pid) {
        sigar->pid = getpid();
    }

    return sigar->pid;
}
#endif

/* XXX: add clear() function */
/* XXX: check for stale-ness using start_time */
SIGAR_DECLARE(int) sigar_proc_cpu_get(sigar_t *sigar, sigar_pid_t pid,
                                      sigar_proc_cpu_t *proccpu)
{
    sigar_cache_entry_t *entry;
    sigar_proc_cpu_t *prev;
    sigar_uint64_t otime, time_now = time(NULL) * 1000;
    sigar_int64_t time_diff, total_diff;
    int status;

    if (!sigar->proc_cpu) {
        sigar->proc_cpu = sigar_cache_new(128);
    }

    entry = sigar_cache_get(sigar->proc_cpu, pid);
    if (entry->value) {
        prev = (sigar_proc_cpu_t *)entry->value;
    }
    else {
        prev = entry->value = malloc(sizeof(*prev));
        SIGAR_ZERO(prev);
    }

    time_diff = time_now - prev->last_time;
    proccpu->last_time = prev->last_time = time_now;

    if (time_diff == 0) {
        /* we were just called within < 1 second ago. */
        memcpy(proccpu, prev, sizeof(*proccpu));
        return SIGAR_OK;
    }

    otime = prev->total;

    status =
        sigar_proc_time_get(sigar, pid,
                            (sigar_proc_time_t *)proccpu);

    if (status != SIGAR_OK) {
        return status;
    }

    memcpy(prev, proccpu, sizeof(*prev));

    if (otime == 0) {
        /* first time called */
        return SIGAR_OK;
    }

    total_diff = proccpu->total - otime;
    proccpu->percent = total_diff / (double)time_diff;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_proc_stat_get(sigar_t *sigar,
                                       sigar_proc_stat_t *procstat)
{
    int status = sigar_proc_count(sigar, &procstat->total);

    return status;
}

static char *sigar_error_string(int err)
{
    switch (err) {
      case SIGAR_ENOTIMPL:
        return "This function has not been implemented on this platform";
      default:
        return "Error string not specified yet";
    }
}

SIGAR_DECLARE(char *) sigar_strerror(sigar_t *sigar, int err)
{
    char *buf;

    if (err > SIGAR_OS_START_ERROR) {
        if ((buf = sigar_os_error_string(sigar, err)) != NULL) {
            return buf;
        }
        return "Unknown OS Error"; /* should never happen */
    }

    if (err > SIGAR_START_ERROR) {
        return sigar_error_string(err);
    }

    return sigar_strerror_get(err, sigar->errbuf, sizeof(sigar->errbuf));
}

char *sigar_strerror_get(int err, char *errbuf, int buflen)
{
    char *buf = NULL;
#ifdef WIN32
    DWORD len;

    len = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS,
                        NULL,
                        err,
                        0, /* default language */
                        (LPTSTR)errbuf,
                        (DWORD)buflen,
                        NULL);
#else

#if defined(HAVE_STRERROR_R) && defined(HAVE_STRERROR_R_GLIBC)
    /*
     * strerror_r man page says:
     * "The GNU version may, but need not, use the user supplied buffer"
     */
    buf = strerror_r(err, errbuf, buflen);
#elif defined(HAVE_STRERROR_R)
    if (strerror_r(err, errbuf, buflen) < 0) {
        buf = "Unknown Error";
    }
#else
    /* strerror() is thread safe on solaris and hpux */
    buf = strerror(err);
#endif

    if (buf != NULL) {
        SIGAR_STRNCPY(errbuf, buf, buflen);
    }
    
#endif
    return errbuf;
}

#include <stdio.h> /* for sprintf */

SIGAR_DECLARE(int) sigar_uptime_string(sigar_t *sigar, 
                                       sigar_uptime_t *uptime,
                                       char *buffer,
                                       int buflen)
{
    char *ptr = buffer;
    int minutes, hours, days, offset = 0;

    /* XXX: get rid of sprintf and/or check for overflow */
    days = uptime->uptime / (60*60*24);

    if (days) {
        offset += sprintf(ptr + offset, "%d day%s, ",
                          days, (days > 1) ? "s" : "");
    }

    minutes = (int)uptime->uptime / 60;
    hours = minutes / 60;
    hours = hours % 24;
    minutes = minutes % 60;

    if (hours) {
        offset += sprintf(ptr + offset, "%2d:%02d",
                          hours, minutes);
    }
    else {
        offset += sprintf(ptr + offset, "%d min", minutes);
    }

    return SIGAR_OK;
}

/* copy apr_strfsize */
SIGAR_DECLARE(char *) sigar_format_size(sigar_uint64_t size, char *buf)
{
    const char ord[] = "KMGTPE";
    const char *o = ord;
    int remain;

    if (size == SIGAR_FIELD_NOTIMPL) {
        buf[0] = '-';
        buf[1] = '\0';
        return buf;
    }

    if (size < 973) {
        sprintf(buf, "%3d ", (int) size);
        return buf;
    }

    do {
        remain = (int)(size & 1023);
        size >>= 10;

        if (size >= 973) {
            ++o;
            continue;
        }

        if (size < 9 || (size == 9 && remain < 973)) {
            if ((remain = ((remain * 5) + 256) / 512) >= 10) {
                ++size;
                remain = 0;
            }
            sprintf(buf, "%d.%d%c", (int) size, remain, *o);
            return buf;
        }

        if (remain >= 512) {
            ++size;
        }

        sprintf(buf, "%3d%c", (int) size, *o);

        return buf;
    } while (1);
}

SIGAR_DECLARE(int) sigar_sys_info_get(sigar_t *sigar,
                                      sigar_sys_info_t *sysinfo)
{
    SIGAR_ZERO(sysinfo);

#ifndef WIN32
    sigar_sys_info_get_uname(sysinfo);
#endif

    sigar_os_sys_info_get(sigar, sysinfo);

    return SIGAR_OK;
}

#ifndef WIN32

#include <sys/utsname.h>

int sigar_sys_info_get_uname(sigar_sys_info_t *sysinfo)
{
    struct utsname name;

    uname(&name);

    SIGAR_SSTRCPY(sysinfo->version, name.release);
    SIGAR_SSTRCPY(sysinfo->vendor_name, name.sysname);
    SIGAR_SSTRCPY(sysinfo->name, name.sysname);
    SIGAR_SSTRCPY(sysinfo->machine, name.machine);
    SIGAR_SSTRCPY(sysinfo->arch, name.machine);
    SIGAR_SSTRCPY(sysinfo->patch_level, "unknown");

    return SIGAR_OK;
}

#include <pwd.h>
#include <grp.h>

int sigar_user_name_get(sigar_t *sigar, int uid, char *buf, int buflen)
{
    struct passwd *pw = NULL;
    /* XXX cache lookup */

# ifdef HAVE_GETPWUID_R
    struct passwd pwbuf;
    char buffer[512];

    if (getpwuid_r(uid, &pwbuf, buffer, sizeof(buffer), &pw) != 0) {
        return errno;
    }
    if (!pw) {
        return ENOENT;
    }
# else
    if ((pw = getpwuid(uid)) == NULL) {
        return errno;
    }
# endif

    strncpy(buf, pw->pw_name, buflen);
    buf[buflen-1] = '\0';

    return SIGAR_OK;
}

int sigar_group_name_get(sigar_t *sigar, int gid, char *buf, int buflen)
{
    struct group *gr;
    /* XXX cache lookup */

# ifdef HAVE_GETGRGID_R
    struct group grbuf;
    char buffer[512];

    if (getgrgid_r(gid, &grbuf, buffer, sizeof(buffer), &gr) != 0) {
        return errno;
    }
# else
    if ((gr = getgrgid(gid)) == NULL) {
        return errno;
    }
# endif

    if (gr && gr->gr_name) {
        strncpy(buf, gr->gr_name, buflen);
    }
    else {
        /* seen on linux.. apache httpd.conf has:
         * Group #-1
         * results in uid == -1 and gr == NULL.
         * wtf getgrgid_r doesnt fail instead? 
         */
        sprintf(buf, "%d", gid);
    }
    buf[buflen-1] = '\0';

    return SIGAR_OK;
}

int sigar_user_id_get(sigar_t *sigar, const char *name, int *uid)
{
    /* XXX cache lookup */
    struct passwd *pw;

# ifdef HAVE_GETPWNAM_R
    struct passwd pwbuf;
    char buf[512];

    if (getpwnam_r(name, &pwbuf, buf, sizeof(buf), &pw) != 0) {
        return errno;
    }
# else
    if (!(pw = getpwnam(name))) {
        return errno;
    }
# endif

    *uid = (int)pw->pw_uid;
    return SIGAR_OK;
}

SIGAR_DECLARE(int)
sigar_proc_cred_name_get(sigar_t *sigar, sigar_pid_t pid,
                         sigar_proc_cred_name_t *proccredname)
{
    sigar_proc_cred_t cred;

    int status = sigar_proc_cred_get(sigar, pid, &cred);

    if (status != SIGAR_OK) {
        return status;
    }

    status = sigar_user_name_get(sigar, cred.uid,
                                 proccredname->user,
                                 sizeof(proccredname->user));

    if (status != SIGAR_OK) {
        return status;
    }

    status = sigar_group_name_get(sigar, cred.gid,
                                  proccredname->group,
                                  sizeof(proccredname->group));

    return status;
}

#endif /* WIN32 */

int sigar_proc_list_create(sigar_proc_list_t *proclist)
{
    proclist->number = 0;
    proclist->size = SIGAR_PROC_LIST_MAX;
    proclist->data = malloc(sizeof(*(proclist->data)) *
                            proclist->size);
    return SIGAR_OK;
}

int sigar_proc_list_grow(sigar_proc_list_t *proclist)
{
    proclist->data = realloc(proclist->data,
                             sizeof(*(proclist->data)) *
                             (proclist->size + SIGAR_PROC_LIST_MAX));
    proclist->size += SIGAR_PROC_LIST_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_proc_list_destroy(sigar_t *sigar,
                                           sigar_proc_list_t *proclist)
{
    if (proclist->size) {
        free(proclist->data);
        proclist->number = proclist->size = 0;
    }

    return SIGAR_OK;
}

int sigar_proc_args_create(sigar_proc_args_t *procargs)
{
    procargs->number = 0;
    procargs->size = SIGAR_PROC_ARGS_MAX;
    procargs->data = malloc(sizeof(*(procargs->data)) *
                            procargs->size);
    return SIGAR_OK;
}

int sigar_proc_args_grow(sigar_proc_args_t *procargs)
{
    procargs->data = realloc(procargs->data,
                             sizeof(*(procargs->data)) *
                             (procargs->size + SIGAR_PROC_ARGS_MAX));
    procargs->size += SIGAR_PROC_ARGS_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_proc_args_destroy(sigar_t *sigar,
                                           sigar_proc_args_t *procargs)
{
    unsigned int i;

    if (procargs->size) {
        for (i=0; i<procargs->number; i++) {
            free(procargs->data[i]);
        }
        free(procargs->data);
        procargs->number = procargs->size = 0;
    }

    return SIGAR_OK;
}

int sigar_file_system_list_create(sigar_file_system_list_t *fslist)
{
    fslist->number = 0;
    fslist->size = SIGAR_FS_MAX;
    fslist->data = malloc(sizeof(*(fslist->data)) *
                          fslist->size);
    return SIGAR_OK;
}

int sigar_file_system_list_grow(sigar_file_system_list_t *fslist)
{
    fslist->data = realloc(fslist->data,
                           sizeof(*(fslist->data)) *
                           (fslist->size + SIGAR_FS_MAX));
    fslist->size += SIGAR_FS_MAX;

    return SIGAR_OK;
}

/* indexed with sigar_file_system_type_e */
static const char *fstype_names[] = {
    "unknown", "none", "local", "remote", "ram", "cdrom", "swap"
};

static int sigar_common_fs_type_get(sigar_file_system_t *fsp)
{
    char *type = fsp->sys_type_name;

    switch (*type) {
      case 'n':
        if (strEQ(type, "nfs")) {
            fsp->type = SIGAR_FSTYPE_NETWORK;
        }
        break;
      case 's':
        if (strEQ(type, "smbfs")) { /* samba */
            fsp->type = SIGAR_FSTYPE_NETWORK;
        }
        else if (strEQ(type, "swap")) {
            fsp->type = SIGAR_FSTYPE_SWAP;
        }
        break;
      case 'a':
        if (strEQ(type, "afs")) {
            fsp->type = SIGAR_FSTYPE_NETWORK;
        }
        break;
      case 'i':
        if (strEQ(type, "iso9660")) {
            fsp->type = SIGAR_FSTYPE_CDROM;
        }
        break;
      case 'm':
        if (strEQ(type, "msdos") || strEQ(type, "minix")) {
            fsp->type = SIGAR_FSTYPE_LOCAL_DISK;
        }
        break;
      case 'h':
        if (strEQ(type, "hpfs")) {
            fsp->type = SIGAR_FSTYPE_LOCAL_DISK;
        }
        break;
      case 'v':
        if (strEQ(type, "vfat")) {
            fsp->type = SIGAR_FSTYPE_LOCAL_DISK;
        }
        break;
    }

    return fsp->type;
}

void sigar_fs_type_get(sigar_file_system_t *fsp)
{
    if (!(fsp->type ||                    /* already set */
          sigar_os_fs_type_get(fsp) ||    /* try os specifics first */
          sigar_common_fs_type_get(fsp))) /* try common ones last */
    {
        fsp->type = SIGAR_FSTYPE_NONE;
    }

    if (fsp->type >= SIGAR_FSTYPE_MAX) {
        fsp->type = SIGAR_FSTYPE_NONE;
    }

    strcpy(fsp->type_name, fstype_names[fsp->type]);
}


SIGAR_DECLARE(int)
sigar_file_system_list_destroy(sigar_t *sigar,
                               sigar_file_system_list_t *fslist)
{
    if (fslist->size) {
        free(fslist->data);
        fslist->number = fslist->size = 0;
    }

    return SIGAR_OK;
}

int sigar_cpu_info_list_create(sigar_cpu_info_list_t *cpu_infos)
{
    cpu_infos->number = 0;
    cpu_infos->size = SIGAR_CPU_INFO_MAX;
    cpu_infos->data = malloc(sizeof(*(cpu_infos->data)) *
                             cpu_infos->size);
    return SIGAR_OK;
}

int sigar_cpu_info_list_grow(sigar_cpu_info_list_t *cpu_infos)
{
    cpu_infos->data = realloc(cpu_infos->data,
                              sizeof(*(cpu_infos->data)) *
                              (cpu_infos->size + SIGAR_CPU_INFO_MAX));
    cpu_infos->size += SIGAR_CPU_INFO_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int)
sigar_cpu_info_list_destroy(sigar_t *sigar,
                            sigar_cpu_info_list_t *cpu_infos)
{
    if (cpu_infos->size) {
        free(cpu_infos->data);
        cpu_infos->number = cpu_infos->size = 0;
    }

    return SIGAR_OK;
}

int sigar_cpu_list_create(sigar_cpu_list_t *cpulist)
{
    cpulist->number = 0;
    cpulist->size = SIGAR_CPU_INFO_MAX;
    cpulist->data = malloc(sizeof(*(cpulist->data)) *
                           cpulist->size);
    return SIGAR_OK;
}

int sigar_cpu_list_grow(sigar_cpu_list_t *cpulist)
{
    cpulist->data = realloc(cpulist->data,
                            sizeof(*(cpulist->data)) *
                            (cpulist->size + SIGAR_CPU_INFO_MAX));
    cpulist->size += SIGAR_CPU_INFO_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_cpu_list_destroy(sigar_t *sigar,
                                          sigar_cpu_list_t *cpulist)
{
    if (cpulist->size) {
        free(cpulist->data);
        cpulist->number = cpulist->size = 0;
    }

    return SIGAR_OK;
}

int sigar_net_route_list_create(sigar_net_route_list_t *routelist)
{
    routelist->number = 0;
    routelist->size = SIGAR_NET_ROUTE_LIST_MAX;
    routelist->data = malloc(sizeof(*(routelist->data)) *
                             routelist->size);
    return SIGAR_OK;
}

int sigar_net_route_list_grow(sigar_net_route_list_t *routelist)
{
    routelist->data =
        realloc(routelist->data,
                sizeof(*(routelist->data)) *
                (routelist->size + SIGAR_NET_ROUTE_LIST_MAX));
    routelist->size += SIGAR_NET_ROUTE_LIST_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_net_route_list_destroy(sigar_t *sigar,
                                                sigar_net_route_list_t *routelist)
{
    if (routelist->size) {
        free(routelist->data);
        routelist->number = routelist->size = 0;
    }

    return SIGAR_OK;
}

int sigar_net_interface_list_create(sigar_net_interface_list_t *iflist)
{
    iflist->number = 0;
    iflist->size = SIGAR_NET_IFLIST_MAX;
    iflist->data = malloc(sizeof(*(iflist->data)) *
                          iflist->size);
    return SIGAR_OK;
}

int sigar_net_interface_list_grow(sigar_net_interface_list_t *iflist)
{
    iflist->data = realloc(iflist->data,
                           sizeof(*(iflist->data)) *
                           (iflist->size + SIGAR_NET_IFLIST_MAX));
    iflist->size += SIGAR_NET_IFLIST_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int)
sigar_net_interface_list_destroy(sigar_t *sigar,
                                 sigar_net_interface_list_t *iflist)
{
    unsigned int i;

    if (iflist->size) {
        for (i=0; i<iflist->number; i++) {
            free(iflist->data[i]);
        }
        free(iflist->data);
        iflist->number = iflist->size = 0;
    }

    return SIGAR_OK;
}

int sigar_net_connection_list_create(sigar_net_connection_list_t *connlist)
{
    connlist->number = 0;
    connlist->size = SIGAR_NET_CONNLIST_MAX;
    connlist->data = malloc(sizeof(*(connlist->data)) *
                            connlist->size);
    return SIGAR_OK;
}

int sigar_net_connection_list_grow(sigar_net_connection_list_t *connlist)
{
    connlist->data =
        realloc(connlist->data,
                sizeof(*(connlist->data)) *
                (connlist->size + SIGAR_NET_CONNLIST_MAX));
    connlist->size += SIGAR_NET_CONNLIST_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int)
sigar_net_connection_list_destroy(sigar_t *sigar,
                                  sigar_net_connection_list_t *connlist)
{
    if (connlist->size) {
        free(connlist->data);
        connlist->number = connlist->size = 0;
    }

    return SIGAR_OK;
}

SIGAR_DECLARE(const char *)sigar_net_connection_type_get(int type)
{
    switch (type) {
      case SIGAR_NETCONN_TCP:
        return "tcp";
      case SIGAR_NETCONN_UDP:
        return "udp";
      case SIGAR_NETCONN_RAW:
        return "raw";
      case SIGAR_NETCONN_UNIX:
        return "unix";
      default:
        return "unknown";
    }
}

SIGAR_DECLARE(const char *)sigar_net_connection_state_get(int state)
{
    switch (state) {
      case SIGAR_TCP_ESTABLISHED:
        return "ESTABLISHED";
      case SIGAR_TCP_SYN_SENT:
        return "SYN_SENT";
      case SIGAR_TCP_SYN_RECV:
        return "SYN_RECV";
      case SIGAR_TCP_FIN_WAIT1:
        return "FIN_WAIT1";
      case SIGAR_TCP_FIN_WAIT2:
        return "FIN_WAIT2";
      case SIGAR_TCP_TIME_WAIT:
        return "TIME_WAIT";
      case SIGAR_TCP_CLOSE:
        return "CLOSE";
      case SIGAR_TCP_CLOSE_WAIT:
        return "CLOSE_WAIT";
      case SIGAR_TCP_LAST_ACK:
        return "LAST_ACK";
      case SIGAR_TCP_LISTEN:
        return "LISTEN";
      case SIGAR_TCP_CLOSING:
        return "CLOSING";
      case SIGAR_TCP_IDLE:
        return "IDLE";
      case SIGAR_TCP_BOUND:
        return "BOUND";
      case SIGAR_TCP_UNKNOWN:
      default:
        return "UNKNOWN";
    }
}

#if !defined(__linux__)
/* 
 * implement sigar_net_connection_list_get using sigar_net_connection_walk
 * linux has its own list_get impl.
 */  
static int net_connection_list_walker(sigar_net_connection_walker_t *walker,
                                      sigar_net_connection_t *conn)
{
    sigar_net_connection_list_t *connlist =
        (sigar_net_connection_list_t *)walker->data;

    SIGAR_NET_CONNLIST_GROW(connlist);
    memcpy(&connlist->data[connlist->number++],
           conn, sizeof(*conn));

    return SIGAR_OK; /* continue loop */
}

SIGAR_DECLARE(int)
sigar_net_connection_list_get(sigar_t *sigar,
                              sigar_net_connection_list_t *connlist,
                              int flags)
{
    int status;
    sigar_net_connection_walker_t walker;

    sigar_net_connection_list_create(connlist);

    walker.sigar = sigar;
    walker.flags = flags;
    walker.data = connlist;
    walker.add_connection = net_connection_list_walker;

    status = sigar_net_connection_walk(&walker);

    if (status != SIGAR_OK) {
        sigar_net_connection_list_destroy(sigar, connlist);
    }

    return status;
}
#endif

static void sigar_net_listen_address_add(sigar_t *sigar,
                                         sigar_net_connection_t *conn)
{
    sigar_cache_entry_t *entry =
        sigar_cache_get(sigar->net_listen, conn->local_port);

    if (entry->value) {
        if (conn->local_address.family == SIGAR_AF_INET6) {
            return; /* prefer ipv4 */
        }
    }
    else {
        entry->value = malloc(sizeof(conn->local_address));
    }

    memcpy(entry->value, &conn->local_address,
           sizeof(conn->local_address));
}

SIGAR_DECLARE(int)
sigar_net_listen_address_get(sigar_t *sigar,
                             unsigned long port,
                             sigar_net_address_t *address)
{
    if (!sigar->net_listen ||
        !sigar_cache_find(sigar->net_listen, port))
    {
        sigar_net_stat_t netstat;
        int status =
            sigar_net_stat_get(sigar, &netstat,
                               SIGAR_NETCONN_SERVER|SIGAR_NETCONN_TCP);

        if (status != SIGAR_OK) {
            return status;
        }
    }

    if (sigar_cache_find(sigar->net_listen, port)) {
        void *value = sigar_cache_get(sigar->net_listen, port)->value;
        memcpy(address, value, sizeof(*address));
        return SIGAR_OK;
    }
    else {
        return ENOENT;
    }
}

typedef struct {
    sigar_net_stat_t *netstat;
    sigar_net_connection_list_t *connlist;
} net_stat_getter_t;

static int net_stat_walker(sigar_net_connection_walker_t *walker,
                           sigar_net_connection_t *conn)
{
    int state = conn->state;
    sigar_cache_t *listen_ports = walker->sigar->net_listen;
    net_stat_getter_t *getter =
        (net_stat_getter_t *)walker->data;

    if (conn->type == SIGAR_NETCONN_TCP) {
        getter->netstat->tcp_states[state]++;

        /* XXX listen_ports may get stale */
        if (state == SIGAR_TCP_LISTEN) {
            sigar_net_listen_address_add(walker->sigar, conn);
        }
        else {
            if (sigar_cache_find(listen_ports,
                                 conn->local_port))
            {
                getter->netstat->tcp_inbound_total++;
            }
            else {
                getter->netstat->tcp_outbound_total++;
            }
        }
    }
    else if (conn->type == SIGAR_NETCONN_UDP) {
        /*XXX*/
    }

    getter->netstat->all_inbound_total =
        getter->netstat->tcp_inbound_total;

    getter->netstat->all_outbound_total =
        getter->netstat->tcp_outbound_total;

    return SIGAR_OK;
}

SIGAR_DECLARE(int)
sigar_net_stat_get(sigar_t *sigar,
                   sigar_net_stat_t *netstat,
                   int flags)
{
    sigar_net_connection_walker_t walker;
    net_stat_getter_t getter;

    if (!sigar->net_listen) {
        sigar->net_listen = sigar_cache_new(32);
    }
    
    SIGAR_ZERO(netstat);

    getter.netstat = netstat;

    walker.sigar = sigar;
    walker.data = &getter;
    walker.add_connection = net_stat_walker;

    walker.flags = flags;

    return sigar_net_connection_walk(&walker);
}

typedef struct {
    sigar_net_stat_t *netstat;
    sigar_net_address_t *address;
    unsigned long port;
} net_stat_port_getter_t;

static int net_stat_port_walker(sigar_net_connection_walker_t *walker,
                                sigar_net_connection_t *conn)
{
    net_stat_port_getter_t *getter =
        (net_stat_port_getter_t *)walker->data;
    sigar_net_stat_t *netstat = getter->netstat;

    if (conn->type == SIGAR_NETCONN_TCP) {
        if (conn->local_port == getter->port) {
            netstat->all_inbound_total++;

            if (sigar_net_address_equals(getter->address,
                                         &conn->local_address) == SIGAR_OK)
            {
                netstat->tcp_inbound_total++;
            }
        }
        else if (conn->remote_port == getter->port) {
            netstat->all_outbound_total++;

            if (sigar_net_address_equals(getter->address,
                                         &conn->remote_address) == SIGAR_OK)
            {
                netstat->tcp_outbound_total++;
            }
        }
        else {
            return SIGAR_OK;
        }

        netstat->tcp_states[conn->state]++;
    }
    else if (conn->type == SIGAR_NETCONN_UDP) {
        /*XXX*/
    }

    return SIGAR_OK;
}

SIGAR_DECLARE(int)
sigar_net_stat_port_get(sigar_t *sigar,
                        sigar_net_stat_t *netstat,
                        int flags,
                        sigar_net_address_t *address,
                        unsigned long port)
{
    sigar_net_connection_walker_t walker;
    net_stat_port_getter_t getter;

    SIGAR_ZERO(netstat);

    getter.netstat = netstat;
    getter.address = address;
    getter.port = port;

    walker.sigar = sigar;
    walker.data = &getter;
    walker.add_connection = net_stat_port_walker;

    walker.flags = flags;

    if (SIGAR_LOG_IS_DEBUG(sigar)) {
        char name[SIGAR_FQDN_LEN];
        sigar_net_address_to_string(sigar, address, name);

        sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                         "[net_stat_port] using address '%s:%d'",
                         name, port);
    }

    return sigar_net_connection_walk(&walker);
}

int sigar_who_list_create(sigar_who_list_t *wholist)
{
    wholist->number = 0;
    wholist->size = SIGAR_WHO_LIST_MAX;
    wholist->data = malloc(sizeof(*(wholist->data)) *
                           wholist->size);
    return SIGAR_OK;
}

int sigar_who_list_grow(sigar_who_list_t *wholist)
{
    wholist->data = realloc(wholist->data,
                            sizeof(*(wholist->data)) *
                            (wholist->size + SIGAR_WHO_LIST_MAX));
    wholist->size += SIGAR_WHO_LIST_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_who_list_destroy(sigar_t *sigar,
                                          sigar_who_list_t *wholist)
{
    if (wholist->size) {
        free(wholist->data);
        wholist->number = wholist->size = 0;
    }

    return SIGAR_OK;
}

#if defined(__sun)
#  include <utmpx.h>
#  define SIGAR_UTMP_FILE _UTMPX_FILE
#  define ut_time ut_tv.tv_sec
#elif defined(WIN32)
/* XXX may not be the default */
#define SIGAR_UTMP_FILE "C:\\cygwin\\var\\run\\utmp"
#define UT_LINESIZE	16
#define UT_NAMESIZE	16
#define UT_HOSTSIZE	256
#define UT_IDLEN	2
#define ut_name ut_user

struct utmp {
    short ut_type;	
    int ut_pid;		
    char ut_line[UT_LINESIZE];
    char ut_id[UT_IDLEN];
    time_t ut_time;	
    char ut_user[UT_NAMESIZE];	
    char ut_host[UT_HOSTSIZE];	
    long ut_addr;	
};
#elif defined(NETWARE)
static char *getpass(const char *prompt)
{
    static char password[BUFSIZ];

    fputs(prompt, stderr);
    fgets((char *)&password, sizeof(password), stdin);

    return (char *)&password;
}
#else
#  include <utmp.h>
#  ifdef UTMP_FILE
#    define SIGAR_UTMP_FILE UTMP_FILE
#  else
#    define SIGAR_UTMP_FILE _PATH_UTMP
#  endif
#endif

#if defined(__FreeBSD__) || defined(DARWIN)
#  define ut_user ut_name
#endif

#if !defined(NETWARE) && !defined(_AIX)

#define WHOCPY(dest, src) \
    SIGAR_SSTRCPY(dest, src); \
    if (sizeof(src) < sizeof(dest)) \
        dest[sizeof(src)] = '\0'

static int sigar_who_utmp(sigar_t *sigar,
                          sigar_who_list_t *wholist)
{
    FILE *fp;
#ifdef __sun
    /* use futmpx w/ pid32_t for sparc64 */
    struct futmpx ut;
#else
    struct utmp ut;
#endif
    if (!(fp = fopen(SIGAR_UTMP_FILE, "r"))) {
        return errno;
    }

    while (fread(&ut, sizeof(ut), 1, fp) == 1) {
        sigar_who_t *who;

        if (*ut.ut_name == '\0') {
            continue;
        }

#ifdef USER_PROCESS
        if (ut.ut_type != USER_PROCESS) {
            continue;
        }
#endif

        SIGAR_WHO_LIST_GROW(wholist);
        who = &wholist->data[wholist->number++];

        WHOCPY(who->user, ut.ut_user);
        WHOCPY(who->device, ut.ut_line);
        WHOCPY(who->host, ut.ut_host);

        who->time = ut.ut_time;
    }

    fclose(fp);

    return SIGAR_OK;
}

#endif /* NETWARE */

#if defined(WIN32)

int sigar_who_list_get_win32(sigar_t *sigar,
                             sigar_who_list_t *wholist);

SIGAR_DECLARE(int) sigar_who_list_get(sigar_t *sigar,
                                      sigar_who_list_t *wholist)
{
    sigar_who_list_create(wholist);

    /* cygwin ssh */
    sigar_who_utmp(sigar, wholist);

    sigar_who_list_get_win32(sigar, wholist);

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_resource_limit_get(sigar_t *sigar,
                                            sigar_resource_limit_t *rlimit)
{
    MEMORY_BASIC_INFORMATION meminfo;
    memset(rlimit, 0x7fffffff, sizeof(*rlimit));

    if (VirtualQuery((LPCVOID)&meminfo, &meminfo, sizeof(meminfo))) {
        rlimit->stack_cur =
            (DWORD)&meminfo - (DWORD)meminfo.AllocationBase;
        rlimit->stack_max =
            ((DWORD)meminfo.BaseAddress + meminfo.RegionSize) -
            (DWORD)meminfo.AllocationBase;
    }

    rlimit->virtual_memory_max = rlimit->virtual_memory_cur =
        0x80000000UL;

    return SIGAR_OK;
}

#elif defined(NETWARE)
int sigar_resource_limit_get(sigar_t *sigar,
                             sigar_resource_limit_t *rlimit)
{
    return SIGAR_ENOTIMPL;
}

int sigar_who_list_get(sigar_t *sigar,
                       sigar_who_list_t *wholist)
{
    return SIGAR_ENOTIMPL;
}
#else

#ifndef _AIX
int sigar_who_list_get(sigar_t *sigar,
                       sigar_who_list_t *wholist)
{
    int status;

    sigar_who_list_create(wholist);

    status = sigar_who_utmp(sigar, wholist);
    if (status != SIGAR_OK) {
        sigar_who_list_destroy(sigar, wholist);
        return status;
    }

    return SIGAR_OK;
}
#endif

static int sigar_get_default_gateway(sigar_t *sigar,
                                     char *gateway)
{
    int status, i;
    sigar_net_route_list_t routelist;

    status = sigar_net_route_list_get(sigar, &routelist);
    if (status != SIGAR_OK) {
        return status;
    }

    for (i=0; i<routelist.number; i++) {
        if ((routelist.data[i].flags & SIGAR_RTF_GATEWAY) &&
            (routelist.data[i].destination.addr.in == 0))
        {
            sigar_net_address_to_string(sigar,
                                        &routelist.data[i].gateway,
                                        gateway);
            break;
        }
    }

    sigar_net_route_list_destroy(sigar, &routelist);

    return SIGAR_OK;
}

int sigar_net_info_get(sigar_t *sigar,
                       sigar_net_info_t *netinfo)
{
    int size;
    char buffer[BUFSIZ], *ptr;
    FILE *fp;

    SIGAR_ZERO(netinfo);

    if ((fp = fopen("/etc/resolv.conf", "r"))) {
        while ((ptr = fgets(buffer, sizeof(buffer), fp))) {
            int len;

            SIGAR_SKIP_SPACE(ptr);
            if (!(ptr = strstr(ptr, "nameserver"))) {
                continue;
            }
            ptr += 10;
            SIGAR_SKIP_SPACE(ptr);

            len = strlen(ptr);
            ptr[len-1] = '\0'; /* chop \n */

            if (!netinfo->primary_dns[0]) {
                SIGAR_SSTRCPY(netinfo->primary_dns, ptr);
            }
            else if (!netinfo->secondary_dns[0]) {
                SIGAR_SSTRCPY(netinfo->secondary_dns, ptr);
            }
            else {
                break;
            }
        }
        fclose(fp);
    } /* else /etc/resolv.conf may not exist if unplugged (MacOSX) */

    size = sizeof(netinfo->host_name)-1;
    if (gethostname(netinfo->host_name, size) == 0) {
        netinfo->host_name[size] = '\0';
    }
    else {
        netinfo->host_name[0] = '\0';
    }

    size = sizeof(netinfo->domain_name)-1;
    if (getdomainname(netinfo->domain_name, size) == 0) {
        netinfo->domain_name[size] = '\0';
    }
    else {
        netinfo->domain_name[0] = '\0';
    }

    sigar_get_default_gateway(sigar, netinfo->default_gateway);

    return SIGAR_OK;
}

#include <sys/resource.h>

#define OffsetOf(structure, field) \
   (size_t)(&((structure *)NULL)->field)

#define RlimitOffsets(field) \
    OffsetOf(sigar_resource_limit_t, field##_cur), \
    OffsetOf(sigar_resource_limit_t, field##_max)

#define RlimitSet(structure, ptr, val) \
    *(sigar_uint64_t *)((char *)structure + (int)(long)ptr) = val

typedef struct {
    int resource;
    int factor;
    size_t cur;
    size_t max;
} rlimit_field_t;

#ifndef RLIMIT_RSS
#define RLIMIT_RSS (RLIM_NLIMITS+1)
#endif

#ifndef RLIMIT_NPROC
#define RLIMIT_NPROC (RLIM_NLIMITS+2)
#endif

#define RLIMIT_PSIZE (RLIM_NLIMITS+3)

#ifndef RLIMIT_AS
#define RLIMIT_AS RLIMIT_VMEM
#endif

static rlimit_field_t sigar_rlimits[] = {
    { RLIMIT_CPU,    1,    RlimitOffsets(cpu) },
    { RLIMIT_FSIZE,  1024, RlimitOffsets(file_size) },
    { RLIMIT_DATA,   1024, RlimitOffsets(data) },
    { RLIMIT_STACK,  1024, RlimitOffsets(stack) },
    { RLIMIT_PSIZE,   512, RlimitOffsets(pipe_size) },
    { RLIMIT_CORE,   1024, RlimitOffsets(core) },
    { RLIMIT_RSS,    1024, RlimitOffsets(memory) },
    { RLIMIT_NPROC,  1,    RlimitOffsets(processes) },
    { RLIMIT_NOFILE, 1,    RlimitOffsets(open_files) },
    { RLIMIT_AS,     1024, RlimitOffsets(virtual_memory) },
    { -1 }
};

#define RlimitScale(val) \
    if (val != RLIM_INFINITY) val /= r->factor

#define RlimitHS(val) \
    rl.rlim_cur = rl.rlim_max = (val)

int sigar_resource_limit_get(sigar_t *sigar,
                             sigar_resource_limit_t *rlimit)
{
    int i;

    for (i=0; sigar_rlimits[i].resource != -1; i++) {
        struct rlimit rl;
        rlimit_field_t *r = &sigar_rlimits[i];

        if (r->resource > RLIM_NLIMITS) {
            switch (r->resource) {
              case RLIMIT_NPROC:
                RlimitHS(sysconf(_SC_CHILD_MAX));
                break;
              case RLIMIT_PSIZE:
                RlimitHS(PIPE_BUF/512);
                break;
              default:
                RlimitHS(RLIM_INFINITY);
                break;
            }
        }
        else if (getrlimit(r->resource, &rl) != 0) {
            RlimitHS(RLIM_INFINITY);
        }
        else {
            RlimitScale(rl.rlim_cur);
            RlimitScale(rl.rlim_max);
        }

        RlimitSet(rlimit, r->cur, rl.rlim_cur);
        RlimitSet(rlimit, r->max, rl.rlim_max);
    }

    return SIGAR_OK;
}
#endif

#if !defined(WIN32) && !defined(DARWIN) && !defined(__FreeBSD__) && !defined(NETWARE)

/* XXX: prolly will be moving these stuffs into os_net.c */
#include <sys/ioctl.h>
#include <net/if.h>

#ifndef SIOCGIFCONF
#include <sys/sockio.h>
#endif

#if defined(_AIX) || defined(__osf__) /* good buddies */

#include <net/if_dl.h>

static void hwaddr_aix_lookup(sigar_t *sigar, sigar_net_interface_config_t *ifconfig)
{
    char *ent, *end;
    struct ifreq *ifr;

    /* XXX: assumes sigar_net_interface_list_get has been called */
    end = sigar->ifconf_buf + sigar->ifconf_len;

    for (ent = sigar->ifconf_buf;
         ent < end;
         ent += sizeof(*ifr))
    {
        ifr = (struct ifreq *)ent;

        if (ifr->ifr_addr.sa_family != AF_LINK) {
            continue;
        }

        if (strEQ(ifr->ifr_name, ifconfig->name)) {
            struct sockaddr_dl *sdl = (struct sockaddr_dl *)&ifr->ifr_addr;

            sigar_net_address_mac_set(ifconfig->hwaddr,
                                      LLADDR(sdl),
                                      sdl->sdl_alen);
            return;
        }
    }

    sigar_hwaddr_set_null(ifconfig);
}

#elif !defined(SIOCGIFHWADDR)

#include <net/if_arp.h>

static void hwaddr_arp_lookup(sigar_net_interface_config_t *ifconfig, int sock)
{
    struct arpreq areq;
    struct sockaddr_in *sa;

    memset(&areq, 0, sizeof(areq));
    sa = (struct sockaddr_in *)&areq.arp_pa;
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = ifconfig->address.addr.in;
    
    if (ioctl(sock, SIOCGARP, &areq) < 0) {
        /* ho-hum */
        sigar_hwaddr_set_null(ifconfig);
    }
    else {
        sigar_net_address_mac_set(ifconfig->hwaddr,
                                  areq.arp_ha.sa_data,
                                  SIGAR_IFHWADDRLEN);
    }
}

#endif

#ifdef __linux__

#include <net/if_arp.h>

static void get_interface_type(sigar_net_interface_config_t *ifconfig,
                               int family)
{
    char *type;

    switch (family) {
      case ARPHRD_NETROM:
        type = SIGAR_NIC_NETROM;
        break;
        /* XXX more */
      default:
        type = SIGAR_NIC_ETHERNET;
        break;
    }

    SIGAR_SSTRCPY(ifconfig->type, type);
}

#endif

int sigar_net_interface_config_get(sigar_t *sigar, const char *name,
                                   sigar_net_interface_config_t *ifconfig)
{
    int sock;
    struct ifreq ifr;

    if (!name) {
        return sigar_net_interface_config_primary_get(sigar, ifconfig);
    }

    SIGAR_ZERO(ifconfig);

    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        return errno;
    }

    SIGAR_SSTRCPY(ifconfig->name, name);
    SIGAR_SSTRCPY(ifr.ifr_name, name);

#define ifr_s_addr(ifr) \
    ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr

    if (!ioctl(sock, SIOCGIFADDR, &ifr)) {
        sigar_net_address_set(ifconfig->address,
                              ifr_s_addr(ifr));
    }

    if (!ioctl(sock, SIOCGIFNETMASK, &ifr)) {
        sigar_net_address_set(ifconfig->netmask,
                              ifr_s_addr(ifr));
    }
    
    if (!ioctl(sock, SIOCGIFFLAGS, &ifr)) {
        sigar_uint64_t flags = ifr.ifr_flags;
#ifdef __linux__
        int is_mcast = flags & IFF_MULTICAST;
        int is_slave = flags & IFF_SLAVE;
        /*
         * XXX: should just define SIGAR_IFF_*
         * and test IFF_* bits on given platform.
         * this is the only diff between solaris/hpux/linux
         * for the flags we care about.
         *
         */
        flags &= ~(IFF_MULTICAST|IFF_SLAVE);
        if (is_mcast) {
            flags |= SIGAR_IFF_MULTICAST;
        }
        if (is_slave) {
            flags |= SIGAR_IFF_SLAVE;
        }
#endif
        ifconfig->flags = flags;
    }
    else {
        /* should always be able to get flags for existing device */
        /* other ioctls may fail if device is not enabled: ok */
        close(sock);
        return errno;
    }

    if (ifconfig->flags & IFF_LOOPBACK) {
        sigar_net_address_set(ifconfig->destination,
                              ifconfig->address.addr.in);
        sigar_net_address_set(ifconfig->broadcast, 0);
        sigar_hwaddr_set_null(ifconfig);
        SIGAR_SSTRCPY(ifconfig->type,
                      SIGAR_NIC_LOOPBACK);
    }
    else {
        if (!ioctl(sock, SIOCGIFDSTADDR, &ifr)) {
            sigar_net_address_set(ifconfig->destination,
                                  ifr_s_addr(ifr));
        }

        if (!ioctl(sock, SIOCGIFBRDADDR, &ifr)) {
            sigar_net_address_set(ifconfig->broadcast,
                                  ifr_s_addr(ifr));
        }

#if defined(SIOCGIFHWADDR)
        if (!ioctl(sock, SIOCGIFHWADDR, &ifr)) {
            get_interface_type(ifconfig,
                               ifr.ifr_hwaddr.sa_family);
            sigar_net_address_mac_set(ifconfig->hwaddr,
                                      ifr.ifr_hwaddr.sa_data,
                                      IFHWADDRLEN);
        }
#elif defined(_AIX) || defined(__osf__)
        hwaddr_aix_lookup(sigar, ifconfig);
        SIGAR_SSTRCPY(ifconfig->type,
                      SIGAR_NIC_ETHERNET);
#else
        hwaddr_arp_lookup(ifconfig, sock);
        SIGAR_SSTRCPY(ifconfig->type,
                      SIGAR_NIC_ETHERNET);
#endif
    }

#if defined(SIOCGLIFMTU) && !defined(__hpux)
    {
        struct lifreq lifr;
        SIGAR_SSTRCPY(lifr.lifr_name, name);
        if(!ioctl(sock, SIOCGLIFMTU, &lifr)) {
            ifconfig->mtu = lifr.lifr_mtu;
        }
    }
#elif defined(SIOCGIFMTU)
    if (!ioctl(sock, SIOCGIFMTU, &ifr)) {
#  if defined(__hpux)
        ifconfig->mtu = ifr.ifr_metric;
#  else
        ifconfig->mtu = ifr.ifr_mtu;
#endif
    }
#else
    ifconfig->mtu = 0; /*XXX*/
#endif
    
    if (!ioctl(sock, SIOCGIFMETRIC, &ifr)) {
        ifconfig->metric = ifr.ifr_metric ? ifr.ifr_metric : 1;
    }

    close(sock);    

    /* XXX can we get a better description like win32? */
    SIGAR_SSTRCPY(ifconfig->description,
                  ifconfig->name);

    return SIGAR_OK;
}

#ifdef _AIX
#  define MY_SIOCGIFCONF CSIOCGIFCONF
#else
#  define MY_SIOCGIFCONF SIOCGIFCONF
#endif

#ifdef __osf__
static int sigar_netif_configured(sigar_t *sigar, char *name)
{
    int status;
    sigar_net_interface_config_t ifconfig;

    status = sigar_net_interface_config_get(sigar, name, &ifconfig);

    return status == SIGAR_OK;
}
#endif

#ifdef __linux__
static SIGAR_INLINE int has_interface(sigar_net_interface_list_t *iflist,
                                      char *name)
{
    register int i;
    register int num = iflist->number;
    register char **data = iflist->data;
    for (i=0; i<num; i++) {
        if (strEQ(name, data[i])) {
            return 1;
        }
    }
    return 0;
}

static int proc_net_interface_list_get(sigar_t *sigar,
                                       sigar_net_interface_list_t *iflist)
{
    /* certain interfaces such as VMware vmnic
     * are not returned by ioctl(SIOCGIFCONF).
     * check /proc/net/dev for any ioctl missed.
     */
    char buffer[BUFSIZ];
    FILE *fp = fopen("/proc/net/dev", "r");

    if (!fp) {
        return errno;
    }

    /* skip header */
    fgets(buffer, sizeof(buffer), fp);
    fgets(buffer, sizeof(buffer), fp);

    while (fgets(buffer, sizeof(buffer), fp)) {
        char *ptr, *dev;

        dev = buffer;
        while (isspace(*dev)) {
            dev++;
        }

        if (!(ptr = strchr(dev, ':'))) {
            continue;
        }

        *ptr++ = 0;

        if (has_interface(iflist, dev)) {
            continue;
        }

        SIGAR_NET_IFLIST_GROW(iflist);

        iflist->data[iflist->number++] =
            sigar_strdup(dev);
    }

    fclose(fp);

    return SIGAR_OK;
}
#endif

int sigar_net_interface_list_get(sigar_t *sigar,
                                 sigar_net_interface_list_t *iflist)
{
    int n, lastlen=0;
    struct ifreq *ifr;
    struct ifconf ifc;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        return errno;
    } 

    for (;;) {
        if (!sigar->ifconf_buf || lastlen) {
            sigar->ifconf_len += sizeof(struct ifreq) * SIGAR_NET_IFLIST_MAX;
            sigar->ifconf_buf = realloc(sigar->ifconf_buf, sigar->ifconf_len);
        }

        ifc.ifc_len = sigar->ifconf_len;
        ifc.ifc_buf = sigar->ifconf_buf;

        if (ioctl(sock, MY_SIOCGIFCONF, &ifc) < 0) {
            /* EINVAL should mean num_interfaces > ifc.ifc_len */
            if ((errno != EINVAL) ||
                (lastlen == ifc.ifc_len))
            {
                free(ifc.ifc_buf);
                return errno;
            }
        }

        if (ifc.ifc_len < sigar->ifconf_len) {
            break; /* got em all */
        }

        if (ifc.ifc_len != lastlen) {
            /* might be more */
            lastlen = ifc.ifc_len;
            continue;
        }

        break;
    }

    close(sock);

    iflist->number = 0;
    iflist->size = ifc.ifc_len;
    iflist->data = malloc(sizeof(*(iflist->data)) *
                          iflist->size);

    ifr = ifc.ifc_req;
    for (n = 0; n < ifc.ifc_len; n += sizeof(struct ifreq), ifr++) {
#if defined(_AIX) || defined(__osf__) /* pass the bourbon */
        if (ifr->ifr_addr.sa_family != AF_LINK) {
            /* XXX: dunno if this is right.
             * otherwise end up with two 'en0' and three 'lo0'
             * with the same ip address.
             */
            continue;
        }
#   ifdef __osf__
        /* weed out "sl0", "tun0" and the like */
        /* XXX must be a better way to check this */
        if (!sigar_netif_configured(sigar, ifr->ifr_name)) {
            continue;
        }
#   endif        
#endif
        iflist->data[iflist->number++] =
            sigar_strdup(ifr->ifr_name);
    }

#ifdef __linux__
    proc_net_interface_list_get(sigar, iflist);
#endif

    return SIGAR_OK;
}

#endif /* WIN32 */

#ifndef WIN32
#include <netinet/in.h>
#endif

/* threadsafe alternative to inet_ntoa (inet_ntop4 from apr) */
static int sigar_inet_ntoa(sigar_t *sigar,
                           sigar_uint32_t address,
                           char *addr_str)
{
    char *next=addr_str;
    int n=0;
    const unsigned char *src =
        (const unsigned char *)&address;

    do {
        unsigned char u = *src++;
        if (u > 99) {
            *next++ = '0' + u/100;
            u %= 100;
            *next++ = '0' + u/10;
            u %= 10;
        }
        else if (u > 9) {
            *next++ = '0' + u/10;
            u %= 10;
        }
        *next++ = '0' + u;
        *next++ = '.';
        n++;
    } while (n < 4);

    *--next = 0;

    return SIGAR_OK;
}

static int sigar_ether_ntoa(char *buff, unsigned char *ptr)
{
    sprintf(buff, "%02X:%02X:%02X:%02X:%02X:%02X",
            (ptr[0] & 0xff), (ptr[1] & 0xff), (ptr[2] & 0xff),
            (ptr[3] & 0xff), (ptr[4] & 0xff), (ptr[5] & 0xff));
    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_net_address_equals(sigar_net_address_t *addr1,
                                            sigar_net_address_t *addr2)
                                            
{
    if (addr1->family != addr2->family) {
        return EINVAL;
    }

    switch (addr1->family) {
      case SIGAR_AF_INET:
        return memcmp(&addr1->addr.in, &addr2->addr.in, sizeof(addr1->addr.in));
      case SIGAR_AF_INET6:
        return memcmp(&addr1->addr.in6, &addr2->addr.in6, sizeof(addr1->addr.in6));
      case SIGAR_AF_LINK:
        return memcmp(&addr1->addr.mac, &addr2->addr.mac, sizeof(addr1->addr.mac));
      default:
        return EINVAL;
    }
}

#if !defined(WIN32) && !defined(NETWARE) && !defined(__hpux)
#define sigar_inet_ntop inet_ntop
#define sigar_inet_ntop_errno errno
#else
#define sigar_inet_ntop(af, src, dst, size) NULL
#define sigar_inet_ntop_errno EINVAL
#endif

SIGAR_DECLARE(int) sigar_net_address_to_string(sigar_t *sigar,
                                               sigar_net_address_t *address,
                                               char *addr_str)
{
    switch (address->family) {
      case SIGAR_AF_INET6:
        if (sigar_inet_ntop(AF_INET6, (const void *)&address->addr.in6,
                            addr_str, SIGAR_INET6_ADDRSTRLEN))
        {
            return SIGAR_OK;
        }
        else {
            return sigar_inet_ntop_errno;
        }
      case SIGAR_AF_INET:
        return sigar_inet_ntoa(sigar, address->addr.in, addr_str);
      case SIGAR_AF_UNSPEC:
        return sigar_inet_ntoa(sigar, 0, addr_str); /*XXX*/
      case SIGAR_AF_LINK:
        return sigar_ether_ntoa(addr_str, &address->addr.mac[0]);
      default:
        return EINVAL;
    }
}

SIGAR_DECLARE(sigar_uint32_t) sigar_net_address_hash(sigar_net_address_t *address)
{
    sigar_uint32_t hash = 0;
    unsigned char *data;
    int i=0, size, elts;

    switch (address->family) {
      case SIGAR_AF_UNSPEC:
      case SIGAR_AF_INET:
        return address->addr.in;
      case SIGAR_AF_INET6:
        data = (unsigned char *)&address->addr.in6;
        size = sizeof(address->addr.in6);
        elts = 4;
        break;
      case SIGAR_AF_LINK:
        data = (unsigned char *)&address->addr.mac;
        size = sizeof(address->addr.mac);
        elts = 2;
        break;
      default:
        return -1;
    }

    while (i<size) {
        int j=0;
        int component=0;
        while (j<elts && i<size) {
            component = (component << 8) + data[i];
            j++; 
            i++;
        }
        hash += component;
    }

    return hash;
}

SIGAR_DECLARE(int)
sigar_net_interface_config_primary_get(sigar_t *sigar,
                                       sigar_net_interface_config_t *ifconfig)
{
    int i, status, found=0;
    sigar_net_interface_list_t iflist;
    sigar_net_interface_config_t possible_config;

    possible_config.flags = 0;

    if ((status = sigar_net_interface_list_get(sigar, &iflist)) != SIGAR_OK) {
        return status;
    }

    for (i=0; i<iflist.number; i++) {
        status = sigar_net_interface_config_get(sigar,
                                                iflist.data[i], ifconfig);

        if ((status != SIGAR_OK) ||
            (ifconfig->flags & SIGAR_IFF_LOOPBACK) ||
            !ifconfig->hwaddr.addr.in ||   /* no mac address */
            strchr(iflist.data[i], ':'))  /* alias */
        {
            continue;
        }

        if (!possible_config.flags) {
            /* save for later for use if we're not connected to the net */
            memcpy(&possible_config, ifconfig, sizeof(*ifconfig));
        }
        if (!ifconfig->address.addr.in) {
            continue; /* no ip address */
        }

        found = 1;
        break;
    }

    sigar_net_interface_list_destroy(sigar, &iflist);

    if (found) {
        return SIGAR_OK;
    }
    else if (possible_config.flags) {
        memcpy(ifconfig, &possible_config, sizeof(*ifconfig));
        return SIGAR_OK;
    }
    else {
        return SIGAR_ENXIO;
    }
}

static int fqdn_ip_get(sigar_t *sigar, char *name)
{
    sigar_net_interface_config_t ifconfig;
    int status;

    status = sigar_net_interface_config_primary_get(sigar, &ifconfig);

    if (status != SIGAR_OK) {
        return status;
    }
    if (!ifconfig.address.addr.in) {
        return SIGAR_ENXIO;
    }

    sigar_net_address_to_string(sigar, &ifconfig.address, name);

    sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                     "[fqdn] using ip address '%s' for fqdn",
                     name);

    return SIGAR_OK;
}

struct hostent *sigar_gethostbyname(const char *name,
                                    sigar_hostent_t *data)
{
    struct hostent *hp = NULL;
 
#if defined(__linux__)
    gethostbyname_r(name, &data->hs,
                    data->buffer, sizeof(data->buffer),
                    &hp, &data->error);
#elif defined(__sun)
    hp = gethostbyname_r(name, &data->hs,
                         data->buffer, sizeof(data->buffer),
                         &data->error);
#elif defined(SIGAR_HAS_HOSTENT_DATA)
    if (gethostbyname_r(name, &data->hs, &data->hd) == 0) {
        hp = &data->hs;
    }
    else {
        data->error = h_errno;
    }
#else
    hp = gethostbyname(name);
#endif

    return hp;
}

static struct hostent *sigar_gethostbyaddr(const char *addr,
                                           int len, int type,
                                           sigar_hostent_t *data)
{
    struct hostent *hp = NULL;

#if defined(__linux__)
    gethostbyaddr_r(addr, len, type,
                    &data->hs,
                    data->buffer, sizeof(data->buffer),
                    &hp, &data->error);
#elif defined(__sun)
    hp = gethostbyaddr_r(addr, len, type,
                         &data->hs,
                         data->buffer, sizeof(data->buffer),
                         &data->error);
#elif defined(SIGAR_HAS_HOSTENT_DATA)
    if (gethostbyaddr_r((char *)addr, len, type,
                        &data->hs, &data->hd) == 0)
    {
        hp = &data->hs;
    }
    else {
        data->error = h_errno;
    }
#else
    if (!(hp = gethostbyaddr(addr, len, type))) {
        data->error = h_errno;
    }
#endif

    return hp;
}
#define IS_FQDN(name) \
    (name && strchr(name, '.'))

#define IS_FQDN_MATCH(lookup, name) \
    (IS_FQDN(lookup) && strnEQ(lookup, name, strlen(name)))

#define FQDN_SET(fqdn) \
    SIGAR_STRNCPY(name, fqdn, namelen)

SIGAR_DECLARE(int) sigar_fqdn_get(sigar_t *sigar, char *name, int namelen)
{
    register int is_debug = SIGAR_LOG_IS_DEBUG(sigar);
    sigar_hostent_t data;
    struct hostent *p;
    char domain[SIGAR_FQDN_LEN + 1];
#ifdef WIN32
    int status = sigar_wsa_init(sigar);

    if (status != SIGAR_OK) {
        return status;
    }
#endif

    if (gethostname(name, namelen - 1) != 0) {
        sigar_log_printf(sigar, SIGAR_LOG_ERROR,
                         "[fqdn] gethostname failed: %s",
                         sigar_strerror(sigar, errno));
        return errno;
    }
    else {
        if (is_debug) {
            sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                             "[fqdn] gethostname()=='%s'",
                             name);
        }
    }

    if (!(p = sigar_gethostbyname(name, &data))) {
        if (is_debug) {
            sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                             "[fqdn] gethostbyname(%s) failed: %s",
                             name, sigar_strerror(sigar, errno));
        }

        if (!IS_FQDN(name)) {
            fqdn_ip_get(sigar, name);
        }

        return SIGAR_OK;
    }

    if (IS_FQDN_MATCH(p->h_name, name)) {
        FQDN_SET(p->h_name);

        sigar_log(sigar, SIGAR_LOG_DEBUG,
                  "[fqdn] resolved using gethostbyname.h_name");

        return SIGAR_OK;
    }
    else {
        sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                         "[fqdn] unresolved using gethostbyname.h_name");
    }

    if (p->h_aliases) {
        int i;

        for (i=0; p->h_aliases[i]; i++) {
            if (IS_FQDN_MATCH(p->h_aliases[i], name)) {
                FQDN_SET(p->h_aliases[i]);

                sigar_log(sigar, SIGAR_LOG_DEBUG,
                          "[fqdn] resolved using gethostbyname.h_aliases");

                return SIGAR_OK;
            }
            else if (is_debug) {
                sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                                 "[fqdn] gethostbyname(%s).alias[%d]=='%s'",
                                 name, i, p->h_aliases[i]);
            }
        }
    }

    sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                     "[fqdn] unresolved using gethostbyname.h_aliases");

    if (p->h_addr_list) {
        int i,j;

        for (i=0; p->h_addr_list[i]; i++) {
            char addr[SIGAR_INET6_ADDRSTRLEN];
            struct in_addr *in =
                (struct in_addr *)p->h_addr_list[i];

            struct hostent *q =
                sigar_gethostbyaddr(p->h_addr_list[i],
                                    p->h_length,
                                    p->h_addrtype,
                                    &data);

            if (is_debug) {
                sigar_inet_ntoa(sigar, in->s_addr, addr);
            }

            if (!q) {
                if (is_debug) {
                    sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                                     "[fqdn] gethostbyaddr(%s) failed: %s",
                                     addr,
                                     sigar_strerror(sigar, errno));
                }
                continue;
            }

            if (IS_FQDN_MATCH(q->h_name, name)) {
                FQDN_SET(q->h_name);

                sigar_log(sigar, SIGAR_LOG_DEBUG,
                          "[fqdn] resolved using gethostbyaddr.h_name");

                return SIGAR_OK;
            }
            else {
                if (is_debug) {
                    sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                                     "[fqdn] gethostbyaddr(%s)=='%s'",
                                     addr, q->h_name);
                }

                for (j=0; q->h_aliases[j]; j++) {
                    if (IS_FQDN_MATCH(q->h_aliases[j], name)) {
                        FQDN_SET(q->h_aliases[j]);

                        sigar_log(sigar, SIGAR_LOG_DEBUG,
                                  "[fqdn] resolved using "
                                  "gethostbyaddr.h_aliases");

                        return SIGAR_OK;
                    }
                    else if (is_debug) {
                        sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                                         "[fqdn] gethostbyaddr(%s).alias[%d]=='%s'",
                                         addr, j, q->h_aliases[j]);
                    }
                }
            }
        }
    }

    sigar_log(sigar, SIGAR_LOG_DEBUG,
              "[fqdn] unresolved using gethostbyname.h_addr_list");

#if !defined(WIN32) && !defined(NETWARE)
    if (!IS_FQDN(name) && /* e.g. aix gethostname is already fqdn */
        (getdomainname(domain, sizeof(domain) - 1) == 0) &&
        (domain[0] != '\0') &&
        (domain[0] != '('))  /* linux default is "(none)" */
    {
        /* sprintf(name, "%s.%s", name, domain); */
        char *ptr = name;
        int len = strlen(name);
        ptr += len;
        *ptr++ = '.';
        namelen -= (len+1);
        SIGAR_STRNCPY(ptr, domain, namelen);

        sigar_log(sigar, SIGAR_LOG_DEBUG,
                  "[fqdn] resolved using getdomainname");
    }
    else {
        sigar_log(sigar, SIGAR_LOG_DEBUG,
                  "[fqdn] getdomainname failed");
    }
#endif

    if (!IS_FQDN(name)) {
        fqdn_ip_get(sigar, name);
    }

    return SIGAR_OK;
}

#ifndef MAX_STRING_LEN
#define MAX_STRING_LEN 8192
#endif

#ifdef WIN32
/* The windows version of getPasswordNative was lifted from apr */
SIGAR_DECLARE(char *) sigar_password_get(const char *prompt)
{
    static char password[MAX_STRING_LEN];
    int n = 0;
    int ch;

    fputs(prompt, stderr);
    fflush(stderr);

    while ((ch = _getch()) != '\r') {
        if (ch == EOF) /* EOF */ {
            return NULL;
        }
        else if (ch == 0 || ch == 0xE0) {
            /* FN Keys (0 or E0) are a sentinal for a FN code */ 
            ch = (ch << 4) | _getch();
            /* Catch {DELETE}, {<--}, Num{DEL} and Num{<--} */
            if ((ch == 0xE53 || ch == 0xE4B || ch == 0x053 || ch == 0x04b) && n) {
                password[--n] = '\0';
                fputs("\b \b", stderr);
                fflush(stderr);
            }
            else {
                fputc('\a', stderr);
                fflush(stderr);
            }
        }
        else if ((ch == '\b' || ch == 127) && n) /* BS/DEL */ {
            password[--n] = '\0';
            fputs("\b \b", stderr);
            fflush(stderr);
        }
        else if (ch == 3) /* CTRL+C */ {
            /* _getch() bypasses Ctrl+C but not Ctrl+Break detection! */
            fputs("^C\n", stderr);
            fflush(stderr);
            exit(-1);
        }
        else if (ch == 26) /* CTRL+Z */ {
            fputs("^Z\n", stderr);
            fflush(stderr);
            return NULL;
        }
	else if (ch == 27) /* ESC */ {
            fputc('\n', stderr);
            fputs(prompt, stderr);
            fflush(stderr);
            n = 0;
        }
        else if ((n < sizeof(password) - 1) && !iscntrl(ch)) {
            password[n++] = ch;
            fputc(' ', stderr);
            fflush(stderr);
        }
	else {
            fputc('\a', stderr);
            fflush(stderr);
        }
    }
 
    fputc('\n', stderr);
    fflush(stderr);
    password[n] = '\0';

    return password;
}

#else

/* linux/hpux/solaris getpass() prototype lives here */
#include <unistd.h>

#include <termios.h>

/* from apr_getpass.c */

#if defined(SIGAR_HPUX)
#   define getpass termios_getpass
#elif defined(SIGAR_SOLARIS)
#   define getpass getpassphrase
#endif

#ifdef SIGAR_HPUX
static char *termios_getpass(const char *prompt)
{
    struct termios attr;
    static char password[MAX_STRING_LEN];
    unsigned int n=0;

    fputs(prompt, stderr);
    fflush(stderr);
        
    if (tcgetattr(STDIN_FILENO, &attr) != 0) {
        return NULL;
    }

    attr.c_lflag &= ~(ECHO);

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &attr) != 0) {
        return NULL;
    }

    while ((password[n] = getchar()) != '\n') {
        if (n < (sizeof(password) - 1) && 
            (password[n] >= ' ') && 
            (password[n] <= '~'))
        {
            n++;
        }
        else {
            fprintf(stderr, "\n");
            fputs(prompt, stderr);
            fflush(stderr);
            n = 0;
        }
    }
 
    password[n] = '\0';
    printf("\n");

    if (n > (MAX_STRING_LEN - 1)) {
        password[MAX_STRING_LEN - 1] = '\0';
    }

    attr.c_lflag |= ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &attr);

    return (char *)&password;
}
#endif

SIGAR_DECLARE(char *) sigar_password_get(const char *prompt)
{
    char *buf = NULL;

    /* the linux version of getpass prints the prompt to the tty; ok.
     * the solaris version prints the prompt to stderr; not ok.
     * so print the prompt to /dev/tty ourselves if possible (always should be)
     */

    FILE *tty = NULL;

    if ((tty = fopen("/dev/tty", "w"))) {
        fprintf(tty, "%s", prompt);
        fflush(tty);

        buf = getpass(tty ? "" : prompt);
        fclose(tty);
    }

    return buf;
}

#endif /* WIN32 */
