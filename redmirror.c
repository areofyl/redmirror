#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <signal.h>
#include <curl/curl.h>

#define MAX_BUF      (1 << 22) /* 4MB */
#define MAX_POSTS    100
#define MAX_COMMENTS 500
#define RESP_BUF     (1 << 21) /* 2MB response buffer (html content is bigger) */
#define SERVER_PORT  8000

typedef struct {
	char *data;
	size_t len;
} Buf;

typedef struct {
	char title[512];
	char author[128];
	char url[1024];
	char permalink[512];
	char selftext_html[32768];
	char post_hint[64];
	int score;
	int num_comments;
	int is_self;
	int is_image;
} Post;

typedef struct {
	char author[128];
	char body_html[32768];
	int score;
	int depth;
} Comment;

typedef struct {
	Post *post;
	int post_idx;
	const char *sub;
	const char *outdir;
} CommentJob;

/* ---- curl ---- */

static size_t
write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	Buf *buf = userdata;
	size_t total = size * nmemb;
	if (buf->len + total >= MAX_BUF)
		return 0;
	memcpy(buf->data + buf->len, ptr, total);
	buf->len += total;
	buf->data[buf->len] = '\0';
	return total;
}

static char *
fetch_url(const char *url)
{
	CURL *curl = curl_easy_init();
	if (!curl) return NULL;

	Buf buf;
	buf.data = malloc(MAX_BUF);
	buf.len = 0;
	buf.data[0] = '\0';

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "linux:redmirror:1.0 (by /u/redmirror_bot)");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

	CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		free(buf.data);
		return NULL;
	}
	return buf.data;
}

/* ---- unicode / html entity helpers ---- */

/* encode a unicode codepoint as UTF-8, returns bytes written (1-4) */
static int
utf8_encode(unsigned int cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	} else if (cp < 0x800) {
		out[0] = 0xC0 | (cp >> 6);
		out[1] = 0x80 | (cp & 0x3F);
		return 2;
	} else if (cp < 0x10000) {
		out[0] = 0xE0 | (cp >> 12);
		out[1] = 0x80 | ((cp >> 6) & 0x3F);
		out[2] = 0x80 | (cp & 0x3F);
		return 3;
	} else if (cp < 0x110000) {
		out[0] = 0xF0 | (cp >> 18);
		out[1] = 0x80 | ((cp >> 12) & 0x3F);
		out[2] = 0x80 | ((cp >> 6) & 0x3F);
		out[3] = 0x80 | (cp & 0x3F);
		return 4;
	}
	return 0;
}

/* decode HTML entities in-place: &lt; &gt; &amp; &quot; &apos; &#nn; &#xNN; */
static void
html_entity_decode(char *s)
{
	char *r = s, *w = s;
	while (*r) {
		if (*r != '&') {
			*w++ = *r++;
			continue;
		}
		if (strncmp(r, "&lt;", 4) == 0)        { *w++ = '<'; r += 4; }
		else if (strncmp(r, "&gt;", 4) == 0)    { *w++ = '>'; r += 4; }
		else if (strncmp(r, "&amp;", 5) == 0)    { *w++ = '&'; r += 5; }
		else if (strncmp(r, "&quot;", 6) == 0)   { *w++ = '"'; r += 6; }
		else if (strncmp(r, "&apos;", 6) == 0)   { *w++ = '\''; r += 6; }
		else if (strncmp(r, "&nbsp;", 6) == 0)   { *w++ = ' '; r += 6; }
		else if (r[1] == '#') {
			unsigned int cp = 0;
			const char *p;
			if (r[2] == 'x' || r[2] == 'X') {
				p = r + 3;
				while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
					cp <<= 4;
					if (*p >= '0' && *p <= '9') cp |= *p - '0';
					else if (*p >= 'a' && *p <= 'f') cp |= *p - 'a' + 10;
					else cp |= *p - 'A' + 10;
					p++;
				}
			} else {
				p = r + 2;
				while (*p >= '0' && *p <= '9') {
					cp = cp * 10 + (*p - '0');
					p++;
				}
			}
			if (*p == ';') p++;
			if (cp > 0) {
				int n = utf8_encode(cp, w);
				w += n;
			}
			r = (char *)p;
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
}

/* ---- json helpers ---- */

/* forward declarations */
static const char *find_obj_end(const char *p);
static char *extract_obj(const char *p);

/* skip past a JSON string value (p points to opening quote) */
static const char *
skip_json_string(const char *p)
{
	if (*p != '"') return p;
	p++;
	while (*p) {
		if (*p == '\\') { p += 2; continue; }
		if (*p == '"') return p + 1;
		p++;
	}
	return p;
}

/* find a JSON key at depth 1 (direct children of the outermost object/array).
   skips over string values AND nested objects/arrays to avoid matching
   keys in deeply nested structures. */
static const char *
find_key(const char *json, const char *key)
{
	char pattern[256];
	int plen;
	plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = json;
	int depth = 0;
	while (*p) {
		if (*p == '"') {
			/* only check for key match at depth 1 */
			if (depth == 1 && strncmp(p, pattern, plen) == 0) {
				const char *after = p + plen;
				while (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r')
					after++;
				if (*after == ':') {
					after++;
					while (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r')
						after++;
					return after;
				}
			}
			p = skip_json_string(p);
			continue;
		}
		if (*p == '{' || *p == '[') {
			depth++;
		} else if (*p == '}' || *p == ']') {
			depth--;
			if (depth <= 0) break;
		}
		p++;
	}
	return NULL;
}

static int
extract_string(const char *p, char *out, size_t maxlen)
{
	if (!p || *p != '"') return -1;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i < maxlen - 5) { /* -5 for max utf8 + nul */
		if (*p == '\\' && *(p+1)) {
			p++;
			switch (*p) {
			case 'n': out[i++] = '\n'; break;
			case 't': out[i++] = '\t'; break;
			case '"': out[i++] = '"'; break;
			case '\\': out[i++] = '\\'; break;
			case '/': out[i++] = '/'; break;
			case 'u': {
				if (p[1] && p[2] && p[3] && p[4]) {
					unsigned int cp = 0;
					sscanf(p+1, "%4x", &cp);
					/* handle surrogate pairs */
					if (cp >= 0xD800 && cp <= 0xDBFF && p[5] == '\\' && p[6] == 'u') {
						unsigned int lo = 0;
						sscanf(p+7, "%4x", &lo);
						if (lo >= 0xDC00 && lo <= 0xDFFF) {
							cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
							p += 6; /* extra \uXXXX */
						}
					}
					int n = utf8_encode(cp, out + i);
					if (n > 0) i += n;
					p += 4;
				}
				break;
			}
			default: out[i++] = *p; break;
			}
		} else {
			out[i++] = *p;
		}
		p++;
	}
	out[i] = '\0';
	return 0;
}

static int
extract_int(const char *p)
{
	if (!p) return 0;
	while (*p == ' ') p++;
	return atoi(p);
}

static int
extract_bool(const char *p)
{
	if (!p) return 0;
	while (*p == ' ') p++;
	return strncmp(p, "true", 4) == 0;
}

static const char *
find_child(const char *json, int n)
{
	/* first try to find "children" at depth 1, then try inside "data" */
	const char *val = find_key(json, "children");
	if (!val) {
		/* "children" is inside "data" in Listing objects */
		const char *data = find_key(json, "data");
		if (data && *data == '{') {
			/* search within data for children — need depth-1 search inside data */
			const char *dend = find_obj_end(data);
			if (dend) {
				size_t dlen = dend - data;
				char *dobj = malloc(dlen + 1);
				memcpy(dobj, data, dlen);
				dobj[dlen] = '\0';
				val = find_key(dobj, "children");
				if (val) {
					/* val points inside dobj, convert to offset in original json */
					size_t offset = val - dobj;
					free(dobj);
					val = data + offset;
				} else {
					free(dobj);
					return NULL;
				}
			} else {
				return NULL;
			}
		} else {
			return NULL;
		}
	}
	/* val should point to '[' */
	while (*val && *val != '[') val++;
	if (!*val) return NULL;
	val++;

	int count = 0;
	int depth = 0;
	const char *child_start = NULL;

	const char *p = val;
	while (*p) {
		if (*p == '"') {
			/* skip over string values to avoid false matches */
			p = skip_json_string(p);
			continue;
		}
		if (*p == '{') {
			if (depth == 0) {
				if (count == n) child_start = p;
				count++;
			}
			depth++;
		} else if (*p == '}') {
			depth--;
			if (depth == 0 && child_start)
				return child_start;
		} else if (*p == ']' && depth == 0) {
			break; /* end of children array */
		}
		p++;
	}
	return NULL;
}

static const char *
find_obj_end(const char *p)
{
	int depth = 0;
	while (*p) {
		if (*p == '"') {
			p = skip_json_string(p);
			continue;
		}
		if (*p == '{') depth++;
		else if (*p == '}') { depth--; if (depth == 0) return p + 1; }
		p++;
	}
	return NULL;
}

/* extract an object value starting at p (which points to '{'), returns malloc'd copy */
static char *
extract_obj(const char *p)
{
	if (!p || *p != '{') return NULL;
	const char *end = find_obj_end(p);
	if (!end) return NULL;
	size_t len = end - p;
	char *obj = malloc(len + 1);
	memcpy(obj, p, len);
	obj[len] = '\0';
	return obj;
}

/* ---- html helpers ---- */

static void
html_escape_file(FILE *f, const char *s)
{
	while (*s) {
		switch (*s) {
		case '&': fprintf(f, "&amp;"); break;
		case '<': fprintf(f, "&lt;"); break;
		case '>': fprintf(f, "&gt;"); break;
		case '"': fprintf(f, "&quot;"); break;
		default: fputc(*s, f); break;
		}
		s++;
	}
}

static int
html_escape_buf(char *buf, int maxlen, const char *s)
{
	int i = 0;
	while (*s && i < maxlen - 6) {
		switch (*s) {
		case '&': i += snprintf(buf+i, maxlen-i, "&amp;"); break;
		case '<': i += snprintf(buf+i, maxlen-i, "&lt;"); break;
		case '>': i += snprintf(buf+i, maxlen-i, "&gt;"); break;
		case '"': i += snprintf(buf+i, maxlen-i, "&quot;"); break;
		default: buf[i++] = *s; break;
		}
		s++;
	}
	buf[i] = '\0';
	return i;
}

/* detect if a URL points to an image */
static int
is_image_url(const char *url)
{
	if (!url || !*url) return 0;
	/* known image hosts */
	if (strstr(url, "i.redd.it/")) return 1;
	if (strstr(url, "i.imgur.com/")) return 1;
	if (strstr(url, "pbs.twimg.com/")) return 1;
	/* check file extension */
	const char *dot = strrchr(url, '.');
	if (!dot) return 0;
	/* ignore query params after extension */
	char ext[8] = {0};
	int j = 0;
	dot++;
	while (*dot && *dot != '?' && *dot != '#' && j < 7)
		ext[j++] = *dot++;
	ext[j] = '\0';
	if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0 ||
	    strcmp(ext, "png") == 0 || strcmp(ext, "gif") == 0 ||
	    strcmp(ext, "webp") == 0)
		return 1;
	return 0;
}

#define CSS \
	"body{font-family:monospace;max-width:800px;margin:20px auto;background:#111;color:#ccc;}" \
	"a{color:#7af;}a:visited{color:#a7f;}.score{color:#f84;}.meta{color:#888;font-size:0.9em;}" \
	"hr{border:none;border-top:1px solid #333;}" \
	".comment{border-left:2px solid #333;padding:4px 8px;margin:6px 0;}" \
	".md{line-height:1.5;}.md p{margin:4px 0;}.md pre{background:#1a1a1a;padding:8px;overflow-x:auto;}" \
	".md code{background:#1a1a1a;padding:1px 4px;}.md blockquote{border-left:3px solid #444;margin:4px 0;padding-left:8px;color:#999;}" \
	".md ul,.md ol{padding-left:20px;}.md li{margin:2px 0;}" \
	".md a{color:#7af;}.md h1,.md h2,.md h3{color:#eee;}" \
	".md table{border-collapse:collapse;}.md td,.md th{border:1px solid #444;padding:4px 8px;}" \
	"img{max-width:100%%;height:auto;display:block;margin:8px 0;}" \
	"input{font-family:monospace;background:#222;color:#ccc;border:1px solid #444;padding:6px 10px;font-size:1em;}" \
	"button{font-family:monospace;background:#333;color:#ccc;border:1px solid #555;padding:6px 12px;cursor:pointer;font-size:1em;}" \
	".nav{margin:8px 0;}.nav a{margin-right:12px;}" \
	".thumb{max-height:80px;display:inline;vertical-align:middle;margin-right:8px;}"

/* ---- parsing ---- */

static int
parse_posts(const char *json, Post *posts, int max)
{
	int n = 0;
	for (int i = 0; i < max; i++) {
		const char *child = find_child(json, i);
		if (!child) break;

		const char *end = find_obj_end(child);
		if (!end) break;

		size_t len = end - child;
		char *obj = malloc(len + 1);
		memcpy(obj, child, len);
		obj[len] = '\0';

		/* extract just the "data" object so find_key doesn't descend into nested structures */
		const char *data_ptr = find_key(obj, "data");
		if (!data_ptr || *data_ptr != '{') { free(obj); continue; }
		char *data = extract_obj(data_ptr);
		if (!data) { free(obj); continue; }

		Post *p = &posts[n];
		memset(p, 0, sizeof(*p));

		const char *v;
		v = find_key(data, "title");
		extract_string(v, p->title, sizeof(p->title));
		v = find_key(data, "author");
		extract_string(v, p->author, sizeof(p->author));
		v = find_key(data, "url");
		extract_string(v, p->url, sizeof(p->url));
		v = find_key(data, "permalink");
		extract_string(v, p->permalink, sizeof(p->permalink));
		v = find_key(data, "selftext_html");
		extract_string(v, p->selftext_html, sizeof(p->selftext_html));
		if (p->selftext_html[0])
			html_entity_decode(p->selftext_html);
		v = find_key(data, "post_hint");
		extract_string(v, p->post_hint, sizeof(p->post_hint));
		v = find_key(data, "score");
		p->score = extract_int(v);
		v = find_key(data, "num_comments");
		p->num_comments = extract_int(v);
		v = find_key(data, "is_self");
		p->is_self = extract_bool(v);

		p->is_image = (strcmp(p->post_hint, "image") == 0) || is_image_url(p->url);

		free(data);
		free(obj);
		n++;
	}
	return n;
}

static int
parse_comments_recursive(const char *json, Comment *comments, int max, int *n, int depth)
{
	for (int i = 0; *n < max; i++) {
		const char *child = find_child(json, i);
		if (!child) break;

		const char *end = find_obj_end(child);
		if (!end) break;

		size_t len = end - child;
		char *obj = malloc(len + 1);
		memcpy(obj, child, len);
		obj[len] = '\0';

		const char *kind = find_key(obj, "kind");
		char kindstr[8] = {0};
		extract_string(kind, kindstr, sizeof(kindstr));
		if (strcmp(kindstr, "t1") != 0) {
			free(obj);
			continue;
		}

		/* extract "data" so find_key only searches this comment's fields,
		   not nested replies children (fixes duplication bug) */
		const char *data_ptr = find_key(obj, "data");
		if (!data_ptr || *data_ptr != '{') { free(obj); continue; }
		char *data = extract_obj(data_ptr);
		if (!data) { free(obj); continue; }

		Comment *c = &comments[*n];
		memset(c, 0, sizeof(*c));
		c->depth = depth;

		const char *v;
		v = find_key(data, "author");
		extract_string(v, c->author, sizeof(c->author));
		v = find_key(data, "body_html");
		extract_string(v, c->body_html, sizeof(c->body_html));
		if (c->body_html[0])
			html_entity_decode(c->body_html);
		v = find_key(data, "score");
		c->score = extract_int(v);

		(*n)++;

		/* recurse into replies */
		const char *replies = find_key(data, "replies");
		if (replies && *replies == '{') {
			char *robj = extract_obj(replies);
			if (robj) {
				parse_comments_recursive(robj, comments, max, n, depth + 1);
				free(robj);
			}
		}

		free(data);

		free(obj);
	}
	return *n;
}

/* find second listing in comments json */
static const char *
find_second_listing(const char *json)
{
	const char *p = json;
	int depth = 0;
	int listings = 0;
	while (*p) {
		if (*p == '{') depth++;
		else if (*p == '}') {
			depth--;
			if (depth == 0) {
				listings++;
				if (listings == 1) { p++; return p; }
			}
		}
		p++;
	}
	return json;
}

/* ---- static export ---- */

static void
write_subreddit_page(const char *sub, Post *posts, int npost, const char *outdir)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/index.html", outdir);
	FILE *f = fopen(path, "w");
	if (!f) { perror("fopen"); return; }

	fprintf(f, "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n");
	fprintf(f, "<title>r/%s</title>\n<style>%s</style>\n", sub, CSS);
	fprintf(f, "</head><body>\n");
	fprintf(f, "<a href=\"../../\">&lt; home</a>\n");
	fprintf(f, "<h2>r/%s</h2>\n", sub);

	for (int i = 0; i < npost; i++) {
		Post *p = &posts[i];
		fprintf(f, "<div style=\"margin:10px 0;\">\n");
		fprintf(f, "<span class=\"score\">%d</span> ", p->score);

		char comment_file[256];
		snprintf(comment_file, sizeof(comment_file), "post_%d.html", i);

		if (p->is_image)
			fprintf(f, "<img class=\"thumb\" src=\"%s\" alt=\"\">", p->url);

		fprintf(f, "<a href=\"%s\">", p->is_self ? comment_file : p->url);
		html_escape_file(f, p->title);
		fprintf(f, "</a>");

		if (!p->is_self)
			fprintf(f, " <a href=\"%s\">[comments]</a>", comment_file);

		fprintf(f, "<br><span class=\"meta\">by %s | %d comments</span>\n",
			p->author, p->num_comments);
		fprintf(f, "</div><hr>\n");
	}

	fprintf(f, "</body></html>\n");
	fclose(f);
	printf("wrote %s (%d posts)\n", path, npost);
}

static void
write_comments_page(const char *sub, Post *p, int post_idx,
	Comment *comments, int ncomment, const char *outdir)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/post_%d.html", outdir, post_idx);
	FILE *f = fopen(path, "w");
	if (!f) { perror("fopen"); return; }

	fprintf(f, "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n");
	fprintf(f, "<title>");
	html_escape_file(f, p->title);
	fprintf(f, "</title>\n<style>%s</style>\n", CSS);
	fprintf(f, "</head><body>\n");
	fprintf(f, "<a href=\"index.html\">&lt; back to r/%s</a>\n", sub);
	fprintf(f, "<h2>");
	html_escape_file(f, p->title);
	fprintf(f, "</h2>\n");
	fprintf(f, "<span class=\"meta\">by %s | <span class=\"score\">%d</span> points</span>\n",
		p->author, p->score);

	if (p->is_image)
		fprintf(f, "<img src=\"%s\" alt=\"\">\n", p->url);
	else if (!p->is_self && p->url[0]) {
		fprintf(f, "<p><a href=\"%s\">", p->url);
		html_escape_file(f, p->url);
		fprintf(f, "</a></p>\n");
	}

	if (p->selftext_html[0])
		fprintf(f, "<div class=\"md\">%s</div>\n", p->selftext_html);

	fprintf(f, "<h3>%d comments</h3>\n", ncomment);

	for (int i = 0; i < ncomment; i++) {
		Comment *c = &comments[i];
		int indent = c->depth * 20;
		fprintf(f, "<div class=\"comment\" style=\"margin-left:%dpx;\">\n", indent);
		fprintf(f, "<span class=\"meta\">%s <span class=\"score\">%d</span></span>\n",
			c->author, c->score);
		if (c->body_html[0])
			fprintf(f, "<div class=\"md\">%s</div>", c->body_html);
		fprintf(f, "</div>\n");
	}

	fprintf(f, "</body></html>\n");
	fclose(f);
	printf("wrote %s (%d comments)\n", path, ncomment);
}

static void *
fetch_comments_thread(void *arg)
{
	CommentJob *job = arg;
	char url[1024];
	snprintf(url, sizeof(url), "https://old.reddit.com%s.json?limit=50",
		job->post->permalink);

	char *cjson = fetch_url(url);
	if (!cjson) {
		printf("  post %d: failed, skipping\n", job->post_idx + 1);
		return NULL;
	}

	const char *second = find_second_listing(cjson);
	Comment *comments = calloc(MAX_COMMENTS, sizeof(Comment));
	int n = 0;
	parse_comments_recursive(second, comments, MAX_COMMENTS, &n, 0);
	write_comments_page(job->sub, job->post, job->post_idx, comments, n, job->outdir);

	free(comments);
	free(cjson);
	return NULL;
}

static void
static_export(const char *sub, const char *sort, const char *outdir)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "mkdir -p %s", outdir);
	if (system(cmd) != 0) {
		fprintf(stderr, "failed to create output directory\n");
		return;
	}

	char url[512];
	snprintf(url, sizeof(url), "https://old.reddit.com/r/%s/%s.json?limit=25", sub, sort);
	printf("fetching %s ...\n", url);

	char *json = fetch_url(url);
	if (!json) { fprintf(stderr, "failed to fetch subreddit\n"); return; }

	Post *posts = calloc(MAX_POSTS, sizeof(Post));
	int npost = parse_posts(json, posts, 25);
	printf("parsed %d posts\n", npost);
	free(json);

	write_subreddit_page(sub, posts, npost, outdir);

	/* fetch comments in batches of 5 to avoid rate limiting */
	printf("fetching comments (%d posts)...\n", npost);
	pthread_t *threads = calloc(npost, sizeof(pthread_t));
	CommentJob *jobs = calloc(npost, sizeof(CommentJob));

	int batch = 5;
	for (int start = 0; start < npost; start += batch) {
		int end = start + batch;
		if (end > npost) end = npost;
		for (int i = start; i < end; i++) {
			jobs[i].post = &posts[i];
			jobs[i].post_idx = i;
			jobs[i].sub = sub;
			jobs[i].outdir = outdir;
			pthread_create(&threads[i], NULL, fetch_comments_thread, &jobs[i]);
		}
		for (int i = start; i < end; i++)
			pthread_join(threads[i], NULL);
		if (end < npost) sleep(1);
	}

	free(threads);
	free(jobs);
	free(posts);
	printf("\ndone. open %s/index.html\n", outdir);
}

/* ---- dynamic server ---- */

static void
send_response(int fd, const char *status, const char *ctype, const char *body, int bodylen)
{
	char header[512];
	int hlen = snprintf(header, sizeof(header),
		"HTTP/1.0 %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %d\r\n"
		"Connection: close\r\n"
		"\r\n",
		status, ctype, bodylen);
	write(fd, header, hlen);
	write(fd, body, bodylen);
}

static void
send_redirect(int fd, const char *location)
{
	char resp[1024];
	int len = snprintf(resp, sizeof(resp),
		"HTTP/1.0 302 Found\r\n"
		"Location: %s\r\n"
		"Content-Length: 0\r\n"
		"\r\n",
		location);
	write(fd, resp, len);
}

static void
url_decode(char *s)
{
	char *out = s;
	while (*s) {
		if (*s == '%' && s[1] && s[2]) {
			unsigned int c;
			sscanf(s+1, "%2x", &c);
			*out++ = (char)c;
			s += 3;
		} else if (*s == '+') {
			*out++ = ' ';
			s++;
		} else {
			*out++ = *s++;
		}
	}
	*out = '\0';
}

static int
is_safe_sub(const char *s)
{
	if (!*s) return 0;
	while (*s) {
		char c = *s;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_'))
			return 0;
		s++;
	}
	return 1;
}

static void
render_homepage(int fd)
{
	char *buf = malloc(RESP_BUF);
	int len = snprintf(buf, RESP_BUF,
		"<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n"
		"<title>redmirror</title>\n<style>%s</style>\n"
		"</head><body>\n"
		"<h1>redmirror</h1>\n"
		"<form action=\"/search\" method=\"GET\" style=\"margin:20px 0;\">\n"
		"<input type=\"text\" name=\"q\" placeholder=\"subreddit name\" autofocus>\n"
		"<button type=\"submit\">go</button>\n"
		"</form>\n"
		"<div class=\"nav\">\n"
		"<b>popular:</b><br>\n"
		"<a href=\"/r/linux\">r/linux</a>\n"
		"<a href=\"/r/programming\">r/programming</a>\n"
		"<a href=\"/r/unixporn\">r/unixporn</a>\n"
		"<a href=\"/r/technology\">r/technology</a>\n"
		"<a href=\"/r/gentoo\">r/gentoo</a>\n"
		"<a href=\"/r/suckless\">r/suckless</a>\n"
		"<a href=\"/r/archlinux\">r/archlinux</a>\n"
		"<a href=\"/r/asahi\">r/asahi</a>\n"
		"</div>\n"
		"</body></html>\n", CSS);
	send_response(fd, "200 OK", "text/html; charset=utf-8", buf, len);
	free(buf);
}

static void
render_subreddit(int fd, const char *sub, const char *sort)
{
	char url[512];
	snprintf(url, sizeof(url), "https://old.reddit.com/r/%s/%s.json?limit=25", sub, sort);

	char *json = fetch_url(url);
	if (!json) {
		const char *msg = "failed to fetch subreddit";
		send_response(fd, "502 Bad Gateway", "text/plain", msg, strlen(msg));
		return;
	}

	Post *posts = calloc(MAX_POSTS, sizeof(Post));
	int npost = parse_posts(json, posts, 25);
	free(json);

	if (npost == 0) {
		free(posts);
		const char *msg = "no posts found (subreddit may not exist)";
		send_response(fd, "404 Not Found", "text/plain", msg, strlen(msg));
		return;
	}

	char *buf = malloc(RESP_BUF);
	int len = 0;
	char esc[1024];

	len += snprintf(buf+len, RESP_BUF-len,
		"<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n"
		"<title>r/%s - %s</title>\n<style>%s</style>\n"
		"</head><body>\n"
		"<a href=\"/\">&lt; home</a>\n"
		"<h2>r/%s</h2>\n"
		"<div class=\"nav\">\n"
		"<a href=\"/r/%s/hot\">hot</a>\n"
		"<a href=\"/r/%s/new\">new</a>\n"
		"<a href=\"/r/%s/top\">top</a>\n"
		"<a href=\"/r/%s/rising\">rising</a>\n"
		"</div>\n"
		"<form action=\"/search\" method=\"GET\" style=\"margin:10px 0;\">\n"
		"<input type=\"text\" name=\"q\" placeholder=\"subreddit\" size=\"20\">\n"
		"<button type=\"submit\">go</button>\n"
		"</form><hr>\n",
		sub, sort, CSS, sub, sub, sub, sub, sub);

	for (int i = 0; i < npost && len < RESP_BUF - 8192; i++) {
		Post *p = &posts[i];
		html_escape_buf(esc, sizeof(esc), p->title);

		len += snprintf(buf+len, RESP_BUF-len,
			"<div style=\"margin:10px 0;\">\n"
			"<span class=\"score\">%d</span> ",
			p->score);

		if (p->is_image)
			len += snprintf(buf+len, RESP_BUF-len,
				"<img class=\"thumb\" src=\"%s\" alt=\"\">", p->url);

		if (p->is_self) {
			len += snprintf(buf+len, RESP_BUF-len,
				"<a href=\"%s\">%s</a>",
				p->permalink, esc);
		} else {
			len += snprintf(buf+len, RESP_BUF-len,
				"<a href=\"%s\">%s</a>"
				" <a href=\"%s\">[comments]</a>",
				p->url, esc, p->permalink);
		}

		len += snprintf(buf+len, RESP_BUF-len,
			"<br><span class=\"meta\">by %s | "
			"<a href=\"%s\">%d comments</a></span>\n"
			"</div><hr>\n",
			p->author, p->permalink, p->num_comments);
	}

	len += snprintf(buf+len, RESP_BUF-len, "</body></html>\n");
	send_response(fd, "200 OK", "text/html; charset=utf-8", buf, len);
	free(posts);
	free(buf);
}

static void
render_comments(int fd, const char *permalink)
{
	char url[1024];
	snprintf(url, sizeof(url), "https://old.reddit.com%s.json?limit=200", permalink);

	char *json = fetch_url(url);
	if (!json) {
		const char *msg = "failed to fetch comments";
		send_response(fd, "502 Bad Gateway", "text/plain", msg, strlen(msg));
		return;
	}

	/* parse post from first listing */
	Post post;
	memset(&post, 0, sizeof(post));
	{
		const char *child = find_child(json, 0);
		if (child) {
			const char *end = find_obj_end(child);
			if (end) {
				size_t clen = end - child;
				char *obj = malloc(clen + 1);
				memcpy(obj, child, clen);
				obj[clen] = '\0';
				const char *data_ptr = find_key(obj, "data");
				char *data = (data_ptr && *data_ptr == '{') ? extract_obj(data_ptr) : NULL;
				if (data) {
					const char *v;
					v = find_key(data, "title");
					extract_string(v, post.title, sizeof(post.title));
					v = find_key(data, "author");
					extract_string(v, post.author, sizeof(post.author));
					v = find_key(data, "url");
					extract_string(v, post.url, sizeof(post.url));
					v = find_key(data, "selftext_html");
					extract_string(v, post.selftext_html, sizeof(post.selftext_html));
					if (post.selftext_html[0])
						html_entity_decode(post.selftext_html);
					v = find_key(data, "post_hint");
					extract_string(v, post.post_hint, sizeof(post.post_hint));
					v = find_key(data, "score");
					post.score = extract_int(v);
					v = find_key(data, "is_self");
					post.is_self = extract_bool(v);
					v = find_key(data, "subreddit");
					extract_string(v, post.permalink, sizeof(post.permalink));
					post.is_image = (strcmp(post.post_hint, "image") == 0) || is_image_url(post.url);
					free(data);
				}
				free(obj);
			}
		}
	}

	/* parse comments from second listing */
	const char *second = find_second_listing(json);
	Comment *comments = calloc(MAX_COMMENTS, sizeof(Comment));
	int ncomment = 0;
	parse_comments_recursive(second, comments, MAX_COMMENTS, &ncomment, 0);

	/* render */
	char *buf = malloc(RESP_BUF);
	int len = 0;
	char esc[1024];

	html_escape_buf(esc, sizeof(esc), post.title);
	len += snprintf(buf+len, RESP_BUF-len,
		"<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n"
		"<title>%s</title>\n<style>%s</style>\n"
		"</head><body>\n"
		"<a href=\"/r/%s\">&lt; back to r/%s</a>\n"
		"<h2>%s</h2>\n"
		"<span class=\"meta\">by %s | <span class=\"score\">%d</span> points</span>\n",
		esc, CSS, post.permalink, post.permalink, esc,
		post.author, post.score);

	/* image posts: show the image */
	if (post.is_image) {
		len += snprintf(buf+len, RESP_BUF-len,
			"<img src=\"%s\" alt=\"\">\n", post.url);
	} else if (!post.is_self && post.url[0]) {
		html_escape_buf(esc, sizeof(esc), post.url);
		len += snprintf(buf+len, RESP_BUF-len,
			"<p><a href=\"%s\">%s</a></p>\n", post.url, esc);
	}

	/* selftext as rendered HTML */
	if (post.selftext_html[0])
		len += snprintf(buf+len, RESP_BUF-len,
			"<div class=\"md\">%s</div>\n", post.selftext_html);

	len += snprintf(buf+len, RESP_BUF-len, "<h3>%d comments</h3>\n", ncomment);

	for (int i = 0; i < ncomment && len < RESP_BUF - 65536; i++) {
		Comment *c = &comments[i];
		int indent = c->depth * 20;
		len += snprintf(buf+len, RESP_BUF-len,
			"<div class=\"comment\" style=\"margin-left:%dpx;\">\n"
			"<span class=\"meta\">%s <span class=\"score\">%d</span></span>\n",
			indent, c->author, c->score);
		if (c->body_html[0])
			len += snprintf(buf+len, RESP_BUF-len,
				"<div class=\"md\">%s</div>", c->body_html);
		len += snprintf(buf+len, RESP_BUF-len, "</div>\n");
	}

	len += snprintf(buf+len, RESP_BUF-len, "</body></html>\n");
	send_response(fd, "200 OK", "text/html; charset=utf-8", buf, len);
	free(comments);
	free(json);
	free(buf);
}

static void
handle_request(int fd, const char *path)
{
	if (strcmp(path, "/") == 0) {
		render_homepage(fd);
		return;
	}

	if (strncmp(path, "/search", 7) == 0) {
		const char *q = strstr(path, "q=");
		if (!q) { send_redirect(fd, "/"); return; }
		char sub[128];
		strncpy(sub, q + 2, sizeof(sub) - 1);
		sub[sizeof(sub) - 1] = '\0';
		char *amp = strchr(sub, '&');
		if (amp) *amp = '\0';
		url_decode(sub);
		char *name = sub;
		if (strncmp(name, "r/", 2) == 0) name += 2;
		if (!is_safe_sub(name)) {
			const char *msg = "invalid subreddit name";
			send_response(fd, "400 Bad Request", "text/plain", msg, strlen(msg));
			return;
		}
		char loc[256];
		snprintf(loc, sizeof(loc), "/r/%s", name);
		send_redirect(fd, loc);
		return;
	}

	if (strncmp(path, "/r/", 3) == 0) {
		char pathcopy[1024];
		strncpy(pathcopy, path + 3, sizeof(pathcopy) - 1);
		pathcopy[sizeof(pathcopy) - 1] = '\0';

		if (strstr(pathcopy, "/comments/")) {
			render_comments(fd, path);
			return;
		}

		char *slash = strchr(pathcopy, '/');
		char sub[128] = {0};
		char sort[32] = "hot";

		if (slash) {
			*slash = '\0';
			strncpy(sub, pathcopy, sizeof(sub) - 1);
			strncpy(sort, slash + 1, sizeof(sort) - 1);
			char *ts = strchr(sort, '/');
			if (ts) *ts = '\0';
			if (!sort[0]) strcpy(sort, "hot");
		} else {
			strncpy(sub, pathcopy, sizeof(sub) - 1);
		}

		if (!is_safe_sub(sub)) {
			const char *msg = "invalid subreddit name";
			send_response(fd, "400 Bad Request", "text/plain", msg, strlen(msg));
			return;
		}

		render_subreddit(fd, sub, sort);
		return;
	}

	const char *msg = "not found";
	send_response(fd, "404 Not Found", "text/plain", msg, strlen(msg));
}

static void *
connection_thread(void *arg)
{
	int fd = *(int *)arg;
	free(arg);

	char req[4096] = {0};
	read(fd, req, sizeof(req) - 1);

	char *path_start = strchr(req, ' ');
	if (!path_start) { close(fd); return NULL; }
	path_start++;
	char *path_end = strchr(path_start, ' ');
	if (!path_end) { close(fd); return NULL; }
	*path_end = '\0';

	printf("[%s]\n", path_start);
	handle_request(fd, path_start);

	close(fd);
	return NULL;
}

static void
serve(int port)
{
	int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) { perror("socket"); exit(1); }

	int opt = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);

	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		exit(1);
	}
	listen(srv, 32);
	printf("redmirror running on http://localhost:%d\n", port);

	for (;;) {
		int *cli = malloc(sizeof(int));
		*cli = accept(srv, NULL, NULL);
		if (*cli < 0) { free(cli); continue; }

		pthread_t t;
		pthread_create(&t, NULL, connection_thread, cli);
		pthread_detach(t);
	}
}

/* ---- pages mode (multi-sub static site) ---- */

static void
write_pages_homepage(const char *outdir, const char **subs, int nsubs)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/index.html", outdir);
	FILE *f = fopen(path, "w");
	if (!f) { perror("fopen"); return; }

	fprintf(f, "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n");
	fprintf(f, "<title>redmirror</title>\n<style>%s</style>\n", CSS);
	fprintf(f, "</head><body>\n");
	fprintf(f, "<h1>redmirror</h1>\n");
	fprintf(f, "<p>mirrored subreddits (updated every 6h):</p>\n");
	for (int i = 0; i < nsubs; i++)
		fprintf(f, "<div style=\"margin:6px 0;\"><a href=\"r/%s/index.html\">r/%s</a></div>\n", subs[i], subs[i]);
	fprintf(f, "<hr><p class=\"meta\">add subs by editing subs.txt</p>\n");
	fprintf(f, "</body></html>\n");
	fclose(f);
	printf("wrote %s\n", path);
}

static void
pages_export_sub(const char *sub, const char *outdir)
{
	char subdir[512];
	snprintf(subdir, sizeof(subdir), "%s/r/%s", outdir, sub);
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "mkdir -p %s", subdir);
	if (system(cmd) != 0) return;

	static_export(sub, "hot", subdir);
}

static void
pages_export(const char *outdir, const char **subs, int nsubs)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "mkdir -p %s", outdir);
	if (system(cmd) != 0) {
		fprintf(stderr, "failed to create output directory\n");
		return;
	}

	write_pages_homepage(outdir, subs, nsubs);

	/* .nojekyll so github pages serves raw html */
	char njpath[512];
	snprintf(njpath, sizeof(njpath), "%s/.nojekyll", outdir);
	FILE *nj = fopen(njpath, "w");
	if (nj) fclose(nj);

	for (int i = 0; i < nsubs; i++) {
		printf("\n=== r/%s ===\n", subs[i]);
		pages_export_sub(subs[i], outdir);
		/* delay between subs to avoid reddit rate limiting */
		if (i < nsubs - 1) {
			printf("waiting 3s...\n");
			sleep(3);
		}
	}

	printf("\ndone. site at %s/\n", outdir);
}

/* ---- main ---- */

static void
usage(void)
{
	fprintf(stderr, "usage:\n");
	fprintf(stderr, "  redmirror -s [port]                    start server\n");
	fprintf(stderr, "  redmirror <sub> [sort] [outdir]        static export\n");
	fprintf(stderr, "  redmirror -p <outdir> <sub1> [sub2..]  pages export\n");
	fprintf(stderr, "  sort: hot, new, top, rising (default: hot)\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	if (argc < 2) usage();

	signal(SIGPIPE, SIG_IGN);
	curl_global_init(CURL_GLOBAL_DEFAULT);

	/* server mode */
	if (strcmp(argv[1], "-s") == 0) {
		int port = SERVER_PORT;
		if (argc > 2) port = atoi(argv[2]);
		if (port <= 0) port = SERVER_PORT;
		serve(port);
		return 0;
	}

	/* pages mode: -p <outdir> <sub1> <sub2> ... */
	if (strcmp(argv[1], "-p") == 0) {
		if (argc < 4) usage();
		const char *outdir = argv[2];
		const char **subs = (const char **)&argv[3];
		int nsubs = argc - 3;
		pages_export(outdir, subs, nsubs);
		curl_global_cleanup();
		return 0;
	}

	/* single sub static export */
	const char *sub = argv[1];
	const char *sort = "hot";
	const char *outdir = "out";

	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "hot") || !strcmp(argv[i], "new") ||
		    !strcmp(argv[i], "top") || !strcmp(argv[i], "rising"))
			sort = argv[i];
		else
			outdir = argv[i];
	}

	static_export(sub, sort, outdir);
	curl_global_cleanup();
	return 0;
}
