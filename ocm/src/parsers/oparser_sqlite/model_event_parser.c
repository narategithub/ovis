/*
 * model_event_parser.c
 *
 *  Created on: Oct 22, 2013
 *      Author: nichamon
 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/queue.h>
#include <stddef.h>
#include <inttypes.h>
#include <sqlite3.h>

#include "oparser_util.h"
#include "model_event_parser.h"
#include "oquery_sqlite.h"

sqlite3 *db;

struct oparser_model_q model_q;
struct oparser_action_q action_q;
struct oparser_event_q event_q;

struct oparser_model *model;
struct oparser_action *action;
struct oparser_event *event;
int event_id;
struct oparser_sev_level *sevl;

struct oparser_name_queue mnqueue; /* metric name queue */
int num_mnames; /* Number of metric names */
char metric_name[128];
struct metric_id_s *mid_s;
char prod_comp_types[256];
struct oparser_name_queue cnqueue; /* Component name queue */
int num_cnames;
struct mae_metric_list metric_list;

struct mae_metric_comp {
	char metric_name[128];
	char prod_comp_type[512];
	LIST_ENTRY(mae_metric_comp) entry;
};
LIST_HEAD(mae_metric_comp_list, mae_metric_comp) metric_comp_list;

/* mae = model action event */
static enum mae_parser_obj {
	MAE_OBJ_MODEL = 0,
	MAE_OBJ_ACTION,
	MAE_OBJ_EVENT
} objective;

void oparser_mae_parser_init(sqlite3 *_db)
{
	TAILQ_INIT(&model_q);
	TAILQ_INIT(&action_q);
	TAILQ_INIT(&event_q);
	LIST_INIT(&metric_list);
	LIST_INIT(&metric_comp_list);
	event_id = 0;
	db = _db;
}

static void handle_model(char *value)
{
	objective = MAE_OBJ_MODEL;
	model = calloc(1, sizeof(*model));
	if (!model)
		oom_report(__FUNCTION__);
	TAILQ_INSERT_TAIL(&model_q, model, entry);
}

void handle_metric_name(char *value)
{
	struct mae_metric_comp *m;
	char prod_comps[512], tmp[512];
	char *s;
	char metric_name_full[512];
	int num;

	prod_comps[0] = '\0';
	num = sscanf(value, "%[^{]{%[^]]}", metric_name_full, tmp);

	char core_name[256], variation[256];
	/* at least one comp type is given */
	if (num == 2) {
		s = strtok(tmp, ",");
		sprintf(prod_comps, "'%s'", s);
		while (s = strtok(NULL, ",")) {
			sprintf(prod_comps, "%s,'%s'", prod_comps, s);
		}
	}

	char *wild_card = strstr(metric_name_full, "[*]");
	if (wild_card) {
		int len = wild_card - metric_name_full;
		wild_card = wild_card + strlen("[*]");
		m = malloc(sizeof(*m));
		sprintf(m->metric_name, "%.*s%%%s", len,
				metric_name_full, wild_card);
		sprintf(m->prod_comp_type, "%s", prod_comps);
		LIST_INSERT_HEAD(&metric_comp_list, m, entry);
		return;
	}

	char *need_expand = strchr(metric_name_full, '[');
	if (need_expand) {
		num = process_string_name(metric_name_full, &mnqueue,
							NULL, NULL);
		struct oparser_name *name;
		TAILQ_FOREACH(name, &mnqueue, entry) {
			m = malloc(sizeof(*m));
			sprintf(m->metric_name, "%s", name->name);
			sprintf(m->prod_comp_type, "%s", prod_comps);
			LIST_INSERT_HEAD(&metric_comp_list, m, entry);
		}
		destroy_name_list(&mnqueue);
	} else {
		/* Nothing to be expanded */
		m = malloc(sizeof(*m));
		sprintf(m->metric_name, "%s", metric_name_full);
		sprintf(m->prod_comp_type, "%s", prod_comps);
		LIST_INSERT_HEAD(&metric_comp_list, m, entry);
	}
}

static void handle_name(char *value)
{
	struct oparser_name *mname;
	struct mae_metric_comp *m;
	char **name;
	int num, i;
	char tmp[64];
	switch (objective) {
	case MAE_OBJ_MODEL:
		name = &model->name;
		goto name;
	case MAE_OBJ_ACTION:
		name = &action->name;
		goto name;
	case MAE_OBJ_EVENT:
		name = &event->name;
		goto name;
	default:
		fprintf(stderr, "invalid objective '%d'.\n", objective);
		exit(EINVAL);
	}
name:
	*name = strdup(value);
	if (!*name)
		oom_report(__FUNCTION__);
}

static void handle_model_id(char *value)
{
	int *model_id;
	switch (objective) {
	case MAE_OBJ_MODEL:
		model_id = &model->model_id;
		break;
	case MAE_OBJ_EVENT:
		model_id = &event->model_id;
		break;
	default:
		fprintf(stderr, "invalid objective '%d'.\n", objective);
		exit(EINVAL);
	}
	char *end;
	*model_id = (unsigned short) strtoul(value, &end, 10);
	if (!*value || *end) {
		fprintf(stderr, "%dmodel_id '%s' is not an integer.\n",
						-EINVAL, value);
		exit(EINVAL);
	}
}

static void handle_thresholds(char *value)
{
	model->thresholds = strdup(value);
	if (!model->thresholds)
		oom_report(__FUNCTION__);
}

static void handle_parameters(char *value)
{
	model->params = strdup(value);
	if (!model->params)
		oom_report(__FUNCTION__);
}

static void handle_report_flags(char *value)
{
	if (strlen(value) > 4) {
		fprintf(stderr, "report_flags need to be 4 binary digits, "
				"e.g. 0011. Received '%s'\n", value);
		exit(EINVAL);
	}
	sprintf(model->report_flags, "%s", value);
}

static void handle_action(char *value)
{
	objective = MAE_OBJ_ACTION;
	objective = MAE_OBJ_ACTION;
	action = calloc(1, sizeof(*action));
	if (!action)
		oom_report(__FUNCTION__);
	TAILQ_INSERT_TAIL(&action_q, action, entry);
	return;
}

static void handle_action_name(char *value)
{
	/* Reuse the num_mnames and mnqueue for the action list*/
	num_mnames = process_string_name(value, &mnqueue, NULL, NULL);
	struct oparser_name *name;
	int i = 0;
	TAILQ_FOREACH(name, &mnqueue, entry) {
		if (i == 0) {
			sprintf(sevl->action_name, "%s", name->name);
		} else {
			sprintf(sevl->action_name, "%s,%s",
				sevl->action_name, name->name);
		}
	}
	destroy_name_list(&mnqueue);
}

static void handle_execute(char *value)
{
	action->execute = strdup(value);
	if (!action->execute)
		oom_report(__FUNCTION__);
}

void mae_print_event();
static void handle_event(char *value)
{
	objective = MAE_OBJ_EVENT;
	event = calloc(1, sizeof(*event));
	if (!event)
		oom_report(__FUNCTION__);
	event_id++;
	event->event_id = event_id; /* start from 1 */
	LIST_INIT(&event->mid_list);
	LIST_INIT(&metric_list);
	TAILQ_INSERT_TAIL(&event_q, event, entry);
}

static void handle_metric(char *value)
{
	mid_s = calloc(1, sizeof(*mid_s));
	if (!mid_s)
		oom_report(__FUNCTION__);
	LIST_INSERT_HEAD(&event->mid_list, mid_s, entry);
	event->num_metric_id_set++;
	LIST_INIT(&metric_comp_list);
}

static void handle_components(char *value)
{
	struct mae_metric_comp *m;
	struct mae_metric *metric;
	int i;
	if (strcmp(value, "*") == 0) {
		LIST_FOREACH(m, &metric_comp_list, entry) {
			oquery_metric_id(m->metric_name, m->prod_comp_type,
						&metric_list, NULL, db);
		}
		goto assign;
	}

	num_cnames = process_string_name(value, &cnqueue, NULL, NULL);
	struct oparser_name *name;
	char coll_comps[1024];
	name = TAILQ_FIRST(&cnqueue);
	sprintf(coll_comps, "'%s'", name->name);
	while (name = TAILQ_NEXT(name, entry))
		sprintf(coll_comps, "%s,'%s'", coll_comps, name->name);
	sprintf(coll_comps, "%s", coll_comps);

	LIST_FOREACH(m, &metric_comp_list, entry) {
		oquery_metric_id(m->metric_name, m->prod_comp_type,
				&metric_list, coll_comps, db);
	}

	while (m = LIST_FIRST(&metric_comp_list)) {
		LIST_REMOVE(m, entry);
		free(m);
	}

assign:
	if (LIST_EMPTY(&metric_list)) {
		fprintf(stderr, "%d: Could not find the metrics", ENOENT);
		exit(ENOENT);
	}
	LIST_FOREACH(metric, &metric_list, entry)
		mid_s->num_metric_ids++;

	mid_s->metric_ids = malloc(mid_s->num_metric_ids * sizeof(uint64_t));
	if (!mid_s->metric_ids)
		oom_report(__FUNCTION__);
	i = 0;
	LIST_FOREACH(metric, &metric_list, entry) {
		mid_s->metric_ids[i] = metric->metric_id;
		i++;
	}

	while (metric = LIST_FIRST(&metric_list)) {
		LIST_REMOVE(metric, entry);
		free(metric);
	}
}

static void handle_severity(char *value)
{
}

enum mae_me_sevl str_to_enum_level(char *value)
{
	if (strcmp(value, "info") == 0)
		return MAE_INFO;
	else if (strcmp(value, "nominal") == 0)
		return MAE_NOMINAL;
	else if (strcmp(value, "warning") == 0)
		return MAE_WARNING;
	else if (strcmp(value, "critical") == 0)
		return MAE_CRITICAL;

	fprintf(stderr, "Invalid severity level: %s\n", value);
	exit(EINVAL);
}

char *enum_level_to_str(enum mae_me_sevl level)
{
	switch (level) {
	case MAE_INFO:
		return "INFO";
	case MAE_NOMINAL:
		return "NOMINAL";
	case MAE_WARNING:
		return "WARNING";
	case MAE_CRITICAL:
		return "CRITICAL";
	default:
		break;
	}
	return NULL;
}

static void handle_level(char *value)
{
	enum mae_me_sevl level = str_to_enum_level(value);
	sevl = &event->msg_level[level];
}

static void handle_msg(char *value)
{
	char *s = strstr(value, "$(name)");
	if (!s) {
		sevl->msg = strdup(value);
		goto out;
	}

	int len = s - value;
	s = s + strlen("$(name)");
	char _msg[1024];
	sprintf(_msg, "%.*s%s%s", len, value, event->name, s);
	sevl->msg = strdup(_msg);
out:
	if (!sevl->msg)
		oom_report(__FUNCTION__);
}

static struct kw label_tbl[] = {
	{ "action", handle_action },
	{ "action_name", handle_action_name },
	{ "components", handle_components },
	{ "event", handle_event },
	{ "execute", handle_execute },
	{ "level", handle_level },
	{ "metric", handle_metric },
	{ "metric_name", handle_metric_name },
	{ "model", handle_model },
	{ "model_id", handle_model_id },
	{ "msg", handle_msg },
	{ "name", handle_name },
	{ "parameters", handle_parameters },
	{ "report_flags", handle_report_flags },
	{ "severity", handle_severity },
	{ "thresholds", handle_thresholds },
};

void oparser_parse_model_event_conf(FILE *conf)
{
	char buf[512];
	char key[128], value[256];
	char *s;

	struct kw keyword;
	struct kw *kw;

	fseek(conf, 0, SEEK_SET);

	while (s = fgets(buf, sizeof(buf), conf)) {
		sscanf(buf, " %[^:]: %[^\n]", key, value);
		trim_trailing_space(value);
		/* Ignore the comment line */
		if (key[0] == '#')
			continue;

		keyword.token = key;
		kw = bsearch(&keyword, label_tbl, ARRAY_SIZE(label_tbl),
				sizeof(*kw), kw_comparator);

		if (kw) {
			kw->action(value);
		} else {
			fprintf(stderr, "Invalid key '%s'\n", key);
			exit(EINVAL);
		}

	}
}

void mae_print_model(struct oparser_model *model, FILE *out)
{
	fprintf(out, "model:\n");
	fprintf(out, "\tname: %s\n", model->name);
	fprintf(out, "\tmodel_id: %d\n", model->model_id);
	fprintf(out, "\tthresholds: %s\n", model->thresholds);
	fprintf(out, "\tparam: %s\n", model->params);
	fprintf(out, "\treport_flags: %s\n", model->report_flags);
}

void mae_print_action(struct oparser_action *action, FILE *out)
{
	fprintf(out, "action:\n");
	fprintf(out, "\tname: %s\n", action->name);
	fprintf(out, "\texecute: %s\n", action->execute);
}

void mae_print_event(struct oparser_event *event, FILE *out)
{
	fprintf(out, "event:\n");
	fprintf(out, "\tevent_id: %d\n", event->event_id);

	int i, num = 0;
	LIST_FOREACH(mid_s, &event->mid_list, entry) {
		for (i = 0; i < mid_s->num_metric_ids; i++) {
			if (num == 0) {
				fprintf(out, "\tmetric_ids: %" PRIu64 "",
						mid_s->metric_ids[0]);
			} else {
				fprintf(out, ",%" PRIu64 "",
						mid_s->metric_ids[i]);
			}
			num++;
		}
	}

	fprintf(out, "\n");
	for (i = 0; i < MAE_NUM_LEVELS; i++) {
		fprintf(out, "\t\tlevel: %s\n", enum_level_to_str(i));
		fprintf(out, "\t\t\tmsg: %s\n", event->msg_level[i].msg);
		fprintf(out, "\t\t\taction_name: %s\n",
					event->msg_level[i].action_name);
	}
	fprintf(out, "\n");
}

void oparser_print_models_n_rules(FILE *out)
{
	TAILQ_FOREACH(model, &model_q, entry)
		mae_print_model(model, out);

	TAILQ_FOREACH(action, &action_q, entry)
		mae_print_action(action, out);

	TAILQ_FOREACH(event, &event_q, entry)
		mae_print_event(event, out);
}

void model_to_sqlite(struct oparser_model *model, sqlite3 *db)
{
	char *core = "INSERT INTO models(name, model_id, params, thresholds, "
			"report_flags) VALUES(";
	char stmt[1024];
	char vary[512];

	sprintf(vary, "'%s', %d", model->name, model->model_id);
	if (!model->params)
		sprintf(vary, "%s, NULL", vary);
	else
		sprintf(vary, "%s, '%s'", vary, model->params);

	if (!model->thresholds)
		sprintf(vary, "%s, NULL", vary);
	else
		sprintf(vary, "%s, '%s'", vary, model->thresholds);

	if (!model->report_flags)
		sprintf(vary, "%s, NULL", vary);
	else
		sprintf(vary, "%s, '%s'", vary, model->report_flags);

	sprintf(stmt, "%s%s);", core, vary);
	insert_data(stmt, db);
}

void oparser_models_to_sqlite()
{
	oparser_drop_table("models", db);
	int rc;
	char *stmt;
	stmt = "CREATE TABLE IF NOT EXISTS models(" \
			" name		CHAR(64)	NOT NULL," \
			" model_id	INTEGER	PRIMARY KEY	NOT NULL," \
			" params	TEXT," \
			" thresholds	TEXT," \
			" report_flags	CHAR(4));";

	char *index_stmt = "CREATE INDEX IF NOT EXISTS models_idx ON models(name);";
	create_table(stmt, index_stmt, db);

	struct oparser_model *model;
	TAILQ_FOREACH(model, &model_q, entry) {
		model_to_sqlite(model, db);
	}
}

void oparser_actions_to_sqlite()
{
	oparser_drop_table("actions", db);
	int rc;
	char *stmt;
	stmt = "CREATE TABLE IF NOT EXISTS actions(" \
			" name		CHAR(128) PRIMARY KEY	NOT NULL," \
			" execute	TEXT	NOT NULL);";

	create_table(stmt, NULL, db);

	char *core = "INSERT INTO actions(name, execute) VALUES(";
	char vary[512], insert_stmt[1024];
	struct oparser_action *action;
	TAILQ_FOREACH(action, &action_q, entry) {
		sprintf(vary, "'%s', '%s'", action->name, action->execute);
		sprintf(insert_stmt, "%s%s);", core, vary);
		insert_data(insert_stmt, db);
	}
}

void event_to_sqlite(struct oparser_event *event, sqlite3 *db)
{
	char value[1024], stmt[2048];
	sprintf(stmt, "INSERT INTO rule_templates(event_id, event_name, "
			"model_id) VALUES(%d, '%s', %d);", event->event_id,
			event->name, event->model_id);

	insert_data(stmt, db);

	char *core = "INSERT INTO rule_actions(event_id, level, message, "
							"action_name) VALUES(";

	int i;
	for (i = 0; i < MAE_NUM_LEVELS; i++) {
		if (!event->msg_level[i].msg)
			sprintf(value, "%d, NULL", i);
		else
			sprintf(value, "%d, '%s'", i,
					event->msg_level[i].msg);

		if (event->msg_level[i].action_name[0] == '\0')
			sprintf(value, "%s, NULL", value);
		else
			sprintf(value, "%s, '%s'", value,
					event->msg_level[i].action_name);

		if (!event->msg_level[i].msg &&
				event->msg_level[i].action_name[0] == '\0') {
			value[0] = '\0';
			continue;
		}

		sprintf(stmt, "%s%d, %s);", core, event->event_id, value);
		insert_data(stmt, db);
		value[0] = '\0';
	}

	core = "INSERT INTO rule_metrics(event_id, metric_id) VALUES(";
	value[0] = stmt[0] = '\0';

	struct metric_id_s *mid;
	LIST_FOREACH(mid, &event->mid_list, entry) {
		for (i = 0; i < mid->num_metric_ids; i++) {
			sprintf(value, "%" PRIu64 "", mid->metric_ids[i]);
			sprintf(stmt, "%s%d, %s);", core, event->event_id,
								value);
			insert_data(stmt, db);
		}
	}
}

void oparser_events_to_sqlite()
{
	oparser_drop_table("rule_templates", db);
	oparser_drop_table("rule_actions", db);
	oparser_drop_table("rule_metrics", db);
	int rc;
	char *stmt;
	stmt = "CREATE TABLE IF NOT EXISTS rule_templates (" \
			" event_id	INTEGER PRIMARY KEY	NOT NULL," \
			" event_name	TEXT			NOT NULL," \
			" model_id	INTEGER			NOT NULL);";
	create_table(stmt, NULL, db);

	stmt = "CREATE TABLE IF NOT EXISTS rule_actions (" \
			" event_id	INTEGER		NOT NULL," \
			" level		SMALLINT	NOT NULL," \
			" message	TEXT," \
			" action_name	CHAR(128)," \
			" PRIMARY KEY(event_id, level));";

	char *index_stmt = "CREATE INDEX IF NOT EXISTS rule_actions_idx"
					" ON rule_actions(event_id,level);";
	create_table(stmt, index_stmt, db);

	stmt = "CREATE TABLE IF NOT EXISTS rule_metrics (" \
			" event_id	INTEGER		NOT NULL," \
			" metric_id	SQLITE_uint64	NOT NULL);";

	index_stmt = "CREATE INDEX IF NOT EXISTS rule_metrics_idx ON "
					"rule_metrics(event_id,metric_id);";
	create_table(stmt, index_stmt, db);

	struct oparser_event *event;
	TAILQ_FOREACH(event, &event_q, entry) {
		event_to_sqlite(event, db);
	}
}

#ifdef MAIN
#include <stdarg.h>
#include <getopt.h>

#define FMT "m:d:o:"

int main(int argc, char **argv) {
	int rc, op;
	char *mae_conf, *db_path, *out_path;
	FILE *maef, *out;

	while ((op = getopt(argc, argv, FMT)) != -1) {
		switch (op) {
		case 'm':
			mae_conf = strdup(optarg);
			break;
		case 'd':
			db_path = strdup(optarg);
			break;
		case 'o':
			out_path = strdup(optarg);
			break;
		default:
			fprintf(stderr, "Invalid argument '%c'\n", op);
			exit(EINVAL);
		}
	}

	maef = fopen(mae_conf, "r");
	if (!maef) {
		fprintf(stderr, "Could not open the conf file '%s'\n", mae_conf);
		exit(errno);
	}

	out = fopen(out_path, "w");
	if (!maef) {
		fprintf(stderr, "Could not open the conf file '%s'\n", out_path);
		exit(errno);
	}

	sqlite3 *db;
	char *err_msg = 0;
	rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL);
	if (rc) {
		fprintf(stderr, "%d: Failed to open sqlite '%s': %s\n",
				rc, db_path, sqlite3_errmsg(db));
		sqlite3_close(db);
		sqlite3_free(err_msg);
		exit(rc);
	}

	oparser_mae_parser_init(db);
	printf("start...\n");
	oparser_parse_model_event_conf(maef);
	printf("finish parsing ... \n");
	oparser_print_models_n_rules(out);
	fflush(out);

	oparser_models_to_sqlite();
	printf("Finish models table\n");
	oparser_actions_to_sqlite();
	printf("Finish actions table\n");
	oparser_events_to_sqlite();
	printf("Finish events table\n");
	return 0;
}
#endif
