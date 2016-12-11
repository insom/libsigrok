/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2014 Bert Vermeulen <bert@biot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <zip.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"

#define LOG_PREFIX "output/srzip"

struct out_context {
	gboolean zip_created;
	uint64_t samplerate;
	char *filename;
};

static int init(struct sr_output *o, GHashTable *options)
{
	struct out_context *outc;

	(void)options;

	if (!o->filename || o->filename[0] == '\0') {
		sr_info("srzip output module requires a file name, cannot save.");
		return SR_ERR_ARG;
	}

	outc = g_malloc0(sizeof(struct out_context));
	outc->filename = g_strdup(o->filename);
	o->priv = outc;

	return SR_OK;
}

static int zip_create(const struct sr_output *o)
{
	struct out_context *outc;
	struct zip *zipfile;
	struct zip_source *versrc, *metasrc;
	struct sr_channel *ch;
	GVariant *gvar;
	GKeyFile *meta;
	GSList *l;
	const char *devgroup;
	char *s, *metabuf;
	gsize metalen;

	outc = o->priv;

	if (outc->samplerate == 0 && sr_config_get(o->sdi->driver, o->sdi, NULL,
					SR_CONF_SAMPLERATE, &gvar) == SR_OK) {
		outc->samplerate = g_variant_get_uint64(gvar);
		g_variant_unref(gvar);
	}

	/* Quietly delete it first, libzip wants replace ops otherwise. */
	g_unlink(outc->filename);
	zipfile = zip_open(outc->filename, ZIP_CREATE, NULL);
	if (!zipfile)
		return SR_ERR;

	/* "version" */
	versrc = zip_source_buffer(zipfile, "2", 1, FALSE);
	if (zip_add(zipfile, "version", versrc) < 0) {
		sr_err("Error saving version into zipfile: %s",
			zip_strerror(zipfile));
		zip_source_free(versrc);
		zip_discard(zipfile);
		return SR_ERR;
	}

	/* init "metadata" */
	meta = g_key_file_new();

	g_key_file_set_string(meta, "global", "sigrok version",
			SR_PACKAGE_VERSION_STRING);

	devgroup = "device 1";
	g_key_file_set_string(meta, devgroup, "capturefile", "logic-1");

	g_key_file_set_integer(meta, devgroup, "total probes",
			g_slist_length(o->sdi->channels));

	s = sr_samplerate_string(outc->samplerate);
	g_key_file_set_string(meta, devgroup, "samplerate", s);
	g_free(s);

	for (l = o->sdi->channels; l; l = l->next) {
		ch = l->data;
		if (ch->enabled && ch->type == SR_CHANNEL_LOGIC) {
			s = g_strdup_printf("probe%d", ch->index + 1);
			g_key_file_set_string(meta, devgroup, s, ch->name);
			g_free(s);
		}
	}
	metabuf = g_key_file_to_data(meta, &metalen, NULL);
	g_key_file_free(meta);

	metasrc = zip_source_buffer(zipfile, metabuf, metalen, FALSE);
	if (zip_add(zipfile, "metadata", metasrc) < 0) {
		sr_err("Error saving metadata into zipfile: %s",
			zip_strerror(zipfile));
		zip_source_free(metasrc);
		zip_discard(zipfile);
		g_free(metabuf);
		return SR_ERR;
	}

	if (zip_close(zipfile) < 0) {
		sr_err("Error saving zipfile: %s", zip_strerror(zipfile));
		zip_discard(zipfile);
		g_free(metabuf);
		return SR_ERR;
	}
	g_free(metabuf);

	return SR_OK;
}

static int zip_append(const struct sr_output *o, unsigned char *buf,
		int unitsize, int length)
{
	struct out_context *outc;
	struct zip *archive;
	struct zip_source *logicsrc;
	int64_t i, num_files;
	struct zip_stat zs;
	struct zip_source *metasrc;
	GKeyFile *kf;
	GError *error;
	uint64_t chunk_num;
	const char *entry_name;
	char *metabuf;
	gsize metalen;
	char *chunkname;
	unsigned int next_chunk_num;

	outc = o->priv;
	if (!(archive = zip_open(outc->filename, 0, NULL)))
		return SR_ERR;

	if (zip_stat(archive, "metadata", 0, &zs) < 0) {
		sr_err("Failed to open metadata: %s", zip_strerror(archive));
		zip_discard(archive);
		return SR_ERR;
	}
	kf = sr_sessionfile_read_metadata(archive, &zs);
	if (!kf) {
		zip_discard(archive);
		return SR_ERR_DATA;
	}
	/*
	 * If the file was only initialized but doesn't yet have any
	 * data it in, it won't have a unitsize field in metadata yet.
	 */
	error = NULL;
	metabuf = NULL;
	if (!g_key_file_has_key(kf, "device 1", "unitsize", &error)) {
		if (error && error->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND) {
			sr_err("Failed to check unitsize key: %s", error->message);
			g_error_free(error);
			g_key_file_free(kf);
			zip_discard(archive);
			return SR_ERR;
		}
		g_clear_error(&error);

		/* Add unitsize field. */
		g_key_file_set_integer(kf, "device 1", "unitsize", unitsize);
		metabuf = g_key_file_to_data(kf, &metalen, NULL);
		metasrc = zip_source_buffer(archive, metabuf, metalen, FALSE);

		if (zip_replace(archive, zs.index, metasrc) < 0) {
			sr_err("Failed to replace metadata: %s",
				zip_strerror(archive));
			g_key_file_free(kf);
			zip_source_free(metasrc);
			zip_discard(archive);
			g_free(metabuf);
			return SR_ERR;
		}
	}
	g_key_file_free(kf);

	next_chunk_num = 1;
	num_files = zip_get_num_entries(archive, 0);
	for (i = 0; i < num_files; i++) {
		entry_name = zip_get_name(archive, i, 0);
		if (!entry_name || strncmp(entry_name, "logic-1", 7) != 0)
			continue;
		if (entry_name[7] == '\0') {
			/* This file has no extra chunks, just a single "logic-1".
			 * Rename it to "logic-1-1" * and continue with chunk 2. */
			if (zip_rename(archive, i, "logic-1-1") < 0) {
				sr_err("Failed to rename 'logic-1' to 'logic-1-1': %s",
					zip_strerror(archive));
				zip_discard(archive);
				g_free(metabuf);
				return SR_ERR;
			}
			next_chunk_num = 2;
			break;
		} else if (entry_name[7] == '-') {
			chunk_num = g_ascii_strtoull(entry_name + 8, NULL, 10);
			if (chunk_num < G_MAXINT && chunk_num >= next_chunk_num)
				next_chunk_num = chunk_num + 1;
		}
	}

	if (length % unitsize != 0) {
		sr_warn("Chunk size %d not a multiple of the"
			" unit size %d.", length, unitsize);
	}
	logicsrc = zip_source_buffer(archive, buf, length, FALSE);
	chunkname = g_strdup_printf("logic-1-%u", next_chunk_num);
	i = zip_add(archive, chunkname, logicsrc);
	g_free(chunkname);
	if (i < 0) {
		sr_err("Failed to add chunk 'logic-1-%u': %s",
			next_chunk_num, zip_strerror(archive));
		zip_source_free(logicsrc);
		zip_discard(archive);
		g_free(metabuf);
		return SR_ERR;
	}
	if (zip_close(archive) < 0) {
		sr_err("Error saving session file: %s", zip_strerror(archive));
		zip_discard(archive);
		g_free(metabuf);
		return SR_ERR;
	}
	g_free(metabuf);

	return SR_OK;
}

static int receive(const struct sr_output *o, const struct sr_datafeed_packet *packet,
		GString **out)
{
	struct out_context *outc;
	const struct sr_datafeed_meta *meta;
	const struct sr_datafeed_logic *logic;
	const struct sr_config *src;
	GSList *l;

	int ret;

	*out = NULL;
	if (!o || !o->sdi || !(outc = o->priv))
		return SR_ERR_ARG;

	switch (packet->type) {
	case SR_DF_META:
		meta = packet->payload;
		for (l = meta->config; l; l = l->next) {
			src = l->data;
			if (src->key != SR_CONF_SAMPLERATE)
				continue;
			outc->samplerate = g_variant_get_uint64(src->data);
		}
		break;
	case SR_DF_LOGIC:
		if (!outc->zip_created) {
			if ((ret = zip_create(o)) != SR_OK)
				return ret;
			outc->zip_created = TRUE;
		}
		logic = packet->payload;
		ret = zip_append(o, logic->data, logic->unitsize, logic->length);
		if (ret != SR_OK)
			return ret;
		break;
	}

	return SR_OK;
}

static struct sr_option options[] = {
	ALL_ZERO
};

static const struct sr_option *get_options(void)
{
	if (!options[0].def)
		options[0].def = g_variant_ref_sink(g_variant_new_string(""));

	return options;
}

static int cleanup(struct sr_output *o)
{
	struct out_context *outc;

	outc = o->priv;
	g_variant_unref(options[0].def);
	g_free(outc->filename);
	g_free(outc);
	o->priv = NULL;

	return SR_OK;
}

SR_PRIV struct sr_output_module output_srzip = {
	.id = "srzip",
	.name = "srzip",
	.desc = "srzip session file",
	.exts = (const char*[]){"sr", NULL},
	.flags = SR_OUTPUT_INTERNAL_IO_HANDLING,
	.options = get_options,
	.init = init,
	.receive = receive,
	.cleanup = cleanup,
};