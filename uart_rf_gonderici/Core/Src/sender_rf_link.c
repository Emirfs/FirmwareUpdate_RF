/*
 * sender_rf_link.c — RF Haberlesme Katmani (Gonderici Tarafi)
 *
 * ─── BU DOSYA NE YAPAR? ───────────────────────────────────────────────────
 * Si4432 RF modulu uzerinde paket gonderme/alma islemlerini yonetir.
 * Ust katman (sender_fw_update.c) bu fonksiyonlari dogrudan cagirır;
 * fiziksel RF ayrintilarini bilmesi gerekmez.
 *
 * Uc temel fonksiyon:
 *   RF_SendPacket        — tek seferlik gonderim (ACK beklemez)
 *   RF_WaitForPacket     — zaman asimina kadar gelen paketi bekler
 *   RF_SendChunkReliable — gonder + ACK bekle, basarisiz olursa tekrar dene
 *
 * ─── RF PAKET FORMATI ─────────────────────────────────────────────────────
 * [TYPE:1][SEQ_H:1][SEQ_L:1][PAYLOAD:0-50] + Si4432 donanim CRC (otomatik)
 *   TYPE    : RF_CMD_xxx komutu
 *   SEQ     : 16-bit sequence numarasi (yeniden gonderim tespiti icin)
 *   PAYLOAD : opsiyonel veri
 *
 * ─── DEGISTIRILEBİLECEK SEYLER ───────────────────────────────────────────
 * - ACK bekleme suresi  : RF_ACK_TIMEOUT_MS (rf_protocol.h, su an 2000 ms)
 * - Maks deneme sayisi  : RF_MAX_RETRIES (rf_protocol.h, su an 5)
 * - NACK sonrasi bekleme: HAL_Delay(50) satiri
 *
 * ─── BAGIMLILIKLAR ────────────────────────────────────────────────────────
 * si4432.c → SI4432_SendPacket, SI4432_StartRx, SI4432_CheckRx
 */

#include "sender_rf_link.h"

#include "iwdg.h"
#include "rf_protocol.h"
#include "si4432.h"
#include <string.h>

/* Gelen RF paketlerinin ham verisi burada okunur (maks 64 byte = Si4432 FIFO) */
static uint8_t rf_rx_buf[64];

/*
 * RF_SendPacket — Bir RF paketi gonder (ACK beklemez, "fire and forget")
 *
 * payload NULL veya payload_len 0 ise sadece 3 byte baslik gonderilir.
 * payload_len RF_MAX_DATA'yi (50) asarsa otomatik kisaltilir.
 */
void RF_SendPacket(uint8_t type, uint16_t seq, const uint8_t *payload,
                   uint8_t payload_len) {
  uint8_t pkt[RF_MAX_PACKET];
  uint8_t total_len = RF_HEADER_LEN; // Baslangicta sadece baslik (3 byte)

  pkt[0] = type;                   // Paket tipi (RF_CMD_xxx)
  pkt[1] = (uint8_t)(seq >> 8);    // Sequence number — yuksek byte
  pkt[2] = (uint8_t)(seq & 0xFF);  // Sequence number — dusuk byte

  if (payload && payload_len > 0) {
    if (payload_len > RF_MAX_DATA) {
      payload_len = RF_MAX_DATA; // Si4432 FIFO sinirini asma
    }

    memcpy(&pkt[3], payload, payload_len); // Payload'u basligin ardina kopyala
    total_len += payload_len;
  }

  SI4432_SendPacket(pkt, total_len); // Fiziksel gonderim (si4432.c)
}

/*
 * RF_WaitForPacket — timeout_ms sure icinde gelen paketi bekle
 *
 * Donus: 1 = paket alindi, 0 = zaman asimi
 * Paket alindiysa *type, *seq, payload[] ve *payload_len doldurulur.
 *
 * NOT: Her cagrida SI4432_StartRx() ile alici moda gecer.
 *      Onceki RX durumu sifirlanir.
 */
uint8_t RF_WaitForPacket(uint8_t *type, uint16_t *seq, uint8_t *payload,
                         uint8_t *payload_len, uint32_t timeout_ms) {
  uint32_t start = HAL_GetTick();

  SI4432_StartRx(); // Si4432'yi alici moduna al, RX FIFO'yu temizle

  while ((HAL_GetTick() - start) < timeout_ms) {
    HAL_IWDG_Refresh(&hiwdg); // Watchdog — bekleme suresi uzun olabilir

    uint8_t len = SI4432_CheckRx(rf_rx_buf); // nIRQ'yu kontrol et, paket varsa oku
    if (len >= RF_HEADER_LEN) {              // En az baslik kadar veri = gecerli paket
      *type        = rf_rx_buf[0];                                   // Paket tipi
      *seq         = ((uint16_t)rf_rx_buf[1] << 8) | rf_rx_buf[2];  // Sequence number
      *payload_len = len - RF_HEADER_LEN;                            // Payload uzunlugu
      if (*payload_len > 0 && payload) {
        memcpy(payload, &rf_rx_buf[3], *payload_len); // Payload'u ciktiya kopyala
      }
      return 1; // Basarili
    }
  }

  return 0; // Zaman asimi — paket gelmedi
}

/*
 * RF_SendChunkReliable — Paketi gonder, ACK bekle; basarisizsa tekrar dene
 *
 * RF_MAX_RETRIES kez dener. Her denemede:
 *   1. Paketi gonder
 *   2. RF_ACK_TIMEOUT_MS sure ACK/NACK bekle
 *   3. ACK + dogru seq → 1 don (basarili)
 *   4. NACK → 50 ms bekle, tekrar dene
 *   5. Timeout/yanlis tip → tekrar dene
 *
 * Donus: 1 = basarili, 0 = tum denemeler tukendi
 *
 * Kullanim: Her DATA_CHUNK RF paketi bu fonksiyon ile gonderilir.
 */
uint8_t RF_SendChunkReliable(uint8_t type, uint16_t seq, const uint8_t *payload,
                             uint8_t payload_len) {
  uint8_t rx_type, rx_pld[RF_MAX_DATA];
  uint16_t rx_seq;
  uint8_t rx_pld_len;

  for (uint8_t attempt = 0; attempt < RF_MAX_RETRIES; attempt++) {
    RF_SendPacket(type, seq, payload, payload_len); // Gonder

    /* ACK veya NACK bekle */
    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len,
                         RF_ACK_TIMEOUT_MS)) {
      if (rx_type == RF_CMD_ACK && rx_seq == seq) {
        return 1; // Dogru seq ile ACK alindi — basarili
      }

      if (rx_type == RF_CMD_NACK) {
        HAL_Delay(50); // Alici mesgul — kisa bekle, sonra tekrar dene
      }
      /* Yanlis tip veya seq → bir sonraki denemede tekrar gonder */
    }
    /* Cevap gelmedi (timeout) → tekrar dene */
    HAL_IWDG_Refresh(&hiwdg);
  }

  return 0; // Tum denemeler tukendi — basarisiz
}
