/*
 * xfce4-claude-status-plugin
 * XFCE Panel Plugin showing Claude Max/Pro rate limit usage
 *
 * Copyright (c) 2026 James Curbo
 * SPDX-License-Identifier: MIT
 */

#include <libxfce4panel/libxfce4panel.h>
#include <libxfce4ui/libxfce4ui.h>
#include <time.h>

#include "claude_status_core.h"

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

    /* Rust core handle */
    struct ClaudeStatusCore *core;

    /* Cached display data */
    gchar *plan_name;
    gdouble five_hour_pct_val;
    gdouble seven_day_pct_val;
    gchar *five_hour_reset_str;
    gchar *seven_day_reset_str;
    gchar *five_hour_reset_time;
    gchar *seven_day_reset_time;
    gdouble context_pct;
    gint64 context_tokens;
    gint64 context_window_size;
    gchar *model_name;
    GDateTime *last_updated;

    /* Configuration */
    gint update_interval;
    gint yellow_threshold;
    gint orange_threshold;
    gint red_threshold;
    gchar *creds_file;

    /* Layout state */
    gboolean single_row;
    gint font_size;

    /* Update timer */
    guint timeout_id;

    /* Error state */
    gboolean has_credentials_error;

    /* Retry counter for auth errors */
    gint auth_retry_count;
} ClaudeStatusPlugin;

/* Forward declarations */
static void claude_status_free(XfcePanelPlugin *plugin, ClaudeStatusPlugin *data);
static gboolean claude_status_update(ClaudeStatusPlugin *data);
static void claude_status_fetch_usage(ClaudeStatusPlugin *data);
static void claude_status_save_config(ClaudeStatusPlugin *data);
static void claude_status_read_config(ClaudeStatusPlugin *data);
static void claude_status_configure(XfcePanelPlugin *plugin, ClaudeStatusPlugin *data);
static void claude_status_rebuild_ui(ClaudeStatusPlugin *data);
static void claude_status_size_changed(XfcePanelPlugin *plugin, gint size, ClaudeStatusPlugin *data);

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

/* Get color based on percentage - uses Rust core */
static const gchar* get_color(ClaudeStatusPlugin *data, gdouble pct) {
    return claude_status_core_get_color(data->core, pct);
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

/* Format reset time string from Unix timestamp */
static gchar* format_five_hour_reset(gint64 reset_ts, gchar **full_time_out) {
    GDateTime *reset_dt = g_date_time_new_from_unix_utc(reset_ts);
    if (!reset_dt) return g_strdup("");

    GDateTime *now = g_date_time_new_now_utc();
    GDateTime *reset_local = g_date_time_to_local(reset_dt);
    GTimeSpan diff = g_date_time_difference(reset_dt, now);
    gint hours = diff / G_TIME_SPAN_HOUR;
    gint mins = (diff % G_TIME_SPAN_HOUR) / G_TIME_SPAN_MINUTE;

    gchar *result;
    if (hours > 0) {
        result = g_strdup_printf("(%dh %dm)", hours, mins);
    } else {
        result = g_strdup_printf("(%dm)", mins);
    }

    if (full_time_out) {
        *full_time_out = g_date_time_format(reset_local, "%l:%M %p");
    }

    g_date_time_unref(reset_local);
    g_date_time_unref(reset_dt);
    g_date_time_unref(now);

    return result;
}

static gchar* format_seven_day_reset(gint64 reset_ts, gchar **full_time_out) {
    GDateTime *reset_dt = g_date_time_new_from_unix_utc(reset_ts);
    if (!reset_dt) return g_strdup("");

    GDateTime *now = g_date_time_new_now_utc();
    GDateTime *reset_local = g_date_time_to_local(reset_dt);
    GTimeSpan diff = g_date_time_difference(reset_dt, now);
    gint days = diff / G_TIME_SPAN_DAY;
    gint hours = (diff % G_TIME_SPAN_DAY) / G_TIME_SPAN_HOUR;

    gchar *result;
    if (days > 0) {
        result = g_strdup_printf("(%dd %dh)", days, hours);
    } else {
        result = g_strdup_printf("(%dh)", hours);
    }

    if (full_time_out) {
        *full_time_out = g_date_time_format(reset_local, "%a %l:%M %p");
    }

    g_date_time_unref(reset_local);
    g_date_time_unref(reset_dt);
    g_date_time_unref(now);

    return result;
}

/* Fetch usage from Rust core (runs in thread pool) */
static void fetch_usage_thread(GTask *task, gpointer source_object,
                                gpointer task_data, GCancellable *cancellable) {
    ClaudeStatusPlugin *data = task_data;

    /* Load credentials if needed */
    enum CResultCode cred_result = claude_status_core_load_credentials(
        data->core, data->creds_file);

    if (cred_result != Ok) {
        g_task_return_int(task, cred_result);
        return;
    }

    /* Fetch usage */
    enum CResultCode usage_result = claude_status_core_fetch_usage(data->core);
    if (usage_result != Ok) {
        g_task_return_int(task, usage_result);
        return;
    }

    /* Read context */
    claude_status_core_read_context(data->core);

    g_task_return_int(task, Ok);
}

static void fetch_usage_done(GObject *source_object, GAsyncResult *result, gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;
    GTask *task = G_TASK(result);

    enum CResultCode code = g_task_propagate_int(task, NULL);

    if (code == AuthError) {
        /* Handle auth error with retry */
        if (data->auth_retry_count >= 2) {
            data->auth_retry_count = 0;
            data->has_credentials_error = TRUE;
            claude_status_update(data);
            return;
        }
        data->auth_retry_count++;
        /* Retry */
        claude_status_fetch_usage(data);
        return;
    }

    data->auth_retry_count = 0;

    if (code == NoCredentials || code == InvalidCredentials) {
        data->has_credentials_error = TRUE;
        claude_status_update(data);
        return;
    }

    data->has_credentials_error = FALSE;

    /* Get usage data from core */
    struct CUsageData usage = claude_status_core_get_usage(data->core);
    if (usage.valid) {
        data->five_hour_pct_val = usage.five_hour_pct;
        data->seven_day_pct_val = usage.seven_day_pct;

        g_free(data->five_hour_reset_str);
        g_free(data->five_hour_reset_time);
        data->five_hour_reset_str = format_five_hour_reset(
            usage.five_hour_reset_ts, &data->five_hour_reset_time);

        g_free(data->seven_day_reset_str);
        g_free(data->seven_day_reset_time);
        data->seven_day_reset_str = format_seven_day_reset(
            usage.seven_day_reset_ts, &data->seven_day_reset_time);
    }

    /* Get context info from core */
    struct CContextInfo ctx = claude_status_core_get_context(data->core);
    if (ctx.valid) {
        data->context_pct = ctx.context_pct;
        data->context_tokens = ctx.context_tokens;
        data->context_window_size = ctx.context_window_size;

        g_free(data->model_name);
        data->model_name = ctx.model_name ? g_strdup(ctx.model_name) : NULL;
    }

    /* Get credentials info for plan name */
    struct CCredentialsInfo creds = claude_status_core_get_credentials_info(data->core);
    if (creds.valid) {
        g_free(data->plan_name);
        data->plan_name = creds.plan_name ? g_strdup(creds.plan_name) : NULL;
    }

    /* Update last updated time */
    if (data->last_updated) {
        g_date_time_unref(data->last_updated);
    }
    data->last_updated = g_date_time_new_now_local();

    /* Update UI */
    claude_status_update(data);
}

/* Fetch usage from API */
static void claude_status_fetch_usage(ClaudeStatusPlugin *data) {
    GTask *task = g_task_new(NULL, NULL, fetch_usage_done, data);
    g_task_set_task_data(task, data, NULL);
    g_task_run_in_thread(task, fetch_usage_thread);
    g_object_unref(task);
}

/* Update the UI with current data */
static gboolean claude_status_update(ClaudeStatusPlugin *data) {
    if (!data->plan_label) return TRUE;

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

    g_string_append_printf(tooltip, "<b>Claude %s</b>\n",
                           data->plan_name ? data->plan_name : "—");
    g_string_append(tooltip, "─────────────────\n");

    g_string_append_printf(tooltip, "5-hour:  %.1f%%", data->five_hour_pct_val);
    if (data->five_hour_reset_time) {
        g_string_append_printf(tooltip, " (resets%s)", data->five_hour_reset_time);
    }
    g_string_append(tooltip, "\n");

    g_string_append_printf(tooltip, "7-day:   %.1f%%", data->seven_day_pct_val);
    if (data->seven_day_reset_time) {
        g_string_append_printf(tooltip, " (resets %s)", data->seven_day_reset_time);
    }
    g_string_append(tooltip, "\n");

    if (data->context_window_size > 0) {
        gchar *tokens_str = g_strdup_printf("%ld", (long)data->context_tokens);
        gchar *window_str = g_strdup_printf("%ld", (long)data->context_window_size);
        g_string_append_printf(tooltip, "Context: %s / %s tokens (%.0f%%)\n",
                               tokens_str, window_str, data->context_pct);
        g_free(tokens_str);
        g_free(window_str);
    }

    if (data->model_name) {
        g_string_append_printf(tooltip, "\nModel: %s", data->model_name);
    }

    if (data->last_updated) {
        gchar *updated_str = g_date_time_format(data->last_updated, "%l:%M:%S %p");
        g_string_append_printf(tooltip, "\nUpdated:%s", updated_str);
        g_free(updated_str);
    }

    gtk_widget_set_tooltip_markup(data->box, tooltip->str);
    g_string_free(tooltip, TRUE);

    return TRUE;
}

/* Timer callback */
static gboolean on_timeout(gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;

    /* Check if credentials file changed (from Rust monitor) */
    if (claude_status_core_credentials_changed(data->core)) {
        data->has_credentials_error = FALSE;
    }

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

            /* Update Rust core with thresholds */
            claude_status_core_set_update_interval(data->core, data->update_interval);
            claude_status_core_set_yellow_threshold(data->core, data->yellow_threshold);
            claude_status_core_set_orange_threshold(data->core, data->orange_threshold);
            claude_status_core_set_red_threshold(data->core, data->red_threshold);
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

    /* Update Rust core with defaults */
    claude_status_core_set_update_interval(data->core, data->update_interval);
    claude_status_core_set_yellow_threshold(data->core, data->yellow_threshold);
    claude_status_core_set_orange_threshold(data->core, data->orange_threshold);
    claude_status_core_set_red_threshold(data->core, data->red_threshold);
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
        data->plan_label = create_label(data, "—", "#d4a574", TRUE);
        gtk_grid_attach(GTK_GRID(data->grid), data->plan_label, 0, 0, 1, 1);

        data->five_hour_lbl = create_label(data, "5h:", "#888", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->five_hour_lbl, 1, 0, 1, 1);

        data->five_hour_bar = create_label(data, "░░░░░░░░", "#5faf5f", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->five_hour_bar, 2, 0, 1, 1);

        data->five_hour_pct = create_label(data, "  0%", "#5faf5f", FALSE);
        gtk_grid_attach(GTK_GRID(data->grid), data->five_hour_pct, 3, 0, 1, 1);

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
    claude_status_update(data);
}

/* Handle panel size changes */
static void claude_status_size_changed(XfcePanelPlugin *plugin, gint size, ClaudeStatusPlugin *data) {
    gboolean new_single_row;
    gint new_font_size;

    if (size < 30) {
        new_single_row = TRUE;
        new_font_size = 6000;
    } else if (size < 40) {
        new_single_row = TRUE;
        new_font_size = 7000;
    } else if (size < 50) {
        new_single_row = FALSE;
        new_font_size = 8000;
    } else {
        new_single_row = FALSE;
        new_font_size = 9000;
    }

    if (new_single_row != data->single_row || new_font_size != data->font_size) {
        data->single_row = new_single_row;
        data->font_size = new_font_size;
        claude_status_rebuild_ui(data);
    }
}

/* Configuration dialog callbacks */
static void on_update_interval_changed(GtkSpinButton *btn, gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;
    data->update_interval = gtk_spin_button_get_value_as_int(btn);
    claude_status_core_set_update_interval(data->core, data->update_interval);
}

static void on_yellow_threshold_changed(GtkSpinButton *btn, gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;
    data->yellow_threshold = gtk_spin_button_get_value_as_int(btn);
    claude_status_core_set_yellow_threshold(data->core, data->yellow_threshold);
}

static void on_orange_threshold_changed(GtkSpinButton *btn, gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;
    data->orange_threshold = gtk_spin_button_get_value_as_int(btn);
    claude_status_core_set_orange_threshold(data->core, data->orange_threshold);
}

static void on_red_threshold_changed(GtkSpinButton *btn, gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;
    data->red_threshold = gtk_spin_button_get_value_as_int(btn);
    claude_status_core_set_red_threshold(data->core, data->red_threshold);
}

static void on_creds_file_set(GtkFileChooserButton *button, gpointer user_data) {
    ClaudeStatusPlugin *data = user_data;
    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(button));

    if (filename) {
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

static void on_configure_response(GtkDialog *dialog, gint response, ClaudeStatusPlugin *data) {
    if (response == GTK_RESPONSE_OK || response == GTK_RESPONSE_APPLY) {
        claude_status_save_config(data);
        claude_status_restart_timer(data);

        /* Restart file monitor with new path */
        claude_status_core_start_monitor(data->core, data->creds_file);

        /* Trigger refresh */
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

    gchar *current_path = expand_path(data->creds_file ? data->creds_file : DEFAULT_CREDS_FILE);
    if (g_file_test(current_path, G_FILE_TEST_EXISTS)) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(file_chooser), current_path);
    } else {
        gchar *claude_dir = g_build_filename(g_get_home_dir(), ".claude", NULL);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(file_chooser), claude_dir);
        g_free(claude_dir);
    }
    g_free(current_path);

    GtkFileFilter *json_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(json_filter, "JSON files (*.json)");
    gtk_file_filter_add_pattern(json_filter, "*.json");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file_chooser), json_filter);

    GtkFileFilter *all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All files");
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file_chooser), all_filter);

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

/* About dialog */
static void claude_status_about(XfcePanelPlugin *plugin) {
    const gchar *authors[] = {
        "James Curbo <james@curbo.org>",
        NULL
    };

    gtk_show_about_dialog(NULL,
        "program-name", "Claude Status",
        "version", "0.2.1",
        "comments", "Shows Claude Max/Pro usage limits in the XFCE panel",
        "website", "https://github.com/jcurbo/xfce4-claude-status-plugin",
        "license-type", GTK_LICENSE_MIT_X11,
        "authors", authors,
        "copyright", "Copyright \xc2\xa9 2026 James Curbo",
        NULL);
}

/* Build the plugin UI */
static void claude_status_construct(XfcePanelPlugin *plugin) {
    ClaudeStatusPlugin *data = g_new0(ClaudeStatusPlugin, 1);
    data->plugin = plugin;
    data->five_hour_reset_str = g_strdup("");
    data->seven_day_reset_str = g_strdup("");

    /* Create Rust core */
    data->core = claude_status_core_new();

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

    xfce_panel_plugin_menu_show_configure(plugin);
    xfce_panel_plugin_menu_show_about(plugin);
    g_signal_connect(plugin, "about", G_CALLBACK(claude_status_about), NULL);

    /* Start file monitor via Rust */
    claude_status_core_start_monitor(data->core, data->creds_file);

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

    /* Stop Rust file monitor */
    claude_status_core_stop_monitor(data->core);

    /* Free Rust core */
    claude_status_core_free(data->core);

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
