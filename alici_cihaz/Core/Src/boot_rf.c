/*
 * boot_rf.c — RF Haberlesme Katmani (Alici / Bootloader Tarafi)
 *
 * ─── BU DOSYA NE YAPAR? ───────────────────────────────────────────────────
 * Si4432 RF modulu uzerinde paket gonderme ve alma islemlerini yonetir.
 * boot_flow.c bu fonksiyonlari kullanır; fiziksel RF ayrıntılarını bilmez.
 *
 * Uc fonksiyon:
 *   RF_SendPacket   — paketi gonder, ACK beklemez
 *   RF_WaitForPacket— zaman asimina kadar paket bekle
 *   RF_SendReliable — gonder + ACK bekle, basarisizsa tekrar dene
 *
 * ─── RF PAKET FORMATI ─────────────────────────────────────────────────────
 * [TYPE:1][SEQ_H:1][SEQ_L:1][PAYLOAD:0-50] + Si4432 donanim CRC (otomatik)
 *
 * ─── GONDERICI TARAFINDAN FARK ────────────────────────────────────────────
 * Gonderici tarafinda (sender_rf_link.c) ayni mantik vardir.
 * Iki taraf birebir ayni RF protokolunu kullanir.
 *
 * ─── DEGISTIRILEBİLECEK SEYLER ───────────────────────────────────────────
 * - RF_ACK_TIMEOUT_MS : ACK bekleme suresi (rf_bootloader.h, 2000 ms)
 * - RF_MAX_RETRIES    : Maks deneme (rf_bootloader.h, 5)
 */

#include "boot_rf.h"

#include "iwdg.h"
#include "rf_bootloader.h"
#include "si4432.h"
#include <string.h>

/* Gelen RF paketlerinin ham verisi burada okunur */
static uint8_t rf_rx_buf[64];

/*
 * RF_SendPacket — Paketi gonder, ACK bekleme ("fire and forget")
 *
 * payload NULL veya payload_len 0 ise sadece 3 byte baslik gonderilir.
 * payload_len RF_MAX_PAYLOAD'u asarsa kisaltilir.
 */
void RF_SendPacket(uint8_t type, uint16_t seq, const uint8_t *payload,
		uint8_t payload_len) {
	uint8_t pkt[RF_MAX_PACKET_SIZE];
	uint8_t total_len = RF_HEADER_SIZE; // Baslangicta sadece baslik (3 byte)

	pkt[0] = type;                   // Paket tipi (RF_CMD_xxx)
	pkt[1] = (uint8_t) (seq >> 8);    // Sequence number — yuksek byte
	pkt[2] = (uint8_t) (seq & 0xFF);  // Sequence number — dusuk byte

	if (payload && payload_len > 0) {
		if (payload_len > RF_MAX_PAYLOAD) {
			payload_len = RF_MAX_PAYLOAD; // FIFO sinirini asma
		}

		memcpy(&pkt[3], payload, payload_len);
		total_len += payload_len;
	}

	SI4432_SendPacket(pkt, total_len); // Fiziksel gonderim (si4432.c)
}

/*
 * RF_WaitForPacket — timeout_ms sure icinde gelen paketi bekle
 *
 * Donus: 1 = paket alindi, 0 = zaman asimi
 * Paket alindiysa *type, *seq, payload[], *payload_len doldurulur.
 */
uint8_t RF_WaitForPacket(uint8_t *type, uint16_t *seq, uint8_t *payload,
		uint8_t *payload_len, uint32_t timeout_ms) {
	uint32_t start = HAL_GetTick();

	SI4432_StartRx(); // Si4432'yi alici moduna al, RX FIFO'yu temizle

	while ((HAL_GetTick() - start) < timeout_ms) {
		HAL_IWDG_Refresh(&hiwdg); // Watchdog — uzun bekleme sureleri icin kritik

		uint8_t len = SI4432_CheckRx(rf_rx_buf); // nIRQ kontrol et, paket varsa oku
		if (len >= RF_HEADER_SIZE) {  // En az baslik kadar veri = gecerli paket
			*type = rf_rx_buf[0];                                  // Paket tipi
			*seq = ((uint16_t) rf_rx_buf[1] << 8) | rf_rx_buf[2]; // Sequence number
			*payload_len = len - RF_HEADER_SIZE;             // Payload uzunlugu

			if (*payload_len > 0 && payload) {
				memcpy(payload, &rf_rx_buf[3], *payload_len);
			}

			return 1; // Basarili
		}
	}

	return 0; // Zaman asimi — paket gelmedi
}

/*
 * RF_SendReliable — Paketi gonder, ACK bekle; basarisizsa tekrar dene
 *
 * RF_MAX_RETRIES kez dener. Her denemede:
 *   1. Paketi gonder
 *   2. RF_ACK_TIMEOUT_MS sure ACK bekle
 *   3. ACK + dogru seq → 1 don
 *   4. Yanlis cevap veya timeout → tekrar dene
 *
 * Donus: 1 = basarili (ACK alindi), 0 = basarisiz
 *
 * NOT: boot_flow.c'de DATA_CHUNK icin RF_SendPacket + RF_WaitForPacket
 * dogrudan kullanilir. Bu fonksiyon daha sade durumlar icin vardir.
 */
uint8_t RF_SendReliable(uint8_t type, uint16_t seq, const uint8_t *payload,
		uint8_t payload_len) {
	uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
	uint16_t rx_seq;
	uint8_t rx_pld_len;

	for (uint8_t attempt = 0; attempt < RF_MAX_RETRIES; attempt++) {
		RF_SendPacket(type, seq, payload, payload_len); // Gonder

		if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len,
		RF_ACK_TIMEOUT_MS)) {
			if (rx_type == RF_CMD_ACK && rx_seq == seq) {
				return 1; // Dogru ACK alindi — basarili
			}
		}
		/* ACK gelmediyse veya yanlis — bir sonraki denemede tekrar gonder */
		HAL_IWDG_Refresh(&hiwdg);
	}

	return 0; // Tum denemeler tukendi
}
