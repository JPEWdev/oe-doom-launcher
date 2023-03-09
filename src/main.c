/*
 * SPDX-License-Identifier: MIT
 * Copyright Joshua Watt <JPEWhacker@gmail.com>
 */

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-glib/glib-malloc.h>
#include <avahi-glib/glib-watch.h>
#include <glib.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/queue.h>
#include <sys/wait.h>
#include <systemd/sd-id128.h>

#define CLIENT_SERVICE_NAME "_oe-doom-client._udp"
#define HOST_SERVICE_NAME "_oe-doom-host._udp"

#define WAD_KEY "wad"
#define CAN_HOST_KEY "can-host"

#define DEFAULT_CONFIG_PATH "/etc/oe-zdoom/config.ini"
#define DEFAULT_ZDOOM "zdoom"
#define DEFAULT_MP_WAD "freedm.wad"
#define DEFAULT_MP_MAP "MAP01"
#define DEFAULT_SP_WAD "freedoom1.wad"
#define DEFAULT_SOURCE_WAIT (30)

static struct config {
    uint16_t port;
    char* zdoom;
    char* mp_wad;
    char* mp_map;
    char* mp_config;
    char* sp_wad;
    char* sp_config;
    bool can_host;
    int source_wait;
} config;

struct local_service {
    AvahiEntryGroup* group;
    char* service_name;
    AvahiIfIndex interface;
    AvahiProtocol protocol;
    AvahiPublishFlags flags;
    char* name;
    char* type;
    char* domain;
    char* host;
    uint16_t port;
    AvahiStringList* txt_records;
};

static int timeout_source = 0;
static int child_source = 0;
static GPid child_pid = 0;
static AvahiClient* avahi_client;

static struct local_service local_client_service = {};
static struct local_service local_host_service = {};

struct remote_service {
    TAILQ_ENTRY(remote_service) link;
    AvahiIfIndex interface;
    AvahiProtocol protocol;
    uint16_t port;
    char* name;
    char* type;
    char* domain;
    char* hostname;
    char* wad;
    AvahiLookupResultFlags flags;
    bool can_host;
};

static TAILQ_HEAD(remote_service_head, remote_service)
    client_service_list = TAILQ_HEAD_INITIALIZER(client_service_list);
static struct remote_service* current_host = NULL;
static bool single_player_running = false;

static void create_service(AvahiClient* client, struct local_service* service);

static gboolean on_source_timeout(gpointer userdata);

static void stop_service(struct local_service* service) {
    if (service->group) {
        g_print("Stopping service %s %s\n", service->name, service->type);
        avahi_entry_group_reset(service->group);
        avahi_entry_group_free(service->group);
        service->group = NULL;
    }
}

static void stop_source_timer(void) {
    if (timeout_source) {
        g_source_remove(timeout_source);
        timeout_source = 0;
    }
}

static void restart_source_timer(void) {
    stop_source_timer();
    timeout_source =
        g_timeout_add(config.source_wait * 1000, on_source_timeout, NULL);
}

static void remote_service_free(struct remote_service* service) {
    if (service) {
        g_free(service->name);
        g_free(service->type);
        g_free(service->domain);
        g_free(service->hostname);
        g_free(service->wad);

        g_free(service);
    }
}

static bool remote_service_equal(struct remote_service const* a,
                                 struct remote_service const* b) {
    return (g_strcmp0(a->type, b->type) == 0) &&
           (g_strcmp0(a->name, b->name) == 0) && a->interface == b->interface &&
           a->protocol == b->protocol;
}

static void on_child_exit(GPid pid, gint status, gpointer userdata);

static void spawn_child(char** argv) {
    if (child_pid) {
        kill(child_pid, SIGINT);
        waitpid(child_pid, NULL, 0);
        child_pid = 0;
    }
    if (child_source) {
        g_source_remove(child_source);
        child_source = 0;
    }

    GError* error = NULL;

    g_print("Launching");

    for (int i = 0; argv[i] != NULL; i++) {
        g_print(" %s", argv[i]);
    }

    g_print("\n");

    if (!g_spawn_async(NULL, argv, NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL,
                       NULL, &child_pid, &error)) {
        g_error("Cannot spawn child process: %m");
        g_clear_error(&error);
        return;
    }

    g_print("Child PID is %d\n", child_pid);

    child_source = g_child_watch_add(child_pid, on_child_exit, NULL);
}

static void launch_single_player(void) {
    stop_service(&local_host_service);
    if (!single_player_running) {
        g_print("Launching single player game\n");
        g_autoptr(GStrvBuilder) sb = g_strv_builder_new();

        g_strv_builder_add(sb, config.zdoom);
        g_strv_builder_add(sb, "-iwad");
        g_strv_builder_add(sb, config.sp_wad);
        if (config.sp_config) {
            g_strv_builder_add(sb, "-config");
            g_strv_builder_add(sb, config.sp_config);
        }

        g_auto(GStrv) argv = g_strv_builder_end(sb);
        spawn_child(argv);
        single_player_running = true;
    }
}

static void connect_to_host(void) {
    char port_str[12];
    stop_service(&local_host_service);
    g_print("Connecting to host %s:%d\n", current_host->hostname,
            current_host->port);

    g_autoptr(GStrvBuilder) sb = g_strv_builder_new();

    g_strv_builder_add(sb, config.zdoom);
    g_strv_builder_add(sb, "-iwad");
    g_strv_builder_add(sb, current_host->wad);
    g_strv_builder_add(sb, "-join");
    g_strv_builder_add(sb, current_host->hostname);
    g_strv_builder_add(sb, "-port");
    snprintf(port_str, sizeof(port_str), "%d", current_host->port);
    g_strv_builder_add(sb, port_str);
    if (config.mp_config) {
        g_strv_builder_add(sb, "-config");
        g_strv_builder_add(sb, config.mp_config);
    }

    g_auto(GStrv) argv = g_strv_builder_end(sb);
    spawn_child(argv);
    single_player_running = false;
}

static void host_game(int num_players) {
    g_print("Hosting game for %d players\n", num_players);
    char count_str[12];
    g_autoptr(GStrvBuilder) sb = g_strv_builder_new();

    g_strv_builder_add(sb, config.zdoom);
    g_strv_builder_add(sb, "-iwad");
    g_strv_builder_add(sb, config.mp_wad);
    g_strv_builder_add(sb, "-deathmatch");
    g_strv_builder_add(sb, "+map");
    g_strv_builder_add(sb, config.mp_map);
    g_strv_builder_add(sb, "-host");
    snprintf(count_str, sizeof(count_str), "%i", num_players);
    g_strv_builder_add(sb, count_str);
    g_strv_builder_add(sb, "-port");
    snprintf(count_str, sizeof(count_str), "%i", config.port);
    g_strv_builder_add(sb, count_str);
    if (config.mp_config) {
        g_strv_builder_add(sb, "-config");
        g_strv_builder_add(sb, config.mp_config);
    }

    g_auto(GStrv) argv = g_strv_builder_end(sb);
    spawn_child(argv);

    single_player_running = false;

    create_service(avahi_client, &local_host_service);
}

static void on_child_exit(GPid pid, gint status, gpointer userdata) {
    g_print("Child %d exited with %d\n", pid, status);
    g_spawn_close_pid(pid);
    if (pid != child_pid) {
        return;
    }
    child_source = 0;

    single_player_running = false;
    launch_single_player();
}

static gboolean on_source_timeout(gpointer userdata) {
    g_print("Source timeout\n");
    int other_count = 0;

    // Count number of (non-self) client services
    struct remote_service* client;
    TAILQ_FOREACH(client, &client_service_list, link) {
        if ((client->flags & AVAHI_LOOKUP_RESULT_OUR_OWN) == 0) {
            other_count++;
        }
    }

    struct remote_service* best = TAILQ_FIRST(&client_service_list);
    bool become_host = false;

    if (best != NULL && best->can_host) {
        if (best->flags & AVAHI_LOOKUP_RESULT_OUR_OWN) {
            if (other_count) {
                g_print("This is the best host. Hosting for %i clients....\n",
                        other_count);
                host_game(other_count + 1);
            } else {
                g_print("No peers found\n");
                launch_single_player();
            }
        } else {
            g_print("Best host is %s\n", best->hostname);
            // No change here; wait for the host to start the game
        }
    } else {
        g_print("No suitable hosts\n");
        launch_single_player();
    }

    timeout_source = 0;
    return FALSE;
}

static int cmp_remote_service(struct remote_service* const a,
                              struct remote_service* const b) {
    if (a->can_host != b->can_host) {
        return !!b->can_host - !!a->can_host;
    }

    return g_strcmp0(a->name, b->name);
}

static void handle_collision(struct local_service* service) {
    /* A service name collision with a remote service
     * happened. Let's pick a new name */
    char* n = avahi_alternative_service_name(service->name);
    avahi_free(service->name);
    service->name = n;
    fprintf(stderr, "Service name collision, renaming service to '%s'\n",
            service->name);
    /* And recreate the services */
    create_service(avahi_entry_group_get_client(service->group), service);
}

static void entry_group_callback(AvahiEntryGroup* group,
                                 AvahiEntryGroupState state,
                                 AVAHI_GCC_UNUSED void* userdata) {
    struct local_service* service = userdata;

    /* Called whenever the entry group state changes */
    switch (state) {
        case AVAHI_ENTRY_GROUP_ESTABLISHED:
            /* The entry group has been established successfully */
            fprintf(stderr, "Service '%s' successfully established.\n",
                    service->name);
            break;
        case AVAHI_ENTRY_GROUP_COLLISION:
            handle_collision(service);
            break;

        case AVAHI_ENTRY_GROUP_FAILURE:
            fprintf(stderr, "Entry group failure: %s\n",
                    avahi_strerror(avahi_client_errno(
                        avahi_entry_group_get_client(group))));
            break;

        case AVAHI_ENTRY_GROUP_UNCOMMITED:
        case AVAHI_ENTRY_GROUP_REGISTERING:;
    }
}

static void create_service(AvahiClient* client, struct local_service* service) {
    if (service->name == NULL) {
        sd_id128_t machine_id;
        sd_id128_get_machine(&machine_id);
        service->name = g_strdup_printf(SD_ID128_FORMAT_STR,
                                        SD_ID128_FORMAT_VAL(machine_id));
    }

    if (service->group == NULL) {
        service->group =
            avahi_entry_group_new(client, entry_group_callback, service);
        if (service->group == NULL) {
            g_critical("avahi_entry_group_new() failed: %s\n",
                       avahi_strerror(avahi_client_errno(client)));
            return;
        }
    }

    if (avahi_entry_group_is_empty(service->group)) {
        g_print("Adding service '%s'\n", service->name);

        int ret = avahi_entry_group_add_service_strlst(
            service->group, service->interface, service->protocol,
            service->flags, service->name, service->type, service->domain,
            service->host, service->port, service->txt_records);

        if (ret != 0) {
            if (ret == AVAHI_ERR_COLLISION) {
                avahi_entry_group_reset(service->group);
                handle_collision(service);
                return;
            }

            g_error("Failed to add %s service: %s\n", CLIENT_SERVICE_NAME,
                    avahi_strerror(ret));
            return;
        }

        ret = avahi_entry_group_commit(service->group);
        if (ret != 0) {
            g_warning("Failed to commit entry group: %s\n",
                      avahi_strerror(ret));
        }
    }
}

static void resolve_callback(AvahiServiceResolver* r, AvahiIfIndex interface,
                             AvahiProtocol protocol, AvahiResolverEvent event,
                             const char* name, const char* type,
                             const char* domain, const char* host_name,
                             const AvahiAddress* address, uint16_t port,
                             AvahiStringList* txt, AvahiLookupResultFlags flags,
                             AVAHI_GCC_UNUSED void* userdata) {
    assert(r);
    /* Called whenever a service has been resolved successfully or timed out
     */
    switch (event) {
        case AVAHI_RESOLVER_FAILURE:
            g_error(
                "(Resolver) Failed to resolve service '%s' of type '%s' in "
                "domain '%s': %s\n",
                name, type, domain,
                avahi_strerror(
                    avahi_client_errno(avahi_service_resolver_get_client(r))));
            break;

        case AVAHI_RESOLVER_FOUND: {
            char a[AVAHI_ADDRESS_STR_MAX], *t;
            g_debug("Service '%s' of type '%s' in domain '%s':\n", name, type,
                    domain);
            avahi_address_snprint(a, sizeof(a), address);
            t = avahi_string_list_to_string(txt);
            g_debug(
                "\t%s:%u (%s)\n"
                "\tTXT=%s\n"
                "\tcookie is %u\n"
                "\tis_local: %i\n"
                "\tour_own: %i\n"
                "\twide_area: %i\n"
                "\tmulticast: %i\n"
                "\tcached: %i\n",
                host_name, port, a, t,
                avahi_string_list_get_service_cookie(txt),
                !!(flags & AVAHI_LOOKUP_RESULT_LOCAL),
                !!(flags & AVAHI_LOOKUP_RESULT_OUR_OWN),
                !!(flags & AVAHI_LOOKUP_RESULT_WIDE_AREA),
                !!(flags & AVAHI_LOOKUP_RESULT_MULTICAST),
                !!(flags & AVAHI_LOOKUP_RESULT_CACHED));
            avahi_free(t);

            struct remote_service* service = g_malloc0(sizeof(*service));
            service->name = g_strdup(name);
            service->type = g_strdup(type);
            service->domain = g_strdup(domain);
            service->hostname = g_strdup(host_name);
            service->interface = interface;
            service->protocol = protocol;
            service->flags = flags;
            service->port = port;

            while (txt) {
                char* key;
                char* value;
                size_t size;
                if (avahi_string_list_get_pair(txt, &key, &value, &size)) {
                    continue;
                }

                if (g_strcmp0(key, CAN_HOST_KEY) == 0) {
                    service->can_host = (g_strcmp0(value, "1") == 0);
                } else if (g_strcmp0(key, WAD_KEY) == 0) {
                    service->wad = g_strdup(value);
                }

                avahi_free(key);
                avahi_free(value);
                txt = avahi_string_list_get_next(txt);
            }

            if (g_strcmp0(type, CLIENT_SERVICE_NAME) == 0) {
                // Remove matching entries from client list
                struct remote_service *c, *tmp_c = NULL;
                TAILQ_FOREACH_SAFE(c, &client_service_list, link, tmp_c) {
                    if (remote_service_equal(service, c)) {
                        g_print("Removing client %s\n", c->name);
                        TAILQ_REMOVE(&client_service_list, c, link);
                        remote_service_free(c);
                    }
                }

                g_print("New client %s (%s)\n", service->name,
                        service->hostname);
                g_print("  can-host: %s\n",
                        service->can_host ? "true" : "false");
                g_print("  is-own: %s\n",
                        (service->flags & AVAHI_LOOKUP_RESULT_OUR_OWN)
                            ? "true"
                            : "false");

                // Sorted insert
                TAILQ_FOREACH(c, &client_service_list, link) {
                    if (cmp_remote_service(service, c) < 0) {
                        g_print("Adding new client before %s\n", c->name);
                        TAILQ_INSERT_BEFORE(c, service, link);
                        break;
                    }
                }
                if (c == NULL) {
                    g_print("Adding new client to end of list\n");
                    TAILQ_INSERT_TAIL(&client_service_list, service, link);
                }

                // If this is not our own service, restart the source timer
                if ((service->flags & AVAHI_LOOKUP_RESULT_OUR_OWN) == 0) {
                    restart_source_timer();
                }
            } else if (g_strcmp0(type, HOST_SERVICE_NAME) == 0) {
                if ((service->flags & AVAHI_LOOKUP_RESULT_OUR_OWN) == 0) {
                    g_print("Connecting to new host %s (%s)\n", service->name,
                            service->hostname);
                    remote_service_free(current_host);
                    current_host = service;

                    connect_to_host();

                    stop_source_timer();
                } else {
                    remote_service_free(service);
                }
            } else {
                remote_service_free(service);
            }
        }
    }
    avahi_service_resolver_free(r);
}

static void browse_callback(AvahiServiceBrowser* b, AvahiIfIndex interface,
                            AvahiProtocol protocol, AvahiBrowserEvent event,
                            const char* name, const char* type,
                            const char* domain,
                            AVAHI_GCC_UNUSED AvahiLookupResultFlags flags,
                            void* userdata) {
    AvahiClient* c = userdata;
    assert(b);
    /* Called whenever a new services becomes available on the LAN or is
     * removed from the LAN */
    switch (event) {
        case AVAHI_BROWSER_FAILURE:
            g_warning("(Browser) %s\n",
                      avahi_strerror(avahi_client_errno(
                          avahi_service_browser_get_client(b))));
            return;

        case AVAHI_BROWSER_NEW:
            g_debug("(Browser) NEW: service '%s' of type '%s' in domain '%s'\n",
                    name, type, domain);
            /* We ignore the returned resolver object. In the callback
               function we free it. If the server is terminated before
               the callback function is called the server will free
               the resolver for us. */
            if (!(avahi_service_resolver_new(c, interface, protocol, name, type,
                                             domain, AVAHI_PROTO_INET, 0,
                                             resolve_callback, c)))
                g_warning("Failed to resolve service '%s': %s\n", name,
                          avahi_strerror(avahi_client_errno(c)));
            break;

        case AVAHI_BROWSER_REMOVE: {
            g_debug(
                "(Browser) REMOVE: service '%s' of type '%s' in domain '%s'\n",
                name, type, domain);
            struct remote_service* client;
            struct remote_service* client_tmp;

            TAILQ_FOREACH_SAFE(client, &client_service_list, link, client_tmp) {
                if (g_strcmp0(client->name, name) == 0 &&
                    g_strcmp0(client->type, type) == 0 &&
                    g_strcmp0(client->domain, domain) == 0) {
                    if ((client->flags & AVAHI_LOOKUP_RESULT_OUR_OWN) == 0) {
                        restart_source_timer();
                    }
                    g_print("Removing client %s\n", client->name);
                    TAILQ_REMOVE(&client_service_list, client, link);

                    remote_service_free(client);
                }
            }

            if (current_host && (g_strcmp0(current_host->name, name) == 0) &&
                (g_strcmp0(current_host->type, type) == 0) &&
                (g_strcmp0(current_host->domain, domain) == 0)) {
                remote_service_free(current_host);
                current_host = NULL;

                launch_single_player();
                restart_source_timer();
            }
        } break;

        case AVAHI_BROWSER_ALL_FOR_NOW:
        case AVAHI_BROWSER_CACHE_EXHAUSTED:
            g_debug("(Browser) %s\n", event == AVAHI_BROWSER_CACHE_EXHAUSTED
                                          ? "CACHE_EXHAUSTED"
                                          : "ALL_FOR_NOW");
            break;
    }
}

/* Callback for state changes on the Client */
static void avahi_client_callback(AVAHI_GCC_UNUSED AvahiClient* client,
                                  AvahiClientState state, void* userdata) {
    GMainLoop* loop = userdata;
    g_debug("Avahi Client State Change: %d", state);
    switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
            create_service(client, &local_client_service);
            break;

        case AVAHI_CLIENT_FAILURE:
            /* We we're disconnected from the Daemon */
            g_debug("Disconnected from the Avahi Daemon: %s",
                    avahi_strerror(avahi_client_errno(client)));
            /* Quit the application */
            g_main_loop_quit(loop);
            break;

        default:
            break;
    }
}

static bool parse_config(int* argc, char*** argv) {
    static gchar* config_file_path = DEFAULT_CONFIG_PATH;

    static const GOptionEntry entries[] = {
        {"config", 'c', 0, G_OPTION_ARG_FILENAME, &config_file_path,
         "Config file path"},
        {},
    };

    g_autoptr(GOptionContext) context =
        g_option_context_new(" - OpenEmbedded ZDoom Demo Launcher");
    g_autoptr(GError) error = NULL;

    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, argc, argv, &error)) {
        g_print("Unable to parse options: %m");
        return false;
    }

    g_autoptr(GKeyFile) key_file = g_key_file_new();

    config.port = 5029;
    config.zdoom = g_strdup(DEFAULT_ZDOOM);
    config.mp_wad = g_strdup(DEFAULT_MP_WAD);
    config.mp_map = g_strdup(DEFAULT_MP_MAP);
    config.mp_config = NULL;
    config.sp_wad = g_strdup(DEFAULT_SP_WAD);
    config.sp_config = NULL;
    config.can_host = true;
    config.source_wait = DEFAULT_SOURCE_WAIT;

    if (!g_key_file_load_from_file(key_file, config_file_path, 0, &error)) {
        g_warning("Cannot open %s: %m", config_file_path);
        return (g_strcmp0(config_file_path, DEFAULT_CONFIG_PATH) == 0);
    }

    char* value;
    if ((value = g_key_file_get_string(key_file, "multiplayer", "wad", NULL)) !=
        NULL) {
        g_free(config.mp_wad);
        config.mp_wad = value;
    }

    if ((value = g_key_file_get_string(key_file, "multiplayer", "map", NULL)) !=
        NULL) {
        g_free(config.mp_map);
        config.mp_map = value;
    }

    if ((value = g_key_file_get_string(key_file, "multiplayer", "config",
                                       NULL)) != NULL) {
        g_free(config.mp_config);
        config.mp_config = value;
    }

    if ((value = g_key_file_get_string(key_file, "singleplayer", "wad",
                                       NULL)) != NULL) {
        g_free(config.sp_wad);
        config.sp_wad = value;
    }

    if ((value = g_key_file_get_string(key_file, "singleplayer", "config",
                                       NULL)) != NULL) {
        g_free(config.sp_config);
        config.sp_config = value;
    }

    config.can_host =
        g_key_file_get_boolean(key_file, "multiplayer", "can-host", &error);
    if (!config.can_host) {
        if (error) {
            config.can_host = true;
        }
        g_clear_error(&error);
    }

    int ival;
    ival = g_key_file_get_integer(key_file, "multiplayer", "port", NULL);
    if (ival > 0) {
        config.port = ival;
    }

    ival = g_key_file_get_integer(key_file, "multiplayer", "wait", NULL);
    if (ival > 0) {
        config.source_wait = ival;
    }

    return true;
}

int main(int argc, char** argv) {
    int error = 0;
    if (!parse_config(&argc, &argv)) {
        return 1;
    }

    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

    local_client_service.interface = AVAHI_IF_UNSPEC;
    local_client_service.protocol = AVAHI_PROTO_INET;
    local_client_service.type = CLIENT_SERVICE_NAME;
    local_client_service.port = config.port;
    local_client_service.txt_records =
        avahi_string_list_add_pair(local_client_service.txt_records, "can-host",
                                   config.can_host ? "1" : "0");

    local_host_service.interface = AVAHI_IF_UNSPEC;
    local_host_service.protocol = AVAHI_PROTO_INET;
    local_host_service.type = HOST_SERVICE_NAME;
    local_host_service.port = config.port;
    local_host_service.txt_records = avahi_string_list_add_pair(
        local_host_service.txt_records, "wad", config.mp_wad);

    // Tell Avahi to use glib allocators
    avahi_set_allocator(avahi_glib_allocator());

    AvahiGLibPoll* glib_poll = avahi_glib_poll_new(NULL, G_PRIORITY_DEFAULT);
    AvahiPoll const* poll_api = avahi_glib_poll_get(glib_poll);

    avahi_client =
        avahi_client_new(poll_api, 0, avahi_client_callback, loop, &error);

    AvahiServiceBrowser* client_browser = avahi_service_browser_new(
        avahi_client, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, CLIENT_SERVICE_NAME,
        NULL, 0, browse_callback, avahi_client);

    AvahiServiceBrowser* host_browser = avahi_service_browser_new(
        avahi_client, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, HOST_SERVICE_NAME,
        NULL, 0, browse_callback, avahi_client);

    launch_single_player();

    g_main_loop_run(loop);

    avahi_service_browser_free(host_browser);
    avahi_service_browser_free(client_browser);
    avahi_client_free(avahi_client);
    avahi_glib_poll_free(glib_poll);

    return 0;
}
