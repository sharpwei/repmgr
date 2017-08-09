/*
 * repmgr-action-standby.c
 *
 * Implements standby actions for the repmgr command line utility
 *
 * Copyright (c) 2ndQuadrant, 2010-2017
 */

#include <sys/stat.h>

#include "repmgr.h"
#include "dirutil.h"
#include "compat.h"
#include "controldata.h"

#include "repmgr-client-global.h"
#include "repmgr-action-standby.h"


typedef struct TablespaceDataListCell
{
	struct TablespaceDataListCell *next;
	char	   *name;
	char	   *oid;
	char	   *location;
	/* optional payload */
	FILE       *f;
} TablespaceDataListCell;

typedef struct TablespaceDataList
{
	TablespaceDataListCell *head;
	TablespaceDataListCell *tail;
} TablespaceDataList;


static PGconn  *primary_conn = NULL;
static PGconn  *source_conn = NULL;

static char		local_data_directory[MAXPGPATH];
static bool		local_data_directory_provided = false;

static bool		upstream_record_found = false;
static int	    upstream_node_id = UNKNOWN_NODE_ID;
static char		upstream_data_directory[MAXPGPATH];

static t_conninfo_param_list recovery_conninfo;
static char		recovery_conninfo_str[MAXLEN];
static char		upstream_repluser[NAMEDATALEN];

static int  	source_server_version_num = UNKNOWN_SERVER_VERSION_NUM;

static t_configfile_list config_files = T_CONFIGFILE_LIST_INITIALIZER;

static standy_clone_mode mode;

/* used by barman mode */
static char		local_repmgr_tmp_directory[MAXPGPATH];
static char		datadir_list_filename[MAXLEN];
static char		barman_command_buf[MAXLEN] = "";

static void _do_standby_promote_internal(const char *data_dir);

static void check_barman_config(void);
static void	check_source_server(void);
static void	check_source_server_via_barman(void);
static void check_primary_standby_version_match(PGconn *conn, PGconn *primary_conn);
static void check_recovery_type(PGconn *conn);


static void initialise_direct_clone(t_node_info *node_record);
static int run_basebackup(t_node_info *node_record);
static int run_file_backup(t_node_info *node_record);

static void copy_configuration_files(void);

static void drop_replication_slot_if_exists(PGconn *conn, int node_id, char *slot_name);

static void tablespace_data_append(TablespaceDataList *list, const char *name, const char *oid, const char *location);

static void get_barman_property(char *dst, char *name, char *local_repmgr_directory);
static int  get_tablespace_data_barman(char *, TablespaceDataList *);
static char *make_barman_ssh_command(char *buf);

static NodeStatus parse_node_status_is_shutdown(const char *node_status_output, XLogRecPtr *checkPoint);
static CheckStatus parse_node_check_archiver(const char *node_check_output, int *files, int *threshold);
static CheckStatus parse_node_check_replication_lag(const char *node_check_output, int *seconds, int *threshold);

/*
 * do_standby_clone()
 *
 * Event(s):
 *  - standby_clone
 */

void
do_standby_clone(void)
{
	PQExpBufferData event_details;
	int r;

	/* dummy node record */
	t_node_info node_record = T_NODE_INFO_INITIALIZER;

	/*
	 * conninfo params for the actual upstream node (which might be different
	 * to the node we're cloning from) to write to recovery.conf
	 */

	mode = get_standby_clone_mode();

	/*
	 * Copy the provided data directory; if a configuration file was provided,
	 * use the (mandatory) value from that; if -D/--pgdata was provided, use that;
	 * otherwise repmgr will default to using the same directory path as on the
	 * source host. The last case will only ever occur when executing "repmgr
	 * standby clone" with no configuration file.
	 *
	 * Note that barman mode requires -D/--pgdata.
	 *
	 * If no data directory is explicitly provided, and we're not cloning from barman,
	 * the source host's data directory will be fetched later, after
	 * we've connected to it, in check_source_server().
	 *
	 */

	get_node_data_directory(local_data_directory);
	if (local_data_directory[0] != '\0')
	{
		local_data_directory_provided = true;
		log_notice(_("destination directory \"%s\" provided"),
				   local_data_directory);
	}
	else if (mode == barman)
	{
		/* XXX in Barman mode it's still possible to connect to the upstream,
		 * so only fail if that's not available.
		 */
		log_error(_("Barman mode requires a data directory"));
		log_hint(_("use -D/--pgdata to explicitly specify a data directory"));
		exit(ERR_BAD_CONFIG);
	}

	/* Sanity-check barman connection and installation */
	if (mode == barman)
	{
		/* this will exit with ERR_BARMAN if problems found */
		check_barman_config();
	}

	init_node_record(&node_record);
	node_record.type = STANDBY;

	/*
	 * Initialise list of conninfo parameters which will later be used
	 * to create the `primary_conninfo` string in recovery.conf .
	 *
	 * We'll initialise it with the default values as seen by libpq,
	 * and overwrite them with the host settings specified on the command
	 * line. As it's possible the standby will be cloned from a node different
	 * to its intended upstream, we'll later attempt to fetch the
	 * upstream node record and overwrite the values set here with
	 * those from the upstream node record (excluding that record's
	 * application_name)
	 */
	initialize_conninfo_params(&recovery_conninfo, true);

	copy_conninfo_params(&recovery_conninfo, &source_conninfo);

	/* Set the default application name to this node's name */
	param_set(&recovery_conninfo, "application_name", config_file_options.node_name);

	/*
	 * If application_name is set in repmgr.conf's conninfo parameter, use
	 * this value (if the source host was provided as a conninfo string, any
	 * application_name values set there will be overridden; we assume the only
	 * reason to pass an application_name via the command line is in the
	 * rare corner case where a user wishes to clone a server without
	 * providing repmgr.conf)
	 */
	if (strlen(config_file_options.conninfo))
	{
		char application_name[MAXLEN] = "";

		get_conninfo_value(config_file_options.conninfo, "application_name", application_name);
		if (strlen(application_name))
		{
			param_set(&recovery_conninfo, "application_name", application_name);
		}
	}


	/*
	 * --upstream-conninfo supplied, which we interpret to imply
	 * --no-upstream-connection as well - the use case for this option is when
	 * the upstream is not available, so no point in checking for it.
	 *
	 * XXX not sure of the logic here (and yes I did think this up)
	 *  - we'll need the source connection in any case, just won't connect
	 *    to the "upstream_conninfo" server. We'd probably need to
	 *    to override "no_upstream_connection" if connection params
	 *    actually provided.
	 */

	if (*runtime_options.upstream_conninfo)
		runtime_options.no_upstream_connection = false;

	/* By default attempt to connect to the source server */
	if (runtime_options.no_upstream_connection == false)
	{
		check_source_server();
	}

	if (mode == barman && PQstatus(source_conn) != CONNECTION_OK)
	{
		/*
		 * Here we don't have a connection to the upstream node, and are executing
		 * in Barman mode - we can try and connect via the Barman server to extract
		 * the upstream node's conninfo string.
		 *
		 * To do this we need to extract Barman's conninfo string, replace the database
		 * name with the repmgr one (they could well be different) and remotely execute
		 * psql.
		 */
		check_source_server_via_barman();
	}

	/*
	 * by this point we should know the target data directory - check
	 * there's no running Pg instance
	 */
	if (is_pg_dir(local_data_directory))
	{
		DBState state = get_db_state(local_data_directory);

		if (state != DB_SHUTDOWNED && state != DB_SHUTDOWNED_IN_RECOVERY)
		{
			log_error(_("target data directory appears to contain an active PostgreSQL instance"));
			log_detail(_("instance state is %s"), describe_db_state(state));
			exit(ERR_BAD_CONFIG);
		}
	}


	if (upstream_record_found == true)
	{
		/*  parse returned upstream conninfo string to recovery primary_conninfo params*/
		char	   *errmsg = NULL;
		bool	    parse_success;

		log_verbose(LOG_DEBUG, "parsing upstream conninfo string \"%s\"", recovery_conninfo_str);

		/* parse_conninfo_string() here will remove the upstream's `application_name`, if set */

		parse_success = parse_conninfo_string(recovery_conninfo_str, &recovery_conninfo, errmsg, true);
		if (parse_success == false)
		{
			log_error(_("unable to parse conninfo string \"%s\" for upstream node:\n  %s"),
					  recovery_conninfo_str, errmsg);

			PQfinish(source_conn);
			exit(ERR_BAD_CONFIG);
		}

		/* Write the replication user from the node's upstream record */
		param_set(&recovery_conninfo, "user", upstream_repluser);
	}
	else
	{
		/*
		 * If no upstream node record found, we'll abort with an error here,
		 * unless -F/--force is used, in which case we'll use the parameters
		 * provided on the command line (and assume the user knows what they're
		 * doing).
		 */

		if (!runtime_options.force)
		{
			log_error(_("no record found for upstream node (upstream_node_id: %i)"),
					  upstream_node_id);
			log_hint(_("use -F/--force to create \"primary_conninfo\" based on command-line parameters"));
			PQfinish(source_conn);
			exit(ERR_BAD_CONFIG);
		}
	}

	if (mode != barman)
	{
		initialise_direct_clone(&node_record);
	}

	switch (mode)
	{
		case pg_basebackup:
			log_notice(_("starting backup (using pg_basebackup)..."));
			break;
		case barman:
			log_notice(_("retrieving backup from Barman..."));
			break;
		default:
			/* should never reach here */
			log_error(_("unknown clone mode"));
	}

	if (mode == pg_basebackup)
	{
		if (runtime_options.fast_checkpoint == false)
		{
			log_hint(_("this may take some time; consider using the -c/--fast-checkpoint option"));
		}
	}

	if (mode == pg_basebackup)
	{
		r = run_basebackup(&node_record);
	}
	else
	{
		r = run_file_backup(&node_record);
	}


	/* If the backup failed then exit */
	if (r != SUCCESS)
	{
		/* If a replication slot was previously created, drop it */
		if (config_file_options.use_replication_slots == true)
		{
			drop_replication_slot(source_conn, node_record.slot_name);
		}

		log_error(_("unable to take a base backup of the primary server"));
		log_warning(_("data directory (%s) may need to be cleaned up manually"),
					local_data_directory);

		PQfinish(source_conn);
		exit(r);
	}


	/*
	 * If `--copy-external-config-files` was provided, copy any configuration
	 * files detected to the appropriate location. Any errors encountered
	 * will not be treated as fatal.
	 *
	 * This won't run in Barman mode as "config_files" is only populated in
	 * "initialise_direct_clone()", which isn't called in Barman mode.
	 */
	if (runtime_options.copy_external_config_files && config_files.entries)
	{
		copy_configuration_files();
	}

	/* Write the recovery.conf file */

	create_recovery_file(&node_record, &recovery_conninfo, local_data_directory);

	switch(mode)
	{
		case pg_basebackup:
			log_notice(_("standby clone (using pg_basebackup) complete"));
			break;

		case barman:
			log_notice(_("standby clone (from Barman) complete"));
			break;
	}

	/*
	 * TODO: It might be nice to provide an option to have repmgr start
	 * the PostgreSQL server automatically
	 */

	log_notice(_("you can now start your PostgreSQL server"));

	if (config_file_options.service_start_command[0] != '\0')
	{
		log_hint(_("for example: %s"),
				 config_file_options.service_start_command);
	}
	else if (local_data_directory_provided)
	{
		log_hint(_("for example: pg_ctl -D %s start"),
				 local_data_directory);
	}
	else
	{
		log_hint(_("for example: /etc/init.d/postgresql start"));
	}

	/*
	 * XXX forgetting to (re) register the standby is a frequent cause
	 * of error; we should consider having repmgr automatically
	 * register the standby, either by default with an option
	 * "--no-register", or an option "--register".
	 *
	 * Note that "repmgr standby register" requires the standby to
	 * be running - if not, and we just update the node record,
	 * we'd have an incorrect representation of the replication cluster.
	 * Best combined with an automatic start of the server (see note
	 * above)
	 */

	/*
	 * Check for an existing node record, and output the appropriate
	 * command for registering or re-registering.
	 */
	{
		t_node_info node_record = T_NODE_INFO_INITIALIZER;
		RecordStatus record_status;

		record_status = get_node_record(primary_conn,
										config_file_options.node_id,
										&node_record);

		if (record_status == RECORD_FOUND)
		{
			log_hint(_("after starting the server, you need to re-register this standby with \"repmgr standby register --force\" to overwrite the existing node record"));
		}
		else
		{
			log_hint(_("after starting the server, you need to register this standby with \"repmgr standby register\""));

		}
	}


	/* Log the event */

	initPQExpBuffer(&event_details);

	/* Add details about relevant runtime options used */
	appendPQExpBuffer(&event_details,
					  _("cloned from host '%s', port %s"),
					  runtime_options.host,
					  runtime_options.port);

	appendPQExpBuffer(&event_details,
					  _("; backup method: "));

	switch(mode)
	{
		case pg_basebackup:
			appendPQExpBuffer(&event_details, "pg_basebackup");
			break;
		case barman:
			appendPQExpBuffer(&event_details, "barman");
			break;
	}

	appendPQExpBuffer(&event_details,
					  _("; --force: %s"),
					  runtime_options.force ? "Y" : "N");

	create_event_notification(primary_conn,
						&config_file_options,
						config_file_options.node_id,
						"standby_clone",
						true,
						event_details.data);

	if (PQstatus(primary_conn) == CONNECTION_OK)
		PQfinish(primary_conn);

	if (PQstatus(source_conn) == CONNECTION_OK)
		PQfinish(source_conn);

	exit(r);
}


void
check_barman_config(void)
{
	char		command[MAXLEN];
	bool		command_ok;

	/*
	 * Check that there is at least one valid backup
	 */

	log_info(_("connecting to Barman server to verify backup for %s"), config_file_options.barman_server);

	maxlen_snprintf(command, "%s show-backup %s latest > /dev/null",
					make_barman_ssh_command(barman_command_buf),
				 	config_file_options.barman_server);

	command_ok = local_command(command, NULL);

	if (command_ok == false)
	{
		log_error(_("no valid backup for server %s was found in the Barman catalogue"),
				  config_file_options.barman_server);
		log_hint(_("refer to the Barman documentation for more information"));

		exit(ERR_BARMAN);
	}


	if (!create_pg_dir(local_data_directory, runtime_options.force))
	{
		log_error(_("unable to use directory %s"),
				local_data_directory);
		log_hint(_("use -F/--force option to force this directory to be overwritten"));
		exit(ERR_BAD_CONFIG);
	}


	/*
	 * Create the local repmgr subdirectory
	 */

	maxlen_snprintf(local_repmgr_tmp_directory,
					"%s/repmgr", local_data_directory);

	maxlen_snprintf(datadir_list_filename,
					"%s/data.txt", local_repmgr_tmp_directory);

	if (!create_pg_dir(local_repmgr_tmp_directory, runtime_options.force))
	{
		log_error(_("unable to create directory \"%s\""),
				  local_repmgr_tmp_directory);

		exit(ERR_BAD_CONFIG);
	}

	/*
	 * Fetch server parameters from Barman
	 */
	log_info(_("connecting to Barman server to fetch server parameters"));

	maxlen_snprintf(command, "%s show-server %s > %s/show-server.txt",
					make_barman_ssh_command(barman_command_buf),
					config_file_options.barman_server,
					local_repmgr_tmp_directory);

	command_ok = local_command(command, NULL);

	if (command_ok == false)
	{
		log_error(_("unable to fetch server parameters from Barman server"));

		exit(ERR_BARMAN);
	}

}


/*
 * do_standby_register()
 *
 * Event(s):
 *  - standby_register
 */

void
do_standby_register(void)
{
	PGconn	   *conn;
	PGconn	   *primary_conn;

	bool		record_created;
	t_node_info node_record = T_NODE_INFO_INITIALIZER;
	RecordStatus record_status;

	log_info(_("connecting to standby database"));
	conn = establish_db_connection_quiet(config_file_options.conninfo);

	if (PQstatus(conn) != CONNECTION_OK)
	{
		if (!runtime_options.force)
		{
			log_error(_("unable to connect to local node \"%s\" (ID: %i)"),
					  config_file_options.node_name,
					  config_file_options.node_id);
			log_detail("%s",
					   PQerrorMessage(conn));
			log_hint(_("to register a standby which is not running, provide primary connection parameters and use option -F/--force"));

			exit(ERR_BAD_CONFIG);
		}

		if (!runtime_options.connection_param_provided)
		{
			log_error(_("unable to connect to local node \"%s\" (ID: %i) and no primary connection parameters provided"),
					  config_file_options.node_name,
					  config_file_options.node_id);
			exit(ERR_BAD_CONFIG);
		}
	}


	if (PQstatus(conn) == CONNECTION_OK)
	{
		check_recovery_type(conn);
	}

	/* check if there is a primary in this cluster */
	log_info(_("connecting to primary database"));

	/* Normal case - we can connect to the local node */
	if (PQstatus(conn) == CONNECTION_OK)
	{
		primary_conn = get_primary_connection(conn, NULL, NULL);
	}
	/* User is forcing a registration and must have supplied primary connection info */
	else
	{
		primary_conn = establish_db_connection_by_params(&source_conninfo, false);
	}


	/*
	 * no amount of --force will make it possible to register the standby
	 * without a primary server to connect to
	 */
	if (PQstatus(primary_conn) != CONNECTION_OK)
	{
		log_error(_("unable to connect to the primary database"));
		log_hint(_("a primary must be configured before registering a standby"));
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * Verify that standby and primary are supported and compatible server
	 * versions
	 *
	 * If the user is registering an inactive standby, we'll trust they know
	 * what they're doing
	 */
	if (PQstatus(conn) == CONNECTION_OK)
	{
		check_primary_standby_version_match(conn, primary_conn);
	}


	/*
	 * Check that an active node with the same node_name doesn't exist already
	 */

	record_status = get_node_record_by_name(primary_conn,
											config_file_options.node_name,
											&node_record);

	if (record_status == RECORD_FOUND)
	{
		if (node_record.active == true && node_record.node_id != config_file_options.node_id)
		{
			log_error(_("node %i exists already with node_name \"%s\""),
					  node_record.node_id,
					  config_file_options.node_name);
			PQfinish(primary_conn);
			if (PQstatus(conn) == CONNECTION_OK)
				PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}
	}

	/* Check if node record exists */

	record_status = get_node_record(primary_conn,
									config_file_options.node_id,
									&node_record);

	if (record_status == RECORD_FOUND && !runtime_options.force)
	{
		log_error(_("node %i is already registered"),
				  config_file_options.node_id);
		log_hint(_("use option -F/--force to overwrite an existing node record"));
		PQfinish(primary_conn);
		if (PQstatus(conn) == CONNECTION_OK)
			PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * If an upstream node is defined, check if that node exists and is active
	 * If it doesn't exist, and --force set, create a minimal inactive record
	 */
	if (runtime_options.upstream_node_id != NO_UPSTREAM_NODE)
	{
		RecordStatus upstream_record_status;

		upstream_record_status = get_node_record(primary_conn,
												 runtime_options.upstream_node_id,
												 &node_record);

		if (upstream_record_status != RECORD_FOUND)
		{
			t_node_info upstream_node_record = T_NODE_INFO_INITIALIZER;

			if (!runtime_options.force)
			{
				log_error(_("no record found for upstream node %i"),
						  runtime_options.upstream_node_id);
				/* footgun alert - only do this if you know what you're doing */
				log_hint(_("use option -F/--force to create a dummy upstream record"));
				PQfinish(primary_conn);
				if (PQstatus(conn) == CONNECTION_OK)
					PQfinish(conn);
				exit(ERR_BAD_CONFIG);
			}

			log_notice(_("creating placeholder record for upstream node %i"),
					   runtime_options.upstream_node_id);

			upstream_node_record.node_id = runtime_options.upstream_node_id;
			upstream_node_record.type = STANDBY;
			upstream_node_record.upstream_node_id = NO_UPSTREAM_NODE;
			strncpy(upstream_node_record.conninfo, runtime_options.upstream_conninfo, MAXLEN);
			upstream_node_record.active = false;

			record_created = create_node_record(primary_conn,
												"standby register",
												&upstream_node_record);

			/*
			 * It's possible, in the kind of scenario this functionality is intended
			 * to support, that there's a race condition where the node's actual
			 * record gets inserted, causing the insert of the placeholder record
			 * to fail. If this is the case, we don't worry about this insert failing;
			 * if not we bail out.
			 *
			 * TODO: teach create_node_record() to use ON CONFLICT DO NOTHING for
			 * 9.5 and later.
			 */
			if (record_created == false)
			{
				upstream_record_status = get_node_record(primary_conn,
														 runtime_options.upstream_node_id,
														 &node_record);
				if (upstream_record_status != RECORD_FOUND)
				{
					log_error(_("unable to create placeholder record for upstream node %i"),
							  runtime_options.upstream_node_id);
					PQfinish(primary_conn);
					if (PQstatus(conn) == CONNECTION_OK)
						PQfinish(conn);
					exit(ERR_BAD_CONFIG);
				}

				log_info(_("a record for upstream node %i was already created"),
						 runtime_options.upstream_node_id);
			}

		}
		else if (node_record.active == false)
		{
			/*
			 * upstream node is inactive and --force not supplied - refuse to register
			 */
			if (!runtime_options.force)
			{
				log_error(_("record for upstream node %i is marked as inactive"),
						  runtime_options.upstream_node_id);
				log_hint(_("use option -F/--force to register a standby with an inactive upstream node"));
				PQfinish(primary_conn);
				if (PQstatus(conn) == CONNECTION_OK)
					PQfinish(conn);
				exit(ERR_BAD_CONFIG);
			}

			/*
			 * user is using the --force - notify about the potential footgun
			 */
			log_notice(_("registering node %i with inactive upstream node %i"),
					   config_file_options.node_id,
					   runtime_options.upstream_node_id);
		}
	}


	/*
	 * populate node record structure with current values (this will overwrite
	 * any existing values, which is what we want when updating the record
	 */
	init_node_record(&node_record);
	node_record.type = STANDBY;


	/*
	 * node record exists - update it
	 * (at this point we have already established that -F/--force is in use)
	 */
	if (record_status == RECORD_FOUND)
	{
		record_created = update_node_record(primary_conn,
											"standby register",
											&node_record);
	}
	else
	{
		record_created = create_node_record(primary_conn,
											"standby register",
											&node_record);
	}

	if (record_created == false)
	{
		/* XXX add event description */
		create_event_notification(primary_conn,
								  &config_file_options,
								  config_file_options.node_id,
								  "standby_register",
								  false,
								  NULL);

		PQfinish(primary_conn);
		primary_conn = NULL;

		if (PQstatus(conn) == CONNECTION_OK)
			PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	/* Log the event */
	create_event_notification(primary_conn,
							  &config_file_options,
							  config_file_options.node_id,
							  "standby_register",
							  true,
							  NULL);

	/* if --wait-sync option set, wait for the records to synchronise */

	if (PQstatus(conn) == CONNECTION_OK &&
		runtime_options.wait_register_sync == true &&
		runtime_options.wait_register_sync_seconds > 0)
	{
		bool sync_ok = false;
		int timer = 0;
		RecordStatus node_record_status;
		t_node_info node_record_on_primary = T_NODE_INFO_INITIALIZER;
		t_node_info node_record_on_standby = T_NODE_INFO_INITIALIZER;

		node_record_status = get_node_record(primary_conn,
											 config_file_options.node_id,
											 &node_record_on_primary);

		if (node_record_status != RECORD_FOUND)
		{
			log_error(_("unable to retrieve node record from primary"));
			PQfinish(primary_conn);
			PQfinish(conn);
			exit(ERR_REGISTRATION_SYNC);
		}

		for (;;)
		{
			bool records_match = true;

			if (runtime_options.wait_register_sync_seconds && runtime_options.wait_register_sync_seconds == timer)
				break;

			node_record_status = get_node_record(conn,
												 config_file_options.node_id,
												 &node_record_on_standby);

			if (node_record_status == RECORD_NOT_FOUND)
			{
				/* no record available yet on standby*/
				records_match = false;
			}
			else if (node_record_status == RECORD_FOUND)
			{
				/* compare relevant fields */
				if (node_record_on_standby.upstream_node_id != node_record_on_primary.upstream_node_id)
					records_match = false;

				if (node_record_on_standby.type != node_record_on_primary.type)
					records_match = false;

				if (node_record_on_standby.priority != node_record_on_primary.priority)
					records_match = false;

				if (node_record_on_standby.active != node_record_on_primary.active)
					records_match = false;

				if (strcmp(node_record_on_standby.node_name, node_record_on_primary.node_name) != 0)
					records_match = false;

				if (strcmp(node_record_on_standby.conninfo, node_record_on_primary.conninfo) != 0)
					records_match = false;

				if (strcmp(node_record_on_standby.slot_name, node_record_on_primary.slot_name) != 0)
					records_match = false;

				if (records_match == true)
				{
					sync_ok = true;
					break;
				}
			}

			sleep(1);
			timer ++;
		}

		if (sync_ok == false)
		{
			log_error(_("node record was not synchronised after %i seconds"),
					  runtime_options.wait_register_sync_seconds);
			PQfinish(primary_conn);
			PQfinish(conn);
			exit(ERR_REGISTRATION_SYNC);
		}

		log_info(_("node record on standby synchronised from primary"));
	}


	PQfinish(primary_conn);

	if (PQstatus(conn) == CONNECTION_OK)
		PQfinish(conn);

	log_info(_("standby registration complete"));
	log_notice(_("standby node \"%s\" (id: %i) successfully registered"),
			   config_file_options.node_name, config_file_options.node_id);
	return;
}


/*
 * do_standby_unregister()
 *
 * Event(s):
 *  - standby_unregister
 */
void
do_standby_unregister(void)
{
	PGconn	   *conn;
	PGconn	   *primary_conn;

	int 		target_node_id;
	t_node_info node_info = T_NODE_INFO_INITIALIZER;

	bool		node_record_deleted;

	log_info(_("connecting to local standby"));
	conn = establish_db_connection(config_file_options.conninfo, true);

	/* check if there is a primary in this cluster */
	log_info(_("connecting to primary database"));

	primary_conn = get_primary_connection(conn, NULL, NULL);

	if (PQstatus(primary_conn) != CONNECTION_OK)
	{
		log_error(_("unable to connect to primary server"));
		log_detail("%s", PQerrorMessage(conn));
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * if --node-id was specified, unregister that node rather than the
	 * current one - this enables inactive nodes to be unregistered.
	 */
	if (runtime_options.node_id != UNKNOWN_NODE_ID)
		target_node_id = runtime_options.node_id;
	else
		target_node_id = config_file_options.node_id;

	/* Check node exists and is really a standby */

	if (get_node_record(primary_conn, target_node_id, &node_info) != RECORD_FOUND)
	{
		log_error(_("no record found for node %i"), target_node_id);
		PQfinish(primary_conn);
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	if (node_info.type != STANDBY)
	{
		log_error(_("node %i is not a standby server"), target_node_id);
		PQfinish(primary_conn);
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	/* Now unregister the standby */
	log_notice(_("unregistering node %i"), target_node_id);
	node_record_deleted = delete_node_record(primary_conn,
										     target_node_id);

	if (node_record_deleted == false)
	{
		PQfinish(primary_conn);
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	/* Log the event */
	create_event_notification(primary_conn,
						&config_file_options,
						target_node_id,
						"standby_unregister",
						true,
						NULL);

	PQfinish(primary_conn);
	PQfinish(conn);

	log_info(_("standby unregistration complete"));

	return;
}


/*
 * do_standby_promote()
 *
 * Event(s):
 *  - standby_promote
 */
void
do_standby_promote(void)
{
	PGconn	   *conn;
	PGconn	   *current_primary_conn;
	bool		success;

	RecoveryType recovery_type;

	char		data_dir[MAXLEN];


	int			existing_primary_id = UNKNOWN_NODE_ID;

	log_info(_("connecting to standby database"));
	conn = establish_db_connection(config_file_options.conninfo, true);

	log_verbose(LOG_INFO, _("connected to standby, checking its state"));

	/* Verify that standby is a supported server version */
	check_server_version(conn, "standby", true, NULL);

	/* Check we are in a standby node */
	recovery_type = get_recovery_type(conn);

	if (recovery_type != RECTYPE_STANDBY)
	{
		if (recovery_type == RECTYPE_PRIMARY)
		{
			log_error(_("STANDBY PROMOTE can only be executed on a standby node"));
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}
		else
		{
			log_error(_("connection to node lost"));
			PQfinish(conn);
			exit(ERR_DB_CONN);
		}
	}


	/* check that there's no existing primary */
	current_primary_conn = get_primary_connection_quiet(conn, &existing_primary_id, NULL);

	if (PQstatus(current_primary_conn) == CONNECTION_OK)
	{
		log_error(_("this cluster already has an active primary server"));

		if (existing_primary_id != UNKNOWN_NODE_ID)
		{
			t_node_info primary_rec;

			get_node_record(conn, existing_primary_id, &primary_rec);

			log_detail(_("current primary is %s (node_id: %i)"),
					   primary_rec.node_name,
					   existing_primary_id);
		}

		PQfinish(current_primary_conn);
		PQfinish(conn);
		exit(ERR_PROMOTION_FAIL);
	}


	/* Get the data directory */
	// XXX do we need a superuser check?
	success = get_pg_setting(conn, "data_directory", data_dir);
	PQfinish(conn);

	if (success == false)
	{
		log_error(_("unable to determine data directory"));
		exit(ERR_PROMOTION_FAIL);
	}

	_do_standby_promote_internal(data_dir);
}


static void
_do_standby_promote_internal(const char *data_dir)
{
	char		script[MAXLEN];
	int			r;
	int			i,
				promote_check_timeout  = 60,
				promote_check_interval = 2;
	bool		promote_success = false;
	PQExpBufferData details;
	PGconn	   *conn;
	RecoveryType recovery_type;

	log_notice(_("promoting standby"));

	/*
	 * Promote standby to primary.
	 *
	 * `pg_ctl promote` returns immediately and (prior to 10.0) has no -w option
	 * so we can't be sure when or if the promotion completes.
	 * For now we'll poll the server until the default timeout (60 seconds)
	 */

	get_server_action(ACTION_PROMOTE, script, (char *)data_dir);

	log_notice(_("promoting server using '%s'"),
			   script);

	r = system(script);
	if (r != 0)
	{
		log_error(_("unable to promote server from standby to primary"));
		exit(ERR_PROMOTION_FAIL);
	}

	/* reconnect to check we got promoted */

	log_info(_("reconnecting to promoted server"));
	conn = establish_db_connection(config_file_options.conninfo, true);

	for (i = 0; i < promote_check_timeout; i += promote_check_interval)
	{
		recovery_type = get_recovery_type(conn);

		if (recovery_type == RECTYPE_PRIMARY)
		{
			promote_success = true;
			break;
		}
		sleep(promote_check_interval);
	}

	if (promote_success == false)
	{
		if (recovery_type == RECTYPE_STANDBY)
		{
			log_error(_("STANDBY PROMOTE failed, node is still a standby"));
			PQfinish(conn);
			exit(ERR_PROMOTION_FAIL);
		}
		else
		{
			log_error(_("connection to node lost"));
			PQfinish(conn);
			exit(ERR_DB_CONN);
		}
	}


	/* update node information to reflect new status */
	if (update_node_record_set_primary(conn, config_file_options.node_id) == false)
	{
		initPQExpBuffer(&details);
		appendPQExpBuffer(&details,
						  _("unable to update node record for node %i"),
						  config_file_options.node_id);

		log_error("%s", details.data);

		create_event_notification(NULL,
							&config_file_options,
							config_file_options.node_id,
							"standby_promote",
							false,
							details.data);

		exit(ERR_DB_QUERY);
	}


	initPQExpBuffer(&details);
	appendPQExpBuffer(&details,
					  _("node %i was successfully promoted to primary"),
					  config_file_options.node_id);

	log_notice(_("STANDBY PROMOTE successful"));
	log_detail("%s", details.data);

	/* Log the event */
	create_event_notification(conn,
						&config_file_options,
						config_file_options.node_id,
						"standby_promote",
						true,
						details.data);

	PQfinish(conn);

	return;
}


/*
 * Follow a new primary.
 *
 * This function has two "modes":
 *  1) no primary info provided - determine primary from standby metadata
 *  2) primary info provided - use that info to connect to the primary.
 *
 * (2) is mainly for when a node has been stopped as part of a switchover
 * and needs to be started with recovery.conf correctly configured.
 */

void
do_standby_follow(void)
{
	PGconn	   *local_conn;
	t_node_info local_node_record = T_NODE_INFO_INITIALIZER;
	int			original_upstream_node_id = UNKNOWN_NODE_ID;

	PGconn	   *primary_conn = NULL;
	int			primary_id = UNKNOWN_NODE_ID;
	t_node_info primary_node_record = T_NODE_INFO_INITIALIZER;

	char		data_dir[MAXPGPATH];
	t_conninfo_param_list recovery_conninfo;
	char	   *errmsg = NULL;

	RecordStatus record_status;

	char		restart_command[MAXLEN];
	int			r;

	PQExpBufferData event_details;

	log_verbose(LOG_DEBUG, "do_standby_follow()");

	/*
	 * If -h/--host wasn't provided, attempt to connect to standby
	 * to determine primary, and carry out some other checks while we're
	 * at it.
	 */
	if (runtime_options.host_param_provided == false)
	{
		bool	    success;
		int	    	timer;

		local_conn = establish_db_connection(config_file_options.conninfo, true);

		log_verbose(LOG_INFO, _("connected to local node"));

		check_recovery_type(local_conn);

		success = get_pg_setting(local_conn, "data_directory", data_dir);

		if (success == false)
		{
			log_error(_("unable to determine data directory"));
			PQfinish(local_conn);
			exit(ERR_BAD_CONFIG);
		}

		/*
		 * Attempt to connect to primary.
		 *
		 * If --wait provided, loop for up `primary_response_timeout`
		 * seconds before giving up
		 */
		// XXX ??? primary_follow_timeout
		for (timer = 0; timer < config_file_options.primary_follow_timeout; timer++)
		{
			primary_conn = get_primary_connection_quiet(local_conn,
														&primary_id,
														NULL);

			if (PQstatus(primary_conn) == CONNECTION_OK || runtime_options.wait == false)
			{
				break;
			}
		}

		if (PQstatus(primary_conn) != CONNECTION_OK)
		{
			log_error(_("unable to determine primary node"));
			PQfinish(local_conn);
			exit(ERR_BAD_CONFIG);
		}

		check_primary_standby_version_match(local_conn, primary_conn);

		PQfinish(local_conn);
	}
	/* local data directory and primary server info explictly provided -
	 * attempt to connect to that
	 *
	 * XXX --wait option won't be effective here
	 */
	else
	{
		if (config_file_options.data_directory[0] == '\0')
		{
			if (runtime_options.data_dir[0] == '\0')
			{
				log_error(_("-D/--pgdata required when providing connection parameters for \"standby follow\""));
				exit(ERR_BAD_CONFIG);
			}
			strncpy(data_dir, runtime_options.data_dir, MAXPGPATH);
		}
		else
		{
			strncpy(data_dir, config_file_options.data_directory, MAXPGPATH);
		}

		primary_conn = establish_db_connection_by_params(&source_conninfo, true);

		primary_id = get_primary_node_id(primary_conn);
	}

	if (get_recovery_type(primary_conn) != RECTYPE_PRIMARY)
	{
		log_error(_("the node to follow is not a primary"));
		// XXX log detail
		PQfinish(primary_conn);
		exit(ERR_BAD_CONFIG);
	}


	/*
	 * Fetch our node record so we can write application_name, if set,
	 * and to get the upstream node ID, which we'll need to know if
	 * replication slots are in use and we want to delete the old slot.
	 */
	record_status = get_node_record(primary_conn,
									config_file_options.node_id,
									&local_node_record);

	if (record_status != RECORD_FOUND)
	{
		log_error(_("unable to retrieve record for node %i"),
					config_file_options.node_id);
		PQfinish(primary_conn);
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * If replication slots are in use, we'll need to create a
	 * slot on the new primary
	 */

	if (config_file_options.use_replication_slots)
	{
 		int	primary_server_version_num = get_server_version(primary_conn, NULL);

		initPQExpBuffer(&event_details);

		if (create_replication_slot(primary_conn,
									local_node_record.slot_name,
									primary_server_version_num,
									&event_details) == false)
		{
			log_error("%s", event_details.data);

			create_event_notification(
				primary_conn,
				&config_file_options,
				config_file_options.node_id,
				"standby_follow",
				false,
				event_details.data);

			PQfinish(primary_conn);
			exit(ERR_DB_QUERY);
		}

		termPQExpBuffer(&event_details);
	}

	get_node_record(primary_conn, primary_id, &primary_node_record);

	/* Initialise connection parameters to write as `primary_conninfo` */
	initialize_conninfo_params(&recovery_conninfo, false);

	/* We ignore any application_name set in the primary's conninfo */
	parse_conninfo_string(primary_node_record.conninfo, &recovery_conninfo, errmsg, true);


	/* Set the default application name to this node's name */
	param_set(&recovery_conninfo, "application_name", config_file_options.node_name);

	/* Set the replication user from the primary node record */
	param_set(&recovery_conninfo, "user", primary_node_record.repluser);


	{
		t_conninfo_param_list local_node_conninfo;
		bool parse_success;

		initialize_conninfo_params(&local_node_conninfo, false);

		parse_success = parse_conninfo_string(local_node_record.conninfo, &local_node_conninfo, errmsg, false);

		if (parse_success == false)
		{
			/* this shouldn't happen, but if it does we'll plough on regardless */
			log_warning(_("unable to parse conninfo string \"%s\":\n  %s"),
						local_node_record.conninfo, errmsg);
		}
		else
		{
			char *application_name = param_get(&local_node_conninfo, "application_name");

			if (application_name != NULL && strlen(application_name))
				param_set(&recovery_conninfo, "application_name", application_name);
		}

		/*
		 * store the original upstream node id so we can delete the replication slot,
		 * if exists
		 */
		if (local_node_record.upstream_node_id != UNKNOWN_NODE_ID)
		{
			original_upstream_node_id = local_node_record.upstream_node_id;
		}
		else
		{
			original_upstream_node_id = primary_id;
		}
	}

	log_info(_("changing node %i's primary to node %i"),
			 config_file_options.node_id, primary_id);

	if (!create_recovery_file(&local_node_record, &recovery_conninfo, data_dir))
	{
		PQfinish(primary_conn);
		exit(ERR_BAD_CONFIG);
	}

	/* restart the service */

	// XXX here check if service is running!! if not, start
	//     ensure that problem with pg_ctl output is caught here

	get_server_action(ACTION_RESTART, restart_command, data_dir);

	log_notice(_("restarting server using '%s'"),
			   restart_command);

	r = system(restart_command);
	if (r != 0)
	{
		log_error(_("unable to restart server"));
		PQfinish(primary_conn);
		exit(ERR_NO_RESTART);
	}


	/*
	 * If replication slots are in use, and an inactive one for this node
	 * exists on the former upstream, drop it.
	 *
	 * XXX check if former upstream is current primary?
	 */

	if (config_file_options.use_replication_slots && runtime_options.host_param_provided == false && original_upstream_node_id != UNKNOWN_NODE_ID)
	{
		t_node_info upstream_node_record  = T_NODE_INFO_INITIALIZER;
		RecordStatus upstream_record_status;

		log_verbose(LOG_INFO, "attempting to remove replication slot from old upstream node %i",
					original_upstream_node_id);

		/* XXX should we poll for server restart? */
		local_conn = establish_db_connection(config_file_options.conninfo, true);

		upstream_record_status = get_node_record(local_conn,
												   original_upstream_node_id,
												   &upstream_node_record);

		PQfinish(local_conn);

		if (upstream_record_status != RECORD_FOUND)
		{
			log_warning(_("unable to retrieve node record for old upstream node %i"),
						original_upstream_node_id);
		}
		else
		{
			PGconn *old_upstream_conn = establish_db_connection_quiet(upstream_node_record.conninfo);

			if (PQstatus(old_upstream_conn) != CONNECTION_OK)
			{
				log_info(_("unable to connect to old upstream node %i to remove replication slot"),
						 original_upstream_node_id);
				log_hint(_("if reusing this node, you should manually remove any inactive replication slots"));
			}
			else
			{
				drop_replication_slot_if_exists(old_upstream_conn,
												original_upstream_node_id,
												local_node_record.slot_name);
			}
		}
	}

	/*
	 * It's possible this node was an inactive primary - update the
	 * relevant fields to ensure it's marked as an active standby
	 */
	if (update_node_record_status(primary_conn,
								  config_file_options.node_id,
								  "standby",
								  primary_id,
								  true) == false)
	{
		log_error(_("unable to update upstream node"));
		PQfinish(primary_conn);

		exit(ERR_BAD_CONFIG);
	}

	log_notice(_("STANDBY FOLLOW successful"));

	initPQExpBuffer(&event_details);
	appendPQExpBuffer(&event_details,
					  _("node %i is now attached to node %i"),
					  config_file_options.node_id, primary_id);

	create_event_notification(primary_conn,
							  &config_file_options,
							  config_file_options.node_id,
							  "standby_follow",
							  true,
							  event_details.data);

	log_detail("%s", event_details.data);

	termPQExpBuffer(&event_details);

	PQfinish(primary_conn);

	return;
}


/*
 * Perform a switchover by:
 *  - stopping current primary node
 *  - promoting this standby node to primary
 *  - forcing previous primary node to follow this node
 *
 * Caveats:
 *  - repmgrd must not be running, otherwise it may
 *    attempt a failover
 *    (TODO: find some way of notifying repmgrd of planned
 *     activity like this)
 *  - currently only set up for two-node operation; any other
 *    standbys will probably become downstream cascaded standbys
 *    of the old primary once it's restarted
 *
 * TODO:
 *  - make connection test timeouts/intervals configurable (see below)
 */


void
do_standby_switchover(void)
{
	PGconn	   *local_conn;
	PGconn	   *remote_conn;


	t_node_info local_node_record = T_NODE_INFO_INITIALIZER;


	/* the remote server is the primary to be demoted */
	char	    remote_conninfo[MAXCONNINFO] = "";
	char	    remote_host[MAXLEN] = "";
	int         remote_node_id;
	t_node_info remote_node_record = T_NODE_INFO_INITIALIZER;

	RecordStatus	   record_status;
	RecoveryType	recovery_type;
	PQExpBufferData remote_command_str;
	PQExpBufferData command_output;

	int r, i;

	bool		shutdown_success = false;
	XLogRecPtr remote_last_checkpoint_lsn = InvalidXLogRecPtr;
	ReplInfo 		replication_info = T_REPLINFO_INTIALIZER;

	/* store list of configuration files on the demotion candidate */
	KeyValueList	remote_config_files = { NULL, NULL };

	/*
	 * SANITY CHECKS
	 *
	 * We'll be doing a bunch of operations on the remote server (primary
	 * to be demoted) - careful checks needed before proceding.
	 */


	local_conn = establish_db_connection(config_file_options.conninfo, true);

	record_status = get_node_record(local_conn, config_file_options.node_id, &local_node_record);
	if (record_status != RECORD_FOUND)
	{
		log_error(_("unable to retrieve node record for node %i"),
				  config_file_options.node_id);

		PQfinish(local_conn);
		exit(ERR_DB_QUERY);
	}

	if (!is_streaming_replication(local_node_record.type))
	{
		log_error(_("switchover can only performed with streaming replication"));
		PQfinish(local_conn);
		exit(ERR_BAD_CONFIG);
	}

	if (runtime_options.dry_run == true)
	{
		log_notice(_("checking switchover on node \"%s\" (ID: %i) in --dry-run mode"),
				   local_node_record.node_name,
				   local_node_record.node_id);
	}
	else
	{
		log_notice(_("executing switchover on node \"%s\" (ID: %i)"),
				   local_node_record.node_name,
				   local_node_record.node_id);
	}

	/* Check that this is a standby */
	recovery_type = get_recovery_type(local_conn);
	if (recovery_type != RECTYPE_STANDBY)
	{
		log_error(_("switchover must be executed from the standby node to be promoted"));
		if (recovery_type == RECTYPE_PRIMARY)
		{
			log_detail(_("this node (ID: %i) is the primary"),
					   local_node_record.node_id);
		}
		PQfinish(local_conn);

		exit(ERR_SWITCHOVER_FAIL);
	}

	/* check remote server connection and retrieve its record */
	remote_conn = get_primary_connection(local_conn, &remote_node_id, remote_conninfo);

	if (PQstatus(remote_conn) != CONNECTION_OK)
	{
		log_error(_("unable to connect to current primary node"));
		log_hint(_("check that the cluster is correctly configured and this standby is registered"));
		PQfinish(local_conn);
		exit(ERR_DB_CONN);
	}

	record_status = get_node_record(remote_conn, remote_node_id, &remote_node_record);

	if (record_status != RECORD_FOUND)
	{
		log_error(_("unable to retrieve node record for node %i"),
				  remote_node_id);

		PQfinish(local_conn);
		PQfinish(remote_conn);

		exit(ERR_DB_QUERY);
	}

	/*
	 * Check this standby is attached to the demotion candidate
	 * TODO:
	 *  - check standby is attached to demotion candidate
	 *    (compare primary_conninfo from recovery.conf)
	 */

	if (local_node_record.upstream_node_id != remote_node_record.node_id)
	{
		log_error(_("local node %i is not a downstream of demotion candidate primary %i"),
				  local_node_record.upstream_node_id,
				  remote_node_record.node_id);

		PQfinish(local_conn);
		PQfinish(remote_conn);

		exit(ERR_BAD_CONFIG);
	}

	log_verbose(LOG_DEBUG, "remote node name is \"%s\"", remote_node_record.node_name);


	/*
	 * If --force-rewind specified, check pg_rewind can be used, and pre-emptively
	 * fetch the list of configuration files which should be archived
	 */

	if (runtime_options.force_rewind == true)
	{
		PQExpBufferData reason;

		initPQExpBuffer(&reason);

		if (can_use_pg_rewind(remote_conn, config_file_options.data_directory, &reason) == false)
		{
			log_error(_("--force-rewind specified but pg_rewind cannot be used"));
			log_detail("%s", reason.data);
			termPQExpBuffer(&reason);
			PQfinish(local_conn);
			PQfinish(remote_conn);

			exit(ERR_BAD_CONFIG);
		}

		termPQExpBuffer(&reason);

		get_datadir_configuration_files(remote_conn, &remote_config_files);
	}


	/*
	 * Check that we can connect by SSH to the remote (current primary) server
	 */
	get_conninfo_value(remote_conninfo, "host", remote_host);

	r = test_ssh_connection(remote_host, runtime_options.remote_user);

	if (r != 0)
	{
		log_error(_("unable to connect via SSH to host \"%s\", user \"%s\""),
				  remote_host, runtime_options.remote_user);
		PQfinish(remote_conn);
		PQfinish(local_conn);

		exit(ERR_BAD_CONFIG);
	}

	/* check archive/replication status */
	{
		int lag_seconds = 0;
		CheckStatus status = CHECK_STATUS_UNKNOWN;

		/* archive status - check when "archive_mode" is activated */

		if (guc_set(remote_conn, "archive_mode", "!=", "off"))
		{
			int files = 0;
			int threshold = 0;
			bool command_success;

			initPQExpBuffer(&remote_command_str);
			make_remote_repmgr_path(&remote_command_str);
			appendPQExpBuffer(&remote_command_str,
							  "node check --terse -LERROR --archiver --optformat");

			initPQExpBuffer(&command_output);

			command_success = remote_command(
				remote_host,
				runtime_options.remote_user,
				remote_command_str.data,
				&command_output);

			termPQExpBuffer(&remote_command_str);

			if (command_success == true)
			{
				status = parse_node_check_archiver(command_output.data, &files, &threshold);

				log_debug("%i %i; '%s'", files, threshold, command_output.data);
			}

			termPQExpBuffer(&command_output);
			if (status == CHECK_STATUS_UNKNOWN)
			{
				if (runtime_options.force == false)
				{
					log_error(_("unable to check number of pending archive files on demotion candidate \"%s\""),
							  remote_node_record.node_name);
					log_hint(_("use -F/--force to continue anyway"));
					PQfinish(remote_conn);
					PQfinish(local_conn);

					exit(ERR_SWITCHOVER_FAIL);
				}

				log_warning(_("unable to check number of pending archive files on demotion candidate \"%s\""),
						  remote_node_record.node_name);
				log_notice(_("-F/--force set, continuing with switchover"));

			}
			else if (status == CHECK_STATUS_CRITICAL)
			{
				if (runtime_options.force == false)
				{
					log_error(_("number of pending archive files on demotion candidate \"%s\" is critical"),
							  remote_node_record.node_name);
					log_detail(_("%i pending archive files (critical threshold: %i)"),
							   files, threshold);
					log_hint(_("PostgreSQL will not shut down until all files are archived; use -F/--force to continue anyway"));
					PQfinish(remote_conn);
					PQfinish(local_conn);

					exit(ERR_SWITCHOVER_FAIL);
				}

				log_warning(_("number of pending archive files on demotion candidate \"%s\" is critical"),
							remote_node_record.node_name);
				log_detail(_("%i pending archive files (critical threshold: %i)"),
						   files, threshold);
				log_notice(_("-F/--force set, continuing with switchover"));
			}
			else if (status == CHECK_STATUS_WARNING)
			{
				log_warning(_("number of pending archive files on demotion candidate \"%s\" is warning"),
							remote_node_record.node_name);
				log_detail(_("%i pending archive files (warning threshold: %i)"),
						   files, threshold);
				log_hint(_("PostgreSQL will not shut down until all files are archived"));
			}

		}

		/* check replication lag */
		lag_seconds =  get_replication_lag_seconds(local_conn);

		log_debug("lag is %i ", lag_seconds);

		termPQExpBuffer(&command_output);

		if (lag_seconds >= config_file_options.replication_lag_critical)
		{
			if (runtime_options.force == false)
			{
				log_error(_("replication lag on this node is critical"));
				log_detail(_("lag is %i seconds (critical threshold: %i)"),
						   lag_seconds, config_file_options.replication_lag_critical);
				log_hint(_("PostgreSQL on the demotion candidate will not shut down until pending WAL is flushed to the standby; use -F/--force to continue anyway"));
				PQfinish(remote_conn);
				PQfinish(local_conn);

				exit(ERR_SWITCHOVER_FAIL);
			}

			log_warning(_("replication lag on this node is critical"));
			log_detail(_("lag is %i seconds (critical threshold: %i)"),
					   lag_seconds, config_file_options.replication_lag_critical);
			log_notice(_("-F/--force set, continuing with switchover"));
		}
		else if (lag_seconds >= config_file_options.replication_lag_warning)
		{
			log_warning(_("replication lag on this node is warning"));
			log_detail(_("lag is %i seconds (warning threshold: %i)"),
					   lag_seconds, config_file_options.replication_lag_warning);
		}
		else if (lag_seconds < 0)
		{
			if (runtime_options.force == false)
			{
				log_error(_("unable to check replication lag on local node"));
				log_hint(_("use -F/--force to continue anyway"));
				PQfinish(remote_conn);
				PQfinish(local_conn);

				exit(ERR_SWITCHOVER_FAIL);
			}

			log_warning(_("unable to check replication lag on local node"));
			log_notice(_("-F/--force set, continuing with switchover"));
		}
	}

	PQfinish(remote_conn);
	PQfinish(local_conn);



	/* Determine the remote's configuration file location */
	/* -------------------------------------------------- */

	/* Remote configuration file provided - check it exists */
	/* TODO have remote node verify config file  "node status --config-file */
	if (runtime_options.remote_config_file[0])

	{
		log_verbose(LOG_INFO, _("looking for file \"%s\" on remote server \"%s\""),
					runtime_options.remote_config_file,
					remote_host);

		initPQExpBuffer(&remote_command_str);
		appendPQExpBuffer(&remote_command_str, "ls ");

		appendShellString(&remote_command_str, runtime_options.remote_config_file);
		appendPQExpBuffer(&remote_command_str, " >/dev/null 2>&1 && echo 1 || echo 0");

		initPQExpBuffer(&command_output);

		(void)remote_command(
			remote_host,
			runtime_options.remote_user,
			remote_command_str.data,
			&command_output);

		termPQExpBuffer(&remote_command_str);

		if (*command_output.data == '0')
		{
			log_error(_("unable to find the specified repmgr configuration file on remote server"));
			log_detail(_("remote configuration file is \"%s\""),
					   runtime_options.remote_config_file);
			exit(ERR_BAD_CONFIG);
		}

		log_verbose(LOG_INFO, _("remote configuration file \"%s\" found on remote server"),
					runtime_options.remote_config_file);

		termPQExpBuffer(&command_output);
	}
	/*
	 * No remote configuration file provided - check some default locations:
	 *  - path of configuration file for this repmgr
	 *  - /etc/repmgr.conf
	 */
	else
	{
		int		    i;
		bool		remote_config_file_found = false;

		const char *config_paths[] = {
			runtime_options.config_file,
			"/etc/repmgr.conf",
			NULL
		};

		log_verbose(LOG_INFO, _("no remote configuration file provided - checking default locations"));

		for (i = 0; config_paths[i] && remote_config_file_found == false; ++i)
		{
			/*
			 * Don't attempt to check for an empty filename - this might be the case
			 * if no local configuration file was found.
			 */
			if (!strlen(config_paths[i]))
				continue;

			log_verbose(LOG_INFO, _("checking \"%s\"\n"), config_paths[i]);

			initPQExpBuffer(&remote_command_str);
			appendPQExpBuffer(&remote_command_str, "ls ");

			appendShellString(&remote_command_str, config_paths[i]);
			appendPQExpBuffer(&remote_command_str, " >/dev/null 2>&1 && echo 1 || echo 0");

			initPQExpBuffer(&command_output);

			(void)remote_command(
				remote_host,
				runtime_options.remote_user,
				remote_command_str.data,
				&command_output);

			termPQExpBuffer(&remote_command_str);

			if (*command_output.data == '1')
			{
				strncpy(runtime_options.remote_config_file, config_paths[i], MAXLEN);
				log_verbose(LOG_INFO, _("configuration file \"%s\" found on remote server"),
							runtime_options.remote_config_file);
				remote_config_file_found = true;
			}

			termPQExpBuffer(&command_output);
		}

		if (remote_config_file_found == false)
		{
			log_error(_("no remote configuration file supplied or found in a default location - terminating"));
			log_hint(_("specify the remote configuration file with -C/--remote-config-file"));
			exit(ERR_BAD_CONFIG);
		}
	}


	/*
	 * Sanity checks completed - prepare for the switchover
	 */

	log_detail(_("local node \"%s\" (ID: %i) will be promoted to primary; "
				 "current primary \"%s\" (ID: %i) will be demoted to standby"),
			   local_node_record.node_name,
			   local_node_record.node_id,
			   remote_node_record.node_name,
			   remote_node_record.node_id);

	/*
	 * Stop the remote primary
	 *
	 * We'll issue the pg_ctl command but not force it not to wait; we'll check
	 * the connection from here - and error out if no shutdown is detected
	 * after a certain time.
	 */

	initPQExpBuffer(&remote_command_str);
	initPQExpBuffer(&command_output);

	make_remote_repmgr_path(&remote_command_str);


	if (runtime_options.dry_run == true)
	{
		appendPQExpBuffer(&remote_command_str,
						  "node service --terse -LERROR --list-actions --action=stop");

	}
	else
	{
		appendPQExpBuffer(&remote_command_str,
						  "node service --action=stop --checkpoint");
	}

	// XXX handle failure

	(void)remote_command(
		remote_host,
		runtime_options.remote_user,
		remote_command_str.data,
		&command_output);


	termPQExpBuffer(&remote_command_str);


	/*
	 * --dry-run ends here with display of command which would be used to
	 * shut down the remote server
	 */
	if (runtime_options.dry_run == true)
	{
		log_info(_("following shutdown command would be run on node \"%s\":\n  \"%s\""),
				 remote_node_record.node_name,
				 command_output.data);
		termPQExpBuffer(&command_output);
		return;
	}

	termPQExpBuffer(&command_output);
	shutdown_success = false;

	/* loop for timeout waiting for current primary to stop */

	for (i = 0; i < config_file_options.reconnect_attempts; i++)
	{
		/* Check whether primary is available */
		PGPing ping_res;

		log_info(_("checking primary status; %i of %i attempts"),
				 i + 1, config_file_options.reconnect_attempts);
		ping_res = PQping(remote_conninfo);


		/* database server could not be contacted */
		if (ping_res == PQPING_NO_RESPONSE ||PQPING_NO_ATTEMPT)
		{
			bool command_success;

			/*
			 * remote server can't be contacted at protocol level - that
			 * doesn't necessarily mean it's shut down, so we'll ask
			 * its repmgr to check at data directory level, and if shut down
			 * also return the last checkpoint LSN.
			 */

			initPQExpBuffer(&remote_command_str);
			make_remote_repmgr_path(&remote_command_str);
			appendPQExpBuffer(&remote_command_str,
							  "node status --is-shutdown");

			initPQExpBuffer(&command_output);

			command_success = remote_command(
				remote_host,
				runtime_options.remote_user,
				remote_command_str.data,
				&command_output);

			termPQExpBuffer(&remote_command_str);

			if (command_success == true)
			{
				NodeStatus status = parse_node_status_is_shutdown(command_output.data, &remote_last_checkpoint_lsn);

				if (status == NODE_STATUS_DOWN && remote_last_checkpoint_lsn != InvalidXLogRecPtr)
				{
					shutdown_success = true;
					log_notice(_("current primary has been shut down at location %X/%X"),
							   format_lsn(remote_last_checkpoint_lsn));
					termPQExpBuffer(&command_output);

					break;
				}
			}

			termPQExpBuffer(&command_output);
		}

		/* XXX make configurable? */
		sleep(config_file_options.reconnect_interval);
		i++;
	}

	if (shutdown_success == false)
	{
		log_error(_("shutdown of the primary server could not be confirmed"));
		log_hint(_("check the primary server status before performing any further actions"));
		exit(ERR_SWITCHOVER_FAIL);
	}


	local_conn = establish_db_connection(config_file_options.conninfo, false);

	if (PQstatus(local_conn) != CONNECTION_OK)
	{
		log_error(_("unable to reestablish connection to local node \"%s\""),
				  local_node_record.node_name);
		exit(ERR_DB_CONN);
	}

	get_replication_info(local_conn, &replication_info);

	if (replication_info.last_wal_receive_lsn < remote_last_checkpoint_lsn)
	{
		log_warning(_("local node \"%s\" is behind shutdown primary \"%s\""),
					local_node_record.node_name,
					remote_node_record.node_name);
		log_detail(_("local node last receive LSN is %X/%X, primary shutdown checkpoint LSN is %X/%X"),
				   format_lsn(replication_info.last_wal_receive_lsn),
				   format_lsn(remote_last_checkpoint_lsn));

		if (runtime_options.always_promote == false)
		{
			log_notice(_("aborting switchover"));
			log_hint(_("use --always-promote to force promotion of standby"));
			PQfinish(local_conn);
			exit(ERR_SWITCHOVER_FAIL);
		}
	}

	/* promote standby */
	_do_standby_promote_internal(config_file_options.data_directory);

	if (1 || replication_info.last_wal_receive_lsn < remote_last_checkpoint_lsn)
	{
		KeyValueListCell *cell;
		bool first_entry = true;

		if (runtime_options.force_rewind == false)
		{
			log_error(_("new primary diverges from former primary and --force-rewind not provided"));
			/* TODO: "repmgr node rejoin" example, when available */
			log_hint(_("the former primary will need to be restored manually"));
			PQfinish(local_conn);
			exit(ERR_SWITCHOVER_FAIL);
		}

		initPQExpBuffer(&remote_command_str);
		make_remote_repmgr_path(&remote_command_str);
		appendPQExpBuffer(&remote_command_str,
						  "node rejoin --upstream-conninfo='%s'",
						  local_node_record.conninfo);
		appendPQExpBuffer(&remote_command_str,
						  " --config-files=");

		for (cell = remote_config_files.head; cell; cell = cell->next)
		{
			if (first_entry == false)
				appendPQExpBuffer(&remote_command_str, ",");
			else
				first_entry = false;

			appendPQExpBuffer(&remote_command_str, "%s", cell->key);
		}

		log_debug("executing:\n  \"%s\"", remote_command_str.data);

		(void)remote_command(
			remote_host,
			runtime_options.remote_user,
			remote_command_str.data,
			NULL);

		termPQExpBuffer(&remote_command_str);
	}
	else
	{
		/*
		 * Execute `repmgr standby follow` to create recovery.conf and start
		 * the remote server
		 *
		 * XXX replace with "node rejoin"
		 */
		initPQExpBuffer(&remote_command_str);
		make_remote_repmgr_path(&remote_command_str);
		appendPQExpBuffer(&remote_command_str,
						  " -d \\'%s\\' standby follow",
						  local_node_record.conninfo);
		log_debug("executing:\n  \"%s\"", remote_command_str.data);
		(void)remote_command(
			remote_host,
			runtime_options.remote_user,
			remote_command_str.data,
			NULL);

		termPQExpBuffer(&remote_command_str);
	}

	/* TODO: verify this node's record was updated correctly */

	create_event_record(local_conn,
						&config_file_options,
						config_file_options.node_id,
						"standby_switchover",
						true,
						NULL);

	PQfinish(local_conn);


	/* clean up remote node */
	remote_conn = establish_db_connection(remote_node_record.conninfo, false);

	// check replication status
	if (PQstatus(remote_conn) != CONNECTION_OK)
	{
		log_error(_("unable to reestablish connection to remote node \"%s\""),
				  remote_node_record.node_name);
		//log_hint(_("")); // depends on replication status
	}
	else
	{
		if (config_file_options.use_replication_slots == true)
		{
			drop_replication_slot_if_exists(remote_conn,
											remote_node_record.node_id,
											local_node_record.slot_name);
		}
		/* TODO warn about any inactive replication slots*/
	}

	log_notice(_("switchover was successful"));
	log_detail(_("node \"%s\" is now primary"),
			   local_node_record.node_name);

	return;
}


static void
check_source_server()
{
	PGconn			  *superuser_conn = NULL;
	PGconn			  *privileged_conn = NULL;

	char			   cluster_size[MAXLEN];
	t_node_info		   node_record = T_NODE_INFO_INITIALIZER;
	RecordStatus	   record_status;
	ExtensionStatus	   extension_status;

	/* Attempt to connect to the upstream server to verify its configuration */
	log_info(_("connecting to upstream node"));

	source_conn = establish_db_connection_by_params(&source_conninfo, false);

	/*
	 * Unless in barman mode, exit with an error;
	 * establish_db_connection_by_params() will have already logged an error message
	 */
	if (PQstatus(source_conn) != CONNECTION_OK)
	{
		PQfinish(source_conn);

		if (mode == barman)
			return;
		else
			exit(ERR_DB_CONN);
	}

	/*
	 * If a connection was established, perform some sanity checks on the
	 * provided upstream connection
	 */


	/* Verify that upstream node is a supported server version */
	log_verbose(LOG_INFO, _("connected to source node, checking its state"));

	source_server_version_num = check_server_version(source_conn, "primary", true, NULL);

	check_upstream_config(source_conn, source_server_version_num, true);

	if (get_cluster_size(source_conn, cluster_size) == false)
		exit(ERR_DB_QUERY);

	log_info(_("successfully connected to source node"));
	log_detail(_("current installation size is %s"),
			   cluster_size);

	/*
	 * If the upstream node is a standby, try to connect to the primary too so we
	 * can write an event record
	 */
	if (get_recovery_type(source_conn) == RECTYPE_STANDBY)
	{
		primary_conn = get_primary_connection(source_conn, NULL, NULL);

		// XXX check this worked?
	}
	else
	{
		primary_conn = source_conn;
	}

	/*
	 * Sanity-check that the primary node has a repmgr schema - if not
	 * present, fail with an error unless -F/--force is used (to enable
	 * repmgr to be used as a standalone clone tool)
	 */

	extension_status = get_repmgr_extension_status(primary_conn);

	if (extension_status != REPMGR_INSTALLED)
	{
		if (!runtime_options.force)
		{
			if (extension_status == REPMGR_UNKNOWN)
			{
				PQfinish(source_conn);
				exit(ERR_DB_QUERY);
			}

			/* schema doesn't exist */
			log_error(_("repmgr extension not found on source node"));

			if (extension_status == REPMGR_AVAILABLE)
			{
				log_detail(_("repmgr extension is available but not installed in database \"%s\""),
						   param_get(&source_conninfo, "dbname"));
			}
			else if (extension_status == REPMGR_UNAVAILABLE)
			{
				log_detail(_("repmgr extension is not available on the upstream server"));
			}

			log_hint(_("check that the upstream server is part of a repmgr cluster"));
			PQfinish(source_conn);
			exit(ERR_BAD_CONFIG);
		}

		log_warning(_("repmgr extension not found on source node"));
	}

	/* Fetch the source's data directory */
	get_superuser_connection(&source_conn, &superuser_conn, &privileged_conn);

	if (get_pg_setting(privileged_conn, "data_directory", upstream_data_directory) == false)
	{
		log_error(_("unable to retrieve source node's data directory"));
		log_hint(_("STANDBY CLONE must be run as a database superuser"));
		PQfinish(source_conn);
		if(superuser_conn != NULL)
			PQfinish(superuser_conn);

		exit(ERR_BAD_CONFIG);
	}
	if(superuser_conn != NULL)
		PQfinish(superuser_conn);

	/*
	 * If no target data directory was explicitly provided, we'll default to
	 * the source host's data directory.
	 */
	if (local_data_directory_provided == false)
	{
		strncpy(local_data_directory, upstream_data_directory, MAXPGPATH);

		log_notice(_("setting data directory to: \"%s\""), local_data_directory);
		log_hint(_("use -D/--pgdata to explicitly specify a data directory"));
	}

	/*
	 * In the default pg_basebackup mode, we'll cowardly refuse to overwrite
	 * an existing data directory
	 */
	if (mode == pg_basebackup)
	{
		if (is_pg_dir(local_data_directory) && runtime_options.force != true)
		{
			log_error(_("target data directory appears to be a PostgreSQL data directory"));
			log_detail(_("target data directory is \"%s\""), local_data_directory);
			log_hint(_("ensure the target data directory is empty before running \"STANDBY CLONE\" in pg_basebackup mode"));
			PQfinish(source_conn);
			exit(ERR_BAD_CONFIG);
		}
	}

	/*
	 * Copy the source connection so that we have some default values,
	 * particularly stuff like passwords extracted from PGPASSFILE;
	 * these will be overridden from the upstream conninfo, if provided.
	 */
	conn_to_param_list(source_conn, &recovery_conninfo);

	/*
	 * Attempt to find the upstream node record
	 */
	if (runtime_options.upstream_node_id == NO_UPSTREAM_NODE)
		upstream_node_id = get_primary_node_id(source_conn);
	else
		upstream_node_id = runtime_options.upstream_node_id;

	record_status = get_node_record(source_conn, upstream_node_id, &node_record);
	if (record_status == RECORD_FOUND)
	{
		upstream_record_found = true;
		strncpy(recovery_conninfo_str, node_record.conninfo, MAXLEN);
		strncpy(upstream_repluser, node_record.repluser, NAMEDATALEN);
	}

	/*
	 * check that there's no existing node record with the same name but
	 * different ID
	 */
	record_status = get_node_record_by_name(source_conn, config_file_options.node_name, &node_record);

	if (record_status == RECORD_FOUND && node_record.node_id != config_file_options.node_id)
	{
		log_error(_("another node (node_id: %i) already exists with node_name \"%s\""),
				  node_record.node_id,
				  config_file_options.node_name);
		PQfinish(source_conn);
		exit(ERR_BAD_CONFIG);
	}

}


static void
check_source_server_via_barman()
{
	char		    buf[MAXLEN];
	char		    barman_conninfo_str[MAXLEN];
	t_conninfo_param_list barman_conninfo;
	char		   *errmsg = NULL;
	bool		    parse_success,
				    command_success;
	char		    where_condition[MAXLEN];
	PQExpBufferData command_output;
	PQExpBufferData repmgr_conninfo_buf;

	int c;

	get_barman_property(barman_conninfo_str, "conninfo", local_repmgr_tmp_directory);

	initialize_conninfo_params(&barman_conninfo, false);

	/* parse_conninfo_string() here will remove the upstream's `application_name`, if set */
	parse_success = parse_conninfo_string(barman_conninfo_str, &barman_conninfo, errmsg, true);

	if (parse_success == false)
	{
		log_error(_("Unable to parse barman conninfo string \"%s\":\n%s"),
				barman_conninfo_str, errmsg);
		exit(ERR_BARMAN);
	}

	/* Overwrite database name in the parsed parameter list */
	param_set(&barman_conninfo, "dbname", runtime_options.dbname);

	/* Rebuild the Barman conninfo string */
	initPQExpBuffer(&repmgr_conninfo_buf);

	for (c = 0; c < barman_conninfo.size && barman_conninfo.keywords[c] != NULL; c++)
	{
		if (repmgr_conninfo_buf.len != 0)
			appendPQExpBufferChar(&repmgr_conninfo_buf, ' ');

		appendPQExpBuffer(&repmgr_conninfo_buf, "%s=",
						  barman_conninfo.keywords[c]);
		appendConnStrVal(&repmgr_conninfo_buf,
						 barman_conninfo.values[c]);
	}

	log_verbose(LOG_DEBUG,
				"repmgr database conninfo string on barman server: %s",
				repmgr_conninfo_buf.data);

	// XXX check this works in all cases
	maxlen_snprintf(where_condition, "node_id=%i", upstream_node_id);

	initPQExpBuffer(&command_output);
	maxlen_snprintf(buf,
					"ssh %s \"psql -Aqt \\\"%s\\\" -c \\\""
					" SELECT conninfo"
					" FROM repmgr.nodes"
					" WHERE %s"
					" AND active IS TRUE"
					"\\\"\"",
					config_file_options.barman_host,
					repmgr_conninfo_buf.data,
					where_condition);

	termPQExpBuffer(&repmgr_conninfo_buf);

	command_success = local_command(buf, &command_output);

	if (command_success == false)
	{
		log_error(_("unable to execute database query via Barman server"));
		exit(ERR_BARMAN);
	}

	maxlen_snprintf(recovery_conninfo_str, "%s", command_output.data);
	string_remove_trailing_newlines(recovery_conninfo_str);

	upstream_record_found = true;
	log_verbose(LOG_DEBUG,
				"upstream node conninfo string extracted via barman server: %s",
				recovery_conninfo_str);

	termPQExpBuffer(&command_output);
}



/*
 * initialise_direct_clone()
 *
 * In pg_basebackup mode, configure the target data directory
 * if necessary, and fetch information about tablespaces and configuration
 * files.
 *
 * Event(s):
 * - standby_clone
 */
static void
initialise_direct_clone(t_node_info *node_record)
{
	PGconn			 *superuser_conn = NULL;
	PGconn			 *privileged_conn = NULL;
	bool	 		  success;

	/*
	 * Check the destination data directory can be used
	 * (in Barman mode, this directory will already have been created)
	 */

	if (!create_pg_dir(local_data_directory, runtime_options.force))
	{
		log_error(_("unable to use directory \"%s\""),
				  local_data_directory);
		log_hint(_("use -F/--force to force this directory to be overwritten"));
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * Check that tablespaces named in any `tablespace_mapping` configuration
	 * file parameters exist.
	 *
	 * pg_basebackup doesn't verify mappings, so any errors will not be caught.
	 * We'll do that here as a value-added service.
	 *
	 */

	if (config_file_options.tablespace_mapping.head != NULL)
	{
		TablespaceListCell *cell;
		KeyValueList not_found = { NULL, NULL };
		int total = 0, matched = 0;

		for (cell = config_file_options.tablespace_mapping.head; cell; cell = cell->next)
		{
			char *old_dir_escaped     = escape_string(source_conn, cell->old_dir);
			char name[MAXLEN] = "";

			success = get_tablespace_name_by_location(source_conn, old_dir_escaped, name);
			pfree(old_dir_escaped);

			if (success == true)
			{
				matched ++;
			}
			else
			{
				key_value_list_set(
					&not_found,
					cell->old_dir,
					"");
			}

			total ++;
		}

		if (not_found.head != NULL)
		{
			PQExpBufferData	  detail;
			KeyValueListCell *kv_cell;

			log_error(_("%i of %i mapped tablespaces not found"),
					  total - matched, total);

			initPQExpBuffer(&detail);

			for (kv_cell = not_found.head; kv_cell; kv_cell = kv_cell->next)
			{
				appendPQExpBuffer(
					&detail,
					"  %s\n", kv_cell->key);
			}

			log_detail(_("following tablespaces not found:\n%s"),
					   detail.data);
			termPQExpBuffer(&detail);

			exit(ERR_BAD_CONFIG);
		}
	}

	/*
	 * Obtain configuration file locations
	 *
	 * We'll check to see whether the configuration files are in the data
	 * directory - if not we'll have to copy them via SSH, if copying
	 * requested.
	 *
	 * This will require superuser permissions, so we'll attempt to connect
	 * as -S/--superuser (if provided), otherwise check the current connection
	 * user has superuser rights.
	 *
	 * XXX: if configuration files are symlinks to targets outside the data
	 * directory, they won't be copied by pg_basebackup, but we can't tell
	 * this from the below query; we'll probably need to add a check for their
	 * presence and if missing force copy by SSH
	 */

	get_superuser_connection(&source_conn, &superuser_conn, &privileged_conn);

	success = get_configuration_file_locations(privileged_conn, &config_files);

	if (success == false)
	{
		log_notice(_("unable to proceed without establishing configuration file locations"));
		PQfinish(source_conn);

		if (superuser_conn != NULL)
			PQfinish(superuser_conn);

		exit(ERR_BAD_CONFIG);
	}


	/*
	 * If replication slots requested, create appropriate slot on the
	 * primary; this must be done before pg_basebackup is called.
	 *
	 * Replication slots are not supported (and not very useful
	 * anyway) in Barman mode.
	 */

	if (config_file_options.use_replication_slots == true)
	{
		PQExpBufferData event_details;
		initPQExpBuffer(&event_details);

		if (create_replication_slot(privileged_conn, node_record->slot_name, source_server_version_num, &event_details) == false)
		{
			log_error("%s", event_details.data);

			create_event_notification(
				primary_conn,
				&config_file_options,
				config_file_options.node_id,
				"standby_clone",
				false,
				event_details.data);

			PQfinish(source_conn);

			if (superuser_conn != NULL)
				PQfinish(superuser_conn);

			exit(ERR_DB_QUERY);
		}

		termPQExpBuffer(&event_details);

		log_notice(_("replication slot \"%s\" created on upstream node (node_id: %i)"),
				   node_record->slot_name,
				   upstream_node_id);
	}

	if (superuser_conn != NULL)
		PQfinish(superuser_conn);

	return;
}


static int
run_basebackup(t_node_info *node_record)
{
	char				  script[MAXLEN];
	int					  r = SUCCESS;
	PQExpBufferData 	  params;
	TablespaceListCell   *cell;
	t_basebackup_options  backup_options = T_BASEBACKUP_OPTIONS_INITIALIZER;

	/*
	 * Parse the pg_basebackup_options provided in repmgr.conf - we'll want
	 * to check later whether certain options were set by the user
	 */
	parse_pg_basebackup_options(config_file_options.pg_basebackup_options,
								&backup_options,
								server_version_num,
								NULL);

	/* Create pg_basebackup command line options */

	initPQExpBuffer(&params);

	appendPQExpBuffer(&params, " -D %s", local_data_directory);

	/*
	 * conninfo string provided - pass it to pg_basebackup as the -d option
	 * (pg_basebackup doesn't require or want a database name, but for
	 * consistency with other applications accepts a conninfo string
	 * under -d/--dbname)
	 */
	if (runtime_options.conninfo_provided == true)
	{
		t_conninfo_param_list conninfo;
		char *conninfo_str;

		initialize_conninfo_params(&conninfo, false);

		/* string will already have been parsed */
		(void) parse_conninfo_string(runtime_options.dbname, &conninfo, NULL, false);

		if (*runtime_options.replication_user)
			param_set(&conninfo, "user", runtime_options.replication_user);

		conninfo_str = param_list_to_string(&conninfo);

		appendPQExpBuffer(&params, " -d '%s'", conninfo_str);

		pfree(conninfo_str);
	}

	/*
	 * Connection parameters not passed to repmgr as conninfo string - provide
	 * them individually to pg_basebackup (-d/--dbname not required)
	 */
	else
	{
		if (strlen(runtime_options.host))
		{
			appendPQExpBuffer(&params, " -h %s", runtime_options.host);
		}

		if (strlen(runtime_options.port))
		{
			appendPQExpBuffer(&params, " -p %s", runtime_options.port);
		}

		if (strlen(runtime_options.replication_user))
		{
			appendPQExpBuffer(&params, " -U %s", runtime_options.replication_user);
		}
		else if (strlen(runtime_options.username))
		{
			appendPQExpBuffer(&params, " -U %s", runtime_options.username);
		}
	}

	if (runtime_options.fast_checkpoint) {
		appendPQExpBuffer(&params, " -c fast");
	}

	if (config_file_options.tablespace_mapping.head != NULL)
	{
		for (cell = config_file_options.tablespace_mapping.head; cell; cell = cell->next)
		{
			appendPQExpBuffer(&params, " -T %s=%s", cell->old_dir, cell->new_dir);
		}
	}

	/*
	 * To ensure we have all the WALs needed during basebackup execution we stream
	 * them as the backup is taking place.
	 *
	 * From 9.6, if replication slots are in use, we'll have previously
	 * created a slot with reserved LSN, and will stream from that slot to avoid
	 * WAL buildup on the primary using the -S/--slot, which requires -X/--xlog-method=stream
	 * (from 10, -X/--wal-method=stream)
	 */
	if (!strlen(backup_options.xlog_method))
	{
		appendPQExpBuffer(&params, " -X stream");
	}

	/*
	 * From 9.6, pg_basebackup accepts -S/--slot, which forces WAL streaming to use
	 * the specified replication slot. If replication slot usage is specified, the
	 * slot will already have been created.
	 *
	 * NOTE: currently there's no way of disabling the --slot option while using
	 *   --xlog-method=stream - it's hard to imagine a use case for this, so no
	 *   provision has been made for doing it.
	 *
	 * NOTE:
	 *   It's possible to set 'pg_basebackup_options' with an invalid combination
	 *   of values for --wal-method (--xlog-method) and --slot - we're not checking that, just that
	 *   we're not overriding any user-supplied values
	 */
	if (server_version_num >= 90600 && config_file_options.use_replication_slots)
	{
		bool slot_add = true;

		/*
		 * Check whether 'pg_basebackup_options' in repmgr.conf has the --slot option set,
		 * or if --wal-method (--xlog-method) is set to a value other than "stream"
		 * (in which case we can't use --slot).
		 */
		if (strlen(backup_options.slot) || (strlen(backup_options.xlog_method) && strcmp(backup_options.xlog_method, "stream") != 0)) {
			slot_add = false;
		}

		if (slot_add == true)
		{
			appendPQExpBuffer(&params, " -S %s", node_record->slot_name);
		}
	}

	maxlen_snprintf(script,
					"%s -l \"repmgr base backup\" %s %s",
					make_pg_path("pg_basebackup"),
					params.data,
					config_file_options.pg_basebackup_options);

	termPQExpBuffer(&params);

	log_info(_("executing: '%s'"), script);

	/*
	 * As of 9.4, pg_basebackup only ever returns 0 or 1
	 */

	r = system(script);

	if (r != 0)
		return ERR_BAD_BASEBACKUP;

	return SUCCESS;
}


static int
run_file_backup(t_node_info *node_record)
{
	int r = SUCCESS, i;

	char		command[MAXLEN];
	char		filename[MAXLEN];
	char		buf[MAXLEN];
	char		basebackups_directory[MAXLEN];
	char		backup_id[MAXLEN] = "";
	char		*p, *q;
	TablespaceDataList tablespace_list = { NULL, NULL };
	TablespaceDataListCell *cell_t;

	PQExpBufferData tablespace_map;
	bool		tablespace_map_rewrite = false;

	if (mode == barman)
	{
		/*
		 * Locate Barman's base backups directory
		 */

		get_barman_property(basebackups_directory, "basebackups_directory", local_repmgr_tmp_directory);

		/*
		 * Read the list of backup files into a local file. In the
		 * process:
		 *
		 * - determine the backup ID;
		 * - check, and remove, the prefix;
		 * - detect tablespaces;
		 * - filter files in one list per tablespace;
		 */

		{
			FILE *fi; /* input stream */
			FILE *fd; /* output for data.txt */
			char prefix[MAXLEN];
			char output[MAXLEN];
			int n;

			maxlen_snprintf(command, "%s list-files --target=data %s latest",
							make_barman_ssh_command(barman_command_buf),
							config_file_options.barman_server);

			log_verbose(LOG_DEBUG, "executing:\n  %s\n", command);

			fi = popen(command, "r");
			if (fi == NULL)
			{
				log_error("cannot launch command: %s", command);
				exit(ERR_BARMAN);
			}

			fd = fopen(datadir_list_filename, "w");
			if (fd == NULL)
			{
				log_error("cannot open file: %s", datadir_list_filename);
				exit(ERR_BARMAN);
			}

			maxlen_snprintf(prefix, "%s/", basebackups_directory);

			while (fgets(output, MAXLEN, fi) != NULL)
			{
				/*
				 * Remove prefix
				 */
				p = string_skip_prefix(prefix, output);
				if (p == NULL)
				{
					log_error("unexpected output from \"barman list-files\": %s",
							output);
					exit(ERR_BARMAN);
				}

				/*
				 * Remove and note backup ID; copy backup.info
				 */
				if (! strcmp(backup_id, ""))
				{
					FILE *fi2;

					n = strcspn(p, "/");

					strncpy(backup_id, p, n);

					strncat(prefix,backup_id,MAXLEN-1);
					strncat(prefix,"/",MAXLEN-1);
					p = string_skip_prefix(backup_id, p);
					p = string_skip_prefix("/", p);

					/*
					 * Copy backup.info
					 */
					maxlen_snprintf(command,
									"rsync -a %s:%s/%s/backup.info %s",
									config_file_options.barman_host,
									basebackups_directory,
									backup_id,
									local_repmgr_tmp_directory);
					(void)local_command(
						command,
						NULL);

					/*
					 * Get tablespace data
					 */
					maxlen_snprintf(filename, "%s/backup.info",
									local_repmgr_tmp_directory);
					fi2 = fopen(filename, "r");
					if (fi2 == NULL)
					{
						log_error("cannot open file: %s", filename);
						exit(ERR_INTERNAL);
					}
					while (fgets(buf, MAXLEN, fi2) != NULL)
					{
						q = string_skip_prefix("tablespaces=", buf);
						if (q != NULL && strncmp(q, "None\n", 5))
						{
							get_tablespace_data_barman(q, &tablespace_list);
						}
						q = string_skip_prefix("version=", buf);
						if (q != NULL)
						{
							server_version_num = strtol(q, NULL, 10);
						}
					}
					fclose(fi2);
					unlink(filename);

					continue;
				}

				/*
				 * Skip backup.info
				 */
				if (string_skip_prefix("backup.info", p))
					continue;

				/*
				 * Filter data directory files
				 */
				if ((q = string_skip_prefix("data/", p)) != NULL)
				{
					fputs(q, fd);
					continue;
				}

				/*
				 * Filter other files (i.e. tablespaces)
				 */
				for (cell_t = tablespace_list.head; cell_t; cell_t = cell_t->next)
				{
					if ((q = string_skip_prefix(cell_t->oid, p)) != NULL && *q == '/')
					{
						if (cell_t->f == NULL)
						{
							maxlen_snprintf(filename, "%s/%s.txt", local_repmgr_tmp_directory, cell_t->oid);
							cell_t->f = fopen(filename, "w");
							if (cell_t->f == NULL)
							{
								log_error("cannot open file: %s", filename);
								exit(ERR_INTERNAL);
							}
						}
						fputs(q + 1, cell_t->f);
						break;
					}
				}
			}

			fclose(fd);

			pclose(fi);
		}

		/* For 9.5 and greater, create our own tablespace_map file */
		if (server_version_num >= 90500)
		{
			initPQExpBuffer(&tablespace_map);
		}

		/*
		 * As of Barman version 1.6.1, the file structure of a backup
		 * is as follows:
		 *
		 * base/ - base backup
		 * wals/ - WAL files associated to the backup
		 *
		 * base/<ID> - backup files
		 *
		 *   here ID has the standard timestamp form yyyymmddThhmmss
		 *
		 * base/<ID>/backup.info - backup metadata, in text format
		 * base/<ID>/data        - data directory
		 * base/<ID>/<OID>       - tablespace with the given oid
		 */

		/*
		 * Copy all backup files from the Barman server
		 */
		maxlen_snprintf(command,
						"rsync --progress -a --files-from=%s %s:%s/%s/data %s",
						datadir_list_filename,
						config_file_options.barman_host,
						basebackups_directory,
						backup_id,
						local_data_directory);

		(void)local_command(
			command,
			NULL);

		unlink(datadir_list_filename);

		/*
		 * We must create some PGDATA subdirectories because they are
		 * not included in the Barman backup.
		 *
		 * See class RsyncBackupExecutor in the Barman source (barman/backup_executor.py)
		 * for a definitive list of excluded directories.
		 */
		{
			const char* const dirs[] = {
				/* Only from 10 */
				"pg_wal",
				/* Only from 9.5 */
				"pg_commit_ts",
				/* Only from 9.4 */
				"pg_dynshmem", "pg_logical", "pg_logical/snapshots", "pg_logical/mappings", "pg_replslot",
				/* Already in 9.3 */
				"pg_notify", "pg_serial", "pg_snapshots", "pg_stat", "pg_stat_tmp", "pg_tblspc",
				"pg_twophase", "pg_xlog", 0
			};
			const int vers[] = {
				100000,
				90500,
				90400, 90400, 90400, 90400, 90400,
				0, 0, 0, 0, 0, 0,
				0, -100000, 0
			};
			for (i = 0; dirs[i]; i++)
			{
				/* directory exists in newer versions than this server - skip */
				if (vers[i] > 0 && server_version_num < vers[i])
					continue;

				/* directory existed in earlier versions than this server but has been removed/renamed - skip */
				if (vers[i] < 0 && server_version_num >= abs(vers[i]))
					continue;

				maxlen_snprintf(filename, "%s/%s", local_data_directory, dirs[i]);
				if (mkdir(filename, S_IRWXU) != 0 && errno != EEXIST)
				{
					log_error(_("unable to create the %s directory"), dirs[i]);
					exit(ERR_INTERNAL);
				}
			}
		}
	}


	for (cell_t = tablespace_list.head; cell_t; cell_t = cell_t->next)
	{
		bool mapping_found = false;
		TablespaceListCell *cell;
		char *tblspc_dir_dest;

		/* Check if tablespace path matches one of the provided tablespace mappings */
		if (config_file_options.tablespace_mapping.head != NULL)
		{
			for (cell = config_file_options.tablespace_mapping.head; cell; cell = cell->next)
			{
				if (strcmp(cell_t->location, cell->old_dir) == 0)
				{
					mapping_found = true;
					break;
				}
			}
		}

		if (mapping_found == true)
		{
			tblspc_dir_dest = cell->new_dir;
			log_debug(_("mapping source tablespace '%s' (OID %s) to '%s'"),
					  cell_t->location, cell_t->oid, tblspc_dir_dest);
		}
		else
		{
			tblspc_dir_dest = cell_t->location;
		}

		/*
		 * Tablespace file copy
		 */

		if (mode == barman)
		{
			create_pg_dir(cell_t->location, false);

			if (cell_t->f != NULL) /* cell_t->f == NULL iff the tablespace is empty */
			{
				maxlen_snprintf(command,
								"rsync --progress -a --files-from=%s/%s.txt %s:%s/base/%s/%s %s",
								local_repmgr_tmp_directory,
								cell_t->oid,
								config_file_options.barman_host,
								basebackups_directory,
								backup_id,
								cell_t->oid,
								tblspc_dir_dest);
				(void)local_command(
					command,
					NULL);
				fclose(cell_t->f);
				maxlen_snprintf(filename,
								"%s/%s.txt",
								local_repmgr_tmp_directory,
								cell_t->oid);
				unlink(filename);
			}
		}


		/*
		 * If a valid mapping was provide for this tablespace, arrange for it to
		 * be remapped
		 * (if no tablespace mapping was provided, the link will be copied as-is
		 * by pg_basebackup and no action is required)
		 */
		if (mapping_found == true || mode == barman)
		{
			/* 9.5 and later - append to the tablespace_map file */
			if (server_version_num >= 90500)
			{
				tablespace_map_rewrite = true;
				appendPQExpBuffer(&tablespace_map,
								  "%s %s\n",
								  cell_t->oid,
								  tblspc_dir_dest);
			}
			/* Pre-9.5, we have to manipulate the symlinks in pg_tblspc/ ourselves */
			else
			{
				PQExpBufferData tblspc_symlink;

				initPQExpBuffer(&tblspc_symlink);
				appendPQExpBuffer(&tblspc_symlink, "%s/pg_tblspc/%s",
								  local_data_directory,
								  cell_t->oid);

				if (unlink(tblspc_symlink.data) < 0 && errno != ENOENT)
				{
					log_error(_("unable to remove tablespace symlink %s"), tblspc_symlink.data);

					r = ERR_BAD_BASEBACKUP;
					goto stop_backup;
				}

				if (symlink(tblspc_dir_dest, tblspc_symlink.data) < 0)
				{
					log_error(_("unable to create tablespace symlink from %s to %s"), tblspc_symlink.data, tblspc_dir_dest);

					r = ERR_BAD_BASEBACKUP;
					goto stop_backup;
				}
			}
		}
	}

	/*
	 * For 9.5 and later, if tablespace remapping was requested, we'll need
	 * to rewrite the tablespace map file ourselves.
	 * The tablespace map file is read on startup and any links created by
	 * the backend; we could do this ourselves like for pre-9.5 servers, but
	 * it's better to rely on functionality the backend provides.
	 */

	if (server_version_num >= 90500 && tablespace_map_rewrite == true)
	{
		PQExpBufferData tablespace_map_filename;
		FILE	   *tablespace_map_file;
		initPQExpBuffer(&tablespace_map_filename);
		appendPQExpBuffer(&tablespace_map_filename, "%s/%s",
						  local_data_directory,
						  TABLESPACE_MAP);

		/* Unlink any existing file (it should be there, but we don't care if it isn't) */
		if (unlink(tablespace_map_filename.data) < 0 && errno != ENOENT)
		{
			log_error(_("unable to remove tablespace_map file %s: %s"),
					  tablespace_map_filename.data,
					  strerror(errno));

			r = ERR_BAD_BASEBACKUP;
			goto stop_backup;
		}

		tablespace_map_file = fopen(tablespace_map_filename.data, "w");
		if (tablespace_map_file == NULL)
		{
			log_error(_("unable to create tablespace_map file '%s'"), tablespace_map_filename.data);

			r = ERR_BAD_BASEBACKUP;
			goto stop_backup;
		}

		if (fputs(tablespace_map.data, tablespace_map_file) == EOF)
		{
			log_error(_("unable to write to tablespace_map file '%s'"), tablespace_map_filename.data);

			r  = ERR_BAD_BASEBACKUP;
			goto stop_backup;
		}

		fclose(tablespace_map_file);
	}



stop_backup:

	if (mode == barman)
	{
		/* In Barman mode, remove local_repmgr_directory */
		rmtree(local_repmgr_tmp_directory, true);
	}

	return r;
}


static char *
make_barman_ssh_command(char *buf)
{
	static char config_opt[MAXLEN] = "";

	if (strlen(config_file_options.barman_config))
		maxlen_snprintf(config_opt,
						" --config=%s",
						config_file_options.barman_config);

	maxlen_snprintf(buf,
					"ssh %s barman%s",
					config_file_options.barman_host,
					config_opt);

	return buf;
}


static int
get_tablespace_data_barman
( char *tablespace_data_barman,
  TablespaceDataList *tablespace_list)
{
	/*
	 * Example:
	 * [('main', 24674, '/var/lib/postgresql/tablespaces/9.5/main'), ('alt', 24678, '/var/lib/postgresql/tablespaces/9.5/alt')]
	 */

	char name[MAXLEN];
	char oid[MAXLEN];
	char location[MAXPGPATH];
	char *p = tablespace_data_barman;
	int i;

	tablespace_list->head = NULL;
	tablespace_list->tail = NULL;

	p = string_skip_prefix("[", p);
	if (p == NULL) return -1;

	while (*p == '(')
	{
		p = string_skip_prefix("('", p);
		if (p == NULL) return -1;

		i = strcspn(p, "'");
		strncpy(name, p, i);
		name[i] = 0;

		p = string_skip_prefix("', ", p + i);
		if (p == NULL) return -1;

		i = strcspn(p, ",");
		strncpy(oid, p, i);
		oid[i] = 0;

		p = string_skip_prefix(", '", p + i);
		if (p == NULL) return -1;

		i = strcspn(p, "'");
		strncpy(location, p, i);
		location[i] = 0;

		p = string_skip_prefix("')", p + i);
		if (p == NULL) return -1;

		tablespace_data_append (tablespace_list, name, oid, location);

		if (*p == ']')
			break;

		p = string_skip_prefix(", ", p);
		if (p == NULL) return -1;
	}

	return SUCCESS;
}


void
get_barman_property(char *dst, char *name, char *local_repmgr_directory)
{
	PQExpBufferData command_output;
	char buf[MAXLEN];
	char command[MAXLEN];
	char *p;

	initPQExpBuffer(&command_output);

	maxlen_snprintf(command,
					"grep \"^\t%s:\" %s/show-server.txt",
					name, local_repmgr_tmp_directory);
	(void)local_command(command, &command_output);

	maxlen_snprintf(buf, "\t%s: ", name);
	p = string_skip_prefix(buf, command_output.data);
	if (p == NULL)
	{
		log_error("unexpected output from Barman: %s",
				  command_output.data);
		exit(ERR_INTERNAL);
	}

	strncpy(dst, p, MAXLEN);
	string_remove_trailing_newlines(dst);

	termPQExpBuffer(&command_output);
}


static void
copy_configuration_files(void)
{
	int i, r;
	t_configfile_info *file;
	char *host;

	/* get host from upstream record */
	host = param_get(&recovery_conninfo, "host");

	if (host == NULL)
		host = runtime_options.host;

	log_notice(_("copying external configuration files from upstream node, host \"%s\""), host);

	r = test_ssh_connection(host, runtime_options.remote_user);

	if (r != 0)
	{
		log_error(_("remote host \"%s\" is not reachable via SSH - unable to copy external configuration files"),
				  host);
		return;
	}

	for (i = 0; i < config_files.entries; i++)
	{
		char dest_path[MAXPGPATH];
		file = config_files.files[i];

		/*
		 * Skip files in the data directory - these will be copied during
		 * the main backup
		 */
		if (file->in_data_directory == true)
			continue;

		if (runtime_options.copy_external_config_files_destination == CONFIG_FILE_SAMEPATH)
		{
			strncpy(dest_path, file->filepath, MAXPGPATH);
		}
		else
		{
			snprintf(dest_path, MAXPGPATH,
					 "%s/%s",
					 local_data_directory,
					 file->filename);
		}

		r = copy_remote_files(runtime_options.host, runtime_options.remote_user,
							  file->filepath, dest_path, false, server_version_num);
		if (WEXITSTATUS(r))
		{
			log_error(_("standby clone: unable to copy config file \"%s\""),
					  file->filename);
		}
	}

	return;
}


static void
tablespace_data_append(TablespaceDataList *list, const char *name, const char *oid, const char *location)
{
	TablespaceDataListCell *cell;

	cell = (TablespaceDataListCell *) pg_malloc0(sizeof(TablespaceDataListCell));

	if (cell == NULL)
	{
		log_error(_("unable to allocate memory; terminating"));
		exit(ERR_OUT_OF_MEMORY);
	}

	cell->oid      = pg_malloc(1 + strlen(oid     ));
	cell->name     = pg_malloc(1 + strlen(name    ));
	cell->location = pg_malloc(1 + strlen(location));

	strncpy(cell->oid     , oid     , 1 + strlen(oid     ));
	strncpy(cell->name    , name    , 1 + strlen(name    ));
	strncpy(cell->location, location, 1 + strlen(location));

	if (list->tail)
		list->tail->next = cell;
	else
		list->head = cell;

	list->tail = cell;
}



/*
 * check_primary_standby_version_match()
 *
 * Check server versions of supplied connections are compatible for
 * replication purposes.
 *
 * Exits on error.
 */
static void
check_primary_standby_version_match(PGconn *conn, PGconn *primary_conn)
{
	char		standby_version[MAXVERSIONSTR];
	int			standby_version_num = 0;

	char		primary_version[MAXVERSIONSTR];
	int			primary_version_num = 0;

	standby_version_num = check_server_version(conn, "standby", true, standby_version);

	/* Verify that primary is a supported server version */
	primary_version_num = check_server_version(conn, "primary", false, primary_version);
	if (primary_version_num < 0)
	{
		PQfinish(conn);
		PQfinish(primary_conn);
		exit(ERR_BAD_CONFIG);
	}

	/* primary and standby version should match */
	if ((primary_version_num / 100) != (standby_version_num / 100))
	{
		PQfinish(conn);
		PQfinish(primary_conn);
		log_error(_("PostgreSQL versions on primary (%s) and standby (%s) must match"),
				  primary_version, standby_version);
		exit(ERR_BAD_CONFIG);
	}
}


static void
check_recovery_type(PGconn *conn)
{
	RecoveryType recovery_type = get_recovery_type(conn);

	if (recovery_type != RECTYPE_STANDBY)
	{
		if (recovery_type == RECTYPE_PRIMARY)
		{
			log_error(_("this node should be a standby (%s)"),
					  config_file_options.conninfo);
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}
		else
		{
			log_error(_("connection to node (%s) lost"),
					  config_file_options.conninfo);
			PQfinish(conn);
			exit(ERR_DB_CONN);
		}
	}
}


static void
drop_replication_slot_if_exists(PGconn *conn, int node_id, char *slot_name)
{
	t_replication_slot  slot_info;
	int					record_status;

	record_status = get_slot_record(conn, slot_name, &slot_info);

	log_verbose(LOG_DEBUG, "attempting to delete slot \"%s\" on node %i",
				slot_name, node_id);
	if (record_status != RECORD_FOUND)
	{
		log_info(_("no slot record found for slot \"%s\" on node %i"),
				 slot_name, node_id);
	}
	else
	{
		if (slot_info.active == false)
		{
			if (drop_replication_slot(conn, slot_name) == true)
			{
				log_notice(_("replication slot \"%s\" deleted on node %i"), slot_name, node_id);
			}
			else
			{
				log_error(_("unable to delete replication slot \"%s\" on node %i"), slot_name, node_id);
			}
		}
		/* if active replication slot exists, call Houston as we have a problem */
		else
		{
			log_warning(_("replication slot \"%s\" is still active on node %i"), slot_name, node_id);
		}
	}
}


/* TODO: consolidate code in below functions */
static NodeStatus
parse_node_status_is_shutdown(const char *node_status_output, XLogRecPtr *checkPoint)
{
	int   options_len = 0;
	char *options_string = NULL;
	char *options_string_ptr = NULL;
	NodeStatus node_status = NODE_STATUS_UNKNOWN;

	/*
	 * Add parsed options to this list, then copy to an array
	 * to pass to getopt
	 */
	static ItemList option_argv = { NULL, NULL };

	char *argv_item;
	int c, argc_item = 1;

	char **argv_array;
	ItemListCell *cell;

	int			optindex = 0;

	/* We're only interested in these options */
	static struct option long_options[] =
	{
		{"last-checkpoint-lsn", required_argument, NULL, 'L'},
		{"state", required_argument, NULL, 'S'},
		{NULL, 0, NULL, 0}
	};

	/* Don't attempt to tokenise an empty string */
	if (!strlen(node_status_output))
	{
		*checkPoint = InvalidXLogRecPtr;
		return node_status;
	}

	options_len = strlen(node_status_output) + 1;
	options_string = pg_malloc(options_len);
	options_string_ptr = options_string;

	/* Copy the string before operating on it with strtok() */
	strncpy(options_string, node_status_output, options_len);

	/* Extract arguments into a list and keep a count of the total */
	while ((argv_item = strtok(options_string_ptr, " ")) != NULL)
	{
		item_list_append(&option_argv, argv_item);

		argc_item++;

		if (options_string_ptr != NULL)
			options_string_ptr = NULL;
	}

	/*
	 * Array of argument values to pass to getopt_long - this will need to
	 * include an empty string as the first value (normally this would be
	 * the program name)
	 */
	argv_array = pg_malloc0(sizeof(char *) * (argc_item + 2));

	/* Insert a blank dummy program name at the start of the array */
	argv_array[0] = pg_malloc0(1);

	c = 1;

	/*
	 * Copy the previously extracted arguments from our list to the array
	 */
	for (cell = option_argv.head; cell; cell = cell->next)
	{
		int argv_len = strlen(cell->string) + 1;

		argv_array[c] = pg_malloc0(argv_len);

		strncpy(argv_array[c], cell->string, argv_len);

		c++;
	}

	argv_array[c] = NULL;

	/* Reset getopt's optind variable */
	optind = 0;

	/* Prevent getopt from emitting errors */
	opterr = 0;

	while ((c = getopt_long(argc_item, argv_array, "L:S:", long_options,
							&optindex)) != -1)
	{
		switch (c)
		{
			/* --last-checkpoint-lsn */
			case 'L':
				*checkPoint = parse_lsn(optarg);
				break;
			/* --state */
			case 'S':
			{
				if (strncmp(optarg, "RUNNING", MAXLEN) == 0)
				{
					node_status = NODE_STATUS_UP;
				}
				else if (strncmp(optarg, "SHUTDOWN", MAXLEN) == 0)
				{
					node_status = NODE_STATUS_DOWN;
				}
				else if (strncmp(optarg, "UNKNOWN", MAXLEN) == 0)
				{
					node_status = NODE_STATUS_UNKNOWN;
				}
			}
			break;
		}
	}

	return node_status;
}


static CheckStatus
parse_node_check_archiver(const char *node_check_output, int *files, int *threshold)
{
	int   options_len = 0;
	char *options_string = NULL;
	char *options_string_ptr = NULL;

	CheckStatus status = CHECK_STATUS_UNKNOWN;


	/*
	 * Add parsed options to this list, then copy to an array
	 * to pass to getopt
	 */
	static ItemList option_argv = { NULL, NULL };

	char *argv_item;
	int c, argc_item = 1;

	char **argv_array;
	ItemListCell *cell;

	int			optindex = 0;

	/* We're only interested in these options */
	static struct option long_options[] =
	{
		{"status", required_argument, NULL, 'S'},
		{"files", required_argument, NULL, 'f'},
		{"threshold", required_argument, NULL, 't'},
		{NULL, 0, NULL, 0}
	};

	*files = 0;
	*threshold = 0;

	/* Don't attempt to tokenise an empty string */
	if (!strlen(node_check_output))
	{
		return status;
	}

	options_len = strlen(node_check_output) + 1;
	options_string = pg_malloc(options_len);
	options_string_ptr = options_string;

	/* Copy the string before operating on it with strtok() */
	strncpy(options_string, node_check_output, options_len);

	/* Extract arguments into a list and keep a count of the total */
	while ((argv_item = strtok(options_string_ptr, " ")) != NULL)
	{
		item_list_append(&option_argv, argv_item);

		argc_item++;

		if (options_string_ptr != NULL)
			options_string_ptr = NULL;
	}

	/*
	 * Array of argument values to pass to getopt_long - this will need to
	 * include an empty string as the first value (normally this would be
	 * the program name)
	 */
	argv_array = pg_malloc0(sizeof(char *) * (argc_item + 2));

	/* Insert a blank dummy program name at the start of the array */
	argv_array[0] = pg_malloc0(1);

	c = 1;

	/*
	 * Copy the previously extracted arguments from our list to the array
	 */
	for (cell = option_argv.head; cell; cell = cell->next)
	{
		int argv_len = strlen(cell->string) + 1;

		argv_array[c] = pg_malloc0(argv_len);

		strncpy(argv_array[c], cell->string, argv_len);

		c++;
	}

	argv_array[c] = NULL;

	/* Reset getopt's optind variable */
	optind = 0;

	/* Prevent getopt from emitting errors */
	opterr = 0;

	while ((c = getopt_long(argc_item, argv_array, "f:S:t:", long_options,
							&optindex)) != -1)
	{
		switch (c)
		{
			/* --files */
			case 'f':
				*files = atoi(optarg);
				break;

			case 't':
				*threshold = atoi(optarg);
				break;

			/* --status */
			case 'S':
			{
				if (strncmp(optarg, "OK", MAXLEN) == 0)
				{
					status = CHECK_STATUS_OK;
				}
				else if (strncmp(optarg, "WARNING", MAXLEN) == 0)
				{
					status = CHECK_STATUS_WARNING;
				}
				else if (strncmp(optarg, "CRITICAL", MAXLEN) == 0)
				{
					status = CHECK_STATUS_CRITICAL;
				}
				else if (strncmp(optarg, "UNKNOWN", MAXLEN) == 0)
				{
					status = CHECK_STATUS_UNKNOWN;
				}
				else
				{
					status = CHECK_STATUS_UNKNOWN;
				}
			}
			break;
		}
	}

	return status;
}



static CheckStatus
parse_node_check_replication_lag(const char *node_check_output, int *seconds, int *threshold)
{
	int   options_len = 0;
	char *options_string = NULL;
	char *options_string_ptr = NULL;

	CheckStatus status = CHECK_STATUS_UNKNOWN;


	/*
	 * Add parsed options to this list, then copy to an array
	 * to pass to getopt
	 */
	static ItemList option_argv = { NULL, NULL };

	char *argv_item;
	int c, argc_item = 1;

	char **argv_array;
	ItemListCell *cell;

	int			optindex = 0;

	/* We're only interested in these options */
	static struct option long_options[] =
	{
		{"status", required_argument, NULL, 'S'},
		{"lag", required_argument, NULL, 'l'},
		{"threshold", required_argument, NULL, 't'},
		{NULL, 0, NULL, 0}
	};

	*seconds = 0;
	*threshold = 0;

	/* Don't attempt to tokenise an empty string */
	if (!strlen(node_check_output))
	{
		return status;
	}

	options_len = strlen(node_check_output) + 1;
	options_string = pg_malloc(options_len);
	options_string_ptr = options_string;

	/* Copy the string before operating on it with strtok() */
	strncpy(options_string, node_check_output, options_len);

	/* Extract arguments into a list and keep a count of the total */
	while ((argv_item = strtok(options_string_ptr, " ")) != NULL)
	{
		item_list_append(&option_argv, argv_item);

		argc_item++;

		if (options_string_ptr != NULL)
			options_string_ptr = NULL;
	}

	/*
	 * Array of argument values to pass to getopt_long - this will need to
	 * include an empty string as the first value (normally this would be
	 * the program name)
	 */
	argv_array = pg_malloc0(sizeof(char *) * (argc_item + 2));

	/* Insert a blank dummy program name at the start of the array */
	argv_array[0] = pg_malloc0(1);

	c = 1;

	/*
	 * Copy the previously extracted arguments from our list to the array
	 */
	for (cell = option_argv.head; cell; cell = cell->next)
	{
		int argv_len = strlen(cell->string) + 1;

		argv_array[c] = pg_malloc0(argv_len);

		strncpy(argv_array[c], cell->string, argv_len);

		c++;
	}

	argv_array[c] = NULL;

	/* Reset getopt's optind variable */
	optind = 0;

	/* Prevent getopt from emitting errors */
	opterr = 0;

	while ((c = getopt_long(argc_item, argv_array, "l:S:t:", long_options,
							&optindex)) != -1)
	{
		switch (c)
		{
			/* --files */
			case 'l':
				*seconds = atoi(optarg);
				break;

			case 't':
				*threshold = atoi(optarg);
				break;

			/* --status */
			case 'S':
			{
				if (strncmp(optarg, "OK", MAXLEN) == 0)
				{
					status = CHECK_STATUS_OK;
				}
				else if (strncmp(optarg, "WARNING", MAXLEN) == 0)
				{
					status = CHECK_STATUS_WARNING;
				}
				else if (strncmp(optarg, "CRITICAL", MAXLEN) == 0)
				{
					status = CHECK_STATUS_CRITICAL;
				}
				else if (strncmp(optarg, "UNKNOWN", MAXLEN) == 0)
				{
					status = CHECK_STATUS_UNKNOWN;
				}
				else
				{
					status = CHECK_STATUS_UNKNOWN;
				}
			}
			break;
		}
	}

	return status;
}
