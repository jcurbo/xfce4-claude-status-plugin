/*
 * xfce4-claude-status-plugin
 * XFCE Panel Plugin showing Claude Max/Pro rate limit usage
 *
 * Copyright (c) 2026 James Curbo
 * SPDX-License-Identifier: MIT
 */

#include <libxfce4panel/libxfce4panel.h>
#include <libxfce4ui/libxfce4ui.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

/* Context window sizes by model (in tokens) */
#define CONTEXT_WINDOW_DEFAULT 200000
#define CONTEXT_WINDOW_1M 1000000

/* Default configuration values */
#define DEFAULT_UPDATE_INTERVAL 30
#define DEFAULT_YELLOW_THRESHOLD 25
#define DEFAULT_ORANGE_THRESHOLD 50
#define DEFAULT_RED_THRESHOLD 75
#define DEFAULT_CREDS_FILE "~/.claude/.credentials.json"

/* Plugin data structure */
typedef struct {
    XfcePanelPlugin *plugin;

    /* Widgets */
    GtkWidget *box;
    GtkWidget *grid;

    /* Row 1 widgets */
    GtkWidget *plan_label;
    GtkWidget *five_hour_lbl;
    GtkWidget *five_hour_bar;
    GtkWidget *five_hour_pct;
    GtkWidget *five_hour_reset;

    /* Row 2 widgets */
    GtkWidget *ctx_label;
    GtkWidget *seven_day_lbl;
    GtkWidget *seven_day_bar;
    GtkWidget *seven_day_pct;
    GtkWidget *seven_day_reset;

    /* HTTP session */
    SoupSession *session;

    /* Cached data */
    gchar *access_token;
    gchar *plan_name;
    gdouble five_hour_pct_val;
    gdouble seven_day_pct_val;
    gchar *five_hour_reset_str;
    gchar *seven_day_reset_str;
    gchar *five_hour_reset_time;   /* Full reset time for tooltip */
    gchar *seven_day_reset_time;   /* Full reset time for tooltip */
    gdouble context_pct;
    gint64 context_tokens;         /* Actual token count */
    gint64 context_window_size;    /* Context window size */
    gchar *model_name;             /* Current model name */
    GDateTime *last_updated;       /* Last successful update time */

    /* Configuration */
    gint update_interval;
    gint yellow_threshold;
    gint orange_threshold;
    gint red_threshold;
    gchar *creds_file;

    /* Layout state */
    gboolean single_row;
    gint font_size;  /* in Pango units (1000 = 1pt) */

    /* Update timer */
    guint timeout_id;

    /* Credentials file monitor */
    GFileMonitor *creds_monitor;

    /* Error state */
    gboolean has_credentials_error;

    /* HTTP request cancellation */
    GCancellable *cancellable;

    /* Retry counter for 401 errors */
    gint auth_retry_count;
} ClaudeStatusPlugin;

/* Forward declarations */
static void claude_status_free(XfcePanelPlugin *plugin, ClaudeStatusPlugin *data);
static gboolean claude_status_update(ClaudeStatusPlugin *data);
static void claude_status_fetch_usage(ClaudeStatusPlugin *data);
static void claude_status_read_context(ClaudeStatusPlugin *data);
static void claude_status_save_config(ClaudeStatusPlugin *data);
static void claude_status_read_config(ClaudeStatusPlugin *data);
static void claude_status_configure(XfcePanelPlugin *plugin, ClaudeStatusPlugin *data);
static void claude_status_rebuild_ui(ClaudeStatusPlugin *data);
static void claude_status_size_changed(XfcePanelPlugin *plugin, gint size, ClaudeStatusPlugin *data);
static void claude_status_setup_creds_monitor(ClaudeStatusPlugin *data);
static gboolean load_credentials(ClaudeStatusPlugin *data);

/* Expand ~ to home directory in path */
static gchar* expand_path(const gchar *path) {
    if (path && path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        return g_build_filename(g_get_home_dir(), path + 2, NULL);
    }
    return g_strdup(path);
}

/* Generate a text progress bar */
static gchar* make_bar(gdouble pct, int width) {
    int filled = (int)((pct / 100.0) * width + 0.5);
    if (filled > width) filled = width;
    if (filled < 0) filled = 0;

    GString *bar = g_string_new("");
    for (int i = 0; i < filled; i++) {
        g_string_append(bar, "█");
    }
    for (int i = filled; i < width; i++) {
        g_string_append(bar, "░");
    }
    return g_string_free(bar, FALSE);
}

/* Get color based on percentage and thresholds */
static const gchar* get_color(const ClaudeStatusPlugin *data, gdouble pct) {
    if (pct < data->yellow_threshold) return "#5faf5f";  /* green */
    if (pct < data->orange_threshold) return "#d7af5f";  /* yellow */
    if (pct < data->red_threshold) return "#d78700";     /* orange */
    return "#d75f5f";                                     /* red */
}

/* Load CSS styling */
static void load_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const gchar *css =
        ".claude-status {"
        "  background-color: #1a1a1a;"
        "  border: 1px solid #444;"
        "  border-radius: 4px;"
        "}";

    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);
}

/* Create a label with monospace font */
static GtkWidget* create_label(ClaudeStatusPlugin *data, const gchar *text, const gchar *color, gboolean bold) {
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup;
    if (bold) {
        markup = g_strdup_printf(
            "<span font_family='monospace' font_size='%d' color='%s' weight='bold'>%s</span>",
            data->font_size, color, text);
    } else {
        markup = g_strdup_printf(
            "<span font_family='monospace' font_size='%d' color='%s'>%s</span>",
            data->font_size, color, text);
    }
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    return label;
}

/* Update a label's text and color */
static void update_label(ClaudeStatusPlugin *data, GtkWidget *label, const gchar *text, const gchar *color, gboolean bold) {
    gchar *markup;
    if (bold) {
        markup = g_strdup_printf(
            "<span font_family='monospace' font_size='%d' color='%s' weight='bold'>%s</span>",
            data->font_size, color, text);
    } else {
        markup = g_strdup_printf(
            "<span font_family='monospace' font_size='%d' color='%s'>%s</span>",
            data->font_size, color, text);
    }
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
}

/* Read credentials from Claude Code config */
static gboolean load_credentials(ClaudeStatusPlugin *data) {
    g_free(data->access_token);
    g_free(data->plan_name);
    data->access_token = NULL;
    data->plan_name = NULL;

    gchar *path = expand_path(data->creds_file ? data->creds_file : DEFAULT_CREDS_FILE);
    gchar *contents = NULL;
    gsize length;

    if (!g_file_get_contents(path, &contents, &length, NULL)) {
        g_free(path);
        return FALSE;
    }
    g_free(path);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, contents, length, NULL)) {
        g_free(contents);
        g_object_unref(parser);
        return FALSE;
    }
    g_free(contents);

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return FALSE;
    }

    JsonObject *obj = json_node_get_object(root);
    if (!json_object_has_member(obj, "claudeAiOauth")) {
        g_object_unref(parser);
        return FALSE;
    }

    JsonObject *oauth = json_object_get_object_member(obj, "claudeAiOauth");
    if (!oauth) {
        g_object_unref(parser);
        return FALSE;
    }

    const gchar *token = json_object_get_string_member(oauth, "accessToken");
    const gchar *sub_type = json_object_get_string_member(oauth, "subscriptionType");

    if (token) {
        data->access_token = g_strdup(token);
    }

    if (sub_type) {
        if (g_strstr_len(sub_type, -1, "max")) {
            data->plan_name = g_strdup("Max");
        } else if (g_strstr_len(sub_type, -1, "pro")) {
            data->plan_name = g_strdup("Pro");
        }
    }

    g_object_unref(parser);
    return data->access_token != NULL;
}

/* Callback when credentials file changes */
static void on_creds_file_changed(GFileMonitor *monitor, GFile *file, GFile *other_file,
                                   GFileMonitorEvent event_type, gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;

    if (event_type == G_FILE_MONITOR_EVENT_CHANGED ||
        event_type == G_FILE_MONITOR_EVENT_CREATED) {
        /* Reload credentials and fetch new data */
        if (load_credentials(data)) {
            data->has_credentials_error = FALSE;
            claude_status_fetch_usage(data);
        }
    }
}

/* Set up file monitor for credentials file */
static void claude_status_setup_creds_monitor(ClaudeStatusPlugin *data) {
    /* Cancel existing monitor if any */
    if (data->creds_monitor) {
        g_file_monitor_cancel(data->creds_monitor);
        g_clear_object(&data->creds_monitor);
    }

    gchar *path = expand_path(data->creds_file ? data->creds_file : DEFAULT_CREDS_FILE);
    GFile *file = g_file_new_for_path(path);
    g_free(path);

    GError *error = NULL;
    data->creds_monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, &error);
    g_object_unref(file);

    if (error) {
        g_error_free(error);
        return;
    }

    if (data->creds_monitor) {
        g_signal_connect(data->creds_monitor, "changed",
                         G_CALLBACK(on_creds_file_changed), data);
    }
}

/* Find the most recently modified transcript file */
static gchar* find_latest_transcript(void) {
    gchar *projects_dir = g_build_filename(g_get_home_dir(), ".claude", "projects", NULL);
    DIR *dir = opendir(projects_dir);
    if (!dir) {
        g_free(projects_dir);
        return NULL;
    }

    gchar *latest_path = NULL;
    time_t latest_mtime = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        gchar *project_path = g_build_filename(projects_dir, entry->d_name, NULL);
        DIR *project_dir = opendir(project_path);
        if (!project_dir) {
            g_free(project_path);
            continue;
        }

        struct dirent *file_entry;
        while ((file_entry = readdir(project_dir)) != NULL) {
            if (!g_str_has_suffix(file_entry->d_name, ".jsonl")) continue;

            gchar *file_path = g_build_filename(project_path, file_entry->d_name, NULL);
            struct stat st;
            if (stat(file_path, &st) == 0 && st.st_mtime > latest_mtime) {
                latest_mtime = st.st_mtime;
                g_free(latest_path);
                latest_path = file_path;
            } else {
                g_free(file_path);
            }
        }
        closedir(project_dir);
        g_free(project_path);
    }
    closedir(dir);
    g_free(projects_dir);

    return latest_path;
}

/* Get context window size for a model name */
static gint64 get_context_window_for_model(const gchar *model) {
    if (!model) return CONTEXT_WINDOW_DEFAULT;

    /* Sonnet 4 and 4.5 can have 1M context in beta for tier 4 orgs,
     * but consumer OAuth users get 200K. Default to 200K for safety.
     * This could be made configurable if users have 1M access. */
    return CONTEXT_WINDOW_DEFAULT;
}

/* Read context window usage from transcript */
static void claude_status_read_context(ClaudeStatusPlugin *data) {
    gchar *transcript_path = find_latest_transcript();
    if (!transcript_path) {
        data->context_pct = 0;
        return;
    }

    FILE *f = fopen(transcript_path, "r");
    g_free(transcript_path);
    if (!f) {
        data->context_pct = 0;
        return;
    }

    /* We want the LAST assistant message's input_tokens and model */
    gint64 last_input = 0;
    gint64 last_cache_creation = 0;
    gint64 last_cache_read = 0;
    gchar *last_model = NULL;

    char *line = NULL;
    size_t len = 0;

    while (getline(&line, &len, f) != -1) {
        JsonParser *parser = json_parser_new();
        if (!json_parser_load_from_data(parser, line, -1, NULL)) {
            g_object_unref(parser);
            continue;
        }

        JsonNode *root = json_parser_get_root(parser);
        if (!JSON_NODE_HOLDS_OBJECT(root)) {
            g_object_unref(parser);
            continue;
        }

        JsonObject *obj = json_node_get_object(root);
        const gchar *type = json_object_get_string_member(obj, "type");

        if (type && g_strcmp0(type, "assistant") == 0) {
            JsonObject *message = json_object_get_object_member(obj, "message");
            if (message) {
                /* Get the model name */
                const gchar *model = json_object_get_string_member(message, "model");
                if (model) {
                    g_free(last_model);
                    last_model = g_strdup(model);
                }

                JsonObject *usage = json_object_get_object_member(message, "usage");
                if (usage) {
                    /* Get the latest values - input_tokens + cache tokens = total context */
                    last_input = json_object_get_int_member(usage, "input_tokens");
                    last_cache_creation = json_object_get_int_member(usage, "cache_creation_input_tokens");
                    last_cache_read = json_object_get_int_member(usage, "cache_read_input_tokens");
                }
            }
        }

        g_object_unref(parser);
    }

    free(line);
    fclose(f);

    /* Total context = input + cache_creation + cache_read */
    gint64 total_context = last_input + last_cache_creation + last_cache_read;
    gint64 context_window = get_context_window_for_model(last_model);

    /* Store values for tooltip */
    data->context_tokens = total_context;
    data->context_window_size = context_window;
    g_free(data->model_name);
    data->model_name = last_model;  /* Transfer ownership */

    data->context_pct = (gdouble)total_context / context_window * 100.0;
    if (data->context_pct > 100) data->context_pct = 100;
}

/* HTTP response callback */
static void on_usage_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;
    GError *error = NULL;

    /* Get the message to check status code */
    SoupMessage *msg = soup_session_get_async_result_message(SOUP_SESSION(source), result);
    GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);

    if (error) {
        /* Check if cancelled (plugin being freed) */
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_error_free(error);
            return;
        }
        g_error_free(error);
        return;
    }

    /* Check for 401 Unauthorized - token may have been refreshed */
    if (msg && soup_message_get_status(msg) == 401) {
        g_bytes_unref(bytes);

        /* Limit retries to prevent infinite loop */
        if (data->auth_retry_count >= 2) {
            data->auth_retry_count = 0;
            data->has_credentials_error = TRUE;
            claude_status_update(data);
            return;
        }

        data->auth_retry_count++;

        /* Clear cached token and try to reload credentials */
        g_free(data->access_token);
        data->access_token = NULL;
        if (load_credentials(data)) {
            /* Retry the request with new credentials */
            claude_status_fetch_usage(data);
        } else {
            data->has_credentials_error = TRUE;
            claude_status_update(data);
        }
        return;
    }

    /* Reset retry counter on successful response */
    data->auth_retry_count = 0;

    gsize size;
    const gchar *body = g_bytes_get_data(bytes, &size);

    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, body, size, NULL)) {
        /* Clear any previous error state on successful parse */
        data->has_credentials_error = FALSE;
        JsonNode *root = json_parser_get_root(parser);

        /* Validate root is an object */
        if (!JSON_NODE_HOLDS_OBJECT(root)) {
            g_object_unref(parser);
            g_bytes_unref(bytes);
            return;
        }

        JsonObject *obj = json_node_get_object(root);

        JsonObject *five_hour = json_object_get_object_member(obj, "five_hour");
        JsonObject *seven_day = json_object_get_object_member(obj, "seven_day");

        if (five_hour) {
            data->five_hour_pct_val = json_object_get_double_member(five_hour, "utilization");

            const gchar *reset = json_object_get_string_member(five_hour, "resets_at");
            if (reset) {
                GDateTime *reset_dt = g_date_time_new_from_iso8601(reset, NULL);
                if (reset_dt) {
                    GDateTime *now = g_date_time_new_now_local();
                    GDateTime *reset_local = g_date_time_to_local(reset_dt);
                    GTimeSpan diff = g_date_time_difference(reset_dt, now);
                    gint hours = diff / G_TIME_SPAN_HOUR;
                    gint mins = (diff % G_TIME_SPAN_HOUR) / G_TIME_SPAN_MINUTE;

                    g_free(data->five_hour_reset_str);
                    if (hours > 0) {
                        data->five_hour_reset_str = g_strdup_printf("(%dh %dm)", hours, mins);
                    } else {
                        data->five_hour_reset_str = g_strdup_printf("(%dm)", mins);
                    }

                    /* Store full reset time for tooltip */
                    g_free(data->five_hour_reset_time);
                    data->five_hour_reset_time = g_date_time_format(reset_local, "%l:%M %p");

                    g_date_time_unref(reset_local);
                    g_date_time_unref(reset_dt);
                    g_date_time_unref(now);
                }
            }
        }

        if (seven_day) {
            data->seven_day_pct_val = json_object_get_double_member(seven_day, "utilization");

            const gchar *reset = json_object_get_string_member(seven_day, "resets_at");
            if (reset) {
                GDateTime *reset_dt = g_date_time_new_from_iso8601(reset, NULL);
                if (reset_dt) {
                    GDateTime *now = g_date_time_new_now_local();
                    GDateTime *reset_local = g_date_time_to_local(reset_dt);
                    GTimeSpan diff = g_date_time_difference(reset_dt, now);
                    gint days = diff / G_TIME_SPAN_DAY;
                    gint hours = (diff % G_TIME_SPAN_DAY) / G_TIME_SPAN_HOUR;

                    g_free(data->seven_day_reset_str);
                    if (days > 0) {
                        data->seven_day_reset_str = g_strdup_printf("(%dd %dh)", days, hours);
                    } else {
                        data->seven_day_reset_str = g_strdup_printf("(%dh)", hours);
                    }

                    /* Store full reset time for tooltip */
                    g_free(data->seven_day_reset_time);
                    data->seven_day_reset_time = g_date_time_format(reset_local, "%a %l:%M %p");

                    g_date_time_unref(reset_local);
                    g_date_time_unref(reset_dt);
                    g_date_time_unref(now);
                }
            }
        }

        /* Update last updated time */
        if (data->last_updated) {
            g_date_time_unref(data->last_updated);
        }
        data->last_updated = g_date_time_new_now_local();
    }

    g_object_unref(parser);
    g_bytes_unref(bytes);

    /* Also update context from transcript */
    claude_status_read_context(data);

    /* Update UI */
    claude_status_update(data);
}

/* Fetch usage from API */
static void claude_status_fetch_usage(ClaudeStatusPlugin *data) {
    if (!data->access_token) {
        if (!load_credentials(data)) {
            data->has_credentials_error = TRUE;
            claude_status_update(data);
            return;
        }
        data->has_credentials_error = FALSE;
    }

    SoupMessage *msg = soup_message_new("GET", "https://api.anthropic.com/api/oauth/usage");
    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);

    gchar *auth = g_strdup_printf("Bearer %s", data->access_token);
    soup_message_headers_append(headers, "Authorization", auth);
    soup_message_headers_append(headers, "anthropic-beta", "oauth-2025-04-20");
    soup_message_headers_append(headers, "User-Agent", "xfce-claude-status/0.1");
    g_free(auth);

    soup_session_send_and_read_async(data->session, msg, G_PRIORITY_DEFAULT,
                                      data->cancellable, on_usage_response, data);
    g_object_unref(msg);
}

/* Update the UI with current data */
static gboolean claude_status_update(ClaudeStatusPlugin *data) {
    if (!data->plan_label) return TRUE;  /* UI not built yet */

    /* Show error state if no credentials */
    if (data->has_credentials_error) {
        update_label(data, data->plan_label, "No creds", "#d75f5f", TRUE);
        update_label(data, data->five_hour_bar, "", "#888", FALSE);
        update_label(data, data->five_hour_pct, "", "#888", FALSE);
        update_label(data, data->five_hour_reset, "", "#666", FALSE);
        update_label(data, data->ctx_label, "Run:", "#888", FALSE);
        update_label(data, data->seven_day_bar, "claude", "#d4a574", FALSE);
        update_label(data, data->seven_day_pct, "login", "#d4a574", FALSE);
        update_label(data, data->seven_day_reset, "", "#666", FALSE);
        return TRUE;
    }

    /* Row 1: Plan, 5h */
    update_label(data, data->plan_label, data->plan_name ? data->plan_name : "—", "#d4a574", TRUE);

    gchar *bar5 = make_bar(data->five_hour_pct_val, 8);
    const gchar *color5 = get_color(data, data->five_hour_pct_val);
    update_label(data, data->five_hour_bar, bar5, color5, FALSE);
    g_free(bar5);

    gchar *pct5 = g_strdup_printf("%3.0f%%", data->five_hour_pct_val);
    update_label(data, data->five_hour_pct, pct5, color5, FALSE);
    g_free(pct5);

    update_label(data, data->five_hour_reset, data->five_hour_reset_str ? data->five_hour_reset_str : "", "#666", FALSE);

    /* Row 2 / continued: Context, 7d */
    gchar *ctx = g_strdup_printf("Ctx:%3.0f%%", data->context_pct);
    const gchar *color_ctx = get_color(data, data->context_pct);
    update_label(data, data->ctx_label, ctx, color_ctx, FALSE);
    g_free(ctx);

    gchar *bar7 = make_bar(data->seven_day_pct_val, 8);
    const gchar *color7 = get_color(data, data->seven_day_pct_val);
    update_label(data, data->seven_day_bar, bar7, color7, FALSE);
    g_free(bar7);

    gchar *pct7 = g_strdup_printf("%3.0f%%", data->seven_day_pct_val);
    update_label(data, data->seven_day_pct, pct7, color7, FALSE);
    g_free(pct7);

    update_label(data, data->seven_day_reset, data->seven_day_reset_str ? data->seven_day_reset_str : "", "#666", FALSE);

    /* Update tooltip */
    GString *tooltip = g_string_new("");

    /* Plan name */
    g_string_append_printf(tooltip, "<b>Claude %s</b>\n",
                           data->plan_name ? data->plan_name : "—");
    g_string_append(tooltip, "─────────────────\n");

    /* 5-hour usage */
    g_string_append_printf(tooltip, "5-hour:  %.1f%%", data->five_hour_pct_val);
    if (data->five_hour_reset_time) {
        g_string_append_printf(tooltip, " (resets%s)", data->five_hour_reset_time);
    }
    g_string_append(tooltip, "\n");

    /* 7-day usage */
    g_string_append_printf(tooltip, "7-day:   %.1f%%", data->seven_day_pct_val);
    if (data->seven_day_reset_time) {
        g_string_append_printf(tooltip, " (resets %s)", data->seven_day_reset_time);
    }
    g_string_append(tooltip, "\n");

    /* Context usage */
    if (data->context_window_size > 0) {
        /* Format with thousands separators using GLib */
        gchar *tokens_str = g_strdup_printf("%ld", (long)data->context_tokens);
        gchar *window_str = g_strdup_printf("%ld", (long)data->context_window_size);
        g_string_append_printf(tooltip, "Context: %s / %s tokens (%.0f%%)\n",
                               tokens_str, window_str, data->context_pct);
        g_free(tokens_str);
        g_free(window_str);
    }

    /* Model name */
    if (data->model_name) {
        g_string_append_printf(tooltip, "\nModel: %s", data->model_name);
    }

    /* Last updated time */
    if (data->last_updated) {
        gchar *updated_str = g_date_time_format(data->last_updated, "%l:%M:%S %p");
        g_string_append_printf(tooltip, "\nUpdated:%s", updated_str);
        g_free(updated_str);
    }

    gtk_widget_set_tooltip_markup(data->box, tooltip->str);
    g_string_free(tooltip, TRUE);

    return TRUE;
}

/* Timer callback - fetch new data periodically */
static gboolean on_timeout(gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;
    claude_status_fetch_usage(data);
    return TRUE;
}

/* Restart the timer with new interval */
static void claude_status_restart_timer(ClaudeStatusPlugin *data) {
    if (data->timeout_id > 0) {
        g_source_remove(data->timeout_id);
    }
    data->timeout_id = g_timeout_add_seconds(data->update_interval, on_timeout, data);
}

/* Read configuration from rc file */
static void claude_status_read_config(ClaudeStatusPlugin *data) {
    gchar *file = xfce_panel_plugin_save_location(data->plugin, FALSE);

    if (file) {
        XfceRc *rc = xfce_rc_simple_open(file, TRUE);
        g_free(file);

        if (rc) {
            data->update_interval = xfce_rc_read_int_entry(rc, "update_interval", DEFAULT_UPDATE_INTERVAL);
            data->yellow_threshold = xfce_rc_read_int_entry(rc, "yellow_threshold", DEFAULT_YELLOW_THRESHOLD);
            data->orange_threshold = xfce_rc_read_int_entry(rc, "orange_threshold", DEFAULT_ORANGE_THRESHOLD);
            data->red_threshold = xfce_rc_read_int_entry(rc, "red_threshold", DEFAULT_RED_THRESHOLD);
            const gchar *creds = xfce_rc_read_entry(rc, "creds_file", DEFAULT_CREDS_FILE);
            g_free(data->creds_file);
            data->creds_file = g_strdup(creds);
            xfce_rc_close(rc);
            return;
        }
    }

    /* Defaults */
    data->update_interval = DEFAULT_UPDATE_INTERVAL;
    data->yellow_threshold = DEFAULT_YELLOW_THRESHOLD;
    data->orange_threshold = DEFAULT_ORANGE_THRESHOLD;
    data->red_threshold = DEFAULT_RED_THRESHOLD;
    g_free(data->creds_file);
    data->creds_file = g_strdup(DEFAULT_CREDS_FILE);
}

/* Save configuration to rc file */
static void claude_status_save_config(ClaudeStatusPlugin *data) {
    gchar *file = xfce_panel_plugin_save_location(data->plugin, TRUE);

    if (file) {
        XfceRc *rc = xfce_rc_simple_open(file, FALSE);
        g_free(file);

        if (rc) {
            xfce_rc_write_int_entry(rc, "update_interval", data->update_interval);
            xfce_rc_write_int_entry(rc, "yellow_threshold", data->yellow_threshold);
            xfce_rc_write_int_entry(rc, "orange_threshold", data->orange_threshold);
            xfce_rc_write_int_entry(rc, "red_threshold", data->red_threshold);
            xfce_rc_write_entry(rc, "creds_file", data->creds_file ? data->creds_file : DEFAULT_CREDS_FILE);
            xfce_rc_close(rc);
        }
    }
}

/* Build the plugin UI based on current layout settings */
static void claude_status_rebuild_ui(ClaudeStatusPlugin *data) {
    /* Remove old grid if exists */
    if (data->grid) {
        gtk_widget_destroy(data->grid);
        data->grid = NULL;
        data->plan_label = NULL;
        data->five_hour_lbl = NULL;
        data->five_hour_bar = NULL;
        data->five_hour_pct = NULL;
        data->five_hour_reset = NULL;
        data->ctx_label = NULL;
        data->seven_day_lbl = NULL;
        data->seven_day_bar = NULL;
        data->seven_day_pct = NULL;
        data->seven_day_reset = NULL;
    }

    /* Grid for alignment */
    data->grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(data->grid), data->single_row ? 4 : 6);
    gtk_grid_set_row_spacing(GTK_GRID(data->grid), 2);

    gint margin = data->single_row ? 4 : 8;
    gtk_widget_set_margin_start(data->grid, margin);
    gtk_widget_set_margin_end(data->grid, margin);
    gtk_widget_set_margin_top(data->grid, data->single_row ? 2 : 6);
    gtk_widget_set_margin_bottom(data->grid, data->single_row ? 2 : 6);
    gtk_container_add(GTK_CONTAINER(data->box), data->grid);

    if (data->single_row) {
        /* Single row layout: Plan | 5h: bar pct | 7d: bar pct | Ctx:pct */
        data->plan_label = create_label(data, "—", "#d4a574", TRUE);
        gtk_grid_attach(GTK_GRID(data->grid), data->plan_label, 0, 0, 1, 1);

        data->five_hour_lbl = create_label(data, "5h:", "#888", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->five_hour_lbl, 1, 0, 1, 1);

        data->five_hour_bar = create_label(data, "░░░░░░░░", "#5faf5f", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->five_hour_bar, 2, 0, 1, 1);

        data->five_hour_pct = create_label(data, "  0%", "#5faf5f", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->five_hour_pct, 3, 0, 1, 1);

        /* Skip reset times in single row to save space */
        data->five_hour_reset = create_label(data, "", "#666", FALSE);
        gtk_widget_set_visible(data->five_hour_reset, FALSE);

        data->seven_day_lbl = create_label(data, "7d:", "#888", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->seven_day_lbl, 4, 0, 1, 1);

        data->seven_day_bar = create_label(data, "░░░░░░░░", "#5faf5f", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->seven_day_bar, 5, 0, 1, 1);

        data->seven_day_pct = create_label(data, "  0%", "#5faf5f", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->seven_day_pct, 6, 0, 1, 1);

        data->seven_day_reset = create_label(data, "", "#666", FALSE);
        gtk_widget_set_visible(data->seven_day_reset, FALSE);

        data->ctx_label = create_label(data, "Ctx:  0%", "#5faf5f", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->ctx_label, 7, 0, 1, 1);
    } else {
        /* Two row layout */
        /* Row 1: Plan | 5h: | bar | pct | reset */
        data->plan_label = create_label(data, "—", "#d4a574", TRUE);
        gtk_grid_attach(GTK_GRID(data->grid), data->plan_label, 0, 0, 1, 1);

        data->five_hour_lbl = create_label(data, "5h:", "#888", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->five_hour_lbl, 1, 0, 1, 1);

        data->five_hour_bar = create_label(data, "░░░░░░░░", "#5faf5f", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->five_hour_bar, 2, 0, 1, 1);

        data->five_hour_pct = create_label(data, "  0%", "#5faf5f", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->five_hour_pct, 3, 0, 1, 1);

        data->five_hour_reset = create_label(data, "", "#666", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->five_hour_reset, 4, 0, 1, 1);

        /* Row 2: Ctx | 7d: | bar | pct | reset */
        data->ctx_label = create_label(data, "Ctx:  0%", "#5faf5f", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->ctx_label, 0, 1, 1, 1);

        data->seven_day_lbl = create_label(data, "7d:", "#888", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->seven_day_lbl, 1, 1, 1, 1);

        data->seven_day_bar = create_label(data, "░░░░░░░░", "#5faf5f", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->seven_day_bar, 2, 1, 1, 1);

        data->seven_day_pct = create_label(data, "  0%", "#5faf5f", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->seven_day_pct, 3, 1, 1, 1);

        data->seven_day_reset = create_label(data, "", "#666", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->seven_day_reset, 4, 1, 1, 1);
    }

    gtk_widget_show_all(data->grid);

    /* Update with current values */
    claude_status_update(data);
}

/* Handle panel size changes */
static void claude_status_size_changed(XfcePanelPlugin *plugin, gint size, ClaudeStatusPlugin *data) {
    gboolean new_single_row;
    gint new_font_size;

    /* Determine layout based on panel size */
    if (size < 30) {
        new_single_row = TRUE;
        new_font_size = 6000;  /* 6pt */
    } else if (size < 40) {
        new_single_row = TRUE;
        new_font_size = 7000;  /* 7pt */
    } else if (size < 50) {
        new_single_row = FALSE;
        new_font_size = 8000;  /* 8pt */
    } else {
        new_single_row = FALSE;
        new_font_size = 9000;  /* 9pt */
    }

    /* Only rebuild if layout changed */
    if (new_single_row != data->single_row || new_font_size != data->font_size) {
        data->single_row = new_single_row;
        data->font_size = new_font_size;
        claude_status_rebuild_ui(data);
    }
}

/* Callback for update interval spin button */
static void on_update_interval_changed(GtkSpinButton *btn, gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;
    data->update_interval = gtk_spin_button_get_value_as_int(btn);
}

/* Callback for yellow threshold spin button */
static void on_yellow_threshold_changed(GtkSpinButton *btn, gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;
    data->yellow_threshold = gtk_spin_button_get_value_as_int(btn);
}

/* Callback for orange threshold spin button */
static void on_orange_threshold_changed(GtkSpinButton *btn, gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;
    data->orange_threshold = gtk_spin_button_get_value_as_int(btn);
}

/* Callback for red threshold spin button */
static void on_red_threshold_changed(GtkSpinButton *btn, gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;
    data->red_threshold = gtk_spin_button_get_value_as_int(btn);
}

/* Validate credentials file - check it exists and has required fields */
static gboolean validate_creds_file(const gchar *path, gchar **error_msg) {
    gchar *expanded = NULL;
    if (path && path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        expanded = g_build_filename(g_get_home_dir(), path + 2, NULL);
    } else {
        expanded = g_strdup(path);
    }

    if (!expanded || !g_file_test(expanded, G_FILE_TEST_EXISTS)) {
        if (error_msg) *error_msg = g_strdup("File does not exist");
        g_free(expanded);
        return FALSE;
    }

    gchar *contents = NULL;
    gsize length;
    if (!g_file_get_contents(expanded, &contents, &length, NULL)) {
        if (error_msg) *error_msg = g_strdup("Cannot read file");
        g_free(expanded);
        return FALSE;
    }
    g_free(expanded);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, contents, length, NULL)) {
        if (error_msg) *error_msg = g_strdup("Invalid JSON");
        g_free(contents);
        g_object_unref(parser);
        return FALSE;
    }
    g_free(contents);

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = json_node_get_object(root);
    JsonObject *oauth = json_object_get_object_member(obj, "claudeAiOauth");

    if (!oauth) {
        if (error_msg) *error_msg = g_strdup("Missing claudeAiOauth section");
        g_object_unref(parser);
        return FALSE;
    }

    const gchar *token = json_object_get_string_member(oauth, "accessToken");
    if (!token || strlen(token) == 0) {
        if (error_msg) *error_msg = g_strdup("Missing accessToken");
        g_object_unref(parser);
        return FALSE;
    }

    g_object_unref(parser);
    return TRUE;
}

/* Callback for file chooser button */
static void on_creds_file_set(GtkFileChooserButton *button, gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;
    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(button));

    if (filename) {
        /* Convert to ~ format if in home directory */
        const gchar *home = g_get_home_dir();
        if (g_str_has_prefix(filename, home)) {
            gchar *relative = g_strdup_printf("~%s", filename + strlen(home));
            g_free(data->creds_file);
            data->creds_file = relative;
        } else {
            g_free(data->creds_file);
            data->creds_file = g_strdup(filename);
        }
        g_free(filename);
    }
}

/* Configuration dialog response */
static void on_configure_response(GtkDialog *dialog, gint response, ClaudeStatusPlugin *data) {
    if (response == GTK_RESPONSE_OK || response == GTK_RESPONSE_APPLY) {
        /* Validate credentials file before saving */
        gchar *error_msg = NULL;
        if (!validate_creds_file(data->creds_file, &error_msg)) {
            GtkWidget *msg_dialog = gtk_message_dialog_new(
                GTK_WINDOW(dialog),
                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_WARNING,
                GTK_BUTTONS_OK,
                "Credentials file validation failed:\n%s\n\nSettings will be saved, but the plugin may not work correctly.",
                error_msg ? error_msg : "Unknown error");
            gtk_dialog_run(GTK_DIALOG(msg_dialog));
            gtk_widget_destroy(msg_dialog);
            g_free(error_msg);
        }

        claude_status_save_config(data);
        claude_status_restart_timer(data);
        /* Re-setup file monitor in case creds file changed */
        claude_status_setup_creds_monitor(data);
        /* Clear cached token to force reload from new file */
        g_free(data->access_token);
        data->access_token = NULL;
        claude_status_fetch_usage(data);
    }

    if (response != GTK_RESPONSE_APPLY) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        xfce_panel_plugin_unblock_menu(data->plugin);
    }
}

/* Configuration dialog */
static void claude_status_configure(XfcePanelPlugin *plugin, ClaudeStatusPlugin *data) {
    GtkWidget *dialog;
    GtkWidget *content;
    GtkWidget *grid;
    GtkWidget *label;
    GtkWidget *spin;
    GtkWidget *file_chooser;

    xfce_panel_plugin_block_menu(plugin);

    dialog = xfce_titled_dialog_new_with_mixed_buttons(
        "Claude Status Settings",
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(plugin))),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        "window-close", "_Close", GTK_RESPONSE_OK,
        NULL);

    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
    gtk_window_set_icon_name(GTK_WINDOW(dialog), "preferences-system");

    content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_container_add(GTK_CONTAINER(content), grid);

    /* Update interval */
    label = gtk_label_new("Update interval (seconds):");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);

    spin = gtk_spin_button_new_with_range(5, 300, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), data->update_interval);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_update_interval_changed), data);
    gtk_grid_attach(GTK_GRID(grid), spin, 1, 0, 1, 1);

    /* Color thresholds header */
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), "<b>Color thresholds (%)</b>");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_margin_top(label, 12);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 2, 1);

    /* Yellow threshold */
    label = gtk_label_new("Yellow (warning):");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 2, 1, 1);

    spin = gtk_spin_button_new_with_range(1, 99, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), data->yellow_threshold);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_yellow_threshold_changed), data);
    gtk_grid_attach(GTK_GRID(grid), spin, 1, 2, 1, 1);

    /* Orange threshold */
    label = gtk_label_new("Orange (caution):");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 3, 1, 1);

    spin = gtk_spin_button_new_with_range(1, 99, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), data->orange_threshold);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_orange_threshold_changed), data);
    gtk_grid_attach(GTK_GRID(grid), spin, 1, 3, 1, 1);

    /* Red threshold */
    label = gtk_label_new("Red (critical):");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 4, 1, 1);

    spin = gtk_spin_button_new_with_range(1, 99, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), data->red_threshold);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_red_threshold_changed), data);
    gtk_grid_attach(GTK_GRID(grid), spin, 1, 4, 1, 1);

    /* Credentials file */
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), "<b>Credentials</b>");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_margin_top(label, 12);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 5, 2, 1);

    label = gtk_label_new("Credentials file:");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 6, 1, 1);

    file_chooser = gtk_file_chooser_button_new("Select Credentials File", GTK_FILE_CHOOSER_ACTION_OPEN);

    /* Set current file if it exists */
    gchar *current_path = expand_path(data->creds_file ? data->creds_file : DEFAULT_CREDS_FILE);
    if (g_file_test(current_path, G_FILE_TEST_EXISTS)) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(file_chooser), current_path);
    } else {
        /* Set to home/.claude directory as starting point */
        gchar *claude_dir = g_build_filename(g_get_home_dir(), ".claude", NULL);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(file_chooser), claude_dir);
        g_free(claude_dir);
    }
    g_free(current_path);

    /* Add JSON file filter */
    GtkFileFilter *json_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(json_filter, "JSON files (*.json)");
    gtk_file_filter_add_pattern(json_filter, "*.json");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file_chooser), json_filter);

    GtkFileFilter *all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All files");
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file_chooser), all_filter);

    /* Show hidden files by default (credentials file starts with .) */
    gtk_file_chooser_set_show_hidden(GTK_FILE_CHOOSER(file_chooser), TRUE);

    g_signal_connect(file_chooser, "file-set", G_CALLBACK(on_creds_file_set), data);
    gtk_grid_attach(GTK_GRID(grid), file_chooser, 1, 6, 1, 1);

    /* Info label */
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label),
        "<small>Layout automatically adjusts to panel size.\n"
        "Narrow panels use single-row compact mode.</small>");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_margin_top(label, 12);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 7, 2, 1);

    g_signal_connect(dialog, "response", G_CALLBACK(on_configure_response), data);

    gtk_widget_show_all(dialog);
}

/* Build the plugin UI */
static void claude_status_construct(XfcePanelPlugin *plugin) {
    ClaudeStatusPlugin *data = g_new0(ClaudeStatusPlugin, 1);
    data->plugin = plugin;
    data->session = soup_session_new();
    data->cancellable = g_cancellable_new();
    data->five_hour_reset_str = g_strdup("");
    data->seven_day_reset_str = g_strdup("");

    /* Load configuration */
    claude_status_read_config(data);

    /* Initial layout settings */
    data->single_row = FALSE;
    data->font_size = 9000;

    load_css();

    /* Main container */
    data->box = gtk_event_box_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(data->box), "claude-status");
    gtk_widget_set_margin_start(data->box, 4);
    gtk_widget_set_margin_end(data->box, 4);
    gtk_widget_set_margin_top(data->box, 2);
    gtk_widget_set_margin_bottom(data->box, 2);
    gtk_container_add(GTK_CONTAINER(plugin), data->box);

    /* Build initial UI */
    claude_status_rebuild_ui(data);

    gtk_widget_show_all(data->box);

    /* Connect signals */
    g_signal_connect(plugin, "free-data", G_CALLBACK(claude_status_free), data);
    g_signal_connect(plugin, "size-changed", G_CALLBACK(claude_status_size_changed), data);
    g_signal_connect(plugin, "configure-plugin", G_CALLBACK(claude_status_configure), data);
    g_signal_connect(plugin, "save", G_CALLBACK(claude_status_save_config), data);

    /* Show configure in right-click menu */
    xfce_panel_plugin_menu_show_configure(plugin);

    /* Set up file monitor for credentials changes */
    claude_status_setup_creds_monitor(data);

    /* Initial fetch */
    claude_status_fetch_usage(data);

    /* Start timer */
    claude_status_restart_timer(data);
}

/* Cleanup */
static void claude_status_free(XfcePanelPlugin *plugin, ClaudeStatusPlugin *data) {
    if (data->timeout_id > 0) {
        g_source_remove(data->timeout_id);
    }

    /* Cancel any in-flight HTTP requests */
    if (data->cancellable) {
        g_cancellable_cancel(data->cancellable);
        g_clear_object(&data->cancellable);
    }

    if (data->creds_monitor) {
        g_file_monitor_cancel(data->creds_monitor);
        g_clear_object(&data->creds_monitor);
    }

    g_clear_object(&data->session);
    g_free(data->access_token);
    g_free(data->plan_name);
    g_free(data->five_hour_reset_str);
    g_free(data->seven_day_reset_str);
    g_free(data->five_hour_reset_time);
    g_free(data->seven_day_reset_time);
    g_free(data->model_name);
    g_free(data->creds_file);
    if (data->last_updated) {
        g_date_time_unref(data->last_updated);
    }
    g_free(data);
}

/* Register the plugin */
XFCE_PANEL_PLUGIN_REGISTER(claude_status_construct)
