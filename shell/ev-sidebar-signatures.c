/* ev-sidebar-signatures.c
 *  this file is part of evince, a gnome document viewer
 *
 * Copyright © 2012 Vasco Dias  <contact@vascodias.me>
 *
 * Evince is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Evince is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "config.h"

#include <glib/gi18n.h>

#include "ev-sidebar-signatures.h"
#include "ev-sidebar-page.h"
#include "ev-document-signatures.h"
#include "ev-jobs.h"
#include "ev-job-scheduler.h"

struct _EvSidebarSignaturesPrivate {
  GtkWidget    *tree_view;

  GtkTreeStore *model;
};

enum {
  PROP_0,
  PROP_WIDGET
};

enum {
  COL_ICON,
  COL_HAS_ICON,
  COL_SIGN_TEXT,
  COL_MAKE_BOLD,
  N_COLUMNS
};

enum {
  SIGNATURES_VISIBLE,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void ev_sidebar_signatures_page_iface_init      (EvSidebarPageInterface    *iface);
static gboolean ev_sidebar_signatures_support_document (EvSidebarPage             *sidebar_page,
                                                        EvDocument                *document);
static void ev_sidebar_signatures_set_model            (EvSidebarPage             *sidebar_page,
                                                        EvDocumentModel           *model);
static const gchar* ev_sidebar_signatures_get_label    (EvSidebarPage             *sidebar_page);

GtkWidget* ev_sidebar_signatures_new (void);
static void ev_sidebar_signatures_init                 (EvSidebarSignatures       *ev_sign);
static void ev_sidebar_signatures_class_init           (EvSidebarSignaturesClass  *ev_sidebar_signatures_class);
static void ev_sidebar_signatures_get_property         (GObject                   *object,
                                                        guint                     prop_id,
                                                        GValue                    *value,
                                                        GParamSpec                *pspec);
static void ev_sidebar_signatures_dispose              (GObject                   *object);

static void ev_sidebar_signatures_tree_add_problem(GtkTreeStore               *model);

static void ev_sidebar_signatures_tree_add_sign_info   (GtkTreeStore               *model,
                                                        gchar                      *signer_name,
                                                        gboolean                    is_valid_sign,
                                                        gboolean                    is_signer_known,
                                                        gchar                      *sign_time);


static void job_finished_callback                      (EvJobSignatures            *job,
                                                        EvSidebarPage        *sidebar);
static void ev_sidebar_signatures_document_changed_cb  (EvDocumentModel            *model,
                                                        GParamSpec                 *pspec,
                                                        EvSidebarPage              *sidebar_page);

static void render_icon_func                           (GtkTreeViewColumn          *column,
                                                        GtkCellRenderer            *renderer,
                                                        GtkTreeModel               *model,
                                                        GtkTreeIter                *iter,
                                                        gpointer                    user_data);

static void render_bold_func                           (GtkTreeViewColumn          *column,
                                                        GtkCellRenderer            *renderer,
                                                        GtkTreeModel               *model,
                                                        GtkTreeIter                *iter,
                                                        gpointer                    user_data);

G_DEFINE_TYPE_EXTENDED (EvSidebarSignatures,
                        ev_sidebar_signatures,
                        GTK_TYPE_VBOX,
                        0,
                        G_IMPLEMENT_INTERFACE (EV_TYPE_SIDEBAR_PAGE,
                        ev_sidebar_signatures_page_iface_init))

#define EV_SIDEBAR_SIGNATURES_GET_PRIVATE(object) \
                (G_TYPE_INSTANCE_GET_PRIVATE ((object), EV_TYPE_SIDEBAR_SIGNATURES, EvSidebarSignaturesPrivate))

static void
ev_sidebar_signatures_page_iface_init (EvSidebarPageInterface *iface)
{
  iface->support_document = ev_sidebar_signatures_support_document;
  iface->set_model = ev_sidebar_signatures_set_model;
  iface->get_label = ev_sidebar_signatures_get_label;
}

static gboolean
ev_sidebar_signatures_support_document (EvSidebarPage   *sidebar_page,
                                        EvDocument      *document)
{
  return (EV_IS_DOCUMENT_SIGNATURES (document) &&
          ev_document_signatures_has_signatures (EV_DOCUMENT_SIGNATURES (document)));
}

static const gchar *
ev_sidebar_signatures_get_label (EvSidebarPage *sidebar_page)
{
  return _("Signatures");
}

static void
job_finished_callback (EvJobSignatures *job, EvSidebarPage *sidebar)
{
  GtkTreeStore *model = EV_SIDEBAR_SIGNATURES(sidebar)->priv->model;
  GList *l;

  gchar *signer_name;
  gboolean is_sign_valid;
  gboolean is_signer_known;
  gchar *sign_time;

  for (l = job->signatures; l && l->data; l = g_list_next (l)) {
    EvSignature *signature = EV_SIGNATURE (l->data);

    g_object_get (G_OBJECT (signature),
                  "signer-name", &signer_name,
                  "signature-valid", &is_sign_valid,
                  "signer-identity-known", &is_signer_known,
                  "signature-time", &sign_time,
                  NULL);

    /* if the signer-name comes NULL, we got a problem to report to the user
     * because a signature should always have this info */
    if (!signer_name)
      ev_sidebar_signatures_tree_add_problem (model);
    else
      ev_sidebar_signatures_tree_add_sign_info (model, signer_name, is_sign_valid, is_signer_known, sign_time);
  }
}

static void
ev_sidebar_signatures_document_changed_cb (EvDocumentModel     *model,
                                           GParamSpec          *pspec,
                                           EvSidebarPage       *sidebar_page)
{
  EvDocument *document = ev_document_model_get_document (model);
  EvJob *job = ev_job_signatures_new (document);

  if (!ev_sidebar_signatures_support_document (sidebar_page, document))
    return;

  g_signal_connect (job, "finished",
        G_CALLBACK (job_finished_callback),
        sidebar_page);

  /* The priority doesn't matter for this job */
  ev_job_scheduler_push_job (job, EV_JOB_PRIORITY_NONE);
}

static void
ev_sidebar_signatures_set_model (EvSidebarPage   *sidebar_page,
                                 EvDocumentModel *model)
{
  g_signal_connect (model, "notify::document",
        G_CALLBACK (ev_sidebar_signatures_document_changed_cb),
        sidebar_page);
}

GtkWidget *
ev_sidebar_signatures_new (void)
{
  return GTK_WIDGET (g_object_new (EV_TYPE_SIDEBAR_SIGNATURES, NULL));
}

static void
ev_sidebar_signatures_init (EvSidebarSignatures *ev_sign)
{
  EvSidebarSignaturesPrivate *priv;
  GtkWidget    *swindow;

  /* initialize the private structure that holds everything we need */
  priv = ev_sign->priv = EV_SIDEBAR_SIGNATURES_GET_PRIVATE (ev_sign);

  /* create a scrolled window for the sidebar */
  swindow = gtk_scrolled_window_new (NULL, NULL);

  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (swindow),
                                       GTK_SHADOW_IN);

  /* the main widget of the sidebar will be the scrollable area */
  gtk_box_pack_start (GTK_BOX (ev_sign), swindow, TRUE, TRUE, 0);

  /* create the tree view where all the info is contained */
  priv->tree_view = gtk_tree_view_new ();
  GtkTreeViewColumn *col = gtk_tree_view_column_new ();
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new ();
  GtkCellRenderer *icon_renderer = gtk_cell_renderer_pixbuf_new ();

  /* create model */
  priv->model = gtk_tree_store_new (N_COLUMNS, G_TYPE_STRING, G_TYPE_BOOLEAN,
                                    G_TYPE_STRING, G_TYPE_BOOLEAN);

  /* make the associations for it to show something */
  gtk_tree_view_append_column (GTK_TREE_VIEW (priv->tree_view), col);

  gtk_tree_view_column_pack_start (col, icon_renderer, FALSE);
  gtk_tree_view_column_add_attribute (col, icon_renderer, "stock-id", COL_ICON);

  /* we use a custom render function to select per row if we need to
     show the icon or not. */
  gtk_tree_view_column_set_cell_data_func (col, icon_renderer, render_icon_func, NULL, NULL);

  gtk_tree_view_column_pack_start (col, renderer, TRUE);
  gtk_tree_view_column_add_attribute (col, renderer, "text", COL_SIGN_TEXT);
  gtk_tree_view_column_set_cell_data_func (col, renderer, render_bold_func, NULL, NULL);

  /* associate the model to the view */
  gtk_tree_view_set_model (GTK_TREE_VIEW (priv->tree_view), GTK_TREE_MODEL (priv->model));

  /* some other options for the tree view */
  gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (priv->tree_view), FALSE);
  gtk_tree_view_set_tooltip_column (GTK_TREE_VIEW (priv->tree_view), COL_SIGN_TEXT);

  /* add the tree view to the scrolled area */
  gtk_container_add (GTK_CONTAINER (swindow), priv->tree_view);

  gtk_widget_show_all (GTK_WIDGET (ev_sign));
}

static void
ev_sidebar_signatures_map (GtkWidget *widget)
{
  EvSidebarSignatures *sidebar;

  sidebar = EV_SIDEBAR_SIGNATURES (widget);

  GTK_WIDGET_CLASS (ev_sidebar_signatures_parent_class)->map (widget);

  g_signal_emit (sidebar, signals[SIGNATURES_VISIBLE], 0);
}

static void
ev_sidebar_signatures_class_init (EvSidebarSignaturesClass *ev_sidebar_signatures_class)
{
  GObjectClass *g_object_class;
  GtkWidgetClass *widget_class;

  g_object_class = G_OBJECT_CLASS (ev_sidebar_signatures_class);
  widget_class = GTK_WIDGET_CLASS (ev_sidebar_signatures_class);

  g_object_class->get_property = ev_sidebar_signatures_get_property;
  g_object_class->dispose = ev_sidebar_signatures_dispose;
  widget_class->map = ev_sidebar_signatures_map;

  g_object_class_override_property (g_object_class, PROP_WIDGET, "main-widget");

  g_type_class_add_private (g_object_class, sizeof (EvSidebarSignaturesPrivate));

  signals[SIGNATURES_VISIBLE] =
    g_signal_new ("signatures-visible",
            G_TYPE_FROM_CLASS (g_object_class),
            G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
            G_STRUCT_OFFSET (EvSidebarSignaturesClass, visible),
            NULL, NULL,
            g_cclosure_marshal_VOID__VOID,
            G_TYPE_NONE, 0);
}

static void
ev_sidebar_signatures_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  EvSidebarSignatures *sidebar = EV_SIDEBAR_SIGNATURES (object);

  switch (prop_id)
    {
    case PROP_WIDGET:
      g_value_set_object (value, sidebar->priv->tree_view);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ev_sidebar_signatures_dispose (GObject *object)
{
  EvSidebarSignatures *sidebar = EV_SIDEBAR_SIGNATURES (object);

  if (sidebar->priv->model) {
    g_object_unref (sidebar->priv->model);
    sidebar->priv->model = NULL;
  }

  G_OBJECT_CLASS (ev_sidebar_signatures_parent_class)->dispose (object);
}

static void
render_icon_func (GtkTreeViewColumn *column,
                  GtkCellRenderer   *icon_renderer,
                  GtkTreeModel      *model,
                  GtkTreeIter       *iter,
                  gpointer          user_data)
{
  gboolean show_icon;

  gtk_tree_model_get (model, iter, COL_HAS_ICON, &show_icon, -1);

  gtk_cell_renderer_set_visible (icon_renderer, show_icon);
}

static void
render_bold_func (GtkTreeViewColumn *column,
                  GtkCellRenderer   *text_renderer,
                  GtkTreeModel      *model,
                  GtkTreeIter       *iter,
                  gpointer          user_data)
{
  gboolean make_bold;

  gtk_tree_model_get (model, iter, COL_MAKE_BOLD, &make_bold, -1);

  if (make_bold)
    {
      g_object_set (text_renderer, "weight", PANGO_WEIGHT_BOLD,
                                   "weight-set", TRUE,
                                   NULL);
    }
  else
    {
      g_object_set (text_renderer, "weight-set", FALSE, NULL);
    }
}

static void
ev_sidebar_signatures_tree_add_problem (GtkTreeStore *model)
{
  GtkTreeIter parent;
  GtkTreeIter details;

  const gchar *problem_text = _("Problem getting this signature");
  const gchar *problem_icon = GTK_STOCK_DIALOG_WARNING;

  const gchar *details_text = _("The file could be corrupted");

  gtk_tree_store_insert_with_values (model, &parent, NULL, -1,
                                     COL_SIGN_TEXT, problem_text,
                                     COL_ICON, problem_icon,
                                     COL_HAS_ICON, TRUE,
                                     COL_MAKE_BOLD, TRUE,
                                     -1);

  gtk_tree_store_insert_with_values (model, &details, &parent, -1,
                                     COL_SIGN_TEXT, details_text,
                                     COL_HAS_ICON, FALSE,
                                     COL_MAKE_BOLD, FALSE,
                                     -1);
}

static void
ev_sidebar_signatures_tree_add_sign_info (GtkTreeStore  *model,
                                          gchar         *signer_name,
                                          gboolean       is_valid_sign,
                                          gboolean       is_signer_known,
                                          gchar         *sign_time)
{
  GtkTreeIter parent;
  GtkTreeIter conclusion;
  GtkTreeIter details;

  const gchar *status_text;
  const gchar *sign_ok_icon;

  const gchar *sign_valid_text;
  const gchar *sign_valid_icon;

  const gchar *signer_known_text;
  const gchar *signer_known_icon;

  /* do some logic to know from the returned values if
     the signature is valid or not and what the problem is:
      * both verifications are TRUE, everything is fine
        (common case 1)
      * signature is good but signer is unknown
        (common case 2)
      * when signature fails verification poppler won't
        even care to know if signer is known */
  if (is_valid_sign)
    {
      sign_valid_text = _("Document has not  been modified since the signature was applied");
      sign_valid_icon = GTK_STOCK_OK;
    }
  else
    {
      sign_valid_text = _("Document was changed since the signature was applied");
      sign_valid_icon = GTK_STOCK_NO;
    }

  if (is_valid_sign && is_signer_known)
    {
      status_text = _("Signature is valid");
      sign_ok_icon = GTK_STOCK_OK;

      signer_known_text = _("Signer's identity is known");
      signer_known_icon = GTK_STOCK_OK;
    }
  else if (is_valid_sign && !is_signer_known)
    {
      status_text = _("Signature has problems");
      sign_ok_icon = GTK_STOCK_DIALOG_WARNING;

      signer_known_text = _("Signer's identity is unknown");
      signer_known_icon = GTK_STOCK_NO;
    }
  else
    {
      status_text = _("Signature is invalid");
      sign_ok_icon = GTK_STOCK_STOP;

      signer_known_text = _("Signer's identity wasn't checked");
      signer_known_icon = GTK_STOCK_NO;
    }

  /* do we have enough info about the time to show an icon ? */
  const gchar *time_text = sign_time ? sign_time : _("Time not available");

  /* create the 1st level node with the signature name */
  gchar *signed_by = g_strdup_printf (_("Signed by: %s"), signer_name);
  gtk_tree_store_insert_with_values (model, &parent, NULL, -1,
                                     COL_SIGN_TEXT, signed_by,
                                     COL_ICON, sign_ok_icon,
                                     COL_HAS_ICON, TRUE,
                                     COL_MAKE_BOLD, TRUE,
                                     -1);
  g_free (signed_by);

  /* create the node with the validity status */
  gtk_tree_store_insert_with_values (model, &conclusion, &parent, -1,
                                     COL_SIGN_TEXT, status_text,
                                     COL_HAS_ICON, FALSE,
                                     COL_MAKE_BOLD, FALSE,
                                     -1);

  /* append the remaining information about the signature as child nodes */
  gtk_tree_store_insert_with_values (model, &details, &conclusion, -1,
                                     COL_SIGN_TEXT, sign_valid_text,
                                     COL_HAS_ICON, TRUE,
                                     COL_ICON, sign_valid_icon,
                                     COL_MAKE_BOLD, FALSE,
                                     -1);

  gtk_tree_store_insert_with_values (model, &details, &conclusion, -1,
                                     COL_SIGN_TEXT, signer_known_text,
                                     COL_HAS_ICON, TRUE,
                                     COL_ICON, signer_known_icon,
                                     COL_MAKE_BOLD, FALSE,
                                     -1);

  gtk_tree_store_insert_with_values (model, &details, &conclusion, -1,
                                     COL_SIGN_TEXT, time_text,
                                     COL_HAS_ICON, FALSE,
                                     COL_MAKE_BOLD, FALSE,
                                     -1);
}
