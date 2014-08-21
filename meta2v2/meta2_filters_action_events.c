#ifndef G_LOG_DOMAIN
# define G_LOG_DOMAIN "grid.meta2.disp"
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <attr/xattr.h>

#include <metautils/lib/metautils.h>
#include <metautils/lib/metacomm.h>
#include <server/transport_gridd.h>
#include <server/gridd_dispatcher_filters.h>
#include <cluster/lib/gridcluster.h>
#include <cluster/events/gridcluster_events.h>
#include <meta2v2/meta2_macros.h>
#include <meta2v2/meta2_filter_context.h>
#include <meta2v2/meta2_filters.h>
#include <meta2v2/meta2_backend_internals.h>
#include <meta2v2/meta2_bean.h>
#include <meta2v2/meta2v2_remote.h>
#include <meta2v2/generic.h>
#include <meta2v2/autogen.h>

static GError*
_meta2_event_add_raw_v1(gridcluster_event_t *event, meta2_raw_content_t *v1)
{
	GByteArray *gba;

	if (!(gba = meta2_maintenance_marshall_content(v1, NULL)))
		return NEWERROR(500, "V1 serialisation error");

	gridcluster_event_add_buffer(event, META2_EVTFIELD_RAWCONTENT,
			gba->data, gba->len);
	g_byte_array_free(gba, TRUE);
	return NULL;
}


static GError*
_meta2_event_add_raw_v2(gridcluster_event_t *event, meta2_raw_content_v2_t *v2)
{
	GSList singleton = {.data=NULL,.next=NULL};
	GByteArray *gba = NULL;

	singleton.data = v2;
	if (!(gba = meta2_raw_content_v2_marshall_gba(&singleton, NULL)))
		return NEWERROR(500, "V2 serialisation error");

	gridcluster_event_add_buffer(event, META2_EVTFIELD_RAWCONTENT_V2,
			gba->data, gba->len);
	g_byte_array_free(gba, TRUE);
	return NULL;
}

static GError*
_meta2_event_add_bean(gridcluster_event_t *event, const gchar *key, GSList* bean_chunk)
{
	GByteArray* gba = NULL;

	if (!(gba = bean_sequence_marshall(bean_chunk)))
		return NEWERROR(500, "V2 serialisation error");

	gridcluster_event_add_buffer(event, key, gba->data, gba->len);

	g_byte_array_free(gba, TRUE);
	return NULL;
}


static gint64
get_id64(struct event_config_s *evt)
{
    gint64 res;

    g_mutex_lock(event_get_lock(evt));
    res = event_get_and_inc_seq(evt);
    g_mutex_unlock(event_get_lock(evt));

    return res;
}


static gchar *
ueid_generate(struct event_config_s *evt_config,
		gchar *d, gsize dsize)
{
	struct timeval tv;
	gchar hostname[256];

	memset(hostname, 0, sizeof(hostname));
	gethostname(hostname, sizeof(hostname)-1);

	gettimeofday(&tv, NULL);

	g_snprintf(d, dsize, "%.*s_%lu_%lu_%d_%"G_GUINT64_FORMAT,
			(int) sizeof(hostname), hostname,
			tv.tv_sec, tv.tv_usec, getpid(), get_id64(evt_config));

	return d;
}

static gridcluster_event_t *
_build_event(struct meta2_backend_s *m2b, const gchar *str_type,
		struct hc_url_s *url)
{
	gchar str_ueid[512];
	gridcluster_event_t *event;
	struct event_config_s *evt_config = NULL;

	g_assert(m2b != NULL);
	g_assert(str_type != NULL);
	g_assert(url != NULL);
	g_assert(*str_type != '\0');

	evt_config = meta2_backend_get_event_config(m2b, hc_url_get(url, HCURL_NS));

	event = gridcluster_create_event();
	if (!event) {
		errno = ENOMEM;
		return NULL;
	}

	gridcluster_event_set_type(event, str_type);

	ueid_generate(evt_config, str_ueid, sizeof(str_ueid));
	gridcluster_event_add_string(event, META2_EVTFIELD_UEID, str_ueid);

	/* mandatory fields */
	gridcluster_event_add_string(event, META2_EVTFIELD_CID,
			hc_url_get(url, HCURL_HEXID));
	gridcluster_event_add_string(event, META2_EVTFIELD_CPATH,
			hc_url_get(url, HCURL_PATH));

	/* optional fields */
	if (hc_url_has(url, HCURL_REFERENCE)) {
		gridcluster_event_add_string(event, META2_EVTFIELD_CNAME,
				hc_url_get(url, HCURL_REFERENCE));
	}
	if (hc_url_has(url, HCURL_NS)) {
		gridcluster_event_add_string(event, META2_EVTFIELD_NAMESPACE,
				hc_url_get(url, HCURL_NS));
	}
	if (event_is_aggregate(evt_config)) {
		gchar *aggrname = g_strconcat(hc_url_get(url, HCURL_HEXID),
				",", hc_url_get(url, HCURL_PATH), ",CHANGE", NULL);
		gridcluster_event_add_string(event, META2_EVTFIELD_AGGRNAME, aggrname);
		g_free(aggrname);
	}

	/* additional fields */
	gridcluster_event_add_string(event, META2_EVTFIELD_URL,
			hc_url_get(url, HCURL_WHOLE));

	return event;
}


static GError*
touch_v2_content(struct meta2_backend_s *m2b, struct hc_url_s *url,
		struct meta2_raw_content_v2_s *v2, const char *evt_type)
{
	GError *err = NULL;
	gridcluster_event_t *event = NULL;

	struct event_config_s *evt_config = meta2_backend_get_event_config(m2b,
			hc_url_get(url, HCURL_NS));

	GRID_INFO("Touch v2 content");

	event = _build_event(m2b, evt_type, url);

	if (v2) {
		meta2_raw_content_t *v1;

		if (NULL != (err = _meta2_event_add_raw_v2(event, v2)))
			goto error;

		if (!(v1 = meta2_raw_content_v2_get_v1(v2, NULL)))
			WARN("V2 to V1 mapping error");
		else {
			err = _meta2_event_add_raw_v1(event, v1);
			meta2_maintenance_destroy_content(v1);
			if (err)
				goto error;
		}
	}

	if (event_get_dir(evt_config)) {
		GRID_INFO("writing event");
		err = gridcluster_event_SaveNewEvent(evt_config, event);
		//err = event_write(evt_config, event);
	}

error:
	g_hash_table_destroy(event);
	return err;
}


static GError*
touch_v2_content_chunkonly(struct meta2_backend_s *m2b, struct hc_url_s *url,
		GSList* beans, const char *evt_type)
{
	GError *err = NULL;
	gridcluster_event_t *event = NULL;

	struct event_config_s *evt_config = meta2_backend_get_event_config(m2b,
			hc_url_get(url, HCURL_NS));
	const gchar* m2url = meta2_backend_get_local_addr(m2b);

	GRID_INFO("Touch v2 content");

	event = _build_event(m2b, evt_type, url);

	/* add optionnal information, for asynchronious purge services */
	err = _meta2_event_add_bean(event, META2_EVTFIELD_CHUNKS, beans);
	if (err)
		goto error;

	/* add optionnal information, for asynchronious purge services  */
	if (m2url)
		gridcluster_event_add_string(event, META2_EVTFIELD_M2ADDR, m2url);

	if (event_get_dir(evt_config)) {
		GRID_INFO("writing event");
		err = gridcluster_event_SaveNewEvent(evt_config, event);
	}

error:
	g_hash_table_destroy(event);
	return err;
}


static GError *
touch_ALIAS_beans(struct meta2_backend_s *m2b, struct hc_url_s *url,
		GSList *beans, const char *evt_type)
{
	GError *err = NULL;
	struct meta2_raw_content_v2_s *v2;

	v2 = raw_content_v2_from_m2v2_beans(hc_url_get_id(url), beans);
	if (!v2)
		err = NEWERROR(500, "Conversion error");
	else {
		err = touch_v2_content(m2b, url, v2, evt_type);
		meta2_raw_content_v2_clean(v2);
	}

	return err;
}

static GError* 
touch_ALIAS_chunk(struct meta2_backend_s *m2b, struct hc_url_s *url,
		GSList *chunk, const char *evt_type)
{
	GError *err = touch_v2_content_chunkonly(m2b, url, chunk, evt_type);
	//_bean_cleanl2(chunk);
	return err;
}


static GError *
touch_ALIAS(struct meta2_backend_s *m2b, struct hc_url_s *url,
		struct bean_ALIASES_s *alias, const char *evt_type)
{
	GError *err = NULL;

	hc_url_set(url, HCURL_PATH, ALIASES_get_alias(alias)->str);
	GPtrArray *tmp = g_ptr_array_new();
	if (tmp->len > 0) {
		g_ptr_array_add(tmp, NULL);
		GSList *bl = metautils_array_to_list(tmp->pdata);
		if (bl) {
			err = touch_ALIAS_beans(m2b, url, bl, evt_type);
			g_slist_free(bl);
		}
	}
	_bean_cleanv2(tmp);
	return err;
}


static void
_get_beans(gpointer udata, gpointer b)
{
	struct on_bean_ctx_s *ctx = (struct on_bean_ctx_s*) udata;
	if(ctx) 
		ctx->l = g_slist_prepend(ctx->l, b);	 

	GRID_DEBUG("_get_beans: encore un beans (%d)", g_slist_length(ctx->l));
}


/**
 * if (bChunk_purged_only==TRUE) 
 * 		if purged_chunk if != NULL: used it
 * 		else call get_purged_chunk function() to get chunk
 * else
 * 		all content write on event
 */
static int
_notify_content(struct gridd_filter_ctx_s *ctx, struct gridd_reply_ctx_s *reply,
		gboolean bChunk_purged_only, struct on_bean_ctx_s *purged_chunk, const char *evt_type)
{
	TRACE_FILTER();
	(void) reply;

	GError *err = NULL;
	struct meta2_backend_s *m2b = meta2_filter_ctx_get_backend(ctx);
	struct hc_url_s *url = meta2_filter_ctx_get_url(ctx);
	struct event_config_s *evt_config = meta2_backend_get_event_config(m2b,
			hc_url_get(url, HCURL_NS));


	if(!event_is_enabled(evt_config)){
		GRID_TRACE("Event not enabled");
		return FILTER_OK;
	}

	// add RAW field
	if (!bChunk_purged_only) {
		GRID_INFO("Going to notify content creation");
		GSList *beans = (GSList*)meta2_filter_ctx_get_input_udata(ctx);
		GRID_INFO("Found %d beans in context", g_slist_length(beans));
		err = touch_ALIAS_beans(m2b, url, beans, evt_type);
	}
	// add 'purged chunk' field
	else {
		struct on_bean_ctx_s *obc = NULL;
		if (purged_chunk == NULL) {
			guint32 flags = M2V2_MODE_DRYRUN;
			obc = _on_bean_ctx_init(ctx, reply);
			err = meta2_backend_purge_container(m2b, url, flags, _get_beans, obc);
		} else
			obc = purged_chunk;

		GRID_ERROR("%s: [%d beans to purged]", __FUNCTION__, g_slist_length(obc->l));
		err = touch_ALIAS_chunk(m2b, url, obc->l, evt_type);
		if (purged_chunk == NULL)
			_on_bean_ctx_clean(obc);
	}

	if(NULL != err) {
		GRID_ERROR("Content notification failure (%d): %s",
				err->code, err->message);
		g_clear_error(&err);
	}

	GRID_INFO("No error, event generated");
	return FILTER_OK;
}

int
meta2_filter_action_notify_content_PUT(struct gridd_filter_ctx_s *ctx,
		struct gridd_reply_ctx_s *reply)
{
	return _notify_content(ctx, reply, FALSE, NULL, META2_EVTTYPE_PUT);
}

int
meta2_filter_action_notify_content_DELETE(struct gridd_filter_ctx_s *ctx,
		struct gridd_reply_ctx_s *reply)
{
	return _notify_content(ctx, reply, TRUE, NULL, META2_EVTTYPE_DELETE);
}

int
meta2_filter_action_notify_content_DELETE_v2(struct gridd_filter_ctx_s *ctx,
		struct gridd_reply_ctx_s *reply, struct on_bean_ctx_s *purged_chunk)
{
	return _notify_content(ctx, reply, TRUE, purged_chunk, META2_EVTTYPE_DELETE);
}

static int
_notify_container(struct gridd_filter_ctx_s *ctx,
		struct gridd_reply_ctx_s *reply, const char *evt_type)
{
	GError *err = NULL;
	gridcluster_event_t *event = NULL;
	(void) reply;

	struct meta2_backend_s *m2b = meta2_filter_ctx_get_backend(ctx);
	struct hc_url_s *url        = meta2_filter_ctx_get_url(ctx);
	struct event_config_s *evt_config = meta2_backend_get_event_config(m2b,
			hc_url_get(url, HCURL_NS));
	const gchar* m2url = meta2_backend_get_local_addr(m2b);


	if(!event_is_enabled(evt_config)){
		GRID_TRACE("Event not enabled");
		return FILTER_OK;
	}

	event = _build_event(m2b, evt_type, url);

	/* add optionnal information:
	 * for synchronious purge services: if container forced to destroy with not empty 
	 */
	if (m2url)
		gridcluster_event_add_string(event, META2_EVTFIELD_M2ADDR, m2url);


	if (event_get_dir(evt_config)) {
		if(NULL != (err = gridcluster_event_SaveNewEvent(evt_config, event))) {
			GRID_ERROR("Container notification failure (%d): %s",
					err->code, err->message);
			g_clear_error(&err);
		}
	}
	g_hash_table_destroy(event);

	return FILTER_OK;
}

int
meta2_filter_action_notify_container_CREATE(struct gridd_filter_ctx_s *ctx,
		struct gridd_reply_ctx_s *reply)
{
	TRACE_FILTER();

	return _notify_container(ctx, reply, META2_EVTTYPE_CREATE);
}

int
meta2_filter_action_notify_container_DESTROY(struct gridd_filter_ctx_s *ctx,
		struct gridd_reply_ctx_s *reply)
{
	TRACE_FILTER();

	return _notify_container(ctx, reply, META2_EVTTYPE_DESTROY);
}

int
meta2_filter_action_touch_content_v1(struct gridd_filter_ctx_s *ctx,
		struct gridd_reply_ctx_s *reply)
{
	GError *err = NULL;
	struct meta2_backend_s *m2b;
	struct hc_url_s *url;

	TRACE_FILTER();
	(void) reply;
	m2b = meta2_filter_ctx_get_backend(ctx);
	url = meta2_filter_ctx_get_url(ctx);

	GSList *beans = NULL;
	do {
		GPtrArray *tmp = g_ptr_array_new();
		err = meta2_backend_get_alias(m2b, url, 0, _bean_buffer_cb, tmp);
		beans = metautils_gpa_to_list(tmp);
		g_ptr_array_free(tmp, TRUE);
	} while (0);

	if (err) {
		_bean_cleanl2(beans);
		meta2_filter_ctx_set_error(ctx, err);
		return FILTER_KO;
	}

	err = touch_ALIAS_beans(m2b, url, beans, META2_EVTTYPE_PUT);
	_bean_cleanl2(beans);

	if (!err) {
		reply->send_reply(200, "OK");
		return FILTER_OK;
	}
	else {
		meta2_filter_ctx_set_error(ctx, err);
		return FILTER_KO;
	}
}

int
meta2_filter_action_touch_container_v1(struct gridd_filter_ctx_s *ctx,
		struct gridd_reply_ctx_s *reply)
{
	GError *err = NULL;
	struct meta2_backend_s *m2b;
	struct hc_url_s *url;
	guint32  flags_csize = 0;

	TRACE_FILTER();
	(void) reply;
	m2b = meta2_filter_ctx_get_backend(ctx);
	url = meta2_filter_ctx_get_url(ctx);
	const char *fstr = meta2_filter_ctx_get_param(ctx, M2_KEY_GET_FLAGS);
	if (NULL != fstr)
		flags_csize = (guint32) g_ascii_strtoull(fstr, NULL, 10);

	GPtrArray *aliases = g_ptr_array_new();
	struct list_params_s lp;
	memset(&lp, '\0', sizeof(struct list_params_s));
	lp.type = DEFAULT;
	err = meta2_backend_list_aliases(m2b, url, &lp, _bean_buffer_cb, aliases);
	if (err != NULL) {
		meta2_filter_ctx_set_error(ctx, err);
		return FILTER_KO;
	}

	guint u;
	for (u=0; !err && u < aliases->len ;u++) {
		struct bean_ALIASES_s *alias = aliases->pdata[u];
		err = touch_ALIAS(m2b, url, alias, META2_EVTTYPE_PUT);
	}

	_bean_cleanv2(aliases);

	/* refresh container_size to m1 */
	if (!err) {
		gboolean bRecalc = FALSE;
		if (flags_csize&META2TOUCH_FLAGS_RECALCCSIZE) {
			flags_csize|=META2TOUCH_FLAGS_UPDATECSIZE;
			bRecalc = TRUE;
		}

		if (flags_csize&META2TOUCH_FLAGS_UPDATECSIZE)
			err = meta2_backend_refresh_container_size(m2b, url, bRecalc);
	}

	if (!err) {
		reply->send_reply(200, "0K");
		return FILTER_OK;
	}

	meta2_filter_ctx_set_error(ctx, err);
	return FILTER_KO;
}
