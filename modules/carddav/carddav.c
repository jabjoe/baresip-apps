/**
 * @file carddav/carddav.c  CardDAV contacts plugin
 *
 *  CardDAV contacts for Baresip, pulls contacts from given CardDAV
 *
 * Copyright (C) 2026 - Joe Burmeister
 */

#include <stdio.h>
#include <curl/curl.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

#include <re.h>
#include <baresip.h>


/**
 * @defgroup carddav carddav
 *
 * App module add carddav commands using libcurl
 *
 * This module adds command to refresh contacts from a CardDAV.
 *
 * Example config:
 \verbatim
  carddav_gateway       sip.example.co.uk
  carddav_user          myusername:mypassword
  carddav_url           https://my.mextcloud.org/remote.php/dav/addressbooks/\
users/myuser/myshareuuid/
  carddav_buf           32768
 \endverbatim
 */


#define PTRDIFF(a,b) ((uintptr_t)a - (uintptr_t)b)


struct carddav_context
{
	struct contacts *contacts;
	uint32_t buf_len;
	uint32_t buf_used;
	char * buf_a;
	char * buf_b;
	char * gateway;
	const char * user;
	const char * url;
	unsigned count;
};


static bool get_vcard_attr(const char * cardstart,
                    uintptr_t cardend,
                    const char * attr,
                    char result[512])
{
	char attr2[32];
	unsigned attr_len = re_snprintf(attr2,
	                                sizeof(attr2),
	                                "%s:", attr);

	char * found = memmem(cardstart,
	                      cardend-(uintptr_t)cardstart,
	                      attr2, attr_len);

	if (!found) {
		attr2[attr_len-1]=';';

		found = memmem(cardstart,
		               cardend-(uintptr_t)cardstart,
		               attr2, attr_len);

		if (!found)
			return false;
	}

	found+=attr_len;

	char * attrend = memmem(found,
	                        cardend-(uintptr_t)cardstart,
	                       "&#13;", 4);

	if (!attrend)
		return false;

	str_ncpy(result, found, (uintptr_t)attrend-(uintptr_t)found+1);
	return true;
}


static struct contact  * in_contacts(struct contacts * contacts,
                                     const char * uri)
{
	struct pl pl;
	struct sip_addr addr;

	pl_set_str(&pl, uri);

	int err = sip_addr_decode(&addr, &pl);
	if (err)
		return NULL;

	char safe[128] = {0};

	str_ncpy(safe, addr.auri.p, addr.auri.l + 1);

	return contact_find(contacts, safe);
}


static int process_card(const char * cardstart,
                         uintptr_t cardend,
                         struct carddav_context * context)
{
	char name[512] = {0};
	char tel[512] = {0};
	if (!get_vcard_attr(cardstart, cardend, "\nFN", name))
		return EINVAL;
	if (!get_vcard_attr(cardstart, cardend, "\nTEL", tel))
		return EINVAL;

	unsigned long pos = strlen(tel);
	while (pos) {
		if (tel[pos] == ':' || tel[pos] == ':') {
			char addr[1024] = {0};
			struct pl pl;
			unsigned len = re_snprintf(addr,
			                           sizeof(addr),
			                           "\"%s (CardDAV)\" <sip:",
			                           name);
			++pos;
			unsigned hascode=0;
			while (tel[pos]) {
				char c = tel[pos++];

				if ( c == '+') {
					addr[len++]='0';
					++hascode;
				}
				else if (isdigit(c)) {
					if (hascode==1) {
						addr[len++]='0';
						hascode=0;
					}
					else if (hascode>2) {
						len-=(hascode-2);
						hascode=0;
					}
					addr[len++]=c;
				}
			}
			re_snprintf(addr+len, sizeof(addr)-len,
			            "@%s>", context->gateway);

			if (in_contacts(context->contacts, addr)) {
				info("Duplicate SIP %s\n", addr);
				return 0;
			}

			pl_set_str(&pl, addr);

			info("carddav: Adding %s\n", addr);
			int e = contact_add(context->contacts, NULL, &pl);
			if (!e)
				context->count++;
			else
				warning("carddav: Failed to add contact.\n");
			return e;
		}
		--pos;
	}
	return EINVAL;
}


static size_t writefunc(const void *ptr,
                        size_t size,
                        size_t nmemb,
                        void * userdata)
{
	struct carddav_context * context = userdata;

	size_t total = size*nmemb;

	debug("carddav: Chunk of %zu\n", total);

	unsigned freebuf = context->buf_len - context->buf_used;

	if (freebuf < total) {
		warning("carddav : chunks too big for free buffer %u\n",
		        freebuf);
		return 0;
	}

	str_ncpy(context->buf_a + context->buf_used, ptr, total);

	context->buf_used += total;

	char * pos = memmem(context->buf_a,
	                    context->buf_used,
	                    "BEGIN:VCARD",
	                    11);

	if (!pos) {
		debug("carddav: No card started after %u.\n",
		      context->buf_used);
		return total;
	}

	char * end = context->buf_a + context->buf_used;
	char * cardend = memmem(pos,
	                        PTRDIFF(end, pos),
	                       "END:VCARD",
	                       9);

	if (!cardend) {
		debug("carddav: No card complete after %u.\n",
		      context->buf_used);
		return total;
	}

	char * lastpos;

	while (pos && cardend) {
		process_card(pos,
		             (uintptr_t)cardend,
		             context);

		lastpos = cardend + 9;

		pos = memmem(lastpos,
		             PTRDIFF(end, lastpos),
		             "BEGIN:VCARD",
		             11);
		if (!pos)
			break;
		cardend = memmem(pos,
		                 PTRDIFF(end, pos),
		                 "END:VCARD",
		                  9);
	}

	unsigned remaining = (unsigned)PTRDIFF(end, lastpos);

	debug("carddav: Buffer swap with %u\n", remaining);

	str_ncpy(context->buf_b, lastpos, remaining);
	context->buf_used = remaining;

	char * t = context->buf_a;
	context->buf_a = context->buf_b;
	context->buf_b = t;

	return total;
}


static void move_contacts(struct list * contacts_a,
                          struct list * contacts_b,
                          struct contacts * contacts)
{
	info("carddav: wipe existing contacts.\n");
	struct le * cur = list_head(contacts_a);

	while (cur) {
		struct le * next = cur->next;
		mem_ref(cur->data);
		contact_remove(contacts, cur->data);
		if (contacts_b)
			list_prepend(contacts_b, cur, cur->data);
		cur = next;
	}
}


static void upload(struct carddav_context * context)
{
	CURL *curl = curl_easy_init();
	if (curl) {
		int num = rand();

		re_snprintf(context->buf_b,
		            context->buf_len,
		            "%s/%06d.vcf",
		            context->url,
		            num % 1000000);

		curl_easy_setopt(curl, CURLOPT_URL, context->buf_b);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_USERPWD, context->user);

		struct curl_slist *hs;
		hs = curl_slist_append(NULL, "Content-Type: text/vcf");

		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hs);
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");

		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, context->buf_a);

		CURLcode result = curl_easy_perform(curl);
		if (result == CURLE_OK)
			debug("carddav: Uploaded.\n");
		else
			warning("carddav: Upload failed: %s\n",
			        curl_easy_strerror(result));
	}
}


static void upload_phone_contact(struct carddav_context * context,
                                 const char * name,
                                 const char * phonenumber)
{
	re_snprintf(context->buf_a,
	            context->buf_len,
	            "BEGIN:VCARD\n"
	            "VERSION:3.0\n"
	            "FN:%s\n"
	            "TEL;TYPE=cell:%s\n"
	            "END:VCARD\n",
	            name, phonenumber);

	upload(context);
}


static void upload_sip_contact(struct carddav_context * context,
                               const char * name,
                               const char * uri)
{
/* IMPP:sip:johndoe@aol.com */

	re_snprintf(context->buf_a,
	            context->buf_len,
	            "BEGIN:VCARD\n"
	            "VERSION:3.0\n"
	            "FN:%s\n"
	            "IMPP:%s\n"
	            "END:VCARD\n",
	            name, uri);

	upload(context);
}


static void extract_name(char name[64], const char * con_str)
{
	const char * pos = con_str + 1;
	const char * end = strchr(pos, '"');
	unsigned len = PTRDIFF(end, pos) + 1;
	str_ncpy(name, pos, len);
}


static void upload_unique(struct carddav_context * context,
                          struct list * contacts_org,
                          bool just_restore)
{
	struct contacts *contacts = context->contacts;

	for (struct le * cur = list_head(contacts_org);
	    cur;
	    cur = cur->next) {
		struct contact * con = (struct contact*)cur->data;
		const char * con_str = contact_str(con);
		struct pl pl;

		if (!just_restore) {
			const char * uri = contact_uri(con);

			char name[64] = {0};
			extract_name(name, con_str);

			re_snprintf(context->buf_a,
			            context->buf_len,
			            "%s %s", name, uri);

			if (strstr(con_str, "(CardDAV)"))
				continue;

			if (in_contacts(context->contacts, uri)) {
				info("Found \"%s\", not adding.\n", uri);
				continue;
			}

			struct contact  * dup = contact_find(contacts, uri);
			if (dup)
				continue;

			const char * end = strchr(uri, '@');
			if (!end)
				warning("carddav: Contact URI %s, no @\n",
				        uri);

			const char * pos = uri;
			if (strncmp(pos, "sip:", 4))
				warning("carddav: Contact URI %s, no sip:\n",
				        uri);

			pos+=4;
			while (end && pos < end) {
				if (!isdigit(*pos)) {
					info("carddav: Non-number URI %s\n",
					     uri);
					break;
				}
				++pos;
			}

			if (pos == end) {
				unsigned len = PTRDIFF(end, uri) - 3;
				char pn[16] = {0};
				str_ncpy(pn, uri+4, len);

				info("carddav: Push Name \"%s\" "
				     "Phone number %s\n",
				     name,  pn);
				upload_phone_contact(context, name, pn);
			}
			else {
				upload_sip_contact(context, name, uri);
			}
		}

		pl_set_str(&pl, con_str);

		info("carddav: Adding back %s\n", con_str);
		int e = contact_add(contacts, NULL, &pl);
		if (e)
			warning("carddav: Failed to add back %s : %s\n",
			        con_str, strerror(e));
	}
}


static int carddav_sync(void)
{
	struct carddav_context context = {0};
	char user[1024] = {0};
	char url[4096] = {0};
	char gateway[256] = {0};

	if (conf_get_str(conf_cur(), "carddav_gateway",
	                 gateway, sizeof(gateway)) ||
	    conf_get_str(conf_cur(), "carddav_user",
	                 user, sizeof(user)) ||
	    conf_get_str(conf_cur(), "carddav_url",
	                 url, sizeof(url))) {
		warning("carddav: Miss⅞ing config.\n");
		return EINVAL;
	}

	info("carddav: using URL: %s\n", url);
	info("carddav: using user: %s\n", user);
	info("carddav: using gateway: %s\n", gateway);

	conf_get_u32(conf_cur(), "carddav_url", &context.buf_len);
	if (!context.buf_used)
		context.buf_len = 1024 * 32;

	info("carddav: using buffer of: %u\n", context.buf_len);
	context.contacts = baresip_contacts();
	context.gateway = gateway;
	context.url = url;
	context.user = user;

	CURLcode result = curl_global_init(CURL_GLOBAL_ALL);
	if (result != CURLE_OK)
		return (int)result;

	context.buf_a = mem_zalloc(context.buf_len, NULL);
	if (!context.buf_a) {
		warning("carddav: Unable to allocate carddav buffer 1.\n");
		return ENOMEM;
	}
	context.buf_b = mem_zalloc(context.buf_len, NULL);
	if (!context.buf_b) {
		warning("carddav: Unable to allocate carddav buffer 2.\n");
		mem_deref(context.buf_a);
		return ENOMEM;
	}

	CURL *curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_USERPWD, user);

		struct curl_slist *hs;
		hs = curl_slist_append(NULL, "Content-Type: text/xml");

		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hs);

		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");

		curl_easy_setopt(curl, CURLOPT_POSTFIELDS,
		                 "<propfind xmlns='DAV:'>"
		                 "<prop>"
		                 "<address-data "
		                 "xmlns=\"urn:ietf:params:xml:ns:carddav\"/>"
		                 "</prop>"
		                 "</propfind>");


		struct list * contacts_list = contact_list(context.contacts);
		struct list contacts_list_org;
		list_init(&contacts_list_org);
		move_contacts(contacts_list,
		              &contacts_list_org,
		              context.contacts);

		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
		result = curl_easy_perform(curl);
		curl_easy_cleanup(curl);
		if (result == CURLE_OK) {
			info("carddav: Added %u contacts.\n", context.count);
			debug("carddav: curl complete.\n");
			upload_unique(&context, &contacts_list_org,
			              false);
			list_flush(&contacts_list_org);
		}
		else {
			warning("carddav: Download failed: %s\n",
			        curl_easy_strerror(result));
			move_contacts(contacts_list, NULL, context.contacts);
			upload_unique(&context, &contacts_list_org,
			              true);
			list_flush(&contacts_list_org);
		}
	}

	curl_global_cleanup();

	mem_deref(context.buf_a);
	mem_deref(context.buf_b);

	return 0;
}


static int cmd_sync(struct re_printf *pf, void *arg) {
	(void)pf;
	(void)arg;

	return carddav_sync();
}


static const struct cmd cmdv[] = {
	{"refreshcontacts", 0, 0, "Refresh contacts from carddav", cmd_sync},
};


static int module_init(void)
{
	int err = cmd_register(baresip_commands(), cmdv, RE_ARRAY_SIZE(cmdv));

	if (err)
		return err;

	bool at_boot=false;
	conf_get_bool(conf_cur(), "carddav_at_boot", &at_boot);
	if (at_boot)
		carddav_sync();

	return err;
}


static int module_close(void)
{
	cmd_unregister(baresip_commands(), cmdv);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(carddav) = {
	"carddav",
	"application",
	module_init,
	module_close
};
