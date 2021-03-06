/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/*
   Copyright (C) 2017 by Ronnie Sahlberg <ronniesahlberg@gmail.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation; either version 2.1 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/
/*
 * High level api to nfsv4 filesystems
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef AROS
#include "aros_compat.h"
#endif

#ifdef WIN32
#include "win32_compat.h"
#endif

#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#else
#define PRIu64 "llu"
#endif

#ifdef HAVE_UTIME_H
#include <utime.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#ifdef HAVE_SYS_STATVFS_H
#include <sys/statvfs.h>
#endif

#if defined(__ANDROID__) && !defined(HAVE_SYS_STATVFS_H)
#define statvfs statfs
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

#ifdef MAJOR_IN_MKDEV
#include <sys/mkdev.h>
#endif

#ifdef HAVE_SYS_SYSMACROS_H
#include <sys/sysmacros.h>
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "libnfs-zdr.h"
#include "slist.h"
#include "libnfs.h"
#include "libnfs-raw.h"
#include "libnfs-private.h"


static int
nfs_pntoh64(const uint32_t *buf)
{
        uint64_t val;

        val   = ntohl(*(uint32_t *)(void *)buf++);
        val <<= 32;
        val  |= ntohl(*(uint32_t *)(void *)buf);

        return val;
}

static int
check_nfs4_error(struct nfs_context *nfs, int status,
                 struct nfs_cb_data *data, void *command_data,
                 char *op_name)
{
        COMPOUND4res *res = command_data;

        if (status == RPC_STATUS_ERROR) {
                data->cb(-EFAULT, nfs, res, data->private_data);
                return 1;
        }
        if (status == RPC_STATUS_CANCEL) {
                data->cb(-EINTR, nfs, "Command was cancelled",
                         data->private_data);
                return 1;
        }
        if (status == RPC_STATUS_TIMEOUT) {
                data->cb(-EINTR, nfs, "Command timed out",
                         data->private_data);
                return 1;
        }
        if (res && res->status != NFS4_OK) {
                nfs_set_error(nfs, "NFS4: %s (path %s) failed with "
                              "%s(%d)", op_name,
                              data->saved_path,
                              nfsstat4_to_str(res->status),
                              nfsstat4_to_errno(res->status));
                data->cb(nfsstat3_to_errno(res->status), nfs,
                         nfs_get_error(nfs), data->private_data);
                return 1;
        }

        return 0;
}

static char *
nfs4_resolve_path(struct nfs_context *nfs, const char *path)
{
        char *new_path;

        new_path = malloc(strlen(path) + strlen(nfs->cwd) + 2);
        if (new_path == NULL) {
                nfs_set_error(nfs, "Out of memory: failed to "
                              "allocate path string");
                return NULL;
        }
        sprintf(new_path, "%s/%s", nfs->cwd, path);

        if (nfs_normalize_path(nfs, new_path)) {
                nfs_set_error(nfs, "Failed to normalize real path. %s",
                              nfs_get_error(nfs));
                return NULL;
        }

        return new_path;
}

static int
nfs4_num_path_components(struct nfs_context *nfs, const char *path)
{
        int i;

        for (i = 0; (path = strchr(path, '/')); path++, i++)
                ;

        return i;
}

/*
 * Allocate op and populate the path components.
 * Will mutate path.
 *
 * Returns:
 *     -1 : On error.
 *  <idx> : On success. Idx represents the next free index in op.
 *          Caller must free op.
 */
static int
nfs4_allocate_op(struct nfs_context *nfs, struct nfs_cb_data *data,
                 nfs_argop4 **op, struct nfs_fh *fh, char *path, int num_extra)
{
        char *ptr;
        int i, count;

        *op = NULL;

        count = nfs4_num_path_components(nfs, path);

        *op = malloc(sizeof(**op) * (1 + count + num_extra));
        if (*op == NULL) {
                nfs_set_error(nfs, "Failed to allocate op array");
                data->cb(-ENOMEM, nfs,
                         nfs_get_error(nfs), data->private_data);
                free_nfs_cb_data(data);
                return -1;
        }

        i = 0;
        if (fh == NULL) {
                (*op)[i++].argop = OP_PUTROOTFH;
        } else {
                static struct PUTFH4args *pfh;

                pfh = &(*op)[i].nfs_argop4_u.opputfh;
                (*op)[i++].argop = OP_PUTFH;

                pfh->object.nfs_fh4_len = nfs->rootfh.len;
                pfh->object.nfs_fh4_val = nfs->rootfh.val;
        }

        ptr = &path[1];
        while (ptr) {
                char *tmp;
                LOOKUP4args *la;

                tmp = strchr(ptr, '/');
                if (tmp) {
                        *tmp = 0;
                        tmp = tmp + 1;
                }
                (*op)[i].argop = OP_LOOKUP;
                la = &(*op)[i].nfs_argop4_u.oplookup;
                
                la->objname.utf8string_len = strlen(ptr);
                la->objname.utf8string_val = ptr;

                ptr = tmp;
                i++;
        }                

        return i;
}

static void
nfs4_mount_4_cb(struct rpc_context *rpc, int status, void *command_data,
                void *private_data)
{
        struct nfs_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        GETFH4resok *gfhresok;
        int i;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "GETROOTFH")) {
                free_nfs_cb_data(data);
                return;
        }

        for (i = 0; i < res->resarray.resarray_len; i++) {
                if (res->resarray.resarray_val[i].resop == OP_GETFH) {
                        break;
                }
        }
        if (i == res->resarray.resarray_len) {
                nfs_set_error(nfs, "No GETFH result for mount.");
                data->cb(-EINVAL, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs_cb_data(data);
                return;
        }

        gfhresok = &res->resarray.resarray_val[i].nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

        nfs->rootfh.len = gfhresok->object.nfs_fh4_len;
        nfs->rootfh.val = malloc(nfs->rootfh.len);
        if (nfs->rootfh.val == NULL) {
                nfs_set_error(nfs, "%s: %s", __FUNCTION__, nfs_get_error(nfs));
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs_cb_data(data);
                return;
        }
        memcpy(nfs->rootfh.val,
               gfhresok->object.nfs_fh4_val,
               nfs->rootfh.len);


        data->cb(0, nfs, NULL, data->private_data);
        free_nfs_cb_data(data);
}

static void
nfs4_mount_3_cb(struct rpc_context *rpc, int status, void *command_data,
                void *private_data)
{
        struct nfs_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        COMPOUND4args args;
        GETATTR4args *gaargs;
        nfs_argop4 *op;
        int i;
        char *path;
        uint32_t attributes;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "SETCLIENTID_CONFIRM")) {
                free_nfs_cb_data(data);
                return;
        }

        path = nfs4_resolve_path(nfs, data->saved_path);
        if (path == NULL) {
                data->cb(-ENOMEM, nfs,
                         nfs_get_error(nfs), data->private_data);
                free_nfs_cb_data(data);
                return;
        }
        
        if ((i = nfs4_allocate_op(nfs, data, &op, NULL, path, 2)) < 0) {
                free(path);
                return;
        }


        op[i++].argop = OP_GETFH;

        /* We don't actually use the attributes, just check that we can access
         * them for our root directory.
         */
        gaargs = &op[i].nfs_argop4_u.opgetattr;
        op[i++].argop = OP_GETATTR;
        memset(gaargs, 0, sizeof(*gaargs));
        
        attributes = 1 << FATTR4_SUPPORTED_ATTRS;
        gaargs->attr_request.bitmap4_len = 1;
        gaargs->attr_request.bitmap4_val = &attributes;


        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(rpc, nfs4_mount_4_cb, &args,
                                    private_data) != 0) {
                nfs_set_error(nfs, "Failed to queue GETROOTFH. %s",
                              nfs_get_error(nfs));
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs_cb_data(data);
        }
        free(path);
        free(op);
        return;
}

static void
nfs4_mount_2_cb(struct rpc_context *rpc, int status, void *command_data,
                void *private_data)
{
        struct nfs_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        COMPOUND4args args;
        nfs_argop4 op[1];
        SETCLIENTID_CONFIRM4args *scidcargs;
        SETCLIENTID4resok *scidresok;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "SETCLIENTID")) {
                free_nfs_cb_data(data);
                return;
        }

        scidresok = &res->resarray.resarray_val[0].nfs_resop4_u.opsetclientid.SETCLIENTID4res_u.resok4;
        nfs->clientid = scidresok->clientid;
        memcpy(nfs->setclientid_confirm,
               scidresok->setclientid_confirm,
               NFS4_VERIFIER_SIZE);

        memset(op, 0, sizeof(op));
        scidcargs = &op[0].nfs_argop4_u.opsetclientid_confirm;
        op[0].argop = OP_SETCLIENTID_CONFIRM;
        scidcargs->clientid = nfs->clientid;
        memcpy(scidcargs->setclientid_confirm,
               nfs->setclientid_confirm,
               NFS4_VERIFIER_SIZE);
               
        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(rpc, nfs4_mount_3_cb, &args,
                                    private_data) != 0) {
                nfs_set_error(nfs, "Failed to queue SETCLIENTID_CONFIRM. %s",
                              nfs_get_error(nfs));
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs_cb_data(data);
                return;
        }
        return;
}

static void
nfs4_mount_1_cb(struct rpc_context *rpc, int status, void *command_data,
                void *private_data)
{
        struct nfs_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4args args;
        nfs_argop4 op[1];
        SETCLIENTID4args *scidargs;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, NULL, "Connect")) {
                free_nfs_cb_data(data);
                return;
        }

        memset(op, 0, sizeof(op));
        op[0].argop = OP_SETCLIENTID;
        scidargs = &op[0].nfs_argop4_u.opsetclientid;
        memcpy(scidargs->client.verifier, nfs->verifier, sizeof(verifier4));
        scidargs->client.id.id_len = strlen(nfs->client_name);
        scidargs->client.id.id_val = nfs->client_name;
        /* TODO: Decide what we should do here. As long as we only
         * expose a single FD to the application we will not be able to
         * do NFSv4 callbacks easily.
         * Just give it garbage for now until we figure out how we should
         * solve this. Until then we will just have to avoid doing things
         * that require a callback.
         * ( Clients (i.e. Linux) ignore this anyway and just call back to
         *   the originating address and program anyway. )
         */
        scidargs->callback.cb_program = 0; /* NFS4_CALLBACK */
        scidargs->callback.cb_location.r_netid = "tcp";
        scidargs->callback.cb_location.r_addr = "0.0.0.0.0.0";
        scidargs->callback_ident = 0x00000001;
        
        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(rpc, nfs4_mount_2_cb, &args, data) != 0) {
                nfs_set_error(nfs, "Failed to queue SETCLIENTID. %s",
                              nfs_get_error(nfs));
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs_cb_data(data);
                return;
        }
}

int
nfs4_mount_async(struct nfs_context *nfs, const char *server,
                 const char *export, nfs_cb cb, void *private_data)
{
        struct nfs_cb_data *data;
        char *new_server, *new_export;

        new_export = strdup(export);
        if (nfs_normalize_path(nfs, new_export)) {
                nfs_set_error(nfs, "Bad export path. %s",
                              nfs_get_error(nfs));
                free(new_export);
                return -1;
        }

        data = malloc(sizeof(struct nfs_cb_data));
        if (data == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "memory for nfs mount data");
                return -1;
        }
        memset(data, 0, sizeof(struct nfs_cb_data));
        new_server = strdup(server);
        if (nfs->server != NULL) {
                free(nfs->server);
        }
        nfs->server        = new_server;
        if (nfs->export != NULL) {
                free(nfs->export);
        }
        nfs->export        = new_export;
        data->nfs          = nfs;
        data->cb           = cb;
        data->private_data = private_data;
        data->saved_path   = strdup(new_export);

        if (rpc_connect_program_async(nfs->rpc, server,
                                      NFS4_PROGRAM, NFS_V4,
                                      nfs4_mount_1_cb, data) != 0) {
                nfs_set_error(nfs, "Failed to start connection");
                free_nfs_cb_data(data);
                return -1;
        }

        return 0;
}

#define CHECK_GETATTR_BUF_SPACE(len, size)                              \
    if (len < size) {                                                   \
        nfs_set_error(nfs, "Not enough data in fattr4");                \
        data->cb(-EINVAL, nfs, nfs_get_error(nfs), data->private_data); \
        free_nfs_cb_data(data);                                         \
        return;                                                         \
    }

static void
nfs4_xstat64_cb(struct rpc_context *rpc, int status, void *command_data,
                void *private_data)
{
        struct nfs_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        GETATTR4resok *garesok;
        struct nfs_stat_64 st;
        int i, len, slen, pad;
        char *buf;
        nfs_ftype4 type = 0;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, NULL, "Connect")) {
                free_nfs_cb_data(data);
                return;
        }

        for (i = 0; i < res->resarray.resarray_len; i++) {
                if (res->resarray.resarray_val[i].resop == OP_GETATTR) {
                        break;
                }
        }
        if (i == res->resarray.resarray_len) {
                nfs_set_error(nfs, "No GETATTR result for stat64.");
                data->cb(-EINVAL, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs_cb_data(data);
                return;
        }
        garesok = &res->resarray.resarray_val[i].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;
        len = garesok->obj_attributes.attr_vals.attrlist4_len;
        buf = garesok->obj_attributes.attr_vals.attrlist4_val;

        memset(&st, 0, sizeof(st));

        /* Type */
        CHECK_GETATTR_BUF_SPACE(len, 4);
        type = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;
        /* Size */
        CHECK_GETATTR_BUF_SPACE(len, 8);
        st.nfs_size = nfs_pntoh64((uint32_t *)(void *)buf);
        buf += 8;
        len -= 8;
        /* Inode */
        CHECK_GETATTR_BUF_SPACE(len, 8);
        st.nfs_ino = nfs_pntoh64((uint32_t *)(void *)buf);
        buf += 8;
        len -= 8;
        /* Mode */
        CHECK_GETATTR_BUF_SPACE(len, 4);
        st.nfs_mode = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;
        switch (type) {
        case NF4REG:
                st.nfs_mode |= S_IFREG;
                break;
        case NF4DIR:
                st.nfs_mode |= S_IFDIR;
                break;
        case NF4BLK:
                st.nfs_mode |= S_IFBLK;
                break;
        case NF4CHR:
                st.nfs_mode |= S_IFCHR;
                break;
        case NF4LNK:
                st.nfs_mode |= S_IFLNK;
                break;
        case NF4SOCK:
                st.nfs_mode |= S_IFSOCK;
                break;
        case NF4FIFO:
                st.nfs_mode |= S_IFIFO;
                break;
        default:
                break;
        }
        /* Num Links */
        CHECK_GETATTR_BUF_SPACE(len, 4);
        st.nfs_nlink = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;
        /* Owner */
        CHECK_GETATTR_BUF_SPACE(len, 4);
        slen = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;
        pad = (4 - (slen & 0x03)) & 0x03;
        CHECK_GETATTR_BUF_SPACE(len, slen);
        while (slen) {
                if (isdigit(*buf)) {
                        st.nfs_uid *= 10;
                        st.nfs_uid += *buf - '0';
                } else {
                        nfs_set_error(nfs, "Bad digit in fattr3 uid");
                        data->cb(-EINVAL, nfs, nfs_get_error(nfs),
                                 data->private_data);
                        free_nfs_cb_data(data);
                        return;
                }
                buf++;
                slen--;
        }
        CHECK_GETATTR_BUF_SPACE(len, pad);
        buf += pad;
        len -= pad;
        /* Group */
        CHECK_GETATTR_BUF_SPACE(len, 4);
        slen = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;
        pad = (4 - (slen & 0x03)) & 0x03;
        CHECK_GETATTR_BUF_SPACE(len, slen);
        while (slen) {
                if (isdigit(*buf)) {
                        st.nfs_gid *= 10;
                        st.nfs_gid += *buf - '0';
                } else {
                        nfs_set_error(nfs, "Bad digit in fattr3 gid");
                        data->cb(-EINVAL, nfs, nfs_get_error(nfs),
                                 data->private_data);
                        free_nfs_cb_data(data);
                        return;
                }
                buf++;
                slen--;
        }
        CHECK_GETATTR_BUF_SPACE(len, pad);
        buf += pad;
        len -= pad;
        /* Space Used */
        CHECK_GETATTR_BUF_SPACE(len, 8);
        st.nfs_used = nfs_pntoh64((uint32_t *)(void *)buf);
        buf += 8;
        len -= 8;
        /* ATime */
        CHECK_GETATTR_BUF_SPACE(len, 12);
        st.nfs_atime = nfs_pntoh64((uint32_t *)(void *)buf);
        buf += 8;
        len -= 8;
        st.nfs_atime_nsec = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;
        /* CTime */
        CHECK_GETATTR_BUF_SPACE(len, 12);
        st.nfs_ctime = nfs_pntoh64((uint32_t *)(void *)buf);
        buf += 8;
        len -= 8;
        st.nfs_ctime_nsec = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;
        /* MTime */
        CHECK_GETATTR_BUF_SPACE(len, 12);
        st.nfs_mtime = nfs_pntoh64((uint32_t *)(void *)buf);
        buf += 8;
        len -= 8;
        st.nfs_mtime_nsec = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;


        st.nfs_blksize = 4096;
        st.nfs_blocks  = st.nfs_used / 4096;

        data->cb(0, nfs, &st, data->private_data);
        free_nfs_cb_data(data);
}

int
nfs4_stat64_async(struct nfs_context *nfs, const char *path,
                  int no_follow, nfs_cb cb, void *private_data)
{
        COMPOUND4args args;
        GETATTR4args *gaargs;
        nfs_argop4 *op;
        char *npath;
        struct nfs_cb_data *data;
        uint32_t attributes[2];
        int i;

        data = malloc(sizeof(struct nfs_cb_data));
        if (data == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "memory for stat64 data");
                return -1;
        }
        memset(data, 0, sizeof(struct nfs_cb_data));
        data->nfs          = nfs;
        data->cb           = cb;
        data->private_data = private_data;

        npath = nfs4_resolve_path(nfs, path);
        if (path == NULL) {
                free_nfs_cb_data(data);
                return -1;
        }

        if ((i = nfs4_allocate_op(nfs, data, &op, &nfs->rootfh, npath,
                                  1)) < 0) {
                free_nfs_cb_data(data);
                free(npath);
                return -1;
        }

        gaargs = &op[i].nfs_argop4_u.opgetattr;
        op[i++].argop = OP_GETATTR;
        memset(gaargs, 0, sizeof(*gaargs));

        attributes[0] =
                1 << FATTR4_TYPE |
                1 << FATTR4_SIZE |
                1 << FATTR4_FILEID;
        attributes[1] =
                1 << (FATTR4_MODE - 32) |
                1 << (FATTR4_NUMLINKS - 32) |
                1 << (FATTR4_OWNER - 32) |
                1 << (FATTR4_OWNER_GROUP - 32) |
                1 << (FATTR4_SPACE_USED - 32) |
                1 << (FATTR4_TIME_ACCESS - 32) |
                1 << (FATTR4_TIME_METADATA - 32) |
                1 << (FATTR4_TIME_MODIFY - 32);
        gaargs->attr_request.bitmap4_len = 2;
        gaargs->attr_request.bitmap4_val = attributes;


        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_xstat64_cb, &args,
                                    data) != 0) {
                nfs_set_error(nfs, "Failed to queue GETATTR. %s",
                              nfs_get_error(nfs));
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs_cb_data(data);
                free(npath);
                free(op);
                return -1;
        }

        free(npath);
        free(op);
        return 0;
}
