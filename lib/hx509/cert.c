/*
 * Copyright (c) 2004 - 2007 Kungliga Tekniska H�gskolan
 * (Royal Institute of Technology, Stockholm, Sweden). 
 * All rights reserved. 
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met: 
 *
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution. 
 *
 * 3. Neither the name of the Institute nor the names of its contributors 
 *    may be used to endorse or promote products derived from this software 
 *    without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE 
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS 
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY 
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE. 
 */

#include "hx_locl.h"
RCSID("$Id$");
#include "crypto-headers.h"

struct hx509_verify_ctx_data {
    hx509_certs trust_anchors;
    int flags;
#define HX509_VERIFY_CTX_F_TIME_SET			1
#define HX509_VERIFY_CTX_F_ALLOW_PROXY_CERTIFICATE	2
#define HX509_VERIFY_CTX_F_REQUIRE_RFC3280		4
#define HX509_VERIFY_CTX_F_CHECK_TRUST_ANCHORS		8
    time_t time_now;
    unsigned int max_depth;
#define HX509_VERIFY_MAX_DEPTH 30
    hx509_revoke_ctx revoke_ctx;
};

#define REQUIRE_RFC3280(ctx) ((ctx)->flags & HX509_VERIFY_CTX_F_REQUIRE_RFC3280)
#define CHECK_TA(ctx) ((ctx)->flags & HX509_VERIFY_CTX_F_CHECK_TRUST_ANCHORS)

struct _hx509_cert_attrs {
    size_t len;
    hx509_cert_attribute *val;
};

struct hx509_cert_data {
    unsigned int ref;
    char *friendlyname;
    Certificate *data;
    hx509_private_key private_key;
    struct _hx509_cert_attrs attrs;
    hx509_name basename;
    _hx509_cert_release_func release;
    void *ctx;
};

typedef struct hx509_name_constraints {
    NameConstraints *val;
    size_t len;
} hx509_name_constraints;

#define GeneralSubtrees_SET(g,var) \
	(g)->len = (var)->len, (g)->val = (var)->val;

/*
 *
 */

void
_hx509_abort(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    abort();
}

/*
 *
 */

int
hx509_context_init(hx509_context *context)
{
    *context = calloc(1, sizeof(**context));
    if (*context == NULL)
	return ENOMEM;

    _hx509_ks_mem_register(*context);
    _hx509_ks_file_register(*context);
    _hx509_ks_pkcs12_register(*context);
    _hx509_ks_pkcs11_register(*context);
    _hx509_ks_dir_register(*context);

    ENGINE_add_conf_module();
    OpenSSL_add_all_algorithms();

    (*context)->ocsp_time_diff = HX509_DEFAULT_OCSP_TIME_DIFF;

    initialize_hx_error_table_r(&(*context)->et_list);
    initialize_asn1_error_table_r(&(*context)->et_list);

    return 0;
}

void
hx509_context_set_missing_revoke(hx509_context context, int flag)
{
    if (flag)
	context->flags |= HX509_CTX_VERIFY_MISSING_OK;
    else
	context->flags &= ~HX509_CTX_VERIFY_MISSING_OK;
}

void
hx509_context_free(hx509_context *context)
{
    hx509_clear_error_string(*context);
    if ((*context)->ks_ops) {
	free((*context)->ks_ops);
	(*context)->ks_ops = NULL;
    }
    (*context)->ks_num_ops = 0;
    free_error_table ((*context)->et_list);
    free(*context);
    *context = NULL;
}


/*
 *
 */

Certificate *
_hx509_get_cert(hx509_cert cert)
{
    return cert->data;
}

/*
 *
 */

#if 0
void
_hx509_print_cert_subject(hx509_cert cert)
{
    char *subject_name;
    hx509_name name;
    int ret;

    ret = hx509_cert_get_subject(cert, &name);
    if (ret)
	abort();
	
    ret = hx509_name_to_string(name, &subject_name);
    hx509_name_free(&name);
    if (ret)
	abort();

    printf("name: %s\n", subject_name);

    free(subject_name);
}
#endif

/*
 *
 */

int
_hx509_cert_get_version(const Certificate *t)
{
    return t->tbsCertificate.version ? *t->tbsCertificate.version + 1 : 1;
}

int
hx509_cert_init(hx509_context context, const Certificate *c, hx509_cert *cert)
{
    int ret;

    *cert = malloc(sizeof(**cert));
    if (*cert == NULL)
	return ENOMEM;
    (*cert)->ref = 1;
    (*cert)->friendlyname = NULL;
    (*cert)->attrs.len = 0;
    (*cert)->attrs.val = NULL;
    (*cert)->private_key = NULL;
    (*cert)->basename = NULL;
    (*cert)->release = NULL;
    (*cert)->ctx = NULL;

    (*cert)->data = calloc(1, sizeof(*(*cert)->data));
    if ((*cert)->data == NULL) {
	free(*cert);
	return ENOMEM;
    }
    ret = copy_Certificate(c, (*cert)->data);
    if (ret) {
	free((*cert)->data);
	free(*cert);
    }
    return ret;
}

void
_hx509_cert_set_release(hx509_cert cert, 
			_hx509_cert_release_func release,
			void *ctx)
{
    cert->release = release;
    cert->ctx = ctx;
}


/* Doesn't make a copy of `private_key'. */

int
_hx509_cert_assign_key(hx509_cert cert, hx509_private_key private_key)
{
    if (cert->private_key)
	_hx509_private_key_free(&cert->private_key);
    cert->private_key = _hx509_private_key_ref(private_key);
    return 0;
}

void
hx509_cert_free(hx509_cert cert)
{
    int i;

    if (cert == NULL)
	return;

    if (cert->ref <= 0)
	_hx509_abort("refcount <= 0");
    if (--cert->ref > 0)
	return;

    if (cert->release)
	(cert->release)(cert, cert->ctx);

    if (cert->private_key)
	_hx509_private_key_free(&cert->private_key);

    free_Certificate(cert->data);
    free(cert->data);

    for (i = 0; i < cert->attrs.len; i++) {
	der_free_octet_string(&cert->attrs.val[i]->data);
	der_free_oid(&cert->attrs.val[i]->oid);
	free(cert->attrs.val[i]);
    }
    free(cert->attrs.val);
    free(cert->friendlyname);
    if (cert->basename)
	hx509_name_free(&cert->basename);
    memset(cert, 0, sizeof(cert));
    free(cert);
}

hx509_cert
hx509_cert_ref(hx509_cert cert)
{
    if (cert->ref <= 0)
	_hx509_abort("refcount <= 0");
    cert->ref++;
    if (cert->ref == 0)
	_hx509_abort("refcount == 0");
    return cert;
}

int
hx509_verify_init_ctx(hx509_context context, hx509_verify_ctx *ctx)
{
    hx509_verify_ctx c;

    c = calloc(1, sizeof(*c));
    if (c == NULL)
	return ENOMEM;

    c->max_depth = HX509_VERIFY_MAX_DEPTH;

    *ctx = c;
    
    return 0;
}

void
hx509_verify_destroy_ctx(hx509_verify_ctx ctx)
{
    if (ctx)
	memset(ctx, 0, sizeof(*ctx));
    free(ctx);
}

void
hx509_verify_attach_anchors(hx509_verify_ctx ctx, hx509_certs set)
{
    ctx->trust_anchors = set;
}

void
hx509_verify_attach_revoke(hx509_verify_ctx ctx, hx509_revoke_ctx revoke_ctx)
{
    ctx->revoke_ctx = revoke_ctx;
}

void
hx509_verify_set_time(hx509_verify_ctx ctx, time_t t)
{
    ctx->flags |= HX509_VERIFY_CTX_F_TIME_SET;
    ctx->time_now = t;
}

void
hx509_verify_set_proxy_certificate(hx509_verify_ctx ctx, int boolean)
{
    if (boolean)
	ctx->flags |= HX509_VERIFY_CTX_F_ALLOW_PROXY_CERTIFICATE;
    else
	ctx->flags &= ~HX509_VERIFY_CTX_F_ALLOW_PROXY_CERTIFICATE;
}

void
hx509_verify_set_strict_rfc3280_verification(hx509_verify_ctx ctx, int boolean)
{
    if (boolean)
	ctx->flags |= HX509_VERIFY_CTX_F_REQUIRE_RFC3280;
    else
	ctx->flags &= ~HX509_VERIFY_CTX_F_REQUIRE_RFC3280;
}

static const Extension *
find_extension(const Certificate *cert, const heim_oid *oid, int *idx)
{
    const TBSCertificate *c = &cert->tbsCertificate;

    if (c->version == NULL || *c->version < 2 || c->extensions == NULL)
	return NULL;
    
    for (;*idx < c->extensions->len; (*idx)++) {
	if (der_heim_oid_cmp(&c->extensions->val[*idx].extnID, oid) == 0)
	    return &c->extensions->val[(*idx)++];
    }
    return NULL;
}

static int
find_extension_auth_key_id(const Certificate *subject, 
			   AuthorityKeyIdentifier *ai)
{
    const Extension *e;
    size_t size;
    int i = 0;

    memset(ai, 0, sizeof(*ai));

    e = find_extension(subject, oid_id_x509_ce_authorityKeyIdentifier(), &i);
    if (e == NULL)
	return HX509_EXTENSION_NOT_FOUND;
    
    return decode_AuthorityKeyIdentifier(e->extnValue.data, 
					 e->extnValue.length, 
					 ai, &size);
}

int
_hx509_find_extension_subject_key_id(const Certificate *issuer,
				     SubjectKeyIdentifier *si)
{
    const Extension *e;
    size_t size;
    int i = 0;

    memset(si, 0, sizeof(*si));

    e = find_extension(issuer, oid_id_x509_ce_subjectKeyIdentifier(), &i);
    if (e == NULL)
	return HX509_EXTENSION_NOT_FOUND;
    
    return decode_SubjectKeyIdentifier(e->extnValue.data, 
				       e->extnValue.length,
				       si, &size);
}

static int
find_extension_name_constraints(const Certificate *subject, 
				NameConstraints *nc)
{
    const Extension *e;
    size_t size;
    int i = 0;

    memset(nc, 0, sizeof(*nc));

    e = find_extension(subject, oid_id_x509_ce_nameConstraints(), &i);
    if (e == NULL)
	return HX509_EXTENSION_NOT_FOUND;
    
    return decode_NameConstraints(e->extnValue.data, 
				  e->extnValue.length, 
				  nc, &size);
}

static int
find_extension_subject_alt_name(const Certificate *cert, int *i,
				GeneralNames *sa)
{
    const Extension *e;
    size_t size;

    memset(sa, 0, sizeof(*sa));

    e = find_extension(cert, oid_id_x509_ce_subjectAltName(), i);
    if (e == NULL)
	return HX509_EXTENSION_NOT_FOUND;
    
    return decode_GeneralNames(e->extnValue.data, 
			       e->extnValue.length,
			       sa, &size);
}

static int
find_extension_eku(const Certificate *cert, ExtKeyUsage *eku)
{
    const Extension *e;
    size_t size;
    int i = 0;

    memset(eku, 0, sizeof(*eku));

    e = find_extension(cert, oid_id_x509_ce_extKeyUsage(), &i);
    if (e == NULL)
	return HX509_EXTENSION_NOT_FOUND;
    
    return decode_ExtKeyUsage(e->extnValue.data, 
			      e->extnValue.length,
			      eku, &size);
}

static int
add_to_list(hx509_octet_string_list *list, const heim_octet_string *entry)
{
    void *p;
    int ret;

    p = realloc(list->val, (list->len + 1) * sizeof(list->val[0]));
    if (p == NULL)
	return ENOMEM;
    list->val = p;
    ret = der_copy_octet_string(entry, &list->val[list->len]);
    if (ret)
	return ret;
    list->len++;
    return 0;
}

void
hx509_free_octet_string_list(hx509_octet_string_list *list)
{
    int i;
    for (i = 0; i < list->len; i++)
	der_free_octet_string(&list->val[i]);
    free(list->val);
    list->val = NULL;
    list->len = 0;
}

int
hx509_cert_find_subjectAltName_otherName(hx509_cert cert,
					 const heim_oid *oid,
					 hx509_octet_string_list *list)
{
    GeneralNames sa;
    int ret, i, j;

    list->val = NULL;
    list->len = 0;

    i = 0;
    while (1) {
	ret = find_extension_subject_alt_name(_hx509_get_cert(cert), &i, &sa);
	i++;
	if (ret == HX509_EXTENSION_NOT_FOUND) {
	    ret = 0;
	    break;
	} else if (ret != 0)
	    break;


	for (j = 0; j < sa.len; j++) {
	    if (sa.val[j].element == choice_GeneralName_otherName &&
		der_heim_oid_cmp(&sa.val[j].u.otherName.type_id, oid) == 0) 
	    {
		ret = add_to_list(list, &sa.val[j].u.otherName.value);
		if (ret) {
		    free_GeneralNames(&sa);
		    return ret;
		}
	    }
	}
	free_GeneralNames(&sa);
    }
    return ret;
}


static int
check_key_usage(hx509_context context, const Certificate *cert, 
		unsigned flags, int req_present)
{
    const Extension *e;
    KeyUsage ku;
    size_t size;
    int ret, i = 0;
    unsigned ku_flags;

    if (_hx509_cert_get_version(cert) < 3)
	return 0;

    e = find_extension(cert, oid_id_x509_ce_keyUsage(), &i);
    if (e == NULL) {
	if (req_present) {
	    hx509_set_error_string(context, 0, HX509_KU_CERT_MISSING,
				   "Required extension key "
				   "usage missing from certifiate");
	    return HX509_KU_CERT_MISSING;
	}
	return 0;
    }
    
    ret = decode_KeyUsage(e->extnValue.data, e->extnValue.length, &ku, &size);
    if (ret)
	return ret;
    ku_flags = KeyUsage2int(ku);
    if ((ku_flags & flags) != flags) {
	unsigned missing = (~ku_flags) & flags;
	char buf[256], *name;

	unparse_flags(missing, asn1_KeyUsage_units(), buf, sizeof(buf));
	_hx509_unparse_Name(&cert->tbsCertificate.subject, &name);
	hx509_set_error_string(context, 0, HX509_KU_CERT_MISSING,
			       "Key usage %s required but missing "
			       "from certifiate %s", buf, name);
	free(name);
	return HX509_KU_CERT_MISSING;
    }
    return 0;
}

int
_hx509_check_key_usage(hx509_context context, hx509_cert cert, 
		       unsigned flags, int req_present)
{
    return check_key_usage(context, _hx509_get_cert(cert), flags, req_present);
}

enum certtype { PROXY_CERT, EE_CERT, CA_CERT };

static int
check_basic_constraints(hx509_context context, const Certificate *cert, 
			enum certtype type, int depth)
{
    BasicConstraints bc;
    const Extension *e;
    size_t size;
    int ret, i = 0;

    if (_hx509_cert_get_version(cert) < 3)
	return 0;

    e = find_extension(cert, oid_id_x509_ce_basicConstraints(), &i);
    if (e == NULL) {
	switch(type) {
	case PROXY_CERT:
	case EE_CERT:
	    return 0;
	case CA_CERT: {
	    char *name;
	    ret = _hx509_unparse_Name(&cert->tbsCertificate.subject, &name);
	    assert(ret == 0);
	    hx509_set_error_string(context, 0, HX509_EXTENSION_NOT_FOUND,
				   "basicConstraints missing from "
				   "CA certifiacte %s", name);
	    free(name);
	    return HX509_EXTENSION_NOT_FOUND;
	}
	}
    }
    
    ret = decode_BasicConstraints(e->extnValue.data, 
				  e->extnValue.length, &bc,
				  &size);
    if (ret)
	return ret;
    switch(type) {
    case PROXY_CERT:
	if (bc.cA != NULL && *bc.cA)
	    ret = HX509_PARENT_IS_CA;
	break;
    case EE_CERT:
	ret = 0;
	break;
    case CA_CERT:
	if (bc.cA == NULL || !*bc.cA)
	    ret = HX509_PARENT_NOT_CA;
	else if (bc.pathLenConstraint)
	    if (depth - 1 > *bc.pathLenConstraint)
		ret = HX509_CA_PATH_TOO_DEEP;
	break;
    }
    free_BasicConstraints(&bc);
    return ret;
}

int
_hx509_cert_is_parent_cmp(const Certificate *subject,
			  const Certificate *issuer,
			  int allow_self_signed)
{
    int diff;
    AuthorityKeyIdentifier ai;
    SubjectKeyIdentifier si;
    int ret_ai, ret_si;

    diff = _hx509_name_cmp(&issuer->tbsCertificate.subject, 
			   &subject->tbsCertificate.issuer);
    if (diff)
	return diff;
    
    memset(&ai, 0, sizeof(ai));
    memset(&si, 0, sizeof(si));

    /*
     * Try to find AuthorityKeyIdentifier, if its not present in the
     * subject certificate nor the parent.
     */

    ret_ai = find_extension_auth_key_id(subject, &ai);
    if (ret_ai && ret_ai != HX509_EXTENSION_NOT_FOUND)
	return 1;
    ret_si = _hx509_find_extension_subject_key_id(issuer, &si);
    if (ret_si && ret_si != HX509_EXTENSION_NOT_FOUND)
	return -1;

    if (ret_si && ret_ai)
	goto out;
    if (ret_ai)
	goto out;
    if (ret_si) {
	if (allow_self_signed) {
	    diff = 0;
	    goto out;
	} else if (ai.keyIdentifier) {
	    diff = -1;
	    goto out;
	}
    }
    
    if (ai.keyIdentifier == NULL) {
	Name name;

	if (ai.authorityCertIssuer == NULL)
	    return -1;
	if (ai.authorityCertSerialNumber == NULL)
	    return -1;

	diff = der_heim_integer_cmp(ai.authorityCertSerialNumber, 
				    &issuer->tbsCertificate.serialNumber);
	if (diff)
	    return diff;
	if (ai.authorityCertIssuer->len != 1)
	    return -1;
	if (ai.authorityCertIssuer->val[0].element != choice_GeneralName_directoryName)
	    return -1;
	
	name.element = 
	    ai.authorityCertIssuer->val[0].u.directoryName.element;
	name.u.rdnSequence = 
	    ai.authorityCertIssuer->val[0].u.directoryName.u.rdnSequence;

	diff = _hx509_name_cmp(&issuer->tbsCertificate.subject, 
			       &name);
	if (diff)
	    return diff;
	diff = 0;
    } else
	diff = der_heim_octet_string_cmp(ai.keyIdentifier, &si);
    if (diff)
	goto out;

 out:
    free_AuthorityKeyIdentifier(&ai);
    free_SubjectKeyIdentifier(&si);
    return diff;
}

static int
certificate_is_anchor(hx509_context context,
		      hx509_certs trust_anchors,
		      const hx509_cert cert)
{
    hx509_query q;
    hx509_cert c;
    int ret;

    if (trust_anchors == NULL)
	return 0;

    _hx509_query_clear(&q);

    q.match = HX509_QUERY_MATCH_CERTIFICATE;
    q.certificate = _hx509_get_cert(cert);

    ret = hx509_certs_find(context, trust_anchors, &q, &c);
    if (ret == 0)
	hx509_cert_free(c);
    return ret == 0;
}

static int
certificate_is_self_signed(const Certificate *cert)
{
    return _hx509_cert_is_parent_cmp(cert, cert, 1) == 0;
}

/*
 * The subjectName is "null" when its empty set of relative DBs.
 */

static int
subject_null_p(const Certificate *c)
{
    return c->tbsCertificate.subject.u.rdnSequence.len == 0;
}


static int
find_parent(hx509_context context,
	    time_t time_now,
	    hx509_certs trust_anchors,
	    hx509_path *path,
	    hx509_certs pool, 
	    hx509_cert current,
	    hx509_cert *parent)
{
    AuthorityKeyIdentifier ai;
    hx509_query q;
    int ret;

    *parent = NULL;
    memset(&ai, 0, sizeof(ai));
    
    _hx509_query_clear(&q);

    if (!subject_null_p(current->data)) {
	q.match |= HX509_QUERY_FIND_ISSUER_CERT;
	q.subject = _hx509_get_cert(current);
    } else {
	ret = find_extension_auth_key_id(current->data, &ai);
	if (ret) {
	    hx509_set_error_string(context, 0, HX509_CERTIFICATE_MALFORMED,
				   "Subjectless certificate missing AuthKeyID");
	    return HX509_CERTIFICATE_MALFORMED;
	}

	if (ai.keyIdentifier == NULL) {
	    free_AuthorityKeyIdentifier(&ai);
	    hx509_set_error_string(context, 0, HX509_CERTIFICATE_MALFORMED,
				   "Subjectless certificate missing keyIdentifier "
				   "inside AuthKeyID");
	    return HX509_CERTIFICATE_MALFORMED;
	}

	q.subject_id = ai.keyIdentifier;
	q.match = HX509_QUERY_MATCH_SUBJECT_KEY_ID;
    }

    q.path = path;
    q.match |= HX509_QUERY_NO_MATCH_PATH;

    if (pool) {
	q.timenow = time_now;
	q.match |= HX509_QUERY_MATCH_TIME;

	ret = hx509_certs_find(context, pool, &q, parent);
	if (ret == 0) {
	    free_AuthorityKeyIdentifier(&ai);
	    return 0;
	}
	q.match &= ~HX509_QUERY_MATCH_TIME;
    }

    if (trust_anchors) {
	ret = hx509_certs_find(context, trust_anchors, &q, parent);
	if (ret == 0) {
	    free_AuthorityKeyIdentifier(&ai);
	    return ret;
	}
    }
    free_AuthorityKeyIdentifier(&ai);

    {
	hx509_name name;
	char *str;

	ret = hx509_cert_get_subject(current, &name);
	if (ret) {
	    hx509_clear_error_string(context);
	    return HX509_ISSUER_NOT_FOUND;
	}
	ret = hx509_name_to_string(name, &str);
	hx509_name_free(&name);
	if (ret) {
	    hx509_clear_error_string(context);
	    return HX509_ISSUER_NOT_FOUND;
	}
	
	hx509_set_error_string(context, 0, HX509_ISSUER_NOT_FOUND,
			       "Failed to find issuer for "
			       "certificate with subject: %s", str);
	free(str);
    }
    return HX509_ISSUER_NOT_FOUND;
}

/*
 *
 */

static int
is_proxy_cert(hx509_context context, 
	      const Certificate *cert, 
	      ProxyCertInfo *rinfo)
{
    ProxyCertInfo info;
    const Extension *e;
    size_t size;
    int ret, i = 0;

    if (rinfo)
	memset(rinfo, 0, sizeof(*rinfo));

    e = find_extension(cert, oid_id_pe_proxyCertInfo(), &i);
    if (e == NULL) {
	hx509_clear_error_string(context);
	return HX509_EXTENSION_NOT_FOUND;
    }

    ret = decode_ProxyCertInfo(e->extnValue.data, 
			       e->extnValue.length, 
			       &info,
			       &size);
    if (ret) {
	hx509_clear_error_string(context);
	return ret;
    }
    if (size != e->extnValue.length) {
	free_ProxyCertInfo(&info);
	hx509_clear_error_string(context);
	return HX509_EXTRA_DATA_AFTER_STRUCTURE; 
    }
    if (rinfo == NULL)
	free_ProxyCertInfo(&info);
    else
	*rinfo = info;

    return 0;
}

/*
 * Path operations are like MEMORY based keyset, but with exposed
 * internal so we can do easy searches.
 */

int
_hx509_path_append(hx509_context context, hx509_path *path, hx509_cert cert)
{
    hx509_cert *val;
    val = realloc(path->val, (path->len + 1) * sizeof(path->val[0]));
    if (val == NULL) {
	hx509_set_error_string(context, 0, ENOMEM, "out of memory");
	return ENOMEM;
    }

    path->val = val;
    path->val[path->len] = hx509_cert_ref(cert);
    path->len++;

    return 0;
}

void
_hx509_path_free(hx509_path *path)
{
    unsigned i;
    
    for (i = 0; i < path->len; i++)
	hx509_cert_free(path->val[i]);
    free(path->val);
    path->val = NULL;
    path->len = 0;
}

/*
 * Find path by looking up issuer for the top certificate and continue
 * until an anchor certificate is found or max limit is found. A
 * certificate never included twice in the path.
 *
 * If the trust anchors are not given, calculate optimistic path, just
 * follow the chain upward until we no longer find a parent or we hit
 * the max path limit. In this case, a failure will always be returned
 * depending on what error condition is hit first.
 *
 * The path includes a path from the top certificate to the anchor
 * certificate.
 *
 * The caller needs to free `path� both on successful built path and
 * failure.
 */

int
_hx509_calculate_path(hx509_context context,
		      int flags,
		      time_t time_now,
		      hx509_certs anchors,
		      unsigned int max_depth,
		      hx509_cert cert,
		      hx509_certs pool,
		      hx509_path *path)
{
    hx509_cert parent, current;
    int ret;

    if (max_depth == 0)
	max_depth = HX509_VERIFY_MAX_DEPTH;

    ret = _hx509_path_append(context, path, cert);
    if (ret)
	return ret;

    current = hx509_cert_ref(cert);

    while (!certificate_is_anchor(context, anchors, current)) {

	ret = find_parent(context, time_now, anchors, path, 
			  pool, current, &parent);
	hx509_cert_free(current);
	if (ret)
	    return ret;

	ret = _hx509_path_append(context, path, parent);
	if (ret)
	    return ret;
	current = parent;

	if (path->len > max_depth) {
	    hx509_set_error_string(context, 0, HX509_PATH_TOO_LONG,
				   "Path too long while bulding certificate chain");
	    return HX509_PATH_TOO_LONG;
	}
    }

    if ((flags & HX509_CALCULATE_PATH_NO_ANCHOR) && 
	path->len > 0 && 
	certificate_is_anchor(context, anchors, path->val[path->len - 1]))
    {
	hx509_cert_free(path->val[path->len - 1]);
	path->len--;
    }

    hx509_cert_free(current);
    return 0;
}

static int
AlgorithmIdentifier_cmp(const AlgorithmIdentifier *p,
			const AlgorithmIdentifier *q)
{
    int diff;
    diff = der_heim_oid_cmp(&p->algorithm, &q->algorithm);
    if (diff)
	return diff;
    if (p->parameters) {
	if (q->parameters)
	    return heim_any_cmp(p->parameters,
				q->parameters);
	else
	    return 1;
    } else {
	if (q->parameters)
	    return -1;
	else
	    return 0;
    }
}

int
_hx509_Certificate_cmp(const Certificate *p, const Certificate *q)
{
    int diff;
    diff = der_heim_bit_string_cmp(&p->signatureValue, &q->signatureValue);
    if (diff)
	return diff;
    diff = AlgorithmIdentifier_cmp(&p->signatureAlgorithm, 
				   &q->signatureAlgorithm);
    if (diff)
	return diff;
    diff = der_heim_octet_string_cmp(&p->tbsCertificate._save,
				     &q->tbsCertificate._save);
    return diff;
}

int
hx509_cert_cmp(hx509_cert p, hx509_cert q)
{
    return _hx509_Certificate_cmp(p->data, q->data);
}

int
hx509_cert_get_issuer(hx509_cert p, hx509_name *name)
{
    return _hx509_name_from_Name(&p->data->tbsCertificate.issuer, name);
}

int
hx509_cert_get_subject(hx509_cert p, hx509_name *name)
{
    return _hx509_name_from_Name(&p->data->tbsCertificate.subject, name);
}

int
hx509_cert_get_base_subject(hx509_context context, hx509_cert c,
			    hx509_name *name)
{
    if (c->basename)
	return hx509_name_copy(context, c->basename, name);
    if (is_proxy_cert(context, c->data, NULL) == 0) {
	int ret = HX509_PROXY_CERTIFICATE_NOT_CANONICALIZED;
	hx509_set_error_string(context, 0, ret,
			       "Proxy certificate have not been "
			       "canonicalize yet, no base name");
	return ret;
    }
    return _hx509_name_from_Name(&c->data->tbsCertificate.subject, name);
}

int
hx509_cert_get_serialnumber(hx509_cert p, heim_integer *i)
{
    return der_copy_heim_integer(&p->data->tbsCertificate.serialNumber, i);
}

time_t
hx509_cert_get_notBefore(hx509_cert p)
{
    return _hx509_Time2time_t(&p->data->tbsCertificate.validity.notBefore);
}

time_t
hx509_cert_get_notAfter(hx509_cert p)
{
    return _hx509_Time2time_t(&p->data->tbsCertificate.validity.notAfter);
}

int
hx509_cert_get_SPKI(hx509_cert p, SubjectPublicKeyInfo *spki)
{
    return copy_SubjectPublicKeyInfo(&p->data->tbsCertificate.subjectPublicKeyInfo,
				     spki);
}

hx509_private_key
_hx509_cert_private_key(hx509_cert p)
{
    return p->private_key;
}

int
_hx509_cert_private_key_exportable(hx509_cert p)
{
    if (p->private_key == NULL)
	return 0;
    return _hx509_private_key_exportable(p->private_key);
}

int
_hx509_cert_private_decrypt(hx509_context context,
			    const heim_octet_string *ciphertext,
			    const heim_oid *encryption_oid,
			    hx509_cert p,
			    heim_octet_string *cleartext)
{
    cleartext->data = NULL;
    cleartext->length = 0;

    if (p->private_key == NULL) {
	hx509_set_error_string(context, 0, HX509_PRIVATE_KEY_MISSING,
			       "Private key missing");
	return HX509_PRIVATE_KEY_MISSING;
    }

    return _hx509_private_key_private_decrypt(context,
					      ciphertext,
					      encryption_oid,
					      p->private_key, 
					      cleartext);
}

int
_hx509_cert_public_encrypt(hx509_context context,
			   const heim_octet_string *cleartext,
			   const hx509_cert p,
			   heim_oid *encryption_oid,
			   heim_octet_string *ciphertext)
{
    return _hx509_public_encrypt(context,
				 cleartext, p->data,
				 encryption_oid, ciphertext);
}

/*
 *
 */

time_t
_hx509_Time2time_t(const Time *t)
{
    switch(t->element) {
    case choice_Time_utcTime:
	return t->u.utcTime;
    case choice_Time_generalTime:
	return t->u.generalTime;
    }
    return 0;
}

/*
 *
 */

static int
init_name_constraints(hx509_name_constraints *nc)
{
    memset(nc, 0, sizeof(*nc));
    return 0;
}

static int
add_name_constraints(hx509_context context, const Certificate *c, int not_ca,
		     hx509_name_constraints *nc)
{
    NameConstraints tnc;
    int ret;

    ret = find_extension_name_constraints(c, &tnc);
    if (ret == HX509_EXTENSION_NOT_FOUND)
	return 0;
    else if (ret) {
	hx509_set_error_string(context, 0, ret, "Failed getting NameConstraints");
	return ret;
    } else if (not_ca) {
	ret = HX509_VERIFY_CONSTRAINTS;
	hx509_set_error_string(context, 0, ret, "Not a CA and "
			       "have NameConstraints");
    } else {
	NameConstraints *val;
	val = realloc(nc->val, sizeof(nc->val[0]) * (nc->len + 1));
	if (val == NULL) {
	    hx509_clear_error_string(context);
	    ret = ENOMEM;
	    goto out;
	}
	nc->val = val;
	ret = copy_NameConstraints(&tnc, &nc->val[nc->len]);
	if (ret) {
	    hx509_clear_error_string(context);
	    goto out;
	}
	nc->len += 1;
    }
out:
    free_NameConstraints(&tnc);
    return ret;
}

static int
match_RDN(const RelativeDistinguishedName *c,
	  const RelativeDistinguishedName *n)
{
    int i;

    if (c->len != n->len)
	return HX509_NAME_CONSTRAINT_ERROR;
    
    for (i = 0; i < n->len; i++) {
	if (der_heim_oid_cmp(&c->val[i].type, &n->val[i].type) != 0)
	    return HX509_NAME_CONSTRAINT_ERROR;
	if (_hx509_name_ds_cmp(&c->val[i].value, &n->val[i].value) != 0)
	    return HX509_NAME_CONSTRAINT_ERROR;
    }
    return 0;
}

static int
match_X501Name(const Name *c, const Name *n)
{
    int i, ret;

    if (c->element != choice_Name_rdnSequence
	|| n->element != choice_Name_rdnSequence)
	return 0;
    if (c->u.rdnSequence.len > n->u.rdnSequence.len)
	return HX509_NAME_CONSTRAINT_ERROR;
    for (i = 0; i < c->u.rdnSequence.len; i++) {
	ret = match_RDN(&c->u.rdnSequence.val[i], &n->u.rdnSequence.val[i]);
	if (ret)
	    return ret;
    }
    return 0;
} 


static int
match_general_name(const GeneralName *c, const GeneralName *n, int *match)
{
    /* 
     * Name constraints only apply to the same name type, see RFC3280,
     * 4.2.1.11.
     */
    assert(c->element == n->element);

    switch(c->element) {
    case choice_GeneralName_otherName:
	if (der_heim_oid_cmp(&c->u.otherName.type_id,
			 &n->u.otherName.type_id) != 0)
	    return HX509_NAME_CONSTRAINT_ERROR;
	if (heim_any_cmp(&c->u.otherName.value,
			 &n->u.otherName.value) != 0)
	    return HX509_NAME_CONSTRAINT_ERROR;
	*match = 1;
	return 0;
    case choice_GeneralName_rfc822Name: {
	const char *s;
	size_t len1, len2;
	s = strchr(c->u.rfc822Name, '@');
	if (s) {
	    if (strcasecmp(c->u.rfc822Name, n->u.rfc822Name) != 0)
		return HX509_NAME_CONSTRAINT_ERROR;
	} else {
	    s = strchr(n->u.rfc822Name, '@');
	    if (s == NULL)
		return HX509_NAME_CONSTRAINT_ERROR;
	    len1 = strlen(c->u.rfc822Name);
	    len2 = strlen(s + 1);
	    if (len1 > len2)
		return HX509_NAME_CONSTRAINT_ERROR;
	    if (strcasecmp(s + 1 + len2 - len1, c->u.rfc822Name) != 0)
		return HX509_NAME_CONSTRAINT_ERROR;
	    if (len1 < len2 && s[len2 - len1] != '.')
		return HX509_NAME_CONSTRAINT_ERROR;
	}
	*match = 1;
	return 0;
    }
    case choice_GeneralName_dNSName: {
	size_t len1, len2;

	len1 = strlen(c->u.dNSName);
	len2 = strlen(n->u.dNSName);
	if (len1 > len2)
	    return HX509_NAME_CONSTRAINT_ERROR;
	if (strcasecmp(&n->u.dNSName[len2 - len1], c->u.dNSName) != 0)
	    return HX509_NAME_CONSTRAINT_ERROR;
	*match = 1;
	return 0;
    }
    case choice_GeneralName_directoryName: {
	Name c_name, n_name;
	int ret;

	c_name._save.data = NULL;
	c_name._save.length = 0;
	c_name.element = c->u.directoryName.element;
	c_name.u.rdnSequence = c->u.directoryName.u.rdnSequence;

	n_name._save.data = NULL;
	n_name._save.length = 0;
	n_name.element = n->u.directoryName.element;
	n_name.u.rdnSequence = n->u.directoryName.u.rdnSequence;

	ret = match_X501Name(&c_name, &n_name);
	if (ret == 0)
	    *match = 1;
	return ret;
    }
    case choice_GeneralName_uniformResourceIdentifier:
    case choice_GeneralName_iPAddress:
    case choice_GeneralName_registeredID:
    default:
	return HX509_NAME_CONSTRAINT_ERROR;
    }
}

static int
match_alt_name(const GeneralName *n, const Certificate *c, 
	       int *same, int *match)
{
    GeneralNames sa;
    int ret, i, j;

    i = 0;
    do {
	ret = find_extension_subject_alt_name(c, &i, &sa);
	if (ret == HX509_EXTENSION_NOT_FOUND) {
	    ret = 0;
	    break;
	} else if (ret != 0)
	    break;

	for (j = 0; j < sa.len; j++) {
	    if (n->element == sa.val[j].element) {
		*same = 1;
		ret = match_general_name(n, &sa.val[j], match);
	    }
	}
	free_GeneralNames(&sa);
    } while (1);

    return ret;
}


static int
match_tree(const GeneralSubtrees *t, const Certificate *c, int *match)
{
    int name, alt_name, same;
    unsigned int i;
    int ret = 0;

    name = alt_name = same = *match = 0;
    for (i = 0; i < t->len; i++) {
	if (t->val[i].minimum && t->val[i].maximum)
	    return HX509_RANGE;

	/*
	 * If the constraint apply to directoryNames, test is with
	 * subjectName of the certificate if the certificate have a
	 * non-null (empty) subjectName.
	 */

	if (t->val[i].base.element == choice_GeneralName_directoryName
	    && !subject_null_p(c))
	{
	    GeneralName certname;
	    
	    
	    certname.element = choice_GeneralName_directoryName;
	    certname.u.directoryName.element = 
		c->tbsCertificate.subject.element;
	    certname.u.directoryName.u.rdnSequence = 
		c->tbsCertificate.subject.u.rdnSequence;
    
	    ret = match_general_name(&t->val[i].base, &certname, &name);
	}

	/* Handle subjectAltNames, this is icky since they
	 * restrictions only apply if the subjectAltName is of the
	 * same type. So if there have been a match of type, require
	 * altname to be set.
	 */
	ret = match_alt_name(&t->val[i].base, c, &same, &alt_name);
    }
    if (name && (!same || alt_name))
	*match = 1;
    return ret;
}

static int
check_name_constraints(hx509_context context, 
		       const hx509_name_constraints *nc,
		       const Certificate *c)
{
    int match, ret;
    int i;

    for (i = 0 ; i < nc->len; i++) {
	GeneralSubtrees gs;

	if (nc->val[i].permittedSubtrees) {
	    GeneralSubtrees_SET(&gs, nc->val[i].permittedSubtrees);
	    ret = match_tree(&gs, c, &match);
	    if (ret) {
		hx509_clear_error_string(context);
		return ret;
	    }
	    /* allow null subjectNames, they wont matches anything */
	    if (match == 0 && !subject_null_p(c)) {
		hx509_clear_error_string(context);
		return HX509_VERIFY_CONSTRAINTS;
	    }
	}
	if (nc->val[i].excludedSubtrees) {
	    GeneralSubtrees_SET(&gs, nc->val[i].excludedSubtrees);
	    ret = match_tree(&gs, c, &match);
	    if (ret) {
		hx509_clear_error_string(context);
		return ret;
	    }
	    if (match) {
		hx509_clear_error_string(context);
		return HX509_VERIFY_CONSTRAINTS;
	    }
	}
    }
    return 0;
}

static void
free_name_constraints(hx509_name_constraints *nc)
{
    int i;

    for (i = 0 ; i < nc->len; i++)
	free_NameConstraints(&nc->val[i]);
    free(nc->val);
}

int
hx509_verify_path(hx509_context context,
		  hx509_verify_ctx ctx,
		  hx509_cert cert,
		  hx509_certs pool)
{
    hx509_name_constraints nc;
    hx509_path path;
#if 0
    const AlgorithmIdentifier *alg_id;
#endif
    int ret, i, proxy_cert_depth;
    enum certtype type;
    Name proxy_issuer;

    memset(&proxy_issuer, 0, sizeof(proxy_issuer));

    ret = init_name_constraints(&nc);
    if (ret)	
	return ret;

    path.val = NULL;
    path.len = 0;

    if ((ctx->flags & HX509_VERIFY_CTX_F_TIME_SET) == 0)
	ctx->time_now = time(NULL);

    /*
     * Calculate the path from the certificate user presented to the
     * to an anchor.
     */
    ret = _hx509_calculate_path(context, 0, ctx->time_now,
				ctx->trust_anchors, ctx->max_depth,
				cert, pool, &path);
    if (ret)
	goto out;

#if 0
    alg_id = path.val[path->len - 1]->data->tbsCertificate.signature;
#endif

    /*
     * Check CA and proxy certificate chain from the top of the
     * certificate chain. Also check certificate is valid with respect
     * to the current time.
     *
     */

    proxy_cert_depth = 0;

    if (ctx->flags & HX509_VERIFY_CTX_F_ALLOW_PROXY_CERTIFICATE)
	type = PROXY_CERT;
    else
	type = EE_CERT;

    for (i = 0; i < path.len; i++) {
	Certificate *c;
	time_t t;

	c = _hx509_get_cert(path.val[i]);
	
	/*
	 * Lets do some basic check on issuer like
	 * keyUsage.keyCertSign and basicConstraints.cA bit depending
	 * on what type of certificate this is.
	 */

	switch (type) {
	case CA_CERT:
	    /* XXX make constants for keyusage */
	    ret = check_key_usage(context, c, 1 << 5,
				  REQUIRE_RFC3280(ctx) ? TRUE : FALSE);
	    if (ret) {
		hx509_set_error_string(context, HX509_ERROR_APPEND, ret,
				       "Key usage missing from CA certificate");
		goto out;
	    }
	    break;
	case PROXY_CERT: {
	    ProxyCertInfo info;	    

	    if (is_proxy_cert(context, c, &info) == 0) {
		int j;

		if (info.pCPathLenConstraint != NULL &&
		    *info.pCPathLenConstraint < i)
		{
		    free_ProxyCertInfo(&info);
		    ret = HX509_PATH_TOO_LONG;
		    hx509_set_error_string(context, 0, ret,
					   "Proxy certificate chain "
					   "longer then allowed");
		    goto out;
		}
		/* XXX MUST check info.proxyPolicy */
		free_ProxyCertInfo(&info);
		
		j = 0;
		if (find_extension(c, oid_id_x509_ce_subjectAltName(), &j)) {
		    ret = HX509_PROXY_CERT_INVALID;
		    hx509_set_error_string(context, 0, ret, 
					   "Proxy certificate have explicity "
					   "forbidden subjectAltName");
		    goto out;
		}

		j = 0;
		if (find_extension(c, oid_id_x509_ce_issuerAltName(), &j)) {
		    ret = HX509_PROXY_CERT_INVALID;
		    hx509_set_error_string(context, 0, ret, 
					   "Proxy certificate have explicity "
					   "forbidden issuerAltName");
		    goto out;
		}
			
		/* 
		 * The subject name of the proxy certificate should be
		 * CN=XXX,<proxy issuer>, prune of CN and check if its
		 * the same over the whole chain of proxy certs and
		 * then check with the EE cert when we get to it.
		 */

		if (proxy_cert_depth) {
		    ret = _hx509_name_cmp(&proxy_issuer, &c->tbsCertificate.subject);
		    if (ret) {
			ret = HX509_PROXY_CERT_NAME_WRONG;
			hx509_set_error_string(context, 0, ret,
					       "Base proxy name not right");
			goto out;
		    }
		}

		free_Name(&proxy_issuer);

		ret = copy_Name(&c->tbsCertificate.subject, &proxy_issuer);
		if (ret) {
		    hx509_clear_error_string(context);
		    goto out;
		}

		j = proxy_issuer.u.rdnSequence.len;
		if (proxy_issuer.u.rdnSequence.len < 2 
		    || proxy_issuer.u.rdnSequence.val[j - 1].len > 1
		    || der_heim_oid_cmp(&proxy_issuer.u.rdnSequence.val[j - 1].val[0].type,
					oid_id_at_commonName()))
		{
		    ret = HX509_PROXY_CERT_NAME_WRONG;
		    hx509_set_error_string(context, 0, ret,
					   "Proxy name too short or "
					   "does not have Common name "
					   "at the top");
		    goto out;
		}

		free_RelativeDistinguishedName(&proxy_issuer.u.rdnSequence.val[j - 1]);
		proxy_issuer.u.rdnSequence.len -= 1;

		ret = _hx509_name_cmp(&proxy_issuer, &c->tbsCertificate.issuer);
		if (ret != 0) {
		    ret = HX509_PROXY_CERT_NAME_WRONG;
		    hx509_set_error_string(context, 0, ret,
					   "Proxy issuer name not as expected");
		    goto out;
		}

		break;
	    } else {
		/* 
		 * Now we are done with the proxy certificates, this
		 * cert was an EE cert and we we will fall though to
		 * EE checking below.
		 */
		type = EE_CERT;
		/* FALLTHOUGH */
	    }
	}
	case EE_CERT:
	    /*
	     * If there where any proxy certificates in the chain
	     * (proxy_cert_depth > 0), check that the proxy issuer
	     * matched proxy certificates "base" subject.
	     */
	    if (proxy_cert_depth) {

		ret = _hx509_name_cmp(&proxy_issuer,
				      &c->tbsCertificate.subject);
		if (ret) {
		    ret = HX509_PROXY_CERT_NAME_WRONG;
		    hx509_clear_error_string(context);
		    goto out;
		}
		if (cert->basename)
		    hx509_name_free(&cert->basename);
		
		ret = _hx509_name_from_Name(&proxy_issuer, &cert->basename);
		if (ret) {
		    hx509_clear_error_string(context);
		    goto out;
		}
	    }

	    break;
	}

	ret = check_basic_constraints(context, c, type, i - proxy_cert_depth);
	if (ret)
	    goto out;
	    
	/*
	 * Don't check the trust anchors expiration time since they
	 * are transported out of band, from RFC3820.
	 */
	if (i + 1 != path.len || CHECK_TA(ctx)) {

	    t = _hx509_Time2time_t(&c->tbsCertificate.validity.notBefore);
	    if (t > ctx->time_now) {
		ret = HX509_CERT_USED_BEFORE_TIME;
		hx509_clear_error_string(context);
		goto out;
	    }
	    t = _hx509_Time2time_t(&c->tbsCertificate.validity.notAfter);
	    if (t < ctx->time_now) {
		ret = HX509_CERT_USED_AFTER_TIME;
		hx509_clear_error_string(context);
		goto out;
	    }
	}

	if (type == EE_CERT)
	    type = CA_CERT;
	else if (type == PROXY_CERT)
	    proxy_cert_depth++;
    }

    /*
     * Verify constraints, do this backward so path constraints are
     * checked in the right order.
     */

    for (ret = 0, i = path.len - 1; i >= 0; i--) {
	Certificate *c;

	c = _hx509_get_cert(path.val[i]);

#if 0
	/* check that algorithm and parameters is the same */
	/* XXX this is wrong */
	ret = alg_cmp(&c->tbsCertificate.signature, alg_id);
	if (ret) {
	    hx509_clear_error_string(context);
	    ret = HX509_PATH_ALGORITHM_CHANGED;
	    goto out;
	}
#endif

	/* verify name constraints, not for selfsigned and anchor */
	if (!certificate_is_self_signed(c) || i == path.len - 1) {
	    ret = check_name_constraints(context, &nc, c);
	    if (ret) {
		goto out;
	    }
	}
	ret = add_name_constraints(context, c, i == 0, &nc);
	if (ret)
	    goto out;

	/* XXX verify all other silly constraints */

    }

    /*
     * Verify that no certificates has been revoked.
     */

    if (ctx->revoke_ctx) {
	hx509_certs certs;

	ret = hx509_certs_init(context, "MEMORY:revoke-certs", 0,
			       NULL, &certs);
	if (ret)
	    goto out;

	for (i = 0; i < path.len; i++) {
	    ret = hx509_certs_add(context, certs, path.val[i]);
	    if (ret) {
		hx509_certs_free(&certs);
		goto out;
	    }
	}
	ret = hx509_certs_merge(context, certs, pool);
	if (ret) {
	    hx509_certs_free(&certs);
	    goto out;
	}

	for (i = 0; i < path.len - 1; i++) {
	    int parent = (i < path.len - 1) ? i + 1 : i;

	    ret = hx509_revoke_verify(context,
				      ctx->revoke_ctx, 
				      certs,
				      ctx->time_now,
				      path.val[i],
				      path.val[parent]);
	    if (ret) {
		hx509_certs_free(&certs);
		goto out;
	    }
	}
	hx509_certs_free(&certs);
    }

#if 0
    for (i = path.len - 1; i >= 0; i--) {
	_hx509_print_cert_subject(path.val[i]);
    }
#endif

    /*
     * Verify signatures, do this backward so public key working
     * parameter is passed up from the anchor up though the chain.
     */

    for (i = path.len - 1; i >= 0; i--) {
	Certificate *signer, *c;

	c = _hx509_get_cert(path.val[i]);

	/* is last in chain (trust anchor) */
	if (i == path.len - 1) {
	    signer = path.val[i]->data;

	    /* if trust anchor is not self signed, don't check sig */
	    if (!certificate_is_self_signed(signer))
		continue;
	} else {
	    /* take next certificate in chain */
	    signer = path.val[i + 1]->data;
	}

	/* verify signatureValue */
	ret = _hx509_verify_signature_bitstring(context,
						signer,
						&c->signatureAlgorithm,
						&c->tbsCertificate._save,
						&c->signatureValue);
	if (ret) {
	    hx509_set_error_string(context, HX509_ERROR_APPEND, ret,
				   "Failed to verify signature of certificate");
	    goto out;
	}
    }

out:
    free_Name(&proxy_issuer);
    free_name_constraints(&nc);
    _hx509_path_free(&path);

    return ret;
}

int
hx509_verify_signature(hx509_context context,
		       const hx509_cert signer,
		       const AlgorithmIdentifier *alg,
		       const heim_octet_string *data,
		       const heim_octet_string *sig)
{
    return _hx509_verify_signature(context, signer->data, alg, data, sig);
}

int
hx509_verify_hostname(hx509_context context,
		      const hx509_cert cert,
		      int require_match,
		      const char *hostname,
		      const struct sockaddr *sa,
		      /* XXX krb5_socklen_t */ int sa_size) 
{
    if (sa && sa_size <= 0)
	return EINVAL;
    return 0;
}

int
_hx509_set_cert_attribute(hx509_context context,
			  hx509_cert cert, 
			  const heim_oid *oid, 
			  const heim_octet_string *attr)
{
    hx509_cert_attribute a;
    void *d;

    if (hx509_cert_get_attribute(cert, oid) != NULL)
	return 0;

    d = realloc(cert->attrs.val, 
		sizeof(cert->attrs.val[0]) * (cert->attrs.len + 1));
    if (d == NULL) {
	hx509_clear_error_string(context);
	return ENOMEM;
    }
    cert->attrs.val = d;

    a = malloc(sizeof(*a));
    if (a == NULL)
	return ENOMEM;

    der_copy_octet_string(attr, &a->data);
    der_copy_oid(oid, &a->oid);
    
    cert->attrs.val[cert->attrs.len] = a;
    cert->attrs.len++;

    return 0;
}

hx509_cert_attribute
hx509_cert_get_attribute(hx509_cert cert, const heim_oid *oid)
{
    int i;
    for (i = 0; i < cert->attrs.len; i++)
	if (der_heim_oid_cmp(oid, &cert->attrs.val[i]->oid) == 0)
	    return cert->attrs.val[i];
    return NULL;
}

int
hx509_cert_set_friendly_name(hx509_cert cert, const char *name)
{
    if (cert->friendlyname)
	free(cert->friendlyname);
    cert->friendlyname = strdup(name);
    if (cert->friendlyname == NULL)
	return ENOMEM;
    return 0;
}


const char *
hx509_cert_get_friendly_name(hx509_cert cert)
{
    hx509_cert_attribute a;
    PKCS9_friendlyName n;
    size_t sz;
    int ret, i;

    if (cert->friendlyname)
	return cert->friendlyname;

    a = hx509_cert_get_attribute(cert, oid_id_pkcs_9_at_friendlyName());
    if (a == NULL) {
	/* XXX use subject name ? */
	return NULL; 
    }

    ret = decode_PKCS9_friendlyName(a->data.data, a->data.length, &n, &sz);
    if (ret)
	return NULL;
	
    if (n.len != 1) {
	free_PKCS9_friendlyName(&n);
	return NULL;
    }
    
    cert->friendlyname = malloc(n.val[0].length + 1);
    if (cert->friendlyname == NULL) {
	free_PKCS9_friendlyName(&n);
	return NULL;
    }
    
    for (i = 0; i < n.val[0].length; i++) {
	if (n.val[0].data[i] <= 0xff)
	    cert->friendlyname[i] = n.val[0].data[i] & 0xff;
	else
	    cert->friendlyname[i] = 'X';
    }
    cert->friendlyname[i] = '\0';
    free_PKCS9_friendlyName(&n);

    return cert->friendlyname;
}

void
_hx509_query_clear(hx509_query *q)
{
    memset(q, 0, sizeof(*q));
}

int
hx509_query_alloc(hx509_context context, hx509_query **q)
{
    *q = calloc(1, sizeof(**q));
    if (*q == NULL)
	return ENOMEM;
    return 0;
}

void
hx509_query_match_option(hx509_query *q, hx509_query_option option)
{
    switch(option) {
    case HX509_QUERY_OPTION_PRIVATE_KEY:
	q->match |= HX509_QUERY_PRIVATE_KEY;
	break;
    case HX509_QUERY_OPTION_KU_ENCIPHERMENT:
	q->match |= HX509_QUERY_KU_ENCIPHERMENT;
	break;
    case HX509_QUERY_OPTION_KU_DIGITALSIGNATURE:
	q->match |= HX509_QUERY_KU_DIGITALSIGNATURE;
	break;
    case HX509_QUERY_OPTION_KU_KEYCERTSIGN:
	q->match |= HX509_QUERY_KU_KEYCERTSIGN;
	break;
    case HX509_QUERY_OPTION_END:
    default:
	break;
    }
}

int
hx509_query_match_issuer_serial(hx509_query *q,
				const Name *issuer, 
				const heim_integer *serialNumber)
{
    int ret;
    if (q->serial) {
	der_free_heim_integer(q->serial);
	free(q->serial);
    }
    q->serial = malloc(sizeof(*q->serial));
    if (q->serial == NULL)
	return ENOMEM;
    ret = der_copy_heim_integer(serialNumber, q->serial);
    if (ret) {
	free(q->serial);
	q->serial = NULL;
	return ret;
    }
    if (q->issuer_name) {
	free_Name(q->issuer_name);
	free(q->issuer_name);
    }
    q->issuer_name = malloc(sizeof(*q->issuer_name));
    if (q->issuer_name == NULL)
	return ENOMEM;
    ret = copy_Name(issuer, q->issuer_name);
    if (ret) {
	free(q->issuer_name);
	q->issuer_name = NULL;
	return ret;
    }
    q->match |= HX509_QUERY_MATCH_SERIALNUMBER|HX509_QUERY_MATCH_ISSUER_NAME;
    return 0;
}


int
hx509_query_match_friendly_name(hx509_query *q, const char *name)
{
    if (q->friendlyname)
	free(q->friendlyname);
    q->friendlyname = strdup(name);
    if (q->friendlyname == NULL)
	return ENOMEM;
    q->match |= HX509_QUERY_MATCH_FRIENDLY_NAME;
    return 0;
}

int
hx509_query_match_cmp_func(hx509_query *q,
			   int (*func)(void *, hx509_cert),
			   void *ctx)
{
    if (func)
	q->match |= HX509_QUERY_MATCH_FUNCTION;
    else
	q->match &= ~HX509_QUERY_MATCH_FUNCTION;
    q->cmp_func = func;
    q->cmp_func_ctx = ctx;
    return 0;
}


void
hx509_query_free(hx509_context context, hx509_query *q)
{
    if (q->serial) {
	der_free_heim_integer(q->serial);
	free(q->serial);
	q->serial = NULL;
    }
    if (q->issuer_name) {
	free_Name(q->issuer_name);
	free(q->issuer_name);
	q->issuer_name = NULL;
    }
    if (q) {
	free(q->friendlyname);
	memset(q, 0, sizeof(*q));
    }
    free(q);
}

int
_hx509_query_match_cert(hx509_context context, const hx509_query *q, hx509_cert cert)
{
    Certificate *c = _hx509_get_cert(cert);

    if ((q->match & HX509_QUERY_FIND_ISSUER_CERT) &&
	_hx509_cert_is_parent_cmp(q->subject, c, 0) != 0)
	return 0;

    if ((q->match & HX509_QUERY_MATCH_CERTIFICATE) &&
	_hx509_Certificate_cmp(q->certificate, c) != 0)
	return 0;

    if ((q->match & HX509_QUERY_MATCH_SERIALNUMBER)
	&& der_heim_integer_cmp(&c->tbsCertificate.serialNumber, q->serial) != 0)
	return 0;

    if ((q->match & HX509_QUERY_MATCH_ISSUER_NAME)
	&& _hx509_name_cmp(&c->tbsCertificate.issuer, q->issuer_name) != 0)
	return 0;

    if ((q->match & HX509_QUERY_MATCH_SUBJECT_NAME)
	&& _hx509_name_cmp(&c->tbsCertificate.subject, q->subject_name) != 0)
	return 0;

    if (q->match & HX509_QUERY_MATCH_SUBJECT_KEY_ID) {
	SubjectKeyIdentifier si;
	int ret;

	ret = _hx509_find_extension_subject_key_id(c, &si);
	if (ret == 0) {
	    if (der_heim_octet_string_cmp(&si, q->subject_id) != 0)
		ret = 1;
	    free_SubjectKeyIdentifier(&si);
	}
	if (ret)
	    return 0;
    }
    if ((q->match & HX509_QUERY_MATCH_ISSUER_ID))
	return 0;
    if ((q->match & HX509_QUERY_PRIVATE_KEY) && 
	_hx509_cert_private_key(cert) == NULL)
	return 0;

    {
	unsigned ku = 0;
	if (q->match & HX509_QUERY_KU_DIGITALSIGNATURE)
	    ku |= (1 << 0);
	if (q->match & HX509_QUERY_KU_NONREPUDIATION)
	    ku |= (1 << 1);
	if (q->match & HX509_QUERY_KU_ENCIPHERMENT)
	    ku |= (1 << 2);
	if (q->match & HX509_QUERY_KU_DATAENCIPHERMENT)
	    ku |= (1 << 3);
	if (q->match & HX509_QUERY_KU_KEYAGREEMENT)
	    ku |= (1 << 4);
	if (q->match & HX509_QUERY_KU_KEYCERTSIGN)
	    ku |= (1 << 5);
	if (q->match & HX509_QUERY_KU_CRLSIGN)
	    ku |= (1 << 6);
	if (ku && check_key_usage(context, c, ku, TRUE))
	    return 0;
    }
    if ((q->match & HX509_QUERY_ANCHOR))
	return 0;

    if (q->match & HX509_QUERY_MATCH_LOCAL_KEY_ID) {
	hx509_cert_attribute a;

	a = hx509_cert_get_attribute(cert, oid_id_pkcs_9_at_localKeyId());
	if (a == NULL)
	    return 0;
	if (der_heim_octet_string_cmp(&a->data, q->local_key_id) != 0)
	    return 0;
    }

    if (q->match & HX509_QUERY_NO_MATCH_PATH) {
	size_t i;

	for (i = 0; i < q->path->len; i++)
	    if (hx509_cert_cmp(q->path->val[i], cert) == 0)
		return 0;
    }
    if (q->match & HX509_QUERY_MATCH_FRIENDLY_NAME) {
	const char *name = hx509_cert_get_friendly_name(cert);
	if (name == NULL)
	    return 0;
	if (strcasecmp(q->friendlyname, name) != 0)
	    return 0;
    }
    if (q->match & HX509_QUERY_MATCH_FUNCTION) {
	int ret = (*q->cmp_func)(q->cmp_func_ctx, cert);
	if (ret != 0)
	    return 0;
    }

    if (q->match & HX509_QUERY_MATCH_KEY_HASH_SHA1) {
	heim_octet_string os;
	int ret;

	os.data = c->tbsCertificate.subjectPublicKeyInfo.subjectPublicKey.data;
	os.length = 
	    c->tbsCertificate.subjectPublicKeyInfo.subjectPublicKey.length / 8;

	ret = _hx509_verify_signature(context,
				      NULL,
				      hx509_signature_sha1(),
				      &os,
				      q->keyhash_sha1);
	if (ret != 0)
	    return 0;
    }

    if (q->match & HX509_QUERY_MATCH_TIME) {
	time_t t;
	t = _hx509_Time2time_t(&c->tbsCertificate.validity.notBefore);
	if (t > q->timenow)
	    return 0;
	t = _hx509_Time2time_t(&c->tbsCertificate.validity.notAfter);
	if (t < q->timenow)
	    return 0;
    }

    if (q->match & ~HX509_QUERY_MASK)
	return 0;

    return 1;
}

int
hx509_cert_check_eku(hx509_context context, hx509_cert cert,
		     const heim_oid *eku, int allow_any_eku)
{
    ExtKeyUsage e;
    int ret, i;

    ret = find_extension_eku(_hx509_get_cert(cert), &e);
    if (ret) {
	hx509_clear_error_string(context);
	return ret;
    }

    for (i = 0; i < e.len; i++) {
	if (der_heim_oid_cmp(eku, &e.val[i]) == 0) {
	    free_ExtKeyUsage(&e);
	    return 0;
	}
	if (allow_any_eku) {
#if 0
	    if (der_heim_oid_cmp(id_any_eku, &e.val[i]) == 0) {
		free_ExtKeyUsage(&e);
		return 0;
	    }
#endif
	}
    }
    free_ExtKeyUsage(&e);
    hx509_clear_error_string(context);
    return HX509_CERTIFICATE_MISSING_EKU;
}

int
_hx509_cert_get_keyusage(hx509_context context,
			 hx509_cert c,
			 KeyUsage *ku)
{
    Certificate *cert;
    const Extension *e;
    size_t size;
    int ret, i = 0;

    memset(ku, 0, sizeof(*ku));

    cert = _hx509_get_cert(c);

    if (_hx509_cert_get_version(cert) < 3)
	return 0;

    e = find_extension(cert, oid_id_x509_ce_keyUsage(), &i);
    if (e == NULL)
	return HX509_KU_CERT_MISSING;
    
    ret = decode_KeyUsage(e->extnValue.data, e->extnValue.length, ku, &size);
    if (ret)
	return ret;
    return 0;
}

int
_hx509_cert_get_eku(hx509_context context,
		    hx509_cert cert,
		    ExtKeyUsage *e)
{
    int ret;

    memset(e, 0, sizeof(*e));

    ret = find_extension_eku(_hx509_get_cert(cert), e);
    if (ret && ret != HX509_EXTENSION_NOT_FOUND) {
	hx509_clear_error_string(context);
	return ret;
    }
    return 0;
}

int
hx509_cert_binary(hx509_context context, hx509_cert c, heim_octet_string *os)
{
    size_t size;
    int ret;

    os->data = NULL;
    os->length = 0;

    ASN1_MALLOC_ENCODE(Certificate, os->data, os->length, 
		       _hx509_get_cert(c), &size, ret);
    if (ret)
	return ret;
    if (os->length != size)
	_hx509_abort("internal ASN.1 encoder error");

    return ret;
}
