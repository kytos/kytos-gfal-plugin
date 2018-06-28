#include <assert.h>
#include <glib.h>
#include <gfal_plugins_api.h>
#include <regex.h>
#include <string.h>
#include <utils/gfal2_uri.h>

/**
 * Structure used to hold the different pairs notified by gfal2
 */
typedef struct {
    char *source, *destination;
} pair_t;

typedef struct {
    GSequence *pairs;
    gfal2_context_t context;
    off_t total_size;
    off_t vlan_id;
    off_t waiting_to_transfer;
    gchar *kytos_endpoint;
} kytos_t;


typedef struct {
    off_t vlan_id;
    off_t waiting_to_transfer;
} kytos_data;

/**
 * Create a pair from the description sent by gfal2, which is of the form
 * source => destination
 * Note that source and destination are xml-escaped
 */
pair_t* kytos_create_pair(const char* description)
{
    gchar** splitted = g_strsplit(description, " => ", 2);

    pair_t* pair = g_new0(pair_t, 1);
    pair->source = g_strdup(splitted[0]);
    pair->destination = g_strdup(splitted[1]);

    g_strfreev(splitted);

    return pair;
}


/**
 * Frees the memory used by a pair
 */
void kytos_release_pair(gpointer data)
{
    pair_t* pair = (pair_t*)data;
    g_free(pair->source);
    g_free(pair->destination);
    g_free(pair);
}


/**
 * Creates and initializes a kytos_t
 */
kytos_t* kytos_create_data(gfal2_context_t context)
{
    kytos_t* kytos = g_new0(kytos_t, 1);
    kytos->pairs = g_sequence_new(kytos_release_pair);
    kytos->context = context;
    kytos->vlan_id = -1; // no vlan setted 
    kytos->waiting_to_transfer = 0;
    kytos->kytos_endpoint =  gfal2_get_opt_string_with_default(context, "KYTOS PLUGIN", "ENDPOINT", NULL);
    return kytos;
}


/**
 * Frees the memory used by a kytos_t
 */
void kytos_release_data(gpointer p)
{
    kytos_t* kytos = (kytos_t*)p;
    g_sequence_free(kytos->pairs);
    g_free(kytos);
}


/**
 * Stat the source and add its file size to the total_size
 */
void kytos_add_size(gpointer data, gpointer udata)
{
    kytos_t* kytos = (kytos_t*)udata;
    pair_t* pair = (pair_t*)data;

    GError* error = NULL;
    struct stat st;
    if (gfal2_stat(kytos->context, pair->source, &st, &error) < 0) {
        gfal2_log(G_LOG_LEVEL_ERROR, "Could not stat %s (%s)", pair->source, error->message);
        g_error_free(error);
    }
    else {
        kytos->total_size += st.st_size;
        kytos->waiting_to_transfer += 1;
    }
}


/**
 * JMS contrib
 * Get source and destination file names and log then to stdout
 */
void kytos_print_source(gpointer data, gpointer udata)
{
    kytos_t* kytos = (kytos_t*)udata;
    pair_t* pair = (pair_t*)data;

    GError* error = NULL;
    struct stat st;
    if (gfal2_stat(kytos->context, pair->source, &st, &error) < 0) {
        gfal2_log(G_LOG_LEVEL_ERROR, "Could not stat %s (%s)", pair->source, error->message);
        g_error_free(error);
    }
    else {
        //gfal2_log(G_LOG_LEVEL_WARNING, "================ INSIDE KYTOS SDN SETUP ======================");
        gfal2_log(G_LOG_LEVEL_WARNING, "%s ==> %s", pair->source, pair->destination);
    }
}


/**
 * When this enters, it has the list of files that will be transferred
 */
void kytos_notify_remote(kytos_t* data)
{
    gfal2_uri source, destination;
    GError* sourceError = NULL;
    GError* destinError = NULL;

    // Get the hostname and port
    GSequenceIter* iter = g_sequence_get_begin_iter(data->pairs);
    if (!iter)
        return;

    pair_t* pair = (pair_t*)g_sequence_get(iter);
    /*
    gfal2_parse_uri(pair->source, &source, NULL);
    gfal2_parse_uri(pair->destination, &destination, NULL);
    */
    gfal2_parse_uri(pair->source, &sourceError);
    gfal2_parse_uri(pair->destination, &destinError);


    // Calculate the size
    data->total_size = 0;
    g_sequence_foreach(data->pairs, kytos_add_size, data);

    /*
    gfal2_log(G_LOG_LEVEL_WARNING, "Between %s and %s %d files with a total size of %lld bytes",
            source.domain, destination.domain, g_sequence_get_length(data->pairs),
            (long long)data->total_size);
    */
    gfal2_log(G_LOG_LEVEL_WARNING, "Between %s and %s %d files with a total size of %lld bytes",
            pair->source, pair->destination, g_sequence_get_length(data->pairs),
            (long long)data->total_size);
    //*/

    //g_sequence_foreach(data->pairs, kytos_print_source, data);
    // PLACEHOLDER

}

/**
 * When this enter, description holds a string as
 * host:[ip]:port
 * Note that ip is between brackets even for ipv4
 */
static void kytos_dest_pasv(const char* description, kytos_t* data)
{
    regex_t regex;
    int retregex = regcomp(&regex, "([a-zA-Z0-9._-]+):\\[([0-9a-f.:]+)\\]:([0-9]+)", REG_EXTENDED);
  //int retregex = regcomp(&regex, "([a-zA-Z0-9._-]+):([0-9a-f.:]+):([0-9]+)", REG_EXTENDED);
  //int retregex = regcomp(&regex, "([0-9a-f.:]+):([0-9]+)", REG_EXTENDED);
    assert(retregex == 0);

    regmatch_t matches[4];
    retregex = regexec(&regex, description, 4, matches, 0);
    if (retregex == REG_NOMATCH) {
        gfal2_log(G_LOG_LEVEL_CRITICAL, "The description could not be parsed: %s", description);
        return;
    }

    // Host
    char host[256];
    size_t len = matches[1].rm_eo - matches[1].rm_so + 1; // Account for \0
    if (len > sizeof(host))
        len = sizeof(host);
    g_strlcpy(host, description + matches[1].rm_so, len);


    // IP
    char ip[64];
    len = matches[2].rm_eo - matches[2].rm_so + 1; // Account for \0
    if (len > sizeof(ip))
        len = sizeof(ip);
    g_strlcpy(ip, description + matches[2].rm_so, len);

    // Port
    int port = atoi(description + matches[3].rm_so);

    // PLACEHOLDER
    gfal2_log(G_LOG_LEVEL_WARNING, "Got %s:%d for host %s", ip, port, host);

    // ask for a kytos_vlan_id if it does not exist yet
    gfal2_log(G_LOG_LEVEL_WARNING, "There are %d remaining files to transfer", data->waiting_to_transfer);

    if ( data->vlan_id == -1 ) {
       // placeholder for a kytos call
       if ( data->kytos_endpoint == NULL ) {
         gfal2_log(G_LOG_LEVEL_WARNING, "No kytos endpoint configured, skiping the SDN configuration");
       } else {
         gfal2_log(G_LOG_LEVEL_WARNING, "[Kytos] trying to get a VLAN from kytos controller at %s",data->kytos_endpoint);
         g_usleep(20000);
         data->vlan_id = g_random_int_range(20000,30000);
         gfal2_log(G_LOG_LEVEL_WARNING, "       Kytos allocated the VLAN with ID %d for this transfer",data->vlan_id);
       }
    } else {
       gfal2_log(G_LOG_LEVEL_WARNING, "[Kytos] reusing VLAN %d", data->vlan_id);
    }
    
}

static void kytos_close_circuit(const char* description, kytos_t* data)
{
  if ( data->waiting_to_transfer > 0 ) {
    gfal2_log(G_LOG_LEVEL_WARNING, "There are %d files waiting to be transfered.", data->waiting_to_transfer);
    return;
  }

  if ( data->vlan_id == -1 ) {
    gfal2_log(G_LOG_LEVEL_WARNING, "There is no VLAN to release");
    return;
  }

  gfal2_log(G_LOG_LEVEL_WARNING, "[Kytos] releasing VLAN %d at kytos controller on %s",
                                  data->vlan_id, 
                                  data->kytos_endpoint);
  g_usleep(20000);
}

/**
 * This method is called by gfal2 and plugins
 */
void kytos_event_listener(const gfalt_event_t e, gpointer user_data)
{
    const GQuark GFAL_GRIDFTP_PASV_STAGE_QUARK = g_quark_from_static_string("PASV");

    kytos_t* data = (kytos_t*)user_data;

    gfal2_log(G_LOG_LEVEL_WARNING, "======== ");
    gfal2_log(G_LOG_LEVEL_WARNING, "======== CURRENT STAGE %s", g_quark_to_string(e->stage));
    gfal2_log(G_LOG_LEVEL_WARNING, "======== ");

    if (e->stage ==  GFAL_EVENT_LIST_ENTER) {
        g_sequence_remove_range(
                g_sequence_get_begin_iter(data->pairs),
                g_sequence_get_end_iter(data->pairs));
    }
    else if (e->stage == GFAL_EVENT_LIST_ITEM) {
        g_sequence_append(data->pairs, kytos_create_pair(e->description));
    }
    else if (e->stage == GFAL_EVENT_LIST_EXIT) {
        kytos_notify_remote(data);
    }
    else if (e->stage == GFAL_GRIDFTP_PASV_STAGE_QUARK) {
        kytos_dest_pasv(e->description, data);
    }
    else if (e->stage == GFAL_EVENT_TRANSFER_EXIT) {
        kytos_close_circuit(e->description, data);
    } 
}


/**
 * This method will be called when a copy method is called (bulk or single method).
 * The kytos plugin can take this chance to inject its own event listener into the copy configuration.
 * Several listeners can be registered at the same time, so this is safe.
 */
int kytos_copy_enter_hook(plugin_handle plugin_data, gfal2_context_t context,
        gfalt_params_t params, GError** error)
{
    GError* tmp_error = NULL;


    // Register the new listener
    gfalt_add_event_callback(params, kytos_event_listener,
            kytos_create_data(context), kytos_release_data,
            &tmp_error);

    if (tmp_error) {
        gfal2_propagate_prefixed_error(error, tmp_error, __func__);
        return -1;
    }

    gfal2_log(G_LOG_LEVEL_MESSAGE, "Kytos event listener registered");

    return 0;
}

/**
 * Return the plugin name
 */
const char* kytos_get_name()
{
    return "kytos";
}

/**
 * This method is called by gfal2 when a context is instantiated
 */
gfal_plugin_interface gfal_plugin_init(gfal2_context_t handle, GError** err)
{
    kytos_data kdata;
    kdata.vlan_id = -1;
    kdata.waiting_to_transfer = 0;

    gfal2_log(G_LOG_LEVEL_WARNING, "Loading Kytos Plugins");
    gfal_plugin_interface kytos_plugin;
    memset(&kytos_plugin, 0, sizeof(gfal_plugin_interface));

    kytos_plugin.getName = kytos_get_name;
    kytos_plugin.copy_enter_hook = kytos_copy_enter_hook;
    kytos_plugin.plugin_data = &kdata;

    return kytos_plugin;
}
