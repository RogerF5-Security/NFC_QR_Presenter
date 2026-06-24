#include "nfc_qr_nfc.h"

#include <furi_hal_nfc.h>
#include <furi_hal_random.h>
#include <nfc/nfc.h>
#include <nfc/nfc_listener.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <string.h>

#define NFC_QR_NXP_MANUFACTURER_ID 0x04

typedef struct {
    MfUltralightType type;
    uint16_t pages_total;
    uint8_t version_storage_size;
    uint8_t cc_data_area_size;
} NfcQrNtagProfile;

static const uint8_t nfc_qr_ntag21x_version[] =
    {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x00, 0x03};

static const NfcQrNtagProfile nfc_qr_ntag_profiles[] = {
    {
        .type = MfUltralightTypeNTAG213,
        .pages_total = 45,
        .version_storage_size = 0x0F,
        .cc_data_area_size = 0x12,
    },
    {
        .type = MfUltralightTypeNTAG215,
        .pages_total = 135,
        .version_storage_size = 0x11,
        .cc_data_area_size = 0x3E,
    },
    {
        .type = MfUltralightTypeNTAG216,
        .pages_total = 231,
        .version_storage_size = 0x13,
        .cc_data_area_size = 0x6D,
    },
};

static size_t nfc_qr_tlv_len(size_t ndef_len) {
    return (ndef_len < 0xFF) ? (ndef_len + 3) : (ndef_len + 5);
}

static const NfcQrNtagProfile* nfc_qr_pick_profile(size_t ndef_len) {
    const size_t tlv_len = nfc_qr_tlv_len(ndef_len);

    for(size_t i = 0; i < COUNT_OF(nfc_qr_ntag_profiles); i++) {
        const size_t user_bytes =
            (nfc_qr_ntag_profiles[i].pages_total - 9) * MF_ULTRALIGHT_PAGE_SIZE;
        if(tlv_len <= user_bytes) return &nfc_qr_ntag_profiles[i];
    }

    return NULL;
}

static void nfc_qr_generate_uid(uint8_t* uid) {
    uid[0] = NFC_QR_NXP_MANUFACTURER_ID;
    furi_hal_random_fill_buf(&uid[1], 6);
    uid[3] |= 0x01;
    uid[6] &= 0x0F;
    uid[6] |= 0x80;
}

static bool nfc_qr_build_ntag_data(
    MfUltralightData* data,
    const uint8_t* ndef,
    size_t ndef_len) {
    if(!data || !data->iso14443_3a_data || !ndef || (ndef_len == 0)) return false;

    const NfcQrNtagProfile* profile = nfc_qr_pick_profile(ndef_len);
    if(!profile) return false;

    Iso14443_3aData* iso14443_3a_data = data->iso14443_3a_data;
    memset(data, 0, sizeof(MfUltralightData));
    data->iso14443_3a_data = iso14443_3a_data;
    iso14443_3a_reset(data->iso14443_3a_data);

    uint8_t uid[7];
    nfc_qr_generate_uid(uid);
    data->iso14443_3a_data->uid_len = 7;
    if(!mf_ultralight_set_uid(data, uid, sizeof(uid))) return false;
    data->iso14443_3a_data->atqa[0] = 0x44;
    data->iso14443_3a_data->atqa[1] = 0x00;
    data->iso14443_3a_data->sak = 0x00;

    data->type = profile->type;
    data->pages_total = profile->pages_total;
    data->pages_read = profile->pages_total;
    memcpy(&data->version, nfc_qr_ntag21x_version, sizeof(nfc_qr_ntag21x_version));
    data->version.storage_size = profile->version_storage_size;

    data->page[2].data[1] = 0x48;
    data->page[3].data[0] = 0xE1;
    data->page[3].data[1] = 0x10;
    data->page[3].data[2] = profile->cc_data_area_size;
    data->page[3].data[3] = 0x00;

    size_t tlv_pos = 0;
    uint8_t tlv[NFC_QR_NDEF_MAX + 5];
    tlv[tlv_pos++] = 0x03;
    if(ndef_len < 0xFF) {
        tlv[tlv_pos++] = (uint8_t)ndef_len;
    } else {
        tlv[tlv_pos++] = 0xFF;
        tlv[tlv_pos++] = (uint8_t)((ndef_len >> 8) & 0xFF);
        tlv[tlv_pos++] = (uint8_t)(ndef_len & 0xFF);
    }

    memcpy(&tlv[tlv_pos], ndef, ndef_len);
    tlv_pos += ndef_len;
    tlv[tlv_pos++] = 0xFE;

    const size_t first_user_page = 4;
    const size_t user_pages = profile->pages_total - 9;
    if(tlv_pos > user_pages * MF_ULTRALIGHT_PAGE_SIZE) return false;

    for(size_t i = 0; i < tlv_pos; i++) {
        data->page[first_user_page + (i / MF_ULTRALIGHT_PAGE_SIZE)]
            .data[i % MF_ULTRALIGHT_PAGE_SIZE] = tlv[i];
    }

    const uint16_t config_index = profile->pages_total - 4;
    data->page[config_index].data[0] = 0x04;
    data->page[config_index].data[3] = 0xFF;
    data->page[config_index + 1].data[1] = 0x05;
    memset(&data->page[config_index + 2], 0xFF, sizeof(MfUltralightPage));
    data->page[config_index - 1].data[3] = MF_ULTRALIGHT_TEARING_FLAG_DEFAULT;

    for(size_t i = 0; i < COUNT_OF(data->tearing_flag); i++) {
        data->tearing_flag[i].data = MF_ULTRALIGHT_TEARING_FLAG_DEFAULT;
    }

    return true;
}

static NfcCommand nfc_qr_nfc_listener_callback(NfcGenericEvent event, void* context) {
    UNUSED(event);
    UNUSED(context);
    return NfcCommandContinue;
}

static int32_t nfc_qr_nfc_worker_thread(void* context) {
    NfcQrNfcWorker* worker = context;
    MfUltralightData* tag_data = NULL;
    Nfc* nfc = NULL;
    NfcListener* listener = NULL;

    do {
        if(!worker || !worker->ndef_len) break;

        if(furi_hal_nfc_is_hal_ready() != FuriHalNfcErrorNone) {
            worker->status = NfcQrNfcStatusErrorNfc;
            break;
        }

        tag_data = mf_ultralight_alloc();
        if(!tag_data || !tag_data->iso14443_3a_data) {
            worker->status = NfcQrNfcStatusErrorNoMemory;
            break;
        }

        if(!nfc_qr_build_ntag_data(tag_data, worker->ndef, worker->ndef_len)) {
            worker->status = NfcQrNfcStatusErrorTagBuild;
            break;
        }

        nfc = nfc_alloc();
        if(!nfc) {
            worker->status = NfcQrNfcStatusErrorNfc;
            break;
        }

        listener = nfc_listener_alloc(nfc, NfcProtocolMfUltralight, tag_data);
        if(!listener) {
            worker->status = NfcQrNfcStatusErrorListener;
            break;
        }

        nfc_listener_start(listener, nfc_qr_nfc_listener_callback, worker);
        worker->running = true;
        worker->status = NfcQrNfcStatusRunning;

        while(!worker->stop_requested) {
            furi_delay_ms(100);
        }
    } while(false);

    if(listener) {
        if(worker && worker->running) nfc_listener_stop(listener);
        nfc_listener_free(listener);
    }
    if(nfc) nfc_free(nfc);
    if(tag_data) mf_ultralight_free(tag_data);

    if(worker) {
        worker->running = false;
        if(worker->stop_requested) worker->status = NfcQrNfcStatusIdle;
    }

    return 0;
}

void nfc_qr_nfc_worker_init(NfcQrNfcWorker* worker) {
    if(!worker) return;

    memset(worker, 0, sizeof(NfcQrNfcWorker));
    worker->status = NfcQrNfcStatusIdle;
}

bool nfc_qr_nfc_worker_start(NfcQrNfcWorker* worker, const uint8_t* ndef, size_t ndef_len) {
    if(!worker) return false;

    nfc_qr_nfc_worker_stop(worker);

    if(!ndef || (ndef_len == 0) || (ndef_len > sizeof(worker->ndef))) {
        worker->status = NfcQrNfcStatusErrorPayload;
        return false;
    }

    if(!nfc_qr_pick_profile(ndef_len)) {
        worker->status = NfcQrNfcStatusErrorPayload;
        return false;
    }

    memcpy(worker->ndef, ndef, ndef_len);
    worker->ndef_len = ndef_len;
    worker->stop_requested = false;
    worker->running = false;
    worker->status = NfcQrNfcStatusStarting;

    worker->thread =
        furi_thread_alloc_ex("NfcQrNdefEmu", 6144, nfc_qr_nfc_worker_thread, worker);
    if(!worker->thread) {
        worker->status = NfcQrNfcStatusErrorNoMemory;
        return false;
    }

    furi_thread_start(worker->thread);
    return true;
}

void nfc_qr_nfc_worker_stop(NfcQrNfcWorker* worker) {
    if(!worker) return;

    if(worker->thread) {
        worker->stop_requested = true;
        furi_thread_join(worker->thread);
        furi_thread_free(worker->thread);
        worker->thread = NULL;
    }

    worker->running = false;
    worker->stop_requested = false;
    worker->ndef_len = 0;
    worker->status = NfcQrNfcStatusIdle;
}

const char* nfc_qr_nfc_worker_status_text(NfcQrNfcStatus status) {
    switch(status) {
    case NfcQrNfcStatusIdle:
        return "NFC inactivo";
    case NfcQrNfcStatusDisabled:
        return "NDEF listo";
    case NfcQrNfcStatusStarting:
        return "NFC iniciando";
    case NfcQrNfcStatusRunning:
        return "NFC activo";
    case NfcQrNfcStatusErrorPayload:
        return "NDEF excede tag";
    case NfcQrNfcStatusErrorNoMemory:
        return "Memoria NFC insuf.";
    case NfcQrNfcStatusErrorTagBuild:
        return "Tag NDEF invalido";
    case NfcQrNfcStatusErrorNfc:
        return "NFC no disponible";
    case NfcQrNfcStatusErrorListener:
        return "Listener NFC fallo";
    default:
        return "Error NFC";
    }
}
