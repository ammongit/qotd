/*
 * configuration.c
 * qotd - A simple QOTD daemon.
 * Copyright (c) 2015-2016 Ammon Smith
 *
 * qotd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * qotd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with qotd.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <limits.h>
#include <strings.h>
#include <unistd.h>

#include "configuration.h"
#include "daemon.h"
#include "journal.h"
#include "security.h"
#include "standard.h"

#define PORT_MAX		65535    /* Not in limits.h */
#define BUFFER_SIZE		PATH_MAX /* A (very safe) magic number */

struct buffer {
	char data[BUFFER_SIZE];
	size_t length;
};

static bool read_key_and_value(
	const char *conf_file, unsigned int lineno, const struct buffer *line,
	struct buffer *key, struct buffer *val);
static bool file_read_line(
	FILE *fh, const char *filename, unsigned int *lineno, struct buffer *line);
static bool str_to_bool(
	const char *string, const char *filename, unsigned int lineno, bool *success);

void parse_config(const char *conf_file, struct options *opt)
{
	FILE *fh;
	struct buffer line;
	unsigned int lineno = 1;
	int errors = 0;

	/* Journal hasn't been opened yet */
	printf("Reading configuration file at \"%s\"...\n", conf_file);

	if (opt->strict) {
		security_conf_file_check(conf_file);
	}

	fh = fopen(conf_file, "r");
	if (fh == NULL) {
		fprintf(stderr, "Unable to open configuration file \"%s\": %s.\n",
			conf_file, strerror(errno));
		cleanup(EXIT_IO, true);
	}

#if DEBUG
	printf("Raw key/value pairs from config file:\n");
#endif /* DEBUG */

	while (file_read_line(fh, conf_file, &lineno, &line)) {
		struct buffer key, val;
		bool success;
		int ch;

		if (!read_key_and_value(conf_file, lineno, &line, &key, &val)) {
			if (key.length) {
				errors++;
			}

			continue;
		}

		/* Check each possible option */
		if (!strcasecmp(key.data, "Daemonize")) {
			ch = str_to_bool(val.data, conf_file, lineno, &success);

			if (likely(success)) {
				opt->daemonize = ch;
			} else {
				errors++;
			}
		} else if (!strcasecmp(key.data, "TransportProtocol")) {
			if (!strcasecmp(val.data, "tcp")) {
				opt->tproto = PROTOCOL_TCP;
			} else if (!strcasecmp(val.data, "udp")) {
				opt->tproto = PROTOCOL_UDP;
			} else {
				fprintf(stderr, "%s:%d: invalid transport protocol: \"%s\"\n",
					conf_file, lineno, val.data);
				errors++;
			}
		} else if (!strcasecmp(key.data, "InternetProtocol")) {
			if (!strcasecmp(val.data, "both")) {
				opt->iproto = PROTOCOL_BOTH;
			} else if (!strcasecmp(val.data, "ipv4")) {
				opt->iproto = PROTOCOL_IPv4;
			} else if (!strcasecmp(val.data, "ipv6")) {
				opt->iproto = PROTOCOL_IPv6;
			} else {
				fprintf(stderr, "%s:%d: invalid internet protocol: \"%s\"\n",
					conf_file, lineno, val.data);
				errors++;
			}
		} else if (!strcasecmp(key.data, "Port")) {
			/* atoi is ok because it returns 0 in case of failure, and 0 isn't a valid port */
			int port = atoi(val.data);
			if (0 >= port || port > PORT_MAX) {
				fprintf(stderr, "%s:%d: invalid port number: \"%s\"\n",
					conf_file, lineno, val.data);
				errors++;
			} else {
				opt->port = port;
			}
		} else if (!strcasecmp(key.data, "StrictChecking")) {
			ch = str_to_bool(val.data, conf_file, lineno, &success);

			if (likely(success)) {
				opt->strict = ch;
			} else {
				errors++;
			}
		} else if (!strcasecmp(key.data, "DropPrivileges")) {
			ch = str_to_bool(val.data, conf_file, lineno, &success);

			if (likely(success)) {
				opt->drop_privileges = ch;
			} else {
				errors++;
			}
		} else if (!strcasecmp(key.data, "PidFile")) {
			if (!strcmp(val.data, "none")) {
				opt->pid_file = NULL;
				continue;
			}

			opt->pid_file = strdup(val.data);
			if (unlikely(!opt->pid_file)) {
				perror("Unable to allocate memory for config value");
				fclose(fh);
				cleanup(EXIT_MEMORY, true);
			}
		} else if (!strcasecmp(key.data, "RequirePidFile")) {
			ch = str_to_bool(val.data, conf_file, lineno, &success);

			if (likely(success)) {
				opt->require_pidfile = ch;
			} else {
				errors++;
			}
		} else if (!strcasecmp(key.data, "JournalFile")) {
			if (!strcmp(val.data, "-")) {
				opt->journal_file = NULL;
			} else if (strcmp(val.data, "none") != 0) {
				opt->journal_file = strdup(val.data);
				if (unlikely(!opt->journal_file)) {
					perror("Unable to allocate memory for config value");
					fclose(fh);
					cleanup(EXIT_MEMORY, true);
				}
			}
		} else if (!strcasecmp(key.data, "QuotesFile")) {
			opt->quotes_file = strdup(val.data);
			if (unlikely(!opt->quotes_file)) {
				perror("Unable to allocate memory for config value");
				fclose(fh);
				cleanup(EXIT_MEMORY, true);
			}
		} else if (!strcasecmp(key.data, "QuoteDivider")) {
			if (!strcasecmp(val.data, "line")) {
				opt->linediv = DIV_EVERYLINE;
			} else if (!strcasecmp(val.data, "percent")) {
				opt->linediv = DIV_PERCENT;
			} else if (!strcasecmp(val.data, "file")) {
				opt->linediv = DIV_WHOLEFILE;
			} else {
				fprintf(stderr,
					"%s:%d: unsupported division type: \"%s\"\n",
					conf_file, lineno, val.data);
				errors++;
			}
		} else if (!strcasecmp(key.data, "PadQuotes")) {
			ch = str_to_bool(val.data, conf_file, lineno, &success);

			if (likely(success)) {
				opt->pad_quotes = ch;
			} else {
				errors++;
			}
		} else if (!strcasecmp(key.data, "DailyQuotes")) {
			ch = str_to_bool(val.data, conf_file, lineno, &success);

			if (likely(success)) {
				opt->is_daily = ch;
			} else {
				errors++;
			}
		} else if (!strcasecmp(key.data, "AllowBigQuotes")) {
			ch = str_to_bool(val.data, conf_file, lineno, &success);

			if (likely(success)) {
				opt->allow_big = ch;
			} else {
				errors++;
			}
		} else {
			fprintf(stderr, "%s:%d: unknown conf option: \"%s\"\n",
				conf_file, lineno, key.data);
			errors++;
		}

		lineno++;
	}

	fclose(fh);

	if (opt->strict && errors) {
		fprintf(stderr,
			"Your configuration file has %d issue%s. The daemon will not start.\n"
			"(To disable this behavior, use the --lax flag when running).\n",
			errors, PLURAL(errors));
		cleanup(EXIT_SECURITY, true);
	}
}

static bool read_key_and_value(
	const char *conf_file, unsigned int lineno, const struct buffer *line,
	struct buffer *key, struct buffer *val)
{
	size_t l_index; /* position in the line */
	size_t b_index; /* position in the buffer */
	int ch;         /* current character */

	/* Eat up whitespace */
	for (l_index = 0; isspace(line->data[l_index]); l_index++);

	/* Ignore comments and blank lines */
	if (line->data[l_index] == '\0' || line->data[l_index] == '#') {
		key->length = 0;
		return false;
	}

	/* Read the key */
	for (b_index = 0; !isspace((ch = line->data[l_index])); b_index++, l_index++) {
		/* We don't need to check for buffer overflows here because line->data
		 * is at most as long as this buffer
		 */
		key->data[b_index] = ch;

		if (ch == '\0') {
			fprintf(stderr, "%s:%d: unexpected end of line.\n", conf_file, lineno);
			return false;
		}
	}

	/* Terminate key string */
	key->data[b_index] = '\0';
	key->length = b_index;

	/* Eat up whitespace */
	for (; isspace(line->data[l_index]); l_index++);

	/* Read the value */
	for (b_index = 0; !isspace((ch = line->data[l_index])); b_index++, l_index++) {
		/* No buffer size check for reasons mentioned above */
		val->data[b_index] = ch;

		if (ch == '\0') {
			break;
		}
	}

	/* Terminate value string */
	val->data[b_index] = '\0';
	val->length = b_index;

#if DEBUG
	printf("[%s] = [%s]\n", key->data, val->data);
#endif /* DEBUG */

	/* Remove quotes around the value */
	if (val->length > 1 &&
		((val->data[0] == '\'' && val->data[val->length - 1] == '\'') ||
		 (val->data[0] == '\"' && val->data[val->length - 1] == '\"'))) {
		size_t i;
		for (i = 0; i < val->length - 2; i++) {
			val->data[i] = val->data[i + 1];
		}

		val->length -= 2;
		val->data[val->length] = '\0';

#if DEBUG
	printf("DEQUOTED: %s\n", val->data);
#endif /* DEBUG */
	}

	return true;
}

static bool file_read_line(
	FILE *fh, const char *filename, unsigned int *lineno, struct buffer *line)
{
	int i;
	int ch;

	for (i = 0; i < BUFFER_SIZE; i++) {
		ch = fgetc(fh);

		if (ch == '\n') {
			if (i) {
				line->data[i] = '\0';
				line->length = i;
				return true;
			} else {
				/* Ignore empty lines */
				i--;
				(*lineno)++;
			}
		} else if (ch == EOF) {
			if (i) {
				/* Return rest of line when EOF hits before newline */
				line->data[i] = '\0';
				line->length = i;
				return true;
			} else {
				/* EOF */
				return false;
			}
		} else {
			line->data[i] = (char)ch;
		}
	}

	/* Ran out of buffer space */
	fprintf(stderr, "%s:%d: line is too long, must be under %d bytes.\n", filename, *lineno, BUFFER_SIZE);
	cleanup(EXIT_CONFIGURATION, true);
	return false;
}

static bool str_to_bool(const char *string, const char *filename, unsigned int lineno, bool *success)
{
	if (!strcasecmp(string, "yes") ||
	    !strcasecmp(string, "true") ||
	    !strcmp(string, "1")) {
		*success = true;
		return true;
	} else if (!strcasecmp(string, "no") ||
	           !strcasecmp(string, "false") ||
	           !strcmp(string, "0")) {
		*success = true;
		return false;
	} else {
		fprintf(stderr, "%s:%d: not a boolean value: \"%s\"\n", filename, lineno, string);
		*success = false;
		return false;
	}
}

