/*
 * sender_fw_update.c — Firmware Update Ana Akış (Gönderici Tarafı)
 *
 * ─── BU DOSYA NE YAPAR? ───────────────────────────────────────────────────
 * PC'den gelen 'W' komutuyla başlayan firmware güncelleme sürecini yönetir.
 * Protokol adımları sırasıyla:
 *   1. Alıcıya RF üzerinden BOOT_REQUEST gönderir → BOOT_ACK bekler
 *      BOOT_ACK payload'ında alıcı kaldığı yeri bildirir (resume_start_packet)
 *   2. PC'den 12 byte metadata alır → RF ile alıcıya gönderir
 *   3. Alıcının hazır olmasını bekler (FLASH_ERASE_DONE — artık anında gelir)
 *   4. PC'den 148 byte'lık paketler alır (IV+Encrypted+CRC) →
 *      İlk resume_start_packet kadar paketi PC'den alır ama RF'e göndermez
 *      (alıcı zaten yazmış — sadece PC'ye ACK verir, yeniden göndermez).
 *      Kalan paketleri 4 RF DATA_CHUNK parçasına bölerek güvenilir gönderir.
 *   5. Alıcıdan UPDATE_COMPLETE / UPDATE_FAILED bekler → PC'ye sonucu bildirir
 *
 * ─── DEĞİŞTİRİLEBİLECEK ŞEYLER ───────────────────────────────────────────
 * - Boot ACK bekleme süresi: while döngüsündeki 30000 ms
 * - Flash silme bekleme süresi: while döngüsündeki 60000 ms
 * - Final sonuç bekleme süresi: while döngüsündeki 45000 ms
 * - Metadata retry sayısı: RF_MAX_RETRIES (rf_protocol.h)
 * - UART paket bekleme süresi: HAL_UART_Receive'deki 8000 ms
 *
 * ─── BAĞIMLILIKLAR ────────────────────────────────────────────────────────
 * sender_rf_link.c  → RF_SendPacket, RF_WaitForPacket, RF_SendChunkReliable
 * sender_state.h    → fw_packet_buf, rf_seq_counter
 * rf_protocol.h     → RF_CMD_xxx, RF_CHUNKS_PER_PACKET, UART_ACK/NACK
 */

#include "sender_fw_update.h"

#include "c25519.h"
#include "entropy.h"
#include "iwdg.h"
#include "rf_protocol.h"
#include "sender_rf_link.h"
#include "sender_state.h"
#include "sender_uart_debug.h"
#include "usart.h"
#include <string.h>

void FirmwareUpdate_Mode(void) {
  uint8_t ack = UART_ACK;   // PC'ye başarı bildirimi (0x06)
  uint8_t nack = UART_NACK; // PC'ye hata bildirimi (0x15)
  uint8_t rx_type, rx_pld[RF_MAX_DATA];
  uint16_t rx_seq;
  uint8_t rx_pld_len;

  /* Kaldigi yerden devam noktasi: alici BOOT_ACK payload'inda bildirir.
   * 0 = bastan basla, N > 0 = ilk N paket zaten yazilmis, atla. */
  uint32_t resume_start_packet = 0;

  /* FW update sırasında debug print'i kapat — UART meşgul olacak */
  SenderDebug_SetEnabled(0);

  HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET); // LED0 yak: FW update aktif

  /* ── ECDH: pub_sender'i PC'den al ──────────────────────────────────────
   * Yeni UART protokolu: PC 'W'(1) + pub_sender(32) = 33 byte gonderir.
   * pub_sender: Python tarafinda uretilen X25519 public key.
   * Eski protokol ('W' tek byte): pub_sender alanamazsa fallback. */
  uint8_t pub_sender[32] = {0};
  uint8_t uart_start_buf[UART_FW_START_SIZE]; /* 'W' + pub_sender */

  /* Ilk byte'i zaten main.c okumuş ('W'), kalan 32 byte'i oku */
  if (HAL_UART_Receive(&huart1, pub_sender, 32, 500) != HAL_OK) {
    /* Eski protokol / pub_sender gelmedi — pub_sender sifir kalir */
    Print("[FW] pub_sender alinamadi, eski protokol!\r\n");
  }
  (void)uart_start_buf; /* Kullanilmadi — suppress warning */

  Print("[FW] Aliciya BOOT_REQUEST gonderiliyor...\r\n");

  /* ===================================================================
   * ADIM 1: BOOT_REQUEST -> BOOT_ACK (ECDH Key Exchange)
   *
   * Yeni BOOT_REQUEST payload (32 byte): [pub_sender:32]
   * Yeni BOOT_ACK payload (36 byte):     [resume_start:4][pub_receiver:32]
   *
   * Alici BOOT_ACK ile kendi public key'ini gonderir.
   * Biz pub_receiver'i PC'ye iletiyoruz; PC session key'i hesaplar.
   * =================================================================== */
  uint8_t boot_ack_received = 0;
  uint32_t boot_start = HAL_GetTick();
  uint8_t pub_receiver[32] = {0};

  while (!boot_ack_received && (HAL_GetTick() - boot_start) < 30000) {
    HAL_IWDG_Refresh(&hiwdg); // Watchdog'u sifirla — loop uzun surebilir

    /* BOOT_REQUEST gonder: payload = pub_sender (32 byte) */
    RF_SendPacket(RF_CMD_BOOT_REQUEST, rf_seq_counter, pub_sender,
                  BOOT_REQUEST_PLD_SIZE);

    /* 2 saniye BOOT_ACK bekle */
    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 2000)) {
      if (rx_type == RF_CMD_BOOT_ACK && rx_pld_len >= BOOT_ACK_PLD_SIZE) {
        boot_ack_received = 1;

        /* BOOT_ACK payload: [resume_start:4][pub_receiver:32] */
        resume_start_packet = (uint32_t)rx_pld[0]
                            | ((uint32_t)rx_pld[1] << 8)
                            | ((uint32_t)rx_pld[2] << 16)
                            | ((uint32_t)rx_pld[3] << 24);
        memcpy(pub_receiver, &rx_pld[4], 32);

        if (resume_start_packet > 0) {
          Print("[FW] Resume: ");
          PrintHex((uint8_t)(resume_start_packet >> 8));
          PrintHex((uint8_t)(resume_start_packet & 0xFF));
          Print(" paket zaten yazilmis, devam ediliyor\r\n");
        } else {
          Print("[FW] Alici bootloader'a gecti! (bastan baslaniyor)\r\n");
        }
      }
    }

    HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin); // Beklerken LED yanip sonsun
  }

  if (!boot_ack_received) {
    /* 30 saniye icinde BOOT_ACK gelmedi — PC'ye NACK gonder ve cik */
    Print("[FW] HATA: Alici bootloader'a gecilemedi!\r\n");
    HAL_UART_Transmit(&huart1, &nack, 1, 100);
    HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
    return;
  }

  /* Alici hazir — PC'ye pub_receiver(32) + ACK(1) gonder
   * PC bu bilgiyle session key'i hesaplar ve firmware'i sifrelr. */
  HAL_UART_Transmit(&huart1, pub_receiver, 32, 200); /* pub_receiver */
  HAL_UART_Transmit(&huart1, &ack, 1, 100);           /* ACK */

  /* ===================================================================
   * ADIM 1b: KOMUT BYTE'I + OPSİYONEL KEY_UPDATE + METADATA
   *
   * Python her zaman 1 byte komut gönderir:
   *   0x00 = KEY_UPDATE yok, doğrudan metadata (12 byte) izler
   *   0x08 = KEY_UPDATE var (33 byte), sonra metadata (12 byte) + ACK bekle
   *
   * Bu tasarım, metadata'nın ilk byte'ının 0x08 olması riskini ortadan kaldırır.
   * =================================================================== */
  {
    uint8_t cmd_byte = 0;
    if (HAL_UART_Receive(&huart1, &cmd_byte, 1, 10000) != HAL_OK) {
      Print("[FW] HATA: Komut byte'i alinamadi!\r\n");
      HAL_UART_Transmit(&huart1, &nack, 1, 100);
      return;
    }

    if (cmd_byte == RF_CMD_KEY_UPDATE) {
      /* KEY_UPDATE paketi al ve RF ile ileri gonder */
      uint8_t ku_payload[KEY_UPDATE_PLD_SIZE]; /* enc_key(32) + crc8(1) */
      if (HAL_UART_Receive(&huart1, ku_payload, KEY_UPDATE_PLD_SIZE, 5000) != HAL_OK) {
        Print("[FW] HATA: KEY_UPDATE payload alinamadi!\r\n");
        HAL_UART_Transmit(&huart1, &nack, 1, 100);
        return;
      }

      uint8_t ku_sent = 0;
      for (uint8_t retry = 0; retry < RF_MAX_RETRIES && !ku_sent; retry++) {
        RF_SendPacket(RF_CMD_KEY_UPDATE, rf_seq_counter, ku_payload,
                      KEY_UPDATE_PLD_SIZE);
        if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
          if (rx_type == RF_CMD_KEY_UPDATE_ACK) {
            ku_sent = 1;
          }
        }
        HAL_IWDG_Refresh(&hiwdg);
      }
      rf_seq_counter++;
      HAL_UART_Transmit(&huart1, ku_sent ? &ack : &nack, 1, 100);
      if (!ku_sent) {
        Print("[FW] HATA: KEY_UPDATE gonderilemedi!\r\n");
        return;
      }
      Print("[FW] Master key guncellendi!\r\n");
    }
    /* cmd_byte == 0x00 veya baska deger: KEY_UPDATE yok, doğrudan metadata */

    /* 12 byte metadata al ve RF ile ilet */
    uint8_t meta_buf[12];
    if (HAL_UART_Receive(&huart1, meta_buf, 12, 10000) != HAL_OK) {
      Print("[FW] HATA: Metadata alinamadi!\r\n");
      HAL_UART_Transmit(&huart1, &nack, 1, 100);
      return;
    }

    Print("[FW] Metadata alindi, RF ile gonderiliyor...\r\n");

    uint8_t meta_sent = 0;
    for (uint8_t retry = 0; retry < RF_MAX_RETRIES && !meta_sent; retry++) {
      RF_SendPacket(RF_CMD_METADATA, rf_seq_counter, meta_buf, 12);
      if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
        if (rx_type == RF_CMD_ACK) {
          meta_sent = 1;
        }
      }
      HAL_IWDG_Refresh(&hiwdg);
    }
    rf_seq_counter++;

    if (!meta_sent) {
      Print("[FW] HATA: Metadata gonderilemedi!\r\n");
      HAL_UART_Transmit(&huart1, &nack, 1, 100);
      return;
    }
    HAL_UART_Transmit(&huart1, &ack, 1, 100);
  }

  /* ===================================================================
   * [ESKİ ADIM 2 ARTIK YUKARIDA ENTEGRE]
   * ADIM 2: METADATA AL -> RF ILE GONDER
   * Bu adim yukaridaki ADIM 1b blogu ile entegre edildi.
   * =================================================================== */

  /* ===================================================================
   * ADIM 3: ALICI HAZIR BİLDİRİMİ (FLASH_ERASE_DONE)
   * Onceki tasarimda burada alici tum Flash'i siliyordu (~555ms).
   * Artik alici Flash'i sayfa sayfa siliyor (her 128-byte yazimdan once),
   * bu yuzden FLASH_ERASE_DONE cok daha hizli geliyor.
   * =================================================================== */
  Print("[FW] Alicinin hazir olması bekleniyor...\r\n");

  uint8_t erase_done = 0;
  uint32_t erase_start = HAL_GetTick();

  while (!erase_done && (HAL_GetTick() - erase_start) < 60000) { // 60 sn timeout
    HAL_IWDG_Refresh(&hiwdg);

    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 5000)) {
      if (rx_type == RF_CMD_FLASH_ERASE_DONE) {
        erase_done = 1;
        RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0); // Aliciya: flash silme onaylandi
      }
    }
  }

  if (!erase_done) {
    Print("[FW] HATA: Flash silme zaman asimi!\r\n");
    HAL_UART_Transmit(&huart1, &nack, 1, 100);
    return;
  }

  Print("[FW] Flash silindi!\r\n");
  HAL_UART_Transmit(&huart1, &ack, 1, 100); // PC'ye: flash silindi, paket gondermeye basla

  /* ===================================================================
   * ADIM 4: PAKET TRANSFERI (RESUME DESTEKLI)
   *
   * PC'den 148 byte'lik paket al:
   *   [IV:16][AES_Encrypted:128][CRC32:4] = 148 byte
   *
   * İlk resume_start_packet kadar paket ATLANIYOR:
   *   - PC'den paket okunur (UART alimi yapilir)
   *   - RF'e gonderilmez (alici zaten bu paketleri yazmis)
   *   - PC'ye ACK verilir (PC normal akisla devam eder)
   *
   * Sonraki paketler normal sekilde gonderiliyor:
   *   - 148 byte'i 4 RF DATA_CHUNK'a bol
   *   - Her chunk icin ACK alinmazsa RF_MAX_RETRIES kez dene
   *   - Tum chunk'lar basariliysa PC'ye ACK gonderilir
   * =================================================================== */
  if (resume_start_packet > 0) {
    Print("[FW] Resume: ilk paketler atlaniyor...\r\n");
  } else {
    Print("[FW] Paket transferi basliyor...\r\n");
  }

  uint32_t packets_sent = 0; // Toplam islenen paket sayisi (atlananlar dahil)

  while (1) {
    HAL_IWDG_Refresh(&hiwdg);

    /* PC'den bir sonraki firmware paketini bekle (8 sn) */
    HAL_StatusTypeDef uart_status =
        HAL_UART_Receive(&huart1, fw_packet_buf, FW_FULL_PACKET_SIZE, 8000);

    if (uart_status == HAL_TIMEOUT) {
      /* Timeout — PC tum paketleri gonderdi, donguden cik */
      Print("[FW] Paket bekleme timeout - transfer tamamlandi?\r\n");
      break;
    }

    if (uart_status != HAL_OK) {
      Print("[FW] HATA: UART alma hatasi!\r\n");
      HAL_UART_Transmit(&huart1, &nack, 1, 100);
      return;
    }

    /* Resume: bu paket alici tarafindan zaten yazilmissa RF'e gonderme.
     * PC'den okundu, ACK ver, sonraki pakete gec. */
    if (packets_sent < resume_start_packet) {
      packets_sent++;
      HAL_UART_Transmit(&huart1, &ack, 1, 100); // PC'ye: paket alindi (atlanacak)
      continue; // RF gondermeden devam
    }

    /* 148 byte'i 4 RF chunk'a bol ve guvenilir gonder */
    uint8_t all_chunks_ok = 1;

    for (uint8_t chunk = 0; chunk < RF_CHUNKS_PER_PACKET; chunk++) {
      uint8_t chunk_payload[RF_MAX_DATA];
      uint8_t data_offset = chunk * RF_CHUNK_DATA_SIZE; // Her chunk 48 byte ilerler
      uint8_t data_len;

      if (chunk < RF_CHUNKS_PER_PACKET - 1) {
        data_len = RF_CHUNK_DATA_SIZE; // Ilk 3 chunk tam 48 byte
      } else {
        /* Son chunk: kalan veri (148 - 3x48 = 4 byte) */
        data_len = FW_FULL_PACKET_SIZE - data_offset;
        if (data_len > RF_CHUNK_DATA_SIZE) {
          data_len = RF_CHUNK_DATA_SIZE;
        }
      }

      /* Chunk payload yapisi: [chunk_index:1][chunk_toplam:1][veri:data_len] */
      chunk_payload[0] = chunk;                // Hangi parca? (0, 1, 2, 3)
      chunk_payload[1] = RF_CHUNKS_PER_PACKET; // Toplam parca sayisi (4)
      memcpy(&chunk_payload[2], &fw_packet_buf[data_offset], data_len); // Asil veri

      uint16_t chunk_seq = rf_seq_counter++;

      /* Guvenilir gonder: ACK gelene kadar RF_MAX_RETRIES kez dene */
      if (!RF_SendChunkReliable(RF_CMD_DATA_CHUNK, chunk_seq, chunk_payload,
                                data_len + 2)) {
        Print("[FW] HATA: Chunk gonderilemedi!\r\n");
        all_chunks_ok = 0;
        break;
      }
    }

    if (!all_chunks_ok) {
      /* Herhangi bir chunk basarisiz — PC'ye NACK gonder, DEVAM ET.
       * return YOK: FirmwareUpdate_Mode'dan cikmiyoruz.
       * Python ayni 148 byte'i tekrar gonderecek (retry dongusu),
       * biz de sonraki UART aliminda onu isleyecegiz. */
      HAL_UART_Transmit(&huart1, &nack, 1, 100);
      continue; /* while(1) dongusunun basina don — PC'den yeni paket bekle */
    }

    packets_sent++;
    HAL_UART_Transmit(&huart1, &ack, 1, 100); // PC'ye: bu 148 byte teslim edildi

    HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin); // Her pakette LED yanip sonsun

    /* Her 10 pakette bir UART'a ilerleme yaz (debug amacli) */
    if (packets_sent % 10 == 0) {
      Print("[FW] Paket: ");
      PrintHex((uint8_t)(packets_sent >> 8));
      PrintHex((uint8_t)(packets_sent & 0xFF));
      Print("\r\n");
    }
  }

  /* ===================================================================
   * ADIM 5: FINAL DOGRULAMA SONUCU
   * Alici tum veriyi flash'a yazdiktan sonra CRC-32 dogrulamasi yapar.
   * UPDATE_COMPLETE → basari | UPDATE_FAILED → hata kodu ile basarisiz
   * =================================================================== */
  Print("[FW] Final sonucu bekleniyor...\r\n");

  uint8_t final_received = 0;
  uint32_t final_start = HAL_GetTick();

  while (!final_received && (HAL_GetTick() - final_start) < 45000) { // 45 sn timeout
    HAL_IWDG_Refresh(&hiwdg);

    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 5000)) {
      if (rx_type == RF_CMD_UPDATE_COMPLETE) {
        /* Guncelleme basarili */
        final_received = 1;
        Print("[FW] *** GUNCELLEME BASARILI! ***\r\n");
        RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0); // Aliciya: tebrik alindi
        HAL_UART_Transmit(&huart1, &ack, 1, 100);   // PC'ye: basari

        /* Her iki LED 5 kez yanip sonsun — gorsel basari gostergesi */
        for (int i = 0; i < 5; i++) {
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
          HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
          HAL_Delay(200);
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
          HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
          HAL_Delay(200);
          HAL_IWDG_Refresh(&hiwdg);
        }
      } else if (rx_type == RF_CMD_UPDATE_FAILED) {
        /* Guncelleme basarisiz — alici hata kodu gonderdi */
        final_received = 1;
        Print("[FW] HATA: Guncelleme basarisiz! Hata kodu: ");
        if (rx_pld_len > 0) {
          PrintHex(rx_pld[0]); // RF_ERR_xxx kodu (rf_bootloader.h'e bak)
        }
        Print("\r\n");
        RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0); // Aliciya: mesaj alindi
        HAL_UART_Transmit(&huart1, &nack, 1, 100);  // PC'ye: basarisiz

        /* LED0 10 kez hizli yanip sonsun — hata gostergesi */
        for (int i = 0; i < 10; i++) {
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
          HAL_Delay(100);
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
          HAL_Delay(100);
          HAL_IWDG_Refresh(&hiwdg);
        }
      }
    }
  }

  if (!final_received) {
    /* 45 saniye icinde sonuc gelmedi */
    Print("[FW] Uyari: Final sonucu alinamadi (timeout)\r\n");
    HAL_UART_Transmit(&huart1, &nack, 1, 100);
  }

  /* Temizlik: LED'leri sondur, debug print'i tekrar ac */
  HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);

  SenderDebug_SetEnabled(1); // Normal mod debug print'ini tekrar etkinlestir

  Print("[FW] Firmware update modu sona erdi\r\n");
}
