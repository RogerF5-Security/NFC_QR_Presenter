#include "nfc_qr_nfc.h"
#include "nfc_qr_presenter.h"
#include "qrcodegen.h"

#include "../assets/nfc_qr_icon_xbm.h"

#include <furi.h>
#include <gui/elements.h>
#include <gui/gui.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/view_dispatcher.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFC_QR_QR_MAX_VERSION 10

typedef enum {
    NfcQrViewBanner,
    NfcQrViewMain,
    NfcQrViewPayloads,
    NfcQrViewTextInput,
    NfcQrViewDialog,
    NfcQrViewShare,
} NfcQrView;

typedef enum {
    NfcQrMainShare,
    NfcQrMainAdd,
    NfcQrMainEdit,
    NfcQrMainDelete,
} NfcQrMainIndex;

typedef enum {
    NfcQrDialogStatus,
    NfcQrDialogConfirmDelete,
} NfcQrDialogMode;

typedef struct {
    Gui* gui;
    Storage* storage;
    ViewDispatcher* dispatcher;
    Submenu* main_menu;
    Submenu* payload_menu;
    TextInput* text_input;
    DialogEx* dialog;
    View* banner_view;
    View* share_view;

    NfcQrView current_view;
    NfcQrPayloadAction pending_action;
    NfcQrDialogMode dialog_mode;

    NfcQrPayload payloads[NFC_QR_MAX_PAYLOADS];
    size_t payload_count;
    uint32_t selected_index;

    char text_buffer[NFC_QR_TEXT_MAX + 1];
    char status_buffer[96];

    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(NFC_QR_QR_MAX_VERSION)];
    uint8_t qr_temp[qrcodegen_BUFFER_LEN_FOR_VERSION(NFC_QR_QR_MAX_VERSION)];
    bool qr_ready;

    uint8_t ndef[NFC_QR_NDEF_MAX];
    size_t ndef_len;
    NfcQrNfcWorker nfc_worker;

    bool storage_opened;
    bool gui_opened;
    bool views_registered;
    bool share_model_allocated;
} NfcQrApp;

typedef struct {
    NfcQrApp* app;
} NfcQrShareModel;

static void nfc_qr_main_menu_callback(void* context, uint32_t index);
static void nfc_qr_payload_selected_callback(void* context, uint32_t index);
static void nfc_qr_text_input_done(void* context);
static void nfc_qr_app_free(NfcQrApp* app);
static void nfc_qr_tick_callback(void* context);
static void nfc_qr_switch_view(NfcQrApp* app, NfcQrView view);

static void nfc_qr_show_main(NfcQrApp* app) {
    if(!app) return;
    nfc_qr_switch_view(app, NfcQrViewMain);
}

static void nfc_qr_switch_view(NfcQrApp* app, NfcQrView view) {
    if(!app || !app->dispatcher) return;
    app->current_view = view;
    view_dispatcher_switch_to_view(app->dispatcher, view);
}

static void nfc_qr_refresh_payloads(NfcQrApp* app) {
    app->payload_count = nfc_qr_storage_scan(app->storage, app->payloads, COUNT_OF(app->payloads));
    if(app->selected_index >= app->payload_count) app->selected_index = 0;
}

static void nfc_qr_show_status(NfcQrApp* app, const char* header, const char* text) {
    dialog_ex_reset(app->dialog);
    dialog_ex_set_header(app->dialog, header, 64, 10, AlignCenter, AlignCenter);
    dialog_ex_set_text(app->dialog, text, 64, 32, AlignCenter, AlignCenter);
    dialog_ex_set_center_button_text(app->dialog, "OK");
    app->dialog_mode = NfcQrDialogStatus;
    nfc_qr_switch_view(app, NfcQrViewDialog);
}

static void nfc_qr_refresh_main_menu(NfcQrApp* app) {
    submenu_reset(app->main_menu);
    snprintf(
        app->status_buffer,
        sizeof(app->status_buffer),
        "Payloads: %u",
        (unsigned)app->payload_count);
    submenu_set_header(app->main_menu, app->status_buffer);
    submenu_add_item(app->main_menu, "Compartir", NfcQrMainShare, nfc_qr_main_menu_callback, app);
    submenu_add_item(app->main_menu, "Agregar", NfcQrMainAdd, nfc_qr_main_menu_callback, app);
    submenu_add_item(app->main_menu, "Editar", NfcQrMainEdit, nfc_qr_main_menu_callback, app);
    submenu_add_item(app->main_menu, "Borrar", NfcQrMainDelete, nfc_qr_main_menu_callback, app);
}

static const char* nfc_qr_action_header(NfcQrPayloadAction action) {
    switch(action) {
    case NfcQrPayloadActionShare:
        return "Compartir";
    case NfcQrPayloadActionAdd:
        return "Agregar";
    case NfcQrPayloadActionEdit:
        return "Editar";
    case NfcQrPayloadActionDelete:
        return "Borrar";
    default:
        return "Payload";
    }
}

static const char* nfc_qr_share_nfc_label(NfcQrNfcStatus status) {
    switch(status) {
    case NfcQrNfcStatusRunning:
        return "NFC: ON";
    case NfcQrNfcStatusStarting:
        return "NFC: START";
    case NfcQrNfcStatusDisabled:
        return "NFC: NDEF";
    case NfcQrNfcStatusIdle:
        return "NFC: OFF";
    default:
        return "NFC: ERR";
    }
}

static void nfc_qr_banner_draw(Canvas* canvas, void* context) {
    UNUSED(context);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    canvas_draw_rframe(canvas, 3, 4, 122, 48, 4);
    canvas_draw_rframe(canvas, 5, 6, 118, 44, 3);

    canvas_draw_xbm(
        canvas, 13, 13, NFC_QR_ICON_XBM_WIDTH, NFC_QR_ICON_XBM_HEIGHT, nfc_qr_icon_xbm);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 31, 18, "NFC QR Presenter");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 31, 30, "Digital card");
    canvas_draw_str(canvas, 31, 40, "QR + NFC NDEF");

    canvas_draw_rframe(canvas, 101, 12, 15, 15, 2);
    canvas_draw_box(canvas, 104, 15, 3, 3);
    canvas_draw_box(canvas, 110, 15, 3, 3);
    canvas_draw_box(canvas, 104, 21, 3, 3);
    canvas_draw_dot(canvas, 111, 22);
    canvas_draw_dot(canvas, 114, 24);

    canvas_draw_line(canvas, 91, 35, 94, 32);
    canvas_draw_line(canvas, 91, 35, 94, 38);
    canvas_draw_line(canvas, 95, 30, 100, 25);
    canvas_draw_line(canvas, 95, 40, 100, 45);
    canvas_draw_line(canvas, 101, 23, 108, 19);
    canvas_draw_line(canvas, 101, 47, 108, 51);

    elements_button_center(canvas, "OK");
}

static bool nfc_qr_banner_input(InputEvent* event, void* context) {
    NfcQrApp* app = context;
    if(!app || (event->type != InputTypeShort)) return false;

    if((event->key == InputKeyOk) || (event->key == InputKeyRight) ||
       (event->key == InputKeyDown)) {
        nfc_qr_show_main(app);
        return true;
    }

    if(event->key == InputKeyBack) {
        view_dispatcher_stop(app->dispatcher);
        return true;
    }

    return false;
}

static void nfc_qr_refresh_payload_menu(NfcQrApp* app) {
    submenu_reset(app->payload_menu);
    submenu_set_header(app->payload_menu, nfc_qr_action_header(app->pending_action));
    for(size_t i = 0; i < app->payload_count; i++) {
        submenu_add_item(
            app->payload_menu, app->payloads[i].name, i, nfc_qr_payload_selected_callback, app);
    }
    submenu_set_selected_item(app->payload_menu, app->selected_index);
}

static bool nfc_qr_prepare_share(NfcQrApp* app) {
    if(!app || (app->selected_index >= app->payload_count)) {
        if(app) nfc_qr_show_status(app, "Error", "Payload invalido");
        return false;
    }

    NfcQrPayload* payload = &app->payloads[app->selected_index];

    if(!nfc_qr_storage_read_text(
           app->storage, payload, app->text_buffer, sizeof(app->text_buffer))) {
        nfc_qr_show_status(app, "Error", "No se pudo leer TXT");
        return false;
    }

    if(!nfc_qr_storage_read_ndef(
           app->storage, payload, app->ndef, sizeof(app->ndef), &app->ndef_len)) {
        app->ndef_len = nfc_qr_build_ndef_message(app->text_buffer, app->ndef, sizeof(app->ndef));
        if(app->ndef_len == 0) {
            nfc_qr_show_status(app, "Error", "NDEF invalido");
            return false;
        }
    }

    app->qr_ready = qrcodegen_encodeText(
        app->text_buffer,
        app->qr_temp,
        app->qr,
        qrcodegen_Ecc_LOW,
        qrcodegen_VERSION_MIN,
        NFC_QR_QR_MAX_VERSION,
        qrcodegen_Mask_AUTO,
        true);
    if(!app->qr_ready) {
        nfc_qr_show_status(app, "Error", "QR muy grande");
        return false;
    }

    if(!nfc_qr_nfc_worker_start(&app->nfc_worker, app->ndef, app->ndef_len)) {
        app->nfc_worker.status = NfcQrNfcStatusErrorPayload;
    }

    nfc_qr_switch_view(app, NfcQrViewShare);
    return true;
}

static void nfc_qr_payload_selected_callback(void* context, uint32_t index) {
    NfcQrApp* app = context;
    if(!app || (index >= app->payload_count)) return;

    app->selected_index = index;

    switch(app->pending_action) {
    case NfcQrPayloadActionShare:
        nfc_qr_prepare_share(app);
        break;
    case NfcQrPayloadActionAdd:
        nfc_qr_show_main(app);
        break;
    case NfcQrPayloadActionEdit:
        if(nfc_qr_storage_read_text(
               app->storage,
               &app->payloads[app->selected_index],
               app->text_buffer,
               sizeof(app->text_buffer))) {
            text_input_reset(app->text_input);
            text_input_set_header_text(app->text_input, "Editar payload");
            text_input_set_minimum_length(app->text_input, 1);
            text_input_set_result_callback(
                app->text_input,
                nfc_qr_text_input_done,
                app,
                app->text_buffer,
                sizeof(app->text_buffer),
                false);
            nfc_qr_switch_view(app, NfcQrViewTextInput);
        } else {
            nfc_qr_show_status(app, "Error", "No se pudo leer TXT");
        }
        break;
    case NfcQrPayloadActionDelete:
        dialog_ex_reset(app->dialog);
        snprintf(
            app->status_buffer,
            sizeof(app->status_buffer),
            "Eliminar %s?",
            app->payloads[app->selected_index].name);
        dialog_ex_set_header(app->dialog, "Confirmar", 64, 10, AlignCenter, AlignCenter);
        dialog_ex_set_text(app->dialog, app->status_buffer, 64, 32, AlignCenter, AlignCenter);
        dialog_ex_set_left_button_text(app->dialog, "Cancelar");
        dialog_ex_set_right_button_text(app->dialog, "Borrar");
        app->dialog_mode = NfcQrDialogConfirmDelete;
        nfc_qr_switch_view(app, NfcQrViewDialog);
        break;
    }
}

static void nfc_qr_text_input_done(void* context) {
    NfcQrApp* app = context;
    NfcQrPayload saved;
    bool ok = false;

    if(!app || !app->storage) return;

    if(app->pending_action == NfcQrPayloadActionEdit) {
        ok = nfc_qr_storage_write_payload(
            app->storage, app->payloads[app->selected_index].name, app->text_buffer, &saved);
    } else {
        ok = nfc_qr_storage_write_payload(app->storage, NULL, app->text_buffer, &saved);
    }

    nfc_qr_refresh_payloads(app);
    nfc_qr_refresh_main_menu(app);

    if(ok) {
        nfc_qr_show_status(app, "Guardado", saved.name);
    } else {
        nfc_qr_show_status(app, "Error", "No se pudo guardar");
    }
}

static void nfc_qr_main_menu_callback(void* context, uint32_t index) {
    NfcQrApp* app = context;
    if(!app) return;

    if(index == NfcQrMainAdd) {
        app->pending_action = NfcQrPayloadActionAdd;
        app->text_buffer[0] = '\0';
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, "Nuevo payload");
        text_input_set_minimum_length(app->text_input, 1);
        text_input_set_result_callback(
            app->text_input,
            nfc_qr_text_input_done,
            app,
            app->text_buffer,
            sizeof(app->text_buffer),
            true);
        nfc_qr_switch_view(app, NfcQrViewTextInput);
        return;
    }

    if(app->payload_count == 0) {
        nfc_qr_show_status(app, "Sin datos", "Agrega un payload");
        return;
    }

    if(index == NfcQrMainShare) {
        app->pending_action = NfcQrPayloadActionShare;
    } else if(index == NfcQrMainEdit) {
        app->pending_action = NfcQrPayloadActionEdit;
    } else if(index == NfcQrMainDelete) {
        app->pending_action = NfcQrPayloadActionDelete;
    } else {
        nfc_qr_show_status(app, "Error", "Opcion invalida");
        return;
    }

    nfc_qr_refresh_payload_menu(app);
    nfc_qr_switch_view(app, NfcQrViewPayloads);
}

static void nfc_qr_dialog_callback(DialogExResult result, void* context) {
    NfcQrApp* app = context;
    if(!app) return;

    if(app->dialog_mode == NfcQrDialogConfirmDelete) {
        if((result == DialogExResultRight) && (app->selected_index < app->payload_count)) {
            const bool ok =
                nfc_qr_storage_delete_payload(app->storage, &app->payloads[app->selected_index]);
            nfc_qr_refresh_payloads(app);
            nfc_qr_refresh_main_menu(app);
            nfc_qr_show_status(
                app, ok ? "Borrado" : "Error", ok ? "Payload eliminado" : "No se pudo borrar");
        } else if(result == DialogExResultLeft) {
            nfc_qr_show_main(app);
        }
    } else if(app->dialog_mode == NfcQrDialogStatus) {
        if(result == DialogExResultCenter) {
            nfc_qr_show_main(app);
        }
    }
}

static void nfc_qr_draw_payload_preview(Canvas* canvas, const char* text) {
    char preview[48];
    strlcpy(preview, text, sizeof(preview));
    for(size_t i = 0; preview[i]; i++) {
        if((preview[i] == '\n') || (preview[i] == '\r') || (preview[i] == '\t')) preview[i] = ' ';
    }
    canvas_draw_str(canvas, 70, 29, preview);
}

static void nfc_qr_share_draw(Canvas* canvas, void* model) {
    NfcQrShareModel* share_model = model;
    NfcQrApp* app = share_model ? share_model->app : NULL;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    if(!app) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 18, 30, "Modelo invalido");
        elements_button_left(canvas, "Back");
        return;
    }

    if(!app->qr_ready) {
        canvas_draw_xbm(
            canvas, 8, 14, NFC_QR_ICON_XBM_WIDTH, NFC_QR_ICON_XBM_HEIGHT, nfc_qr_icon_xbm);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 48, 28, "QR no listo");
        elements_button_left(canvas, "Back");
        return;
    }

    const int qr_size = qrcodegen_getSize(app->qr);
    const int quiet = 2;
    const int scale = (qr_size <= 25) ? 2 : 1;
    const int box = (qr_size + (quiet * 2)) * scale;
    const int x0 = (box >= 62) ? 1 : 5;
    const int y0 = (64 - box) / 2;

    canvas_draw_frame(canvas, x0 - 1, y0 - 1, box + 2, box + 2);
    for(int y = 0; y < qr_size; y++) {
        for(int x = 0; x < qr_size; x++) {
            if(qrcodegen_getModule(app->qr, x, y)) {
                canvas_draw_box(
                    canvas, x0 + ((x + quiet) * scale), y0 + ((y + quiet) * scale), scale, scale);
            }
        }
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 70, 10, app->payloads[app->selected_index].name);
    canvas_draw_str(canvas, 70, 20, nfc_qr_share_nfc_label(app->nfc_worker.status));
    nfc_qr_draw_payload_preview(canvas, app->text_buffer);
    elements_button_left(canvas, "Back");
}

static bool nfc_qr_share_input(InputEvent* event, void* context) {
    NfcQrApp* app = context;
    if((event->type == InputTypeShort) && (event->key == InputKeyBack)) {
        nfc_qr_nfc_worker_stop(&app->nfc_worker);
        nfc_qr_show_main(app);
        return true;
    }
    return false;
}

static bool nfc_qr_navigation_callback(void* context) {
    NfcQrApp* app = context;

    if((app->current_view == NfcQrViewBanner) || (app->current_view == NfcQrViewMain)) {
        view_dispatcher_stop(app->dispatcher);
    } else {
        if(app->current_view == NfcQrViewShare) {
            nfc_qr_nfc_worker_stop(&app->nfc_worker);
        }
        nfc_qr_show_main(app);
    }

    return true;
}

static NfcQrApp* nfc_qr_app_alloc(void) {
    NfcQrApp* app = malloc(sizeof(NfcQrApp));
    if(!app) return NULL;

    memset(app, 0, sizeof(NfcQrApp));

    app->storage = furi_record_open(RECORD_STORAGE);
    app->storage_opened = app->storage != NULL;
    app->gui = furi_record_open(RECORD_GUI);
    app->gui_opened = app->gui != NULL;
    app->dispatcher = view_dispatcher_alloc();
    app->main_menu = submenu_alloc();
    app->payload_menu = submenu_alloc();
    app->text_input = text_input_alloc();
    app->dialog = dialog_ex_alloc();
    app->banner_view = view_alloc();
    app->share_view = view_alloc();

    if(!app->storage || !app->gui || !app->dispatcher || !app->main_menu || !app->payload_menu ||
       !app->text_input || !app->dialog || !app->banner_view || !app->share_view) {
        nfc_qr_app_free(app);
        return NULL;
    }

    view_dispatcher_set_event_callback_context(app->dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->dispatcher, nfc_qr_navigation_callback);
    view_dispatcher_set_tick_event_callback(app->dispatcher, nfc_qr_tick_callback, 250);

    submenu_add_item(app->main_menu, "Compartir", NfcQrMainShare, nfc_qr_main_menu_callback, app);
    submenu_add_item(app->main_menu, "Agregar", NfcQrMainAdd, nfc_qr_main_menu_callback, app);
    submenu_add_item(app->main_menu, "Editar", NfcQrMainEdit, nfc_qr_main_menu_callback, app);
    submenu_add_item(app->main_menu, "Borrar", NfcQrMainDelete, nfc_qr_main_menu_callback, app);

    dialog_ex_set_context(app->dialog, app);
    dialog_ex_set_result_callback(app->dialog, nfc_qr_dialog_callback);

    view_set_context(app->banner_view, app);
    view_set_draw_callback(app->banner_view, nfc_qr_banner_draw);
    view_set_input_callback(app->banner_view, nfc_qr_banner_input);

    view_set_context(app->share_view, app);
    view_allocate_model(app->share_view, ViewModelTypeLockFree, sizeof(NfcQrShareModel));
    NfcQrShareModel* share_model = view_get_model(app->share_view);
    share_model->app = app;
    view_commit_model(app->share_view, false);
    app->share_model_allocated = true;
    view_set_draw_callback(app->share_view, nfc_qr_share_draw);
    view_set_input_callback(app->share_view, nfc_qr_share_input);

    view_dispatcher_add_view(app->dispatcher, NfcQrViewBanner, app->banner_view);
    view_dispatcher_add_view(app->dispatcher, NfcQrViewMain, submenu_get_view(app->main_menu));
    view_dispatcher_add_view(
        app->dispatcher, NfcQrViewPayloads, submenu_get_view(app->payload_menu));
    view_dispatcher_add_view(
        app->dispatcher, NfcQrViewTextInput, text_input_get_view(app->text_input));
    view_dispatcher_add_view(app->dispatcher, NfcQrViewDialog, dialog_ex_get_view(app->dialog));
    view_dispatcher_add_view(app->dispatcher, NfcQrViewShare, app->share_view);
    app->views_registered = true;
    view_dispatcher_attach_to_gui(app->dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    nfc_qr_nfc_worker_init(&app->nfc_worker);
    return app;
}

static void nfc_qr_app_free(NfcQrApp* app) {
    if(!app) return;

    nfc_qr_nfc_worker_stop(&app->nfc_worker);

    if(app->dispatcher && app->views_registered) {
        view_dispatcher_remove_view(app->dispatcher, NfcQrViewMain);
        view_dispatcher_remove_view(app->dispatcher, NfcQrViewPayloads);
        view_dispatcher_remove_view(app->dispatcher, NfcQrViewTextInput);
        view_dispatcher_remove_view(app->dispatcher, NfcQrViewDialog);
        view_dispatcher_remove_view(app->dispatcher, NfcQrViewBanner);
        view_dispatcher_remove_view(app->dispatcher, NfcQrViewShare);
    }

    if(app->banner_view) view_free(app->banner_view);
    if(app->share_view) {
        if(app->share_model_allocated) view_free_model(app->share_view);
        view_free(app->share_view);
    }
    if(app->dialog) dialog_ex_free(app->dialog);
    if(app->text_input) text_input_free(app->text_input);
    if(app->payload_menu) submenu_free(app->payload_menu);
    if(app->main_menu) submenu_free(app->main_menu);
    if(app->dispatcher) view_dispatcher_free(app->dispatcher);

    if(app->gui_opened) furi_record_close(RECORD_GUI);
    if(app->storage_opened) furi_record_close(RECORD_STORAGE);
    free(app);
}

static void nfc_qr_tick_callback(void* context) {
    NfcQrApp* app = context;
    if(!app || (app->current_view != NfcQrViewShare) || !app->share_view) return;

    view_commit_model(app->share_view, true);
}

int32_t nfc_qr_presenter_app(void* p) {
    UNUSED(p);

    NfcQrApp* app = nfc_qr_app_alloc();
    if(!app) return -1;

    const bool storage_ready = nfc_qr_storage_init(app->storage);

    nfc_qr_refresh_payloads(app);
    nfc_qr_refresh_main_menu(app);
    if(storage_ready) {
        nfc_qr_switch_view(app, NfcQrViewBanner);
    } else {
        nfc_qr_show_status(app, "SD Error", "Revise /ext");
    }
    view_dispatcher_run(app->dispatcher);

    nfc_qr_app_free(app);
    return 0;
}
