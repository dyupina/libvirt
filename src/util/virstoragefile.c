/*
 * virstoragefile.c: file utility functions for FS storage backend
 *
 * Copyright (C) 2007-2017 Red Hat, Inc.
 * Copyright (C) 2007-2008 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include "virstoragefile.h"

#include "viralloc.h"
#include "virxml.h"
#include "viruuid.h"
#include "virerror.h"
#include "virlog.h"
#include "virfile.h"
#include "vircommand.h"
#include "virhash.h"
#include "virstring.h"
#include "virbuffer.h"
#include "virstorageencryption.h"
#include "virsecret.h"

#define VIR_FROM_THIS VIR_FROM_STORAGE

VIR_LOG_INIT("util.storagefile");

static virClassPtr virStorageSourceClass;

VIR_ENUM_IMPL(virStorage,
              VIR_STORAGE_TYPE_LAST,
              "none",
              "file",
              "block",
              "dir",
              "network",
              "volume",
              "nvme",
);

VIR_ENUM_IMPL(virStorageFileFormat,
              VIR_STORAGE_FILE_LAST,
              "none",
              "raw", "dir", "bochs",
              "cloop", "dmg", "iso",
              "vpc", "vdi",
              /* Not direct file formats, but used for various drivers */
              "fat", "vhd", "ploop",
              /* Formats with backing file below here */
              "cow", "qcow", "qcow2", "qed", "vmdk",
);

VIR_ENUM_IMPL(virStorageFileFeature,
              VIR_STORAGE_FILE_FEATURE_LAST,
              "lazy_refcounts",
);

VIR_ENUM_IMPL(virStorageNetProtocol,
              VIR_STORAGE_NET_PROTOCOL_LAST,
              "none",
              "nbd",
              "rbd",
              "sheepdog",
              "gluster",
              "iscsi",
              "http",
              "https",
              "ftp",
              "ftps",
              "tftp",
              "ssh",
              "vxhs",
              "nfs",
);

VIR_ENUM_IMPL(virStorageNetHostTransport,
              VIR_STORAGE_NET_HOST_TRANS_LAST,
              "tcp",
              "unix",
              "rdma",
);

VIR_ENUM_IMPL(virStorageSourcePoolMode,
              VIR_STORAGE_SOURCE_POOL_MODE_LAST,
              "default",
              "host",
              "direct",
);

VIR_ENUM_IMPL(virStorageAuth,
              VIR_STORAGE_AUTH_TYPE_LAST,
              "none", "chap", "ceph",
);


bool
virStorageIsFile(const char *backing)
{
    char *colon;
    char *slash;

    if (!backing)
        return false;

    colon = strchr(backing, ':');
    slash = strchr(backing, '/');

    /* Reject anything that looks like a protocol (such as nbd: or
     * rbd:); if someone really does want a relative file name that
     * includes ':', they can always prefix './'.  */
    if (colon && (!slash || colon < slash))
        return false;
    return true;
}


bool
virStorageIsRelative(const char *backing)
{
    if (backing[0] == '/')
        return false;

    if (!virStorageIsFile(backing))
        return false;

    return true;
}


#ifdef WITH_UDEV
/* virStorageFileGetSCSIKey
 * @path: Path to the SCSI device
 * @key: Unique key to be returned
 * @ignoreError: Used to not report ENOSYS
 *
 * Using a udev specific function, query the @path to get and return a
 * unique @key for the caller to use.
 *
 * Returns:
 *     0 On success, with the @key filled in or @key=NULL if the
 *       returned string was empty.
 *    -1 When WITH_UDEV is undefined and a system error is reported
 *    -2 When WITH_UDEV is defined, but calling virCommandRun fails
 */
int
virStorageFileGetSCSIKey(const char *path,
                         char **key,
                         bool ignoreError G_GNUC_UNUSED)
{
    int status;
    g_autoptr(virCommand) cmd = NULL;

    cmd = virCommandNewArgList("/lib/udev/scsi_id",
                               "--replace-whitespace",
                               "--whitelisted",
                               "--device", path,
                               NULL
                               );
    *key = NULL;

    /* Run the program and capture its output */
    virCommandSetOutputBuffer(cmd, key);
    if (virCommandRun(cmd, &status) < 0)
        return -2;

    /* Explicitly check status == 0, rather than passing NULL
     * to virCommandRun because we don't want to raise an actual
     * error in this scenario, just return a NULL key.
     */
    if (status == 0 && *key) {
        char *nl = strchr(*key, '\n');
        if (nl)
            *nl = '\0';
    }

    if (*key && STREQ(*key, ""))
        VIR_FREE(*key);

    return 0;
}
#else
int virStorageFileGetSCSIKey(const char *path,
                             char **key G_GNUC_UNUSED,
                             bool ignoreError)
{
    if (!ignoreError)
        virReportSystemError(ENOSYS, _("Unable to get SCSI key for %s"), path);
    return -1;
}
#endif


#ifdef WITH_UDEV
/* virStorageFileGetNPIVKey
 * @path: Path to the NPIV device
 * @key: Unique key to be returned
 *
 * Using a udev specific function, query the @path to get and return a
 * unique @key for the caller to use. Unlike the GetSCSIKey method, an
 * NPIV LUN is uniquely identified by its ID_TARGET_PORT value.
 *
 * Returns:
 *     0 On success, with the @key filled in or @key=NULL if the
 *       returned output string didn't have the data we need to
 *       formulate a unique key value
 *    -1 When WITH_UDEV is undefined and a system error is reported
 *    -2 When WITH_UDEV is defined, but calling virCommandRun fails
 */
# define ID_SERIAL "ID_SERIAL="
# define ID_TARGET_PORT "ID_TARGET_PORT="
int
virStorageFileGetNPIVKey(const char *path,
                         char **key)
{
    int status;
    const char *serial;
    const char *port;
    g_autofree char *outbuf = NULL;
    g_autoptr(virCommand) cmd = NULL;

    cmd = virCommandNewArgList("/lib/udev/scsi_id",
                               "--replace-whitespace",
                               "--whitelisted",
                               "--export",
                               "--device", path,
                               NULL
                               );
    *key = NULL;

    /* Run the program and capture its output */
    virCommandSetOutputBuffer(cmd, &outbuf);
    if (virCommandRun(cmd, &status) < 0)
        return -2;

    /* Explicitly check status == 0, rather than passing NULL
     * to virCommandRun because we don't want to raise an actual
     * error in this scenario, just return a NULL key.
     */
    if (status == 0 && *outbuf &&
        (serial = strstr(outbuf, ID_SERIAL)) &&
        (port = strstr(outbuf, ID_TARGET_PORT))) {
        char *tmp;

        serial += strlen(ID_SERIAL);
        port += strlen(ID_TARGET_PORT);

        if ((tmp = strchr(serial, '\n')))
            *tmp = '\0';

        if ((tmp = strchr(port, '\n')))
            *tmp = '\0';

        if (*serial != '\0' && *port != '\0')
            *key = g_strdup_printf("%s_PORT%s", serial, port);
    }

    return 0;
}
#else
int virStorageFileGetNPIVKey(const char *path G_GNUC_UNUSED,
                             char **key G_GNUC_UNUSED)
{
    return -1;
}
#endif

/**
 * virStorageFileParseBackingStoreStr:
 * @str: backing store specifier string to parse
 * @target: returns target device portion of the string
 * @chainIndex: returns the backing store portion of the string
 *
 * Parses the backing store specifier string such as vda[1], or sda into
 * components and returns them via arguments. If the string did not specify an
 * index, 0 is assumed.
 *
 * Returns 0 on success -1 on error
 */
int
virStorageFileParseBackingStoreStr(const char *str,
                                   char **target,
                                   unsigned int *chainIndex)
{
    size_t nstrings;
    unsigned int idx = 0;
    char *suffix;
    g_auto(GStrv) strings = NULL;

    *chainIndex = 0;

    if (!(strings = virStringSplitCount(str, "[", 2, &nstrings)))
        return -1;

    if (nstrings == 2) {
        if (virStrToLong_uip(strings[1], &suffix, 10, &idx) < 0 ||
            STRNEQ(suffix, "]"))
            return -1;
    }

    if (target)
        *target = g_strdup(strings[0]);

    *chainIndex = idx;
    return 0;
}


int
virStorageFileParseChainIndex(const char *diskTarget,
                              const char *name,
                              unsigned int *chainIndex)
{
    unsigned int idx = 0;
    g_autofree char *target = NULL;

    *chainIndex = 0;

    if (!name || !diskTarget)
        return 0;

    if (virStorageFileParseBackingStoreStr(name, &target, &idx) < 0)
        return 0;

    if (idx == 0)
        return 0;

    if (STRNEQ(diskTarget, target)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("requested target '%s' does not match target '%s'"),
                       target, diskTarget);
        return -1;
    }

    *chainIndex = idx;

    return 0;
}


/**
 * virStorageSourceIsBacking:
 * @src: storage source
 *
 * Returns true if @src is a eligible backing store structure. Useful
 * for iterators.
 */
bool
virStorageSourceIsBacking(const virStorageSource *src)
{
    return src && src->type != VIR_STORAGE_TYPE_NONE;
}

/**
 * virStorageSourceHasBacking:
 * @src: storage source
 *
 * Returns true if @src has backing store/chain.
 */
bool
virStorageSourceHasBacking(const virStorageSource *src)
{
    return virStorageSourceIsBacking(src) && src->backingStore &&
           src->backingStore->type != VIR_STORAGE_TYPE_NONE;
}


void
virStorageNetHostDefClear(virStorageNetHostDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->name);
    VIR_FREE(def->socket);
}


void
virStorageNetHostDefFree(size_t nhosts,
                         virStorageNetHostDefPtr hosts)
{
    size_t i;

    if (!hosts)
        return;

    for (i = 0; i < nhosts; i++)
        virStorageNetHostDefClear(&hosts[i]);

    VIR_FREE(hosts);
}


static void
virStoragePermsFree(virStoragePermsPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->label);
    VIR_FREE(def);
}


virStorageNetHostDefPtr
virStorageNetHostDefCopy(size_t nhosts,
                         virStorageNetHostDefPtr hosts)
{
    virStorageNetHostDefPtr ret = NULL;
    size_t i;

    ret = g_new0(virStorageNetHostDef, nhosts);

    for (i = 0; i < nhosts; i++) {
        virStorageNetHostDefPtr src = &hosts[i];
        virStorageNetHostDefPtr dst = &ret[i];

        dst->transport = src->transport;
        dst->port = src->port;

        dst->name = g_strdup(src->name);
        dst->socket = g_strdup(src->socket);
    }

    return ret;
}


void
virStorageAuthDefFree(virStorageAuthDefPtr authdef)
{
    if (!authdef)
        return;

    VIR_FREE(authdef->username);
    VIR_FREE(authdef->secrettype);
    virSecretLookupDefClear(&authdef->seclookupdef);
    VIR_FREE(authdef);
}


virStorageAuthDefPtr
virStorageAuthDefCopy(const virStorageAuthDef *src)
{
    g_autoptr(virStorageAuthDef) authdef = NULL;

    authdef = g_new0(virStorageAuthDef, 1);

    authdef->username = g_strdup(src->username);
    /* Not present for storage pool, but used for disk source */
    authdef->secrettype = g_strdup(src->secrettype);
    authdef->authType = src->authType;

    virSecretLookupDefCopy(&authdef->seclookupdef, &src->seclookupdef);

    return g_steal_pointer(&authdef);
}


virStorageAuthDefPtr
virStorageAuthDefParse(xmlNodePtr node,
                       xmlXPathContextPtr ctxt)
{
    VIR_XPATH_NODE_AUTORESTORE(ctxt)
    virStorageAuthDefPtr ret = NULL;
    xmlNodePtr secretnode = NULL;
    g_autoptr(virStorageAuthDef) authdef = NULL;
    g_autofree char *authtype = NULL;

    ctxt->node = node;

    authdef = g_new0(virStorageAuthDef, 1);

    if (!(authdef->username = virXPathString("string(./@username)", ctxt))) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("missing username for auth"));
        goto cleanup;
    }

    authdef->authType = VIR_STORAGE_AUTH_TYPE_NONE;
    authtype = virXPathString("string(./@type)", ctxt);
    if (authtype) {
        /* Used by the storage pool instead of the secret type field
         * to define whether chap or ceph being used
         */
        if ((authdef->authType = virStorageAuthTypeFromString(authtype)) < 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unknown auth type '%s'"), authtype);
            goto cleanup;
        }
    }

    if (!(secretnode = virXPathNode("./secret ", ctxt))) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Missing <secret> element in auth"));
        goto cleanup;
    }

    /* Used by the domain disk xml parsing in order to ensure the
     * <secret type='%s' value matches the expected secret type for
     * the style of disk (iscsi is chap, nbd is ceph). For some reason
     * the virSecretUsageType{From|To}String() cannot be linked here
     * and because only the domain parsing code cares - just keep
     * it as a string.
     */
    authdef->secrettype = virXMLPropString(secretnode, "type");

    if (virSecretLookupParseSecret(secretnode, &authdef->seclookupdef) < 0)
        goto cleanup;

    ret = g_steal_pointer(&authdef);

 cleanup:

    return ret;
}


void
virStorageAuthDefFormat(virBufferPtr buf,
                        virStorageAuthDefPtr authdef)
{
    if (authdef->authType == VIR_STORAGE_AUTH_TYPE_NONE) {
        virBufferEscapeString(buf, "<auth username='%s'>\n", authdef->username);
    } else {
        virBufferAsprintf(buf, "<auth type='%s' ",
                          virStorageAuthTypeToString(authdef->authType));
        virBufferEscapeString(buf, "username='%s'>\n", authdef->username);
    }

    virBufferAdjustIndent(buf, 2);
    virSecretLookupFormatSecret(buf, authdef->secrettype,
                                &authdef->seclookupdef);
    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</auth>\n");
}


void
virStoragePRDefFree(virStoragePRDefPtr prd)
{
    if (!prd)
        return;

    VIR_FREE(prd->path);
    VIR_FREE(prd->mgralias);
    VIR_FREE(prd);
}


virStoragePRDefPtr
virStoragePRDefParseXML(xmlXPathContextPtr ctxt)
{
    virStoragePRDefPtr prd;
    virStoragePRDefPtr ret = NULL;
    g_autofree char *managed = NULL;
    g_autofree char *type = NULL;
    g_autofree char *path = NULL;
    g_autofree char *mode = NULL;

    prd = g_new0(virStoragePRDef, 1);

    if (!(managed = virXPathString("string(./@managed)", ctxt))) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("missing @managed attribute for <reservations/>"));
        goto cleanup;
    }

    if ((prd->managed = virTristateBoolTypeFromString(managed)) <= 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("invalid value for 'managed': %s"), managed);
        goto cleanup;
    }

    type = virXPathString("string(./source[1]/@type)", ctxt);
    path = virXPathString("string(./source[1]/@path)", ctxt);
    mode = virXPathString("string(./source[1]/@mode)", ctxt);

    if (prd->managed == VIR_TRISTATE_BOOL_NO || type || path || mode) {
        if (!type) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("missing connection type for <reservations/>"));
            goto cleanup;
        }

        if (!path) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("missing path for <reservations/>"));
            goto cleanup;
        }

        if (!mode) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("missing connection mode for <reservations/>"));
            goto cleanup;
        }
    }

    if (type && STRNEQ(type, "unix")) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("unsupported connection type for <reservations/>: %s"),
                       type);
        goto cleanup;
    }

    if (mode && STRNEQ(mode, "client")) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("unsupported connection mode for <reservations/>: %s"),
                       mode);
        goto cleanup;
    }

    prd->path = g_steal_pointer(&path);
    ret = g_steal_pointer(&prd);

 cleanup:
    virStoragePRDefFree(prd);
    return ret;
}


void
virStoragePRDefFormat(virBufferPtr buf,
                      virStoragePRDefPtr prd,
                      bool migratable)
{
    virBufferAsprintf(buf, "<reservations managed='%s'",
                      virTristateBoolTypeToString(prd->managed));
    if (prd->path &&
        (prd->managed == VIR_TRISTATE_BOOL_NO || !migratable)) {
        virBufferAddLit(buf, ">\n");
        virBufferAdjustIndent(buf, 2);
        virBufferAddLit(buf, "<source type='unix'");
        virBufferEscapeString(buf, " path='%s'", prd->path);
        virBufferAddLit(buf, " mode='client'/>\n");
        virBufferAdjustIndent(buf, -2);
        virBufferAddLit(buf, "</reservations>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }
}


bool
virStoragePRDefIsEqual(virStoragePRDefPtr a,
                       virStoragePRDefPtr b)
{
    if (!a && !b)
        return true;

    if (!a || !b)
        return false;

    if (a->managed != b->managed ||
        STRNEQ_NULLABLE(a->path, b->path))
        return false;

    return true;
}


bool
virStoragePRDefIsManaged(virStoragePRDefPtr prd)
{
    return prd && prd->managed == VIR_TRISTATE_BOOL_YES;
}


bool
virStorageSourceChainHasManagedPR(virStorageSourcePtr src)
{
    virStorageSourcePtr n;

    for (n = src; virStorageSourceIsBacking(n); n = n->backingStore) {
        if (virStoragePRDefIsManaged(n->pr))
            return true;
    }

    return false;
}


static virStoragePRDefPtr
virStoragePRDefCopy(virStoragePRDefPtr src)
{
    virStoragePRDefPtr copy = NULL;
    virStoragePRDefPtr ret = NULL;

    copy = g_new0(virStoragePRDef, 1);

    copy->managed = src->managed;

    copy->path = g_strdup(src->path);
    copy->mgralias = g_strdup(src->mgralias);

    ret = g_steal_pointer(&copy);

    virStoragePRDefFree(copy);
    return ret;
}


static virStorageSourceNVMeDefPtr
virStorageSourceNVMeDefCopy(const virStorageSourceNVMeDef *src)
{
    virStorageSourceNVMeDefPtr ret = NULL;

    ret = g_new0(virStorageSourceNVMeDef, 1);

    ret->namespc = src->namespc;
    ret->managed = src->managed;
    virPCIDeviceAddressCopy(&ret->pciAddr, &src->pciAddr);
    return ret;
}


static bool
virStorageSourceNVMeDefIsEqual(const virStorageSourceNVMeDef *a,
                               const virStorageSourceNVMeDef *b)
{
    if (!a && !b)
        return true;

    if (!a || !b)
        return false;

    if (a->namespc != b->namespc ||
        a->managed != b->managed ||
        !virPCIDeviceAddressEqual(&a->pciAddr, &b->pciAddr))
        return false;

    return true;
}


void
virStorageSourceNVMeDefFree(virStorageSourceNVMeDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def);
}


bool
virStorageSourceChainHasNVMe(const virStorageSource *src)
{
    const virStorageSource *n;

    for (n = src; virStorageSourceIsBacking(n); n = n->backingStore) {
        if (n->type == VIR_STORAGE_TYPE_NVME)
            return true;
    }

    return false;
}


virSecurityDeviceLabelDefPtr
virStorageSourceGetSecurityLabelDef(virStorageSourcePtr src,
                                    const char *model)
{
    size_t i;

    for (i = 0; i < src->nseclabels; i++) {
        if (STREQ_NULLABLE(src->seclabels[i]->model, model))
            return src->seclabels[i];
    }

    return NULL;
}


static void
virStorageSourceSeclabelsClear(virStorageSourcePtr def)
{
    size_t i;

    if (def->seclabels) {
        for (i = 0; i < def->nseclabels; i++)
            virSecurityDeviceLabelDefFree(def->seclabels[i]);
        VIR_FREE(def->seclabels);
    }
}


static int
virStorageSourceSeclabelsCopy(virStorageSourcePtr to,
                              const virStorageSource *from)
{
    size_t i;

    if (from->nseclabels == 0)
        return 0;

    to->seclabels = g_new0(virSecurityDeviceLabelDefPtr, from->nseclabels);
    to->nseclabels = from->nseclabels;

    for (i = 0; i < to->nseclabels; i++) {
        if (!(to->seclabels[i] = virSecurityDeviceLabelDefCopy(from->seclabels[i])))
            goto error;
    }

    return 0;

 error:
    virStorageSourceSeclabelsClear(to);
    return -1;
}


void
virStorageNetCookieDefFree(virStorageNetCookieDefPtr def)
{
    if (!def)
        return;

    g_free(def->name);
    g_free(def->value);

    g_free(def);
}


static void
virStorageSourceNetCookiesClear(virStorageSourcePtr src)
{
    size_t i;

    if (!src || !src->cookies)
        return;

    for (i = 0; i < src->ncookies; i++)
        virStorageNetCookieDefFree(src->cookies[i]);

    g_clear_pointer(&src->cookies, g_free);
    src->ncookies = 0;
}


static void
virStorageSourceNetCookiesCopy(virStorageSourcePtr to,
                               const virStorageSource *from)
{
    size_t i;

    if (from->ncookies == 0)
        return;

    to->cookies = g_new0(virStorageNetCookieDefPtr, from->ncookies);
    to->ncookies = from->ncookies;

    for (i = 0; i < from->ncookies; i++) {
        to->cookies[i]->name = g_strdup(from->cookies[i]->name);
        to->cookies[i]->value = g_strdup(from->cookies[i]->value);
    }
}


/* see https://tools.ietf.org/html/rfc6265#section-4.1.1 */
static const char virStorageSourceCookieValueInvalidChars[] =
 "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F"
 "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1A\x1B\x1C\x1D\x1E\x1F"
 " \",;\\";

/* in addition cookie name can't contain these */
static const char virStorageSourceCookieNameInvalidChars[] =
 "()<>@:/[]?={}";

static int
virStorageSourceNetCookieValidate(virStorageNetCookieDefPtr def)
{
    g_autofree char *val = g_strdup(def->value);
    const char *checkval = val;
    size_t len = strlen(val);

    /* name must have at least 1 character */
    if (*(def->name) == '\0') {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("cookie name must not be empty"));
        return -1;
    }

    /* check invalid characters in name */
    if (virStringHasChars(def->name, virStorageSourceCookieValueInvalidChars) ||
        virStringHasChars(def->name, virStorageSourceCookieNameInvalidChars)) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("cookie name '%s' contains invalid characters"),
                       def->name);
        return -1;
    }

    /* check for optional quotes around the cookie value string */
    if (val[0] == '"') {
        if (val[len - 1] != '"') {
            virReportError(VIR_ERR_XML_ERROR,
                           _("value of cookie '%s' contains invalid characters"),
                           def->name);
            return -1;
        }

        val[len - 1] = '\0';
        checkval++;
    }

    /* check invalid characters in value */
    if (virStringHasChars(checkval, virStorageSourceCookieValueInvalidChars)) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("value of cookie '%s' contains invalid characters"),
                       def->name);
        return -1;
    }

    return 0;
}


int
virStorageSourceNetCookiesValidate(virStorageSourcePtr src)
{
    size_t i;
    size_t j;

    for (i = 0; i < src->ncookies; i++) {
        if (virStorageSourceNetCookieValidate(src->cookies[i]) < 0)
            return -1;

        for (j = i + 1; j < src->ncookies; j++) {
            if (STREQ(src->cookies[i]->name, src->cookies[j]->name)) {
                virReportError(VIR_ERR_XML_ERROR, _("duplicate cookie '%s'"),
                               src->cookies[i]->name);
                return -1;
            }
        }
    }

    return 0;
}


static virStorageTimestampsPtr
virStorageTimestampsCopy(const virStorageTimestamps *src)
{
    virStorageTimestampsPtr ret;

    ret = g_new0(virStorageTimestamps, 1);

    memcpy(ret, src, sizeof(*src));

    return ret;
}


static virStoragePermsPtr
virStoragePermsCopy(const virStoragePerms *src)
{
    virStoragePermsPtr ret;

    ret = g_new0(virStoragePerms, 1);

    ret->mode = src->mode;
    ret->uid = src->uid;
    ret->gid = src->gid;

    ret->label = g_strdup(src->label);

    return ret;
}


static virStorageSourcePoolDefPtr
virStorageSourcePoolDefCopy(const virStorageSourcePoolDef *src)
{
    virStorageSourcePoolDefPtr ret;

    ret = g_new0(virStorageSourcePoolDef, 1);

    ret->voltype = src->voltype;
    ret->pooltype = src->pooltype;
    ret->actualtype = src->actualtype;
    ret->mode = src->mode;

    ret->pool = g_strdup(src->pool);
    ret->volume = g_strdup(src->volume);

    return ret;
}


static virStorageSourceSlicePtr
virStorageSourceSliceCopy(const virStorageSourceSlice *src)
{
    virStorageSourceSlicePtr ret = g_new0(virStorageSourceSlice, 1);

    ret->offset = src->offset;
    ret->size = src->size;
    ret->nodename = g_strdup(src->nodename);

    return ret;
}


static void
virStorageSourceSliceFree(virStorageSourceSlicePtr slice)
{
    if (!slice)
        return;

    g_free(slice->nodename);
    g_free(slice);
}


/**
 * virStorageSourcePtr:
 *
 * Deep-copies a virStorageSource structure. If @backing chain is true
 * then also copies the backing chain recursively, otherwise just
 * the top element is copied. This function doesn't copy the
 * storage driver access structure and thus the struct needs to be initialized
 * separately.
 */
virStorageSourcePtr
virStorageSourceCopy(const virStorageSource *src,
                     bool backingChain)
{
    g_autoptr(virStorageSource) def = virStorageSourceNew();

    def->id = src->id;
    def->type = src->type;
    def->protocol = src->protocol;
    def->format = src->format;
    def->capacity = src->capacity;
    def->allocation = src->allocation;
    def->has_allocation = src->has_allocation;
    def->physical = src->physical;
    def->readonly = src->readonly;
    def->shared = src->shared;
    def->haveTLS = src->haveTLS;
    def->tlsFromConfig = src->tlsFromConfig;
    def->detected = src->detected;
    def->debugLevel = src->debugLevel;
    def->debug = src->debug;
    def->iomode = src->iomode;
    def->cachemode = src->cachemode;
    def->discard = src->discard;
    def->detect_zeroes = src->detect_zeroes;
    def->sslverify = src->sslverify;
    def->readahead = src->readahead;
    def->timeout = src->timeout;
    def->metadataCacheMaxSize = src->metadataCacheMaxSize;

    /* storage driver metadata are not copied */
    def->drv = NULL;

    def->path = g_strdup(src->path);
    def->volume = g_strdup(src->volume);
    def->relPath = g_strdup(src->relPath);
    def->backingStoreRaw = g_strdup(src->backingStoreRaw);
    def->backingStoreRawFormat = src->backingStoreRawFormat;
    def->snapshot = g_strdup(src->snapshot);
    def->configFile = g_strdup(src->configFile);
    def->nodeformat = g_strdup(src->nodeformat);
    def->nodestorage = g_strdup(src->nodestorage);
    def->compat = g_strdup(src->compat);
    def->tlsAlias = g_strdup(src->tlsAlias);
    def->tlsCertdir = g_strdup(src->tlsCertdir);
    def->query = g_strdup(src->query);

    if (src->sliceStorage)
        def->sliceStorage = virStorageSourceSliceCopy(src->sliceStorage);

    if (src->nhosts) {
        if (!(def->hosts = virStorageNetHostDefCopy(src->nhosts, src->hosts)))
            return NULL;

        def->nhosts = src->nhosts;
    }

    virStorageSourceNetCookiesCopy(def, src);

    if (src->srcpool &&
        !(def->srcpool = virStorageSourcePoolDefCopy(src->srcpool)))
        return NULL;

    if (src->features)
        def->features = virBitmapNewCopy(src->features);

    if (src->encryption &&
        !(def->encryption = virStorageEncryptionCopy(src->encryption)))
        return NULL;

    if (src->perms &&
        !(def->perms = virStoragePermsCopy(src->perms)))
        return NULL;

    if (src->timestamps &&
        !(def->timestamps = virStorageTimestampsCopy(src->timestamps)))
        return NULL;

    if (virStorageSourceSeclabelsCopy(def, src) < 0)
        return NULL;

    if (src->auth &&
        !(def->auth = virStorageAuthDefCopy(src->auth)))
        return NULL;

    if (src->pr &&
        !(def->pr = virStoragePRDefCopy(src->pr)))
        return NULL;

    if (src->nvme)
        def->nvme = virStorageSourceNVMeDefCopy(src->nvme);

    if (virStorageSourceInitiatorCopy(&def->initiator, &src->initiator) < 0)
        return NULL;

    if (backingChain && src->backingStore) {
        if (!(def->backingStore = virStorageSourceCopy(src->backingStore,
                                                       true)))
            return NULL;
    }

    /* ssh config passthrough for libguestfs */
    def->ssh_host_key_check_disabled = src->ssh_host_key_check_disabled;
    def->ssh_user = g_strdup(src->ssh_user);

    def->nfs_user = g_strdup(src->nfs_user);
    def->nfs_group = g_strdup(src->nfs_group);
    def->nfs_uid = src->nfs_uid;
    def->nfs_gid = src->nfs_gid;

    return g_steal_pointer(&def);
}


/**
 * virStorageSourceIsSameLocation:
 *
 * Returns true if the sources @a and @b point to the same storage location.
 * This does not compare any other configuration option
 */
bool
virStorageSourceIsSameLocation(virStorageSourcePtr a,
                               virStorageSourcePtr b)
{
    size_t i;

    /* there are multiple possibilities to define an empty source */
    if (virStorageSourceIsEmpty(a) &&
        virStorageSourceIsEmpty(b))
        return true;

    if (virStorageSourceGetActualType(a) != virStorageSourceGetActualType(b))
        return false;

    if (STRNEQ_NULLABLE(a->path, b->path) ||
        STRNEQ_NULLABLE(a->volume, b->volume) ||
        STRNEQ_NULLABLE(a->snapshot, b->snapshot))
        return false;

    if (a->type == VIR_STORAGE_TYPE_NETWORK) {
        if (a->protocol != b->protocol ||
            a->nhosts != b->nhosts)
            return false;

        for (i = 0; i < a->nhosts; i++) {
            if (a->hosts[i].transport != b->hosts[i].transport ||
                a->hosts[i].port != b->hosts[i].port ||
                STRNEQ_NULLABLE(a->hosts[i].name, b->hosts[i].name) ||
                STRNEQ_NULLABLE(a->hosts[i].socket, b->hosts[i].socket))
                return false;
        }
    }

    if (a->type == VIR_STORAGE_TYPE_NVME &&
        !virStorageSourceNVMeDefIsEqual(a->nvme, b->nvme))
        return false;

    return true;
}


/**
 * virStorageSourceInitChainElement:
 * @newelem: New backing chain element disk source
 * @old: Existing top level disk source
 * @transferLabels: Transfer security labels.
 *
 * Transfers relevant information from the existing disk source to the new
 * backing chain element if they weren't supplied so that labelling info
 * and possibly other stuff is correct.
 *
 * If @transferLabels is true, security labels from the existing disk are copied
 * to the new disk. Otherwise the default domain imagelabel label will be used.
 *
 * Returns 0 on success, -1 on error.
 */
int
virStorageSourceInitChainElement(virStorageSourcePtr newelem,
                                 virStorageSourcePtr old,
                                 bool transferLabels)
{
    if (transferLabels &&
        !newelem->seclabels &&
        virStorageSourceSeclabelsCopy(newelem, old) < 0)
        return -1;

    newelem->shared = old->shared;
    newelem->readonly = old->readonly;

    return 0;
}


void
virStorageSourcePoolDefFree(virStorageSourcePoolDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->pool);
    VIR_FREE(def->volume);

    VIR_FREE(def);
}


/**
 * virStorageSourceGetActualType:
 * @def: storage source definition
 *
 * Returns type of @def. In case when the type is VIR_STORAGE_TYPE_VOLUME
 * and virDomainDiskTranslateSourcePool was called on @def the actual type
 * of the storage volume is returned rather than VIR_STORAGE_TYPE_VOLUME.
 */
int
virStorageSourceGetActualType(const virStorageSource *def)
{
    if (def->type == VIR_STORAGE_TYPE_VOLUME &&
        def->srcpool &&
        def->srcpool->actualtype != VIR_STORAGE_TYPE_NONE)
        return def->srcpool->actualtype;

    return def->type;
}


bool
virStorageSourceIsLocalStorage(const virStorageSource *src)
{
    virStorageType type = virStorageSourceGetActualType(src);

    switch (type) {
    case VIR_STORAGE_TYPE_FILE:
    case VIR_STORAGE_TYPE_BLOCK:
    case VIR_STORAGE_TYPE_DIR:
        return true;

    case VIR_STORAGE_TYPE_NETWORK:
    case VIR_STORAGE_TYPE_VOLUME:
        /* While NVMe disks are local, they are not accessible via src->path.
         * Therefore, we have to return false here. */
    case VIR_STORAGE_TYPE_NVME:
    case VIR_STORAGE_TYPE_LAST:
    case VIR_STORAGE_TYPE_NONE:
        return false;
    }

    return false;
}


/**
 * virStorageSourceIsEmpty:
 *
 * @src: disk source to check
 *
 * Returns true if the guest disk has no associated host storage source
 * (such as an empty cdrom drive).
 */
bool
virStorageSourceIsEmpty(virStorageSourcePtr src)
{
    if (virStorageSourceIsLocalStorage(src) && !src->path)
        return true;

    if (src->type == VIR_STORAGE_TYPE_NONE)
        return true;

    if (src->type == VIR_STORAGE_TYPE_NETWORK &&
        src->protocol == VIR_STORAGE_NET_PROTOCOL_NONE)
        return true;

    return false;
}


/**
 * virStorageSourceIsBlockLocal:
 * @src: disk source definition
 *
 * Returns true if @src describes a locally accessible block storage source.
 * This includes block devices and host-mapped iSCSI volumes.
 */
bool
virStorageSourceIsBlockLocal(const virStorageSource *src)
{
    return virStorageSourceGetActualType(src) == VIR_STORAGE_TYPE_BLOCK;
}


/**
 * virStorageSourceBackingStoreClear:
 *
 * @src: disk source to clear
 *
 * Clears information about backing store of the current storage file.
 */
void
virStorageSourceBackingStoreClear(virStorageSourcePtr def)
{
    if (!def)
        return;

    VIR_FREE(def->relPath);
    VIR_FREE(def->backingStoreRaw);

    /* recursively free backing chain */
    virObjectUnref(def->backingStore);
    def->backingStore = NULL;
}


void
virStorageSourceClear(virStorageSourcePtr def)
{
    if (!def)
        return;

    VIR_FREE(def->path);
    VIR_FREE(def->volume);
    VIR_FREE(def->snapshot);
    VIR_FREE(def->configFile);
    VIR_FREE(def->query);
    virStorageSourceNetCookiesClear(def);
    virStorageSourcePoolDefFree(def->srcpool);
    virBitmapFree(def->features);
    VIR_FREE(def->compat);
    virStorageEncryptionFree(def->encryption);
    virStoragePRDefFree(def->pr);
    virStorageSourceNVMeDefFree(def->nvme);
    virStorageSourceSeclabelsClear(def);
    virStoragePermsFree(def->perms);
    VIR_FREE(def->timestamps);

    virStorageSourceSliceFree(def->sliceStorage);

    virStorageNetHostDefFree(def->nhosts, def->hosts);
    virStorageAuthDefFree(def->auth);
    virObjectUnref(def->privateData);

    VIR_FREE(def->nodestorage);
    VIR_FREE(def->nodeformat);

    virStorageSourceBackingStoreClear(def);

    VIR_FREE(def->tlsAlias);
    VIR_FREE(def->tlsCertdir);

    VIR_FREE(def->ssh_user);

    VIR_FREE(def->nfs_user);
    VIR_FREE(def->nfs_group);

    virStorageSourceInitiatorClear(&def->initiator);

    /* clear everything except the class header as the object APIs
     * will break otherwise */
    memset((char *) def + sizeof(def->parent), 0,
           sizeof(*def) - sizeof(def->parent));
}


static void
virStorageSourceDispose(void *obj)
{
    virStorageSourcePtr src = obj;

    virStorageSourceClear(src);
}


static int
virStorageSourceOnceInit(void)
{
    if (!VIR_CLASS_NEW(virStorageSource, virClassForObject()))
        return -1;

    return 0;
}


VIR_ONCE_GLOBAL_INIT(virStorageSource);


virStorageSourcePtr
virStorageSourceNew(void)
{
    virStorageSourcePtr ret;

    if (virStorageSourceInitialize() < 0)
        abort();

    if (!(ret = virObjectNew(virStorageSourceClass)))
        abort();

    return ret;
}


static char *
virStorageFileCanonicalizeFormatPath(char **components,
                                     size_t ncomponents,
                                     bool beginSlash,
                                     bool beginDoubleSlash)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    size_t i;
    char *ret = NULL;

    if (beginSlash)
        virBufferAddLit(&buf, "/");

    if (beginDoubleSlash)
        virBufferAddLit(&buf, "/");

    for (i = 0; i < ncomponents; i++) {
        if (i != 0)
            virBufferAddLit(&buf, "/");

        virBufferAdd(&buf, components[i], -1);
    }

    /* if the output string is empty just return an empty string */
    if (!(ret = virBufferContentAndReset(&buf)))
        ret = g_strdup("");

    return ret;
}


static int
virStorageFileCanonicalizeInjectSymlink(const char *path,
                                        size_t at,
                                        char ***components,
                                        size_t *ncomponents)
{
    char **tmp = NULL;
    char **next;
    size_t ntmp = 0;
    int ret = -1;

    if (!(tmp = virStringSplitCount(path, "/", 0, &ntmp)))
        goto cleanup;

    /* prepend */
    for (next = tmp; *next; next++) {
        if (VIR_INSERT_ELEMENT(*components, at, *ncomponents, *next) < 0)
            goto cleanup;

        at++;
    }

    ret = 0;

 cleanup:
    virStringListFreeCount(tmp, ntmp);
    return ret;
}


char *
virStorageFileCanonicalizePath(const char *path,
                               virStorageFileSimplifyPathReadlinkCallback cb,
                               void *cbdata)
{
    GHashTable *cycle = NULL;
    bool beginSlash = false;
    bool beginDoubleSlash = false;
    char **components = NULL;
    size_t ncomponents = 0;
    size_t i = 0;
    size_t j = 0;
    int rc;
    char *ret = NULL;
    g_autofree char *linkpath = NULL;
    g_autofree char *currentpath = NULL;

    if (path[0] == '/') {
        beginSlash = true;

        if (path[1] == '/' && path[2] != '/')
            beginDoubleSlash = true;
    }

    if (!(cycle = virHashNew(NULL)))
        goto cleanup;

    if (!(components = virStringSplitCount(path, "/", 0, &ncomponents)))
        goto cleanup;

    j = 0;
    while (j < ncomponents) {
        /* skip slashes */
        if (STREQ(components[j], "")) {
            VIR_FREE(components[j]);
            VIR_DELETE_ELEMENT(components, j, ncomponents);
            continue;
        }
        j++;
    }

    while (i < ncomponents) {
        /* skip '.'s unless it's the last one remaining */
        if (STREQ(components[i], ".") &&
            (beginSlash || ncomponents  > 1)) {
            VIR_FREE(components[i]);
            VIR_DELETE_ELEMENT(components, i, ncomponents);
            continue;
        }

        /* resolve changes to parent directory */
        if (STREQ(components[i], "..")) {
            if (!beginSlash &&
                (i == 0 || STREQ(components[i - 1], ".."))) {
                i++;
                continue;
            }

            VIR_FREE(components[i]);
            VIR_DELETE_ELEMENT(components, i, ncomponents);

            if (i != 0) {
                VIR_FREE(components[i - 1]);
                VIR_DELETE_ELEMENT(components, i - 1, ncomponents);
                i--;
            }

            continue;
        }

        /* check if the actual path isn't resulting into a symlink */
        if (!(currentpath = virStorageFileCanonicalizeFormatPath(components,
                                                                 i + 1,
                                                                 beginSlash,
                                                                 beginDoubleSlash)))
            goto cleanup;

        if ((rc = cb(currentpath, &linkpath, cbdata)) < 0)
            goto cleanup;

        if (rc == 0) {
            if (virHashLookup(cycle, currentpath)) {
                virReportSystemError(ELOOP,
                                     _("Failed to canonicalize path '%s'"), path);
                goto cleanup;
            }

            if (virHashAddEntry(cycle, currentpath, (void *) 1) < 0)
                goto cleanup;

            if (linkpath[0] == '/') {
                /* kill everything from the beginning including the actual component */
                i++;
                while (i--) {
                    VIR_FREE(components[0]);
                    VIR_DELETE_ELEMENT(components, 0, ncomponents);
                }
                beginSlash = true;

                if (linkpath[1] == '/' && linkpath[2] != '/')
                    beginDoubleSlash = true;
                else
                    beginDoubleSlash = false;

                i = 0;
            } else {
                VIR_FREE(components[i]);
                VIR_DELETE_ELEMENT(components, i, ncomponents);
            }

            if (virStorageFileCanonicalizeInjectSymlink(linkpath,
                                                        i,
                                                        &components,
                                                        &ncomponents) < 0)
                goto cleanup;

            j = 0;
            while (j < ncomponents) {
                /* skip slashes */
                if (STREQ(components[j], "")) {
                    VIR_FREE(components[j]);
                    VIR_DELETE_ELEMENT(components, j, ncomponents);
                    continue;
                }
                j++;
            }

            VIR_FREE(linkpath);
            VIR_FREE(currentpath);

            continue;
        }

        VIR_FREE(currentpath);

        i++;
    }

    ret = virStorageFileCanonicalizeFormatPath(components, ncomponents,
                                               beginSlash, beginDoubleSlash);

 cleanup:
    virHashFree(cycle);
    virStringListFreeCount(components, ncomponents);

    return ret;
}


/**
 * virStorageSourceIsRelative:
 * @src: storage source to check
 *
 * Returns true if given storage source definition is a relative path.
 */
bool
virStorageSourceIsRelative(virStorageSourcePtr src)
{
    virStorageType actual_type = virStorageSourceGetActualType(src);

    if (!src->path)
        return false;

    switch (actual_type) {
    case VIR_STORAGE_TYPE_FILE:
    case VIR_STORAGE_TYPE_BLOCK:
    case VIR_STORAGE_TYPE_DIR:
        return src->path[0] != '/';

    case VIR_STORAGE_TYPE_NETWORK:
    case VIR_STORAGE_TYPE_VOLUME:
    case VIR_STORAGE_TYPE_NVME:
    case VIR_STORAGE_TYPE_NONE:
    case VIR_STORAGE_TYPE_LAST:
        return false;
    }

    return false;
}


static unsigned int
virStorageSourceNetworkDefaultPort(virStorageNetProtocol protocol)
{
    switch (protocol) {
        case VIR_STORAGE_NET_PROTOCOL_HTTP:
            return 80;

        case VIR_STORAGE_NET_PROTOCOL_HTTPS:
            return 443;

        case VIR_STORAGE_NET_PROTOCOL_FTP:
            return 21;

        case VIR_STORAGE_NET_PROTOCOL_FTPS:
            return 990;

        case VIR_STORAGE_NET_PROTOCOL_TFTP:
            return 69;

        case VIR_STORAGE_NET_PROTOCOL_SHEEPDOG:
            return 7000;

        case VIR_STORAGE_NET_PROTOCOL_NBD:
            return 10809;

        case VIR_STORAGE_NET_PROTOCOL_SSH:
            return 22;

        case VIR_STORAGE_NET_PROTOCOL_ISCSI:
            return 3260;

        case VIR_STORAGE_NET_PROTOCOL_GLUSTER:
            return 24007;

        case VIR_STORAGE_NET_PROTOCOL_RBD:
            /* we don't provide a default for RBD */
            return 0;

        case VIR_STORAGE_NET_PROTOCOL_VXHS:
            return 9999;

        case VIR_STORAGE_NET_PROTOCOL_NFS:
            /* Port is not supported by NFS, so no default is provided */
            return 0;

        case VIR_STORAGE_NET_PROTOCOL_LAST:
        case VIR_STORAGE_NET_PROTOCOL_NONE:
            return 0;
    }

    return 0;
}


void
virStorageSourceNetworkAssignDefaultPorts(virStorageSourcePtr src)
{
    size_t i;

    for (i = 0; i < src->nhosts; i++) {
        if (src->hosts[i].transport == VIR_STORAGE_NET_HOST_TRANS_TCP &&
            src->hosts[i].port == 0)
            src->hosts[i].port = virStorageSourceNetworkDefaultPort(src->protocol);
    }
}


int
virStorageSourcePrivateDataParseRelPath(xmlXPathContextPtr ctxt,
                                        virStorageSourcePtr src)
{
    src->relPath = virXPathString("string(./relPath)", ctxt);
    return 0;
}


int
virStorageSourcePrivateDataFormatRelPath(virStorageSourcePtr src,
                                         virBufferPtr buf)
{
    if (src->relPath)
        virBufferEscapeString(buf, "<relPath>%s</relPath>\n", src->relPath);

    return 0;
}

void
virStorageSourceInitiatorParseXML(xmlXPathContextPtr ctxt,
                                  virStorageSourceInitiatorDefPtr initiator)
{
    initiator->iqn = virXPathString("string(./initiator/iqn/@name)", ctxt);
}

void
virStorageSourceInitiatorFormatXML(virStorageSourceInitiatorDefPtr initiator,
                                   virBufferPtr buf)
{
    if (!initiator->iqn)
        return;

    virBufferAddLit(buf, "<initiator>\n");
    virBufferAdjustIndent(buf, 2);
    virBufferEscapeString(buf, "<iqn name='%s'/>\n", initiator->iqn);
    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</initiator>\n");
}

int
virStorageSourceInitiatorCopy(virStorageSourceInitiatorDefPtr dest,
                              const virStorageSourceInitiatorDef *src)
{
    dest->iqn = g_strdup(src->iqn);
    return 0;
}

void
virStorageSourceInitiatorClear(virStorageSourceInitiatorDefPtr initiator)
{
    VIR_FREE(initiator->iqn);
}
